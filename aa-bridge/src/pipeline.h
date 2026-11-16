// SPDX-License-Identifier: GPL-3.0-or-later
//
// GStreamer pipeline construction + feed, ported from the LIVI reference pipeline in
// livi-src/native/crates/livi-video/src/codec.rs (pipeline_desc / caps_for / presink).
#ifndef AA_BRIDGE_PIPELINE_H
#define AA_BRIDGE_PIPELINE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "gst_min.h" // opaque GstElement / GstBus handle types

namespace aa {

enum class Codec { H264, H265 };

// Builds the gst-parse-launch description:
//
//   appsrc name=src is-live=true do-timestamp=true format=time min-latency=0
//          max-latency=0 caps=<caps>
//     ! <parser> name=parse
//     ! queue max-size-buffers=0 max-size-bytes=8388608 max-size-time=2000000000 leaky=downstream
//     ! <decoder> name=dec
//     ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream
//     ! [videoconvert ! ]<sink> name=sink sync=false
//
// `decoder` and `sink` are chosen by the caller (build-time defaults, run-time
// overridable). A `videoconvert` is inserted ahead of the sink for software decoders
// (name starts with "avdec_"), matching codec.rs::presink; hardware decoders
// (e.g. ducatih264dec) emit sink-ready frames and skip it.
//
// The queue ahead of the decoder drops its oldest frames past 2 s or 8 MiB: a decoder that
// stops taking frames without an error then costs a bounded amount of memory rather than all
// of it (appsrc itself queues without limit behind a parser that blocks). In normal play it
// holds at most a frame.
std::string pipeline_desc(Codec codec, const std::string &decoder, const std::string &sink);

// What poll_bus found.
enum class BusEvent { None, Eos, Error, Stall };

// Thin RAII-ish wrapper around a running pipeline + its appsrc.
class Pipeline {
public:
    ~Pipeline();

    // Builds + starts the pipeline (sets it PLAYING). Returns false on construction or
    // immediate state-change failure; `err_out` carries the reason.
    bool start(Codec codec, const std::string &decoder, const std::string &sink,
               std::string &err_out);

    // Wraps a frame of (Annex-B) H.264 NAL bytes in a GstBuffer and pushes it to appsrc.
    // For H.264, an access unit delimiter goes after each frame (see pipeline.cpp).
    bool push(const std::uint8_t *data, std::size_t n);

    void end_of_stream();

    // Waits up to `timeout_ns` for an EOS or an ERROR on the bus (other messages are dropped),
    // then checks for a stall: the decoder (or the sink) took frames and then stopped putting
    // any out while frames kept coming in. On Error or Stall, `err_out` says what happened.
    // Also logs the frame counters every AA_VIDEO_REPORT_S (default 60) while frames move, at
    // once when frames pile up ahead of the decoder, and once more when the pipeline stops.
    BusEvent poll_bus(std::uint64_t timeout_ns, std::string &err_out);

    void stop();
    bool running() const { return pipeline_ != nullptr; }
    // Pictures pushed, and buffers that reached the sink, since start().
    unsigned frames_pushed() const { return stats_.pushed; }
    unsigned frames_shown() const { return stats_.shown; }

private:
    // Frames counted where they enter (push) and leave the parser, leave the decoder and reach
    // the sink, with the time each spent from push to the parser's and the decoder's output.
    // Pad probes on streaming threads update it; poll_bus reports it.
    struct Stats {
        std::atomic<unsigned> pushed{0}, parsed{0}, decoded{0}, shown{0};
        std::mutex mutex;
        std::deque<std::uint64_t> to_parse, to_decode; // push times (us) of frames in flight
        std::uint64_t parse_sum = 0, parse_max = 0, decode_sum = 0, decode_max = 0;
        unsigned parse_n = 0, decode_n = 0;
        unsigned last_pushed = 0, last_shown = 0, last_timeouts = 0;
        std::uint64_t last_report = 0, last_backlog_report = 0;
        // Stall check: the decoded and shown counts the last time each moved, when, and how
        // many frames had come in (or been decoded) by then.
        unsigned mark_decoded = 0, mark_shown = 0, pushed_at_decoded = 0, decoded_at_shown = 0;
        std::uint64_t decoded_moved = 0, shown_moved = 0;

        void reset();
        void on_push(std::uint64_t now);
        void on_parsed(std::uint64_t now);
        void on_decoded(std::uint64_t now);
        void report(std::uint64_t now, bool force = false);
        bool stalled(std::uint64_t now, std::string &why);
    };
    static GstPadProbeReturn parsed_probe(GstPad *, GstPadProbeInfo *, gpointer self);
    static GstPadProbeReturn decoded_probe(GstPad *, GstPadProbeInfo *, gpointer self);
    static GstPadProbeReturn shown_probe(GstPad *, GstPadProbeInfo *, gpointer self);
    void add_probe(const char *element, const char *pad, GstPadProbeCallback cb);

    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstBus *bus_ = nullptr;
    Codec codec_ = Codec::H264;
    bool aud_ = false;
    Stats stats_;
};

// The process's resident memory (KiB) and open file descriptors, for the periodic reports.
void process_usage(unsigned &rss_kb, unsigned &fds);

} // namespace aa

#endif // AA_BRIDGE_PIPELINE_H
