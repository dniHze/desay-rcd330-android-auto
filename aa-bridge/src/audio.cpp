// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"

#include <cstdio>
#include <ctime>

#include "gst_min.h"

namespace aa {

// How long a failed stream (e.g. its ALSA device was busy) waits before it is rebuilt.
static const std::uint64_t kRetryNs = 2000000000ULL;

static std::uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

static std::string caps_for(const AudioFormat &f) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "audio/x-raw,format=S16LE,rate=%u,channels=%u,layout=interleaved",
                  f.rate, f.channels);
    return std::string(buf);
}

static std::string desc_for(const AudioFormat &f, const std::string &sink) {
    std::string d;
    d += "appsrc name=asrc is-live=true do-timestamp=true format=time min-latency=0 max-latency=0 caps=";
    d += caps_for(f);
    // A short leaky queue keeps playback live without unbounded buildup if the sink stalls.
    d += " ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=500000000 leaky=downstream";
    d += " ! audioconvert ! audioresample ! ";
    d += sink;
    d += " name=asink sync=false";
    return d;
}

AudioOut::~AudioOut() { stop_all(); }

const std::string &AudioOut::sink_for(std::uint32_t id) const {
    auto it = sinks_.find(id);
    return (it != sinks_.end()) ? it->second : sink_;
}

AudioFormat AudioOut::format_for(std::uint32_t id) const {
    auto f = formats_.find(id);
    if (f != formats_.end()) return f->second;
    std::fprintf(stderr, "[aa-bridge] audio id=%u has no configured format; using fallback %uHz/%uch\n",
                 id, fallback_.rate, fallback_.channels);
    return fallback_;
}

// The PulseAudio stream for an id whose sink is "pasimple[-open][:<stream name>]", or nullptr.
PaOut *AudioOut::pa_for(std::uint32_t id, const std::string &sink) {
    if (sink.compare(0, 8, "pasimple") != 0) return nullptr;
    std::string rest = sink.substr(8);
    const bool hold = rest.compare(0, 5, "-open") == 0;
    if (hold) rest.erase(0, 5);
    if (!rest.empty() && rest[0] != ':') return nullptr;
    std::unique_ptr<PaOut> &p = pa_[id];
    if (!p) {
        const AudioFormat f = format_for(id);
        const std::string name = rest.size() > 1 ? rest.substr(1) : "sv-as-carlife-prompt";
        std::fprintf(stderr, "[aa-bridge] audio id=%u -> pulse stream '%s' (%uHz/%uch%s)\n", id,
                     name.c_str(), f.rate, f.channels, hold ? ", open for the session" : "");
        p.reset(new PaOut(id, f.rate, f.channels, name, hold));
    }
    return p.get();
}

void AudioOut::open_held() {
    if (!enabled()) return;
    for (const auto &kv : sinks_) {
        if (kv.second.compare(0, 13, "pasimple-open") == 0) pa_for(kv.first, kv.second);
    }
}

void AudioOut::teardown(AudioStream &s) {
    if (s.pipeline) {
        gst_element_set_state(s.pipeline, GST_STATE_NULL);
    }
    if (s.appsrc) gst_object_unref(s.appsrc);
    if (s.bus) gst_object_unref(s.bus);
    if (s.pipeline) gst_object_unref(s.pipeline);
    s.pipeline = nullptr;
    s.appsrc = nullptr;
    s.bus = nullptr;
}

void AudioOut::mark_failed(AudioStream &s) {
    teardown(s);
    s.failed = true;
    s.retry_at_ns = now_ns() + kRetryNs;
}

AudioStream *AudioOut::ensure(std::uint32_t id) {
    auto it = streams_.find(id);
    if (it != streams_.end()) {
        if (!it->second.failed) {
            return &it->second;
        }
        if (now_ns() < it->second.retry_at_ns) {
            return nullptr; // still backing off; drop this record
        }
        std::fprintf(stderr, "[aa-bridge] audio id=%u retrying\n", id);
    }

    AudioStream &s = streams_[id];
    s = AudioStream();
    s.fmt = format_for(id);

    const std::string &sink = sink_for(id);
    const std::string desc = desc_for(s.fmt, sink);
    std::fprintf(stderr, "[aa-bridge] audio id=%u pipeline: %s\n", id, desc.c_str());

    GError *err = nullptr;
    s.pipeline = gst_parse_launch(desc.c_str(), &err);
    if (!s.pipeline) {
        std::fprintf(stderr, "[aa-bridge] audio id=%u gst_parse_launch failed: %s\n", id,
                     (err && err->message) ? err->message : "?");
        aa_free_gerror(err);
        mark_failed(s);
        return nullptr;
    }
    aa_free_gerror(err); // a warning, if any: the pipeline was built
    s.appsrc = gst_bin_get_by_name(s.pipeline, "asrc");
    s.bus = gst_element_get_bus(s.pipeline);
    if (!s.appsrc || gst_element_set_state(s.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "[aa-bridge] audio id=%u failed to start (sink %s); retry in %llus\n",
                     id, sink.c_str(), (unsigned long long) (kRetryNs / 1000000000ULL));
        mark_failed(s);
        return nullptr;
    }
    std::fprintf(stderr, "[aa-bridge] audio id=%u PLAYING (%uHz/%uch -> %s)\n", id, s.fmt.rate,
                 s.fmt.channels, sink.c_str());
    return &s;
}

bool AudioOut::push(std::uint32_t id, const std::uint8_t *data, std::size_t n) {
    if (!enabled() || n == 0) {
        return true;
    }
    if (PaOut *p = pa_for(id, sink_for(id))) {
        p->push(data, n);
        return true;
    }
    AudioStream *s = ensure(id);
    if (!s || !s->appsrc) {
        return true; // errored stream: drop quietly until its retry (already logged)
    }
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, n, nullptr);
    if (!buf) {
        return false;
    }
    gst_buffer_fill(buf, 0, data, n);
    const GstFlowReturn fr = gst_app_src_push_buffer(reinterpret_cast<GstAppSrc *>(s->appsrc), buf);
    return fr == GST_FLOW_OK;
}

void AudioOut::poll() {
    for (auto &kv : streams_) {
        AudioStream &s = kv.second;
        if (!s.bus || s.failed) {
            continue;
        }
        GstMessage *msg = gst_bus_timed_pop_filtered(s.bus, 0, GST_MESSAGE_ERROR);
        if (msg) {
            GError *gerr = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &gerr, &dbg);
            std::fprintf(stderr, "[aa-bridge] audio id=%u ERROR: %s; retry in %llus\n", kv.first,
                         (gerr && gerr->message) ? gerr->message : "?",
                         (unsigned long long) (kRetryNs / 1000000000ULL));
            aa_free_gerror(gerr, dbg);
            gst_mini_object_unref(msg);
            mark_failed(s); // stop pushing into a dead pipeline; ensure() rebuilds it later
        }
    }
}

void AudioOut::stop_all() {
    for (auto &kv : streams_) {
        AudioStream &s = kv.second;
        if (s.pipeline && s.appsrc && !s.failed) {
            gst_app_src_end_of_stream(reinterpret_cast<GstAppSrc *>(s.appsrc));
        }
        teardown(s);
    }
    streams_.clear();
    for (auto &kv : pa_) {
        kv.second->request_stop(); // all at once: a stuck PulseAudio costs one wait, not one per stream
    }
    pa_.clear();
}

} // namespace aa
