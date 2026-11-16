// SPDX-License-Identifier: GPL-3.0-or-later
#include "video.h"

#include <cstdio>

#include "mono.h"
#include "notify.h"
#include "watchdog.h"

namespace aa {

namespace {

// After an error, frames wait this long for a keyframe before the pipeline is rebuilt anyway.
const std::uint64_t kKeyframeWaitUs = 5000000;
// At most this many rebuilds in any minute.
const unsigned kRebuildsPerMinute = 3;
const std::uint64_t kMinuteUs = 60000000;
// "appsrc push rejected" at most once in this long (with the count since the last line).
const std::uint64_t kRejectLogUs = 10000000;

const char *codec_name(Codec c) { return c == Codec::H265 ? "h265" : "h264"; }

// One NAL unit of an Annex-B buffer: its type and its bytes, start code included.
struct Nal {
    unsigned type;
    std::size_t begin, end;
};

std::vector<Nal> nal_units(const std::uint8_t *d, std::size_t n, Codec codec) {
    std::vector<Nal> out;
    std::size_t cur = n; // start of the NAL being read; n = none yet
    unsigned cur_type = 0;
    for (std::size_t i = 0; i + 3 < n; ++i) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
        const std::size_t begin = (i > 0 && d[i - 1] == 0) ? i - 1 : i; // 4-byte start code
        if (cur != n) out.push_back(Nal{cur_type, cur, begin});
        const std::uint8_t h = d[i + 3];
        cur_type = codec == Codec::H265 ? (h >> 1) & 0x3fu : h & 0x1fu;
        cur = begin;
        i += 2;
    }
    if (cur != n) out.push_back(Nal{cur_type, cur, n});
    return out;
}

// H.264: SPS 7, PPS 8. H.265: VPS 32, SPS 33, PPS 34.
bool is_parameter_set(Codec c, unsigned t) { return c == Codec::H265 ? (t >= 32 && t <= 34) : (t == 7 || t == 8); }
bool is_sps(Codec c, unsigned t) { return c == Codec::H265 ? (t == 32 || t == 33) : t == 7; }
// Where a decoder can start: an IDR (H.265: any IRAP picture), or the SPS that goes before one.
bool is_keyframe(Codec c, unsigned t) { return c == Codec::H265 ? ((t >= 16 && t <= 21) || is_sps(c, t)) : (t == 5 || t == 7); }

} // namespace

void VideoOut::set_codec(Codec codec) {
    if (pipeline_.running() && codec != codec_) {
        std::fprintf(stderr, "[aa-bridge] video: codec %s from the next rebuild\n", codec_name(codec));
    }
    codec_ = codec;
}

void VideoOut::remember_parameter_sets(const std::uint8_t *data, std::size_t n) {
    for (const Nal &u : nal_units(data, n, codec_)) {
        if (!is_parameter_set(codec_, u.type)) continue;
        std::vector<std::uint8_t> &p = params_[u.type];
        p.assign(data + u.begin, data + u.end);
        if (p.size() >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) p.insert(p.begin(), 0);
    }
}

bool VideoOut::build() {
    announce_ = built_once_; // the session's first build needs no telling: the shim shows it
    built_once_ = true;
    watchdog_tick("video start");
    std::string err;
    if (!pipeline_.start(codec_, decoder_, sink_, err)) {
        fail("pipeline start failed: " + err);
        return false;
    }
    std::fprintf(stderr, "[aa-bridge] pipeline PLAYING (codec=%s decoder=%s sink=%s)\n", codec_name(codec_),
                 decoder_.c_str(), sink_.c_str());
    return true;
}

void VideoOut::fail(const std::string &why) {
    ++errors_;
    std::fprintf(stderr, "[aa-bridge] video ERROR: %s; video restarts at the next keyframe, audio goes on\n",
                 why.c_str());
    watchdog_tick("video stop");
    pipeline_.stop();
    down_since_ = mono_us();
}

bool VideoOut::may_rebuild(std::uint64_t now) {
    while (!rebuilds_.empty() && now - rebuilds_.front() >= kMinuteUs) rebuilds_.pop_front();
    if (rebuilds_.size() >= kRebuildsPerMinute) {
        if (waiting_logged_ == 0 || now - waiting_logged_ >= kMinuteUs) {
            waiting_logged_ = now;
            std::fprintf(stderr, "[aa-bridge] video: %u rebuilds within a minute; the next in %.0f s\n",
                         kRebuildsPerMinute, (kMinuteUs - (now - rebuilds_.front())) / 1e6);
        }
        return false;
    }
    rebuilds_.push_back(now);
    return true;
}

void VideoOut::push(const std::uint8_t *data, std::size_t n) {
    remember_parameter_sets(data, n);
    std::vector<std::uint8_t> primed;
    if (!pipeline_.running()) {
        if (!built_once_) {
            // The session's first frame, whatever it is (as before: the parser waits for its
            // parameter sets itself).
            if (!build()) return;
        } else {
            bool key = false, sps = false;
            for (const Nal &u : nal_units(data, n, codec_)) {
                key = key || is_keyframe(codec_, u.type);
                sps = sps || is_sps(codec_, u.type);
            }
            const std::uint64_t now = mono_us();
            if (!key && now - down_since_ < kKeyframeWaitUs) return; // dropped: no keyframe yet
            if (!may_rebuild(now)) return;
            std::fprintf(stderr, "[aa-bridge] video: rebuilding the pipeline (%s)\n",
                         key ? "keyframe" : "no keyframe within 5 s");
            if (!build()) return;
            if (!sps && !params_.empty()) {
                // A keyframe without its parameter sets: the last ones seen go first.
                for (const auto &kv : params_) primed.insert(primed.end(), kv.second.begin(), kv.second.end());
                primed.insert(primed.end(), data, data + n);
                data = primed.data();
                n = primed.size();
            }
        }
    }
    if (!pipeline_.push(data, n)) {
        ++rejected_;
        const std::uint64_t now = mono_us();
        if (rejected_logged_ == 0 || now - rejected_logged_ >= kRejectLogUs) {
            std::fprintf(stderr, "[aa-bridge] appsrc push rejected (%u since the last line)\n", rejected_);
            rejected_logged_ = now;
            rejected_ = 0;
        }
    }
}

void VideoOut::poll() {
    if (!pipeline_.running()) return;
    std::string err;
    const BusEvent ev = pipeline_.poll_bus(0, err);
    if (ev == BusEvent::Error || ev == BusEvent::Stall) {
        fail(err);
        return;
    }
    if (announce_ && pipeline_.frames_shown() > 0) {
        announce_ = false;
        notify_shim("video");
        std::fprintf(stderr, "[aa-bridge] video: the rebuilt pipeline shows frames; asked the shim to show its surface\n");
    }
}

void VideoOut::finish(bool drain) {
    // Drain: tell the decoder the stream ended and wait briefly for EOS or a late error. A
    // pipeline that never had a frame has nothing to drain (its parser would only report "No
    // valid frames found").
    if (pipeline_.running() && drain && pipeline_.frames_pushed() > 0) {
        pipeline_.end_of_stream();
        std::string err;
        for (int i = 0; i < 20; ++i) { // up to ~2s
            watchdog_tick("video drain");
            const BusEvent ev = pipeline_.poll_bus(100 * 1000000ULL, err);
            if (ev == BusEvent::Eos) {
                std::fprintf(stderr, "[aa-bridge] pipeline reached EOS cleanly\n");
                break;
            }
            if (ev == BusEvent::Error) {
                ++errors_;
                std::fprintf(stderr, "[aa-bridge] pipeline ERROR (drain): %s\n", err.c_str());
                break;
            }
        }
    }
    watchdog_tick("video stop");
    pipeline_.stop();
}

} // namespace aa
