// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline.h"

#include <cstdio>
#include <cstdlib>

#include <dirent.h>
#include <unistd.h>

#include "gst_min.h"
#include "mono.h"

// wl_dispatch.cpp's counter, in the device build only.
extern "C" unsigned aa_wl_dispatch_timeouts() __attribute__((weak));

namespace aa {

static const char *caps_for(Codec c) {
    // codec.rs::caps_for — AA sends an Annex-B byte stream.
    return (c == Codec::H265) ? "video/x-h265,stream-format=byte-stream"
                              : "video/x-h264,stream-format=byte-stream";
}

static const char *parser_for(Codec c) {
    return (c == Codec::H265) ? "h265parse" : "h264parse"; // codec.rs::parser_for
}

static bool is_software_decoder(const std::string &dec) {
    // codec.rs::is_hw_decoder: the avdec_* family is software (needs videoconvert).
    return dec.rfind("avdec_", 0) == 0;
}

// The unit's ducatih264dec holds frames back for B-frame reordering, and when the stream does
// not say how many it guesses the level's maximum ("Using 16 frames for reordering"): a frame
// then only comes out once ~16 later ones arrive, and Android Auto sends few frames while the
// screen is still, so the picture lagged until the next touch (step 3 on the unit). AA video has
// no B-frames: tell the decoder through the caps field it reads first (num-reorder-frames) and
// its property (max-reorder-frames). AA_VIDEO_REORDER overrides the 0; "auto" leaves it alone.
static std::string reorder_frames() {
    const char *v = std::getenv("AA_VIDEO_REORDER");
    return (v && *v) ? v : "0";
}

static bool is_ducati(const std::string &dec) { return dec.rfind("ducati", 0) == 0; }

// Off the unit (the RCD330 emulator has no IVA-HD), "identity" as the decoder passes the video on
// undecoded, and the parser is an identity too: the sink gets the phone's Annex-B byte stream as
// it came (with the delimiters below). The unit's h264parse with nothing downstream to ask for a
// format turns it into length-prefixed avc, and forced to byte-stream it dropped every frame as
// "no SPS/PPS yet".
static bool is_passthrough(const std::string &dec) { return dec == "identity"; }

// Android Auto sends one access unit per message, with nothing after its last NAL. The unit's
// h264parse (GStreamer 1.2.3) only knows a NAL has ended once it sees the next start code, and an
// access unit once it sees the first NAL of the next one: every frame waited in the parser for
// the next frame, and AA sends none while the screen is still. An access unit delimiter after
// each frame ends both at once; it becomes the first NAL of the next access unit, where it
// belongs. It carries a 4-byte start code because h264parse wants 6 bytes of the next NAL before
// it looks at it. (The last one leaves the parser alone at end of stream.) Checked against the
// unit's own h264parse: tools/parser_latency_unit.sh. AA_VIDEO_AUD=0 turns this off.
static const std::uint8_t kAud[] = {0, 0, 0, 1, 0x09, 0xf0};

static bool use_aud(Codec codec) {
    const char *v = std::getenv("AA_VIDEO_AUD");
    return codec == Codec::H264 && !(v && std::string(v) == "0");
}

// True if the buffer holds a coded slice (NAL type 1 or 5), i.e. a picture rather than only
// parameter sets: the delimiter goes after pictures only.
static bool has_h264_slice(const std::uint8_t *d, std::size_t n) {
    for (std::size_t i = 0; i + 3 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const unsigned type = d[i + 3] & 0x1f;
            if (type == 1 || type == 5) return true;
            i += 2;
        }
    }
    return false;
}

// The frame counters go to the log every AA_VIDEO_REPORT_S seconds (default 60) while frames
// move; frames piling up ahead of the decoder, a stall and the pipeline's end are logged at once.
static std::uint64_t report_interval_us() {
    static const std::uint64_t us = [] {
        const char *v = std::getenv("AA_VIDEO_REPORT_S");
        const long s = (v && *v) ? std::strtol(v, nullptr, 10) : 60;
        return std::uint64_t(s > 0 ? s : 60) * 1000000u;
    }();
    return us;
}

// More frames than this between the appsrc and the decoder's output is logged as it happens.
static const unsigned kBacklogFrames = 30;
// A decoder (or sink) that took frames and then put none out for this long, while this many
// more came in, has stalled.
static const std::uint64_t kStallUs = 5000000;
static const unsigned kStallFrames = 20;

void process_usage(unsigned &rss_kb, unsigned &fds) {
    rss_kb = fds = 0;
    if (FILE *f = std::fopen("/proc/self/statm", "r")) {
        unsigned long size = 0, resident = 0;
        if (std::fscanf(f, "%lu %lu", &size, &resident) == 2) {
            rss_kb = static_cast<unsigned>(resident * (static_cast<unsigned long>(sysconf(_SC_PAGESIZE)) / 1024));
        }
        std::fclose(f);
    }
    if (DIR *d = opendir("/proc/self/fd")) {
        while (const dirent *e = readdir(d)) {
            if (e->d_name[0] != '.') ++fds;
        }
        closedir(d);
        if (fds) --fds; // the directory's own descriptor
    }
}

std::string pipeline_desc(Codec codec, const std::string &decoder, const std::string &sink) {
    const std::string presink = is_software_decoder(decoder) ? "videoconvert ! " : "";
    const std::string reorder = reorder_frames();
    const bool set_reorder = codec == Codec::H264 && is_ducati(decoder) && reorder != "auto";
    std::string d;
    d += "appsrc name=src is-live=true do-timestamp=true format=time ";
    d += "min-latency=0 max-latency=0 caps=";
    d += caps_for(codec);
    d += " ! ";
    d += is_passthrough(decoder) ? "identity" : parser_for(codec);
    d += " name=parse";
    if (set_reorder) d += " ! video/x-h264,num-reorder-frames=(int)" + reorder;
    d += " ! queue max-size-buffers=0 max-size-bytes=8388608 max-size-time=2000000000 leaky=downstream";
    d += " ! ";
    d += decoder;
    if (set_reorder) d += " max-reorder-frames=" + reorder;
    d += " name=dec";
    d += " ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream";
    d += " ! ";
    d += presink;
    d += sink;
    d += " name=sink sync=false";
    return d;
}

void Pipeline::Stats::reset() {
    std::lock_guard<std::mutex> lk(mutex);
    pushed = parsed = decoded = shown = 0;
    to_parse.clear();
    to_decode.clear();
    parse_sum = parse_max = decode_sum = decode_max = 0;
    parse_n = decode_n = 0;
    last_pushed = last_shown = 0;
    last_timeouts = aa_wl_dispatch_timeouts ? aa_wl_dispatch_timeouts() : 0;
    last_report = last_backlog_report = mono_us();
    mark_decoded = mark_shown = pushed_at_decoded = decoded_at_shown = 0;
    decoded_moved = shown_moved = last_report;
}

void Pipeline::Stats::on_push(std::uint64_t now) {
    std::lock_guard<std::mutex> lk(mutex);
    ++pushed;
    to_parse.push_back(now);
}

// A frame the parser or the decoder holds back or drops shifts which push time the next one
// is matched with; bounding the queues keeps that from growing without end. The counts stay
// exact.
void Pipeline::Stats::on_parsed(std::uint64_t now) {
    std::lock_guard<std::mutex> lk(mutex);
    ++parsed;
    if (to_parse.empty()) return;
    const std::uint64_t t = to_parse.front();
    to_parse.pop_front();
    parse_sum += now - t;
    if (now - t > parse_max) parse_max = now - t;
    ++parse_n;
    to_decode.push_back(t);
    if (to_decode.size() > 64) to_decode.pop_front();
}

void Pipeline::Stats::on_decoded(std::uint64_t now) {
    std::lock_guard<std::mutex> lk(mutex);
    ++decoded;
    if (to_decode.empty()) return;
    const std::uint64_t t = to_decode.front();
    to_decode.pop_front();
    decode_sum += now - t;
    if (now - t > decode_max) decode_max = now - t;
    ++decode_n;
}

void Pipeline::Stats::report(std::uint64_t now, bool force) {
    if (!force && now - last_report < report_interval_us()) return;
    const unsigned timeouts = aa_wl_dispatch_timeouts ? aa_wl_dispatch_timeouts() : 0;
    std::lock_guard<std::mutex> lk(mutex);
    const unsigned p = pushed, a = parsed, d = decoded, s = shown;
    if (p == last_pushed && s == last_shown && timeouts == last_timeouts) {
        last_report = now;
        return;
    }
    unsigned rss_kb, fds;
    process_usage(rss_kb, fds);
    std::fprintf(stderr,
                 "[aa-bridge] video %.1fs: frames +%u (%u in, %u parsed, %u decoded, %u shown) | "
                 "in parser %d, parser->decoder out %d, decoded not shown %d | "
                 "push->parsed avg %.1f max %.1f ms, push->decoded avg %.1f max %.1f ms | "
                 "sink dispatch waits over limit +%u | rss %u KiB, fds %u\n",
                 (now - last_report) / 1e6, p - last_pushed, p, a, d, s, int(p - a), int(a - d),
                 int(d - s),
                 parse_n ? parse_sum / 1e3 / parse_n : 0.0, parse_max / 1e3,
                 decode_n ? decode_sum / 1e3 / decode_n : 0.0, decode_max / 1e3,
                 timeouts - last_timeouts, rss_kb, fds);
    parse_sum = parse_max = decode_sum = decode_max = 0;
    parse_n = decode_n = 0;
    last_pushed = p;
    last_shown = s;
    last_timeouts = timeouts;
    last_report = now;
}

// Only a stage that has already put frames out can stall: before its first frame the parser may
// legitimately be waiting for parameter sets, and the queue's limit bounds that wait anyway. An
// idle feed (screen hidden, video focus NATIVE) moves nothing, so it never looks stalled.
bool Pipeline::Stats::stalled(std::uint64_t now, std::string &why) {
    std::lock_guard<std::mutex> lk(mutex);
    const unsigned p = pushed, d = decoded, s = shown;
    if (d != mark_decoded) {
        mark_decoded = d;
        decoded_moved = now;
        pushed_at_decoded = p;
    }
    if (s != mark_shown) {
        mark_shown = s;
        shown_moved = now;
        decoded_at_shown = d;
    }
    char buf[160];
    if (d > 0 && p - pushed_at_decoded >= kStallFrames && now - decoded_moved >= kStallUs) {
        std::snprintf(buf, sizeof(buf), "decoder stalled: %u frames in, none decoded for %.1f s",
                      p - pushed_at_decoded, (now - decoded_moved) / 1e6);
    } else if (s > 0 && d - decoded_at_shown >= kStallFrames && now - shown_moved >= kStallUs) {
        std::snprintf(buf, sizeof(buf), "sink stalled: %u frames decoded, none shown for %.1f s",
                      d - decoded_at_shown, (now - shown_moved) / 1e6);
    } else {
        return false;
    }
    why = buf;
    return true;
}

GstPadProbeReturn Pipeline::parsed_probe(GstPad *, GstPadProbeInfo *, gpointer self) {
    static_cast<Pipeline *>(self)->stats_.on_parsed(mono_us());
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn Pipeline::decoded_probe(GstPad *, GstPadProbeInfo *, gpointer self) {
    static_cast<Pipeline *>(self)->stats_.on_decoded(mono_us());
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn Pipeline::shown_probe(GstPad *, GstPadProbeInfo *, gpointer self) {
    ++static_cast<Pipeline *>(self)->stats_.shown;
    return GST_PAD_PROBE_OK;
}

void Pipeline::add_probe(const char *element, const char *pad, GstPadProbeCallback cb) {
    GstElement *e = gst_bin_get_by_name(pipeline_, element);
    GstPad *p = e ? gst_element_get_static_pad(e, pad) : nullptr;
    if (p) {
        gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, cb, this, nullptr);
        gst_object_unref(p);
    } else {
        std::fprintf(stderr, "[aa-bridge] no %s pad on '%s': not counted\n", pad, element);
    }
    if (e) gst_object_unref(e);
}

Pipeline::~Pipeline() { stop(); }

bool Pipeline::start(Codec codec, const std::string &decoder, const std::string &sink,
                     std::string &err_out) {
    const std::string desc = pipeline_desc(codec, decoder, sink);
    std::fprintf(stderr, "[aa-bridge] pipeline: %s\n", desc.c_str());
    codec_ = codec;
    aud_ = use_aud(codec);
    std::fprintf(stderr, "[aa-bridge] access unit delimiter after each frame: %s\n",
                 aud_ ? "on" : "off");
    stats_.reset();

    GError *err = nullptr;
    pipeline_ = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline_) {
        err_out = (err && err->message) ? err->message : "gst_parse_launch failed";
        aa_free_gerror(err);
        return false;
    }
    if (err) {
        // Built, but something in the description was not taken (e.g. a property the
        // decoder does not have): worth a line, not a failure.
        std::fprintf(stderr, "[aa-bridge] pipeline built with a warning: %s\n", err->message ? err->message : "?");
        aa_free_gerror(err);
    }
    appsrc_ = gst_bin_get_by_name(pipeline_, "src");
    if (!appsrc_) {
        err_out = "appsrc 'src' not found in pipeline";
        stop();
        return false;
    }
    bus_ = gst_element_get_bus(pipeline_);
    add_probe("parse", "src", &Pipeline::parsed_probe);
    add_probe("dec", "src", &Pipeline::decoded_probe);
    add_probe("sink", "sink", &Pipeline::shown_probe);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        err_out = "failed to set pipeline PLAYING";
        stop();
        return false;
    }
    return true;
}

bool Pipeline::push(const std::uint8_t *data, std::size_t n) {
    if (!appsrc_ || n == 0) {
        return true;
    }
    const bool picture = codec_ != Codec::H264 || has_h264_slice(data, n);
    const std::size_t extra = (aud_ && picture) ? sizeof(kAud) : 0;
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, n + extra, nullptr);
    if (!buf) {
        return false;
    }
    gst_buffer_fill(buf, 0, data, n);
    if (extra) gst_buffer_fill(buf, n, kAud, extra);
    if (picture) stats_.on_push(mono_us());
    // push_buffer takes ownership of the buffer ref (we do not unref it).
    const GstFlowReturn fr = gst_app_src_push_buffer(reinterpret_cast<GstAppSrc *>(appsrc_), buf);
    return fr == GST_FLOW_OK;
}

void Pipeline::end_of_stream() {
    if (appsrc_) {
        gst_app_src_end_of_stream(reinterpret_cast<GstAppSrc *>(appsrc_));
    }
}

BusEvent Pipeline::poll_bus(std::uint64_t timeout_ns, std::string &err_out) {
    if (!bus_) {
        return BusEvent::None;
    }
    // One pop for both: a pop for one type drops every other message it passes, and that
    // used to include the EOS the drain waits for.
    GstMessage *msg = gst_bus_timed_pop_filtered(bus_, timeout_ns, GST_MESSAGE_EOS_OR_ERROR);
    if (msg) {
        if (!gst_message_get_structure(msg)) {
            gst_mini_object_unref(msg);
            return BusEvent::Eos;
        }
        GError *gerr = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_error(msg, &gerr, &dbg);
        err_out = (gerr && gerr->message) ? gerr->message : "pipeline error";
        aa_free_gerror(gerr, dbg);
        gst_mini_object_unref(msg);
        return BusEvent::Error;
    }
    const std::uint64_t now = mono_us();
    // Signed: the delimiter after the last frame comes out of the decoder at end of stream
    // without having been counted in.
    const int backlog = static_cast<int>(stats_.pushed - stats_.decoded);
    if (backlog > static_cast<int>(kBacklogFrames) && now - stats_.last_backlog_report >= 5000000) {
        stats_.last_backlog_report = now;
        std::fprintf(stderr, "[aa-bridge] video: %d frames waiting for the decoder\n", backlog);
        stats_.report(now, true);
    } else {
        stats_.report(now);
    }
    if (stats_.stalled(now, err_out)) {
        stats_.report(now, true);
        return BusEvent::Stall;
    }
    return BusEvent::None;
}

void Pipeline::stop() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        stats_.report(mono_us(), true);
    }
    if (appsrc_) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    if (bus_) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

} // namespace aa
