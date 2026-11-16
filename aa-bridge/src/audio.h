// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioOut — plays the AA media/speech/system audio that arrives on the feed socket as
// KIND_AUDIO records (raw interleaved PCM, S16LE). AA negotiates PCM (MediaCodecType
// AudioPcm), so there is nothing to decode: each feed id is pumped straight into its own
//
//   appsrc(caps=audio/x-raw,S16LE,rate,channels) ! queue leaky ! audioconvert ! audioresample ! <sink>
//
// The receiver assigns a DETERMINISTIC feed id per channel (livi-aa-headless audio_feed_id):
// 101=media 48k/2ch, 102=speech 16k/1ch, 103=system 16k/1ch, 104=telephony 16k/1ch. AudioOut
// carries the matching fixed id->format table; `--audio ID:RATE:CH` overrides or adds one.
// An unknown id plays as 48k/2ch and logs it.
//
// Each id can have its own sink (`set_sink`), e.g. media -> alsasink, speech -> pasimple, so
// two AA streams never contend for one exclusive ALSA device. A stream whose pipeline fails
// to start or errors is retried after a short backoff instead of staying dead all session.
//
// The sink "pasimple" (or "pasimple:<stream name>") is not a GStreamer element: that id plays
// through PulseAudio's simple API on its own thread (pa_out.h), as the unit's CarLife service
// plays its prompts, under the stream name "sv-as-carlife-prompt" unless one is given.
// "pasimple-open[:<stream name>]" is the same stream held open for the whole session, silent
// between sounds (open_held; see pa_out.h). Every stream that opens is reported to the CarLife
// shim, which registers it with the unit's audio manager for its volume.
#ifndef AA_BRIDGE_AUDIO_H
#define AA_BRIDGE_AUDIO_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "gst_min.h"
#include "pa_out.h"

namespace aa {

struct AudioFormat {
    std::uint32_t rate;
    std::uint32_t channels;
    // Explicit ctor: in C++11 a struct with default member initializers is not an aggregate,
    // so AudioFormat{rate, ch} would not compile. This keeps brace-init working under gnu++11.
    AudioFormat(std::uint32_t r = 48000, std::uint32_t c = 2) : rate(r), channels(c) {}
};

// One running PCM playback pipeline (one AA audio stream / feed id).
struct AudioStream {
    GstElement *pipeline = nullptr;
    GstElement *appsrc = nullptr;
    GstBus *bus = nullptr;
    AudioFormat fmt;
    bool failed = false;
    std::uint64_t retry_at_ns = 0; // when failed: earliest time to rebuild the pipeline
};

class AudioOut {
public:
    explicit AudioOut(std::string sink) : sink_(std::move(sink)) {}
    ~AudioOut();

    // Register/override the format for a feed id (from --audio ID:RATE:CH).
    void set_format(std::uint32_t id, AudioFormat fmt) { formats_[id] = fmt; }

    // Use `sink` (a gst-launch element description) for `id` instead of the default sink.
    void set_sink(std::uint32_t id, std::string sink) { sinks_[id] = std::move(sink); }

    // Push one PCM record for `id`; lazily builds + PLAYs that id's pipeline. Safe to call
    // with sink "fakesink"/"none" (then it is a no-op counter). Returns false on hard error.
    bool push(std::uint32_t id, const std::uint8_t *data, std::size_t n);

    // Open the "pasimple-open" streams now, at the start of the session, before any sound.
    void open_held();

    // Drain every stream's bus (log + mark errored streams); cheap, call from the main loop.
    void poll();

    // EOS + tear every stream down. The PulseAudio streams get at most 2 s together (pa_out.h).
    void stop_all();

    bool enabled() const { return sink_ != "none"; }

private:
    AudioStream *ensure(std::uint32_t id);
    AudioFormat format_for(std::uint32_t id) const;
    PaOut *pa_for(std::uint32_t id, const std::string &sink);
    const std::string &sink_for(std::uint32_t id) const;
    static void teardown(AudioStream &s);
    static void mark_failed(AudioStream &s);

    std::string sink_;
    AudioFormat fallback_; // for an id with no format: 48k/2ch
    std::map<std::uint32_t, AudioFormat> formats_;
    std::map<std::uint32_t, std::string> sinks_;
    std::map<std::uint32_t, AudioStream> streams_;
    std::map<std::uint32_t, std::unique_ptr<PaOut>> pa_;
};

} // namespace aa

#endif // AA_BRIDGE_AUDIO_H
