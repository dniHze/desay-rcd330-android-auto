// SPDX-License-Identifier: GPL-3.0-or-later
#include "pa_out.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>

#include "notify.h"

namespace aa {

namespace {

// <pulse/sample.h>, <pulse/def.h>: stable public ABI.
struct PaSampleSpec {
    int format; // pa_sample_format_t
    std::uint32_t rate;
    std::uint8_t channels;
};
struct PaBufferAttr {
    std::uint32_t maxlength, tlength, prebuf, minreq, fragsize;
};
const int kPaSampleS16LE = 3;
const int kPaStreamPlayback = 1;
const std::uint32_t kPaDefault = static_cast<std::uint32_t>(-1);

struct PaSimple {
    void *(*open)(const char *server, const char *name, int dir, const char *dev,
                  const char *stream_name, const PaSampleSpec *ss, const void *map,
                  const PaBufferAttr *attr, int *error);
    int (*write)(void *s, const void *data, std::size_t bytes, int *error);
    int (*drain)(void *s, int *error);
    void (*close)(void *s);
    const char *(*strerror)(int error);
    bool ok;
};

const PaSimple &api() {
    static const PaSimple p = [] {
        PaSimple a = {};
        void *lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
        if (!lib) {
            std::fprintf(stderr, "[aa-bridge] pulse: libpulse-simple.so.0: %s\n", dlerror());
            return a;
        }
        a.open = reinterpret_cast<decltype(a.open)>(dlsym(lib, "pa_simple_new"));
        a.write = reinterpret_cast<decltype(a.write)>(dlsym(lib, "pa_simple_write"));
        a.drain = reinterpret_cast<decltype(a.drain)>(dlsym(lib, "pa_simple_drain"));
        a.close = reinterpret_cast<decltype(a.close)>(dlsym(lib, "pa_simple_free"));
        a.strerror = reinterpret_cast<decltype(a.strerror)>(dlsym(lib, "pa_strerror")); // libpulse, a dependency
        a.ok = a.open && a.write && a.drain && a.close;
        if (!a.ok) std::fprintf(stderr, "[aa-bridge] pulse: libpulse-simple.so.0 lacks pa_simple_*\n");
        return a;
    }();
    return p;
}

const char *pa_error(int e) {
    const char *s = api().strerror ? api().strerror(e) : nullptr;
    return s ? s : "?";
}

} // namespace

static const std::uint32_t kSinkRate = 48000; // playback_dmix_0_1 (system.pa)
// How long the session end waits for a stream's thread before leaving it behind.
static const std::uint64_t kJoinUs = 2000000;
// After a failed pa_simple_new, the stream waits this long before it tries again (the data
// that comes meanwhile is dropped): a missing daemon costs one attempt and one line a second.
static const std::uint64_t kRetryUs = 1000000;
// Data this long after the last ends one sound and starts the next (the server's buffer, at most
// 2 s, has run dry by then).
static const std::uint64_t kSoundGapUs = 500000;

// The server's buffer per stream, in ms: AA_PA_BUFFER_MS (50..2000), default 200. Enough to
// ride out AA's bursty delivery without underruns (which break up a sound), short enough for
// prompts. A wait for data longer than it, in the middle of a sound, is likely an underrun;
// each sound's log line counts them.
static std::uint32_t buffer_ms() {
    static const std::uint32_t ms = [] {
        const char *v = std::getenv("AA_PA_BUFFER_MS");
        const long n = v && *v ? std::strtol(v, nullptr, 10) : 200;
        return static_cast<std::uint32_t>(n >= 50 && n <= 2000 ? n : 200);
    }();
    return ms;
}

// The loudest sample of interleaved S16LE, as a magnitude.
static int peak_of(const std::vector<std::uint8_t> &pcm) {
    int peak = 0;
    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        int s = static_cast<std::int16_t>(pcm[i] | (pcm[i + 1] << 8));
        if (s < 0) s = -s;
        if (s > peak) peak = s;
    }
    return peak;
}

PaOut::PaOut(std::uint32_t id, std::uint32_t rate, std::uint32_t channels, std::string stream_name, bool hold)
    : st_(std::make_shared<State>()) {
    State &st = *st_;
    st.id = id;
    st.rate = rate;
    st.channels = channels;
    st.hold = hold;
    st.out_rate = rate;
    st.name = std::move(stream_name);
    if (rate > 0 && rate < kSinkRate && kSinkRate % rate == 0) {
        st.up.reset(new Upsampler(kSinkRate / rate, channels));
        st.out_rate = kSinkRate;
    }
    std::shared_ptr<State> shared = st_;
    thread_ = std::thread([shared] { run(shared); });
}

void PaOut::request_stop() {
    std::lock_guard<std::mutex> lk(st_->mutex);
    st_->stop = true;
    st_->cv.notify_all();
}

PaOut::~PaOut() {
    request_stop();
    bool done;
    {
        std::unique_lock<std::mutex> lk(st_->mutex);
        State *st = st_.get();
        done = st->cv.wait_for_us(lk, kJoinUs, [st] { return st->done; });
    }
    if (done) {
        thread_.join();
    } else {
        std::fprintf(stderr, "[aa-bridge] audio id=%u pulse stream '%s' still in PulseAudio after %llu ms; "
                     "left to finish on its own\n", st_->id, st_->name.c_str(),
                     (unsigned long long) (kJoinUs / 1000));
        thread_.detach();
    }
}

void PaOut::push(const std::uint8_t *data, std::size_t n) {
    State &st = *st_;
    const std::size_t limit = st.rate * st.channels * 2; // ~1 s
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        st.queue.emplace_back(data, data + n);
        st.arrived.push_back(mono_us());
        st.queued += n;
        while (st.queued > limit && st.queue.size() > 1) {
            st.queued -= st.queue.front().size();
            st.queue.pop_front();
            st.arrived.pop_front();
            ++st.dropped;
        }
    }
    st.cv.notify_all();
}

void PaOut::run(std::shared_ptr<State> shared) {
    State &st = *shared;
    const PaSimple &pa = api();
    void *s = nullptr;
    std::uint64_t bytes = 0;
    // Every 2 s while sound plays: bytes written and the longest pa_simple_write (a PulseAudio
    // that stalls shows as long writes).
    std::uint64_t win_start = 0, win_bytes = 0, win_longest_us = 0, longest_us = 0;
    // Whether a sound plays (a held stream is fed silence otherwise), when the last data came,
    // and when to try opening again after a failure.
    bool sounding = false;
    std::uint64_t last_data_us = 0, retry_us = 0;
    // This sound, for its log line: when its first data came, its bytes as they came (before
    // upsampling), the longest wait between two pieces of it and how many were longer than the
    // server's buffer (likely underruns), its loudest sample.
    std::uint64_t sound_start_us = 0, sound_in = 0, longest_wait_us = 0;
    unsigned long_waits = 0;
    int peak = 0;
    const std::uint64_t buffer_us = buffer_ms() * 1000ull;
    const auto sound_line = [&](const char *how) {
        const double secs = static_cast<double>(sound_in) / (st.rate * st.channels * 2);
        const std::uint64_t overs = st.up ? st.up->overs() : 0;
        std::fprintf(stderr, "[aa-bridge] audio id=%u sound %s: %.2f s of audio over %.2f s, longest wait for data "
                     "%llu ms (%u longer than the %u ms buffer), peak %.1f dBFS, %llu samples limited, "
                     "longest write %.1f ms\n", st.id, how, secs, (last_data_us - sound_start_us) / 1e6,
                     (unsigned long long) (longest_wait_us / 1000), long_waits, buffer_ms(),
                     peak > 0 ? 20 * std::log10(peak / 32768.0) : -96.0, (unsigned long long) overs,
                     longest_us / 1e3);
    };
    unsigned open_failures = 0; // since the last successful open; only the first is logged
    const std::size_t silence_bytes = st.out_rate / 50 * st.channels * 2; // 20 ms
    const std::vector<std::uint8_t> silence(silence_bytes, 0);
    const auto has_work = [&st] { return st.stop || !st.queue.empty(); };
    for (;;) {
        std::vector<std::uint8_t> chunk;
        std::uint64_t arrived = 0; // when it came: waits are measured on arrival, not on a slow write
        bool quiet = false;        // write silence this round
        {
            std::unique_lock<std::mutex> lk(st.mutex);
            bool idle;
            if (st.hold && pa.ok && !st.stop && st.queue.empty() && (!s || !sounding)) {
                // Held: open now (or retry), and feed silence until data comes. A blocking
                // pa_simple_write paces this at the sink's rate.
                const std::uint64_t now = mono_us();
                if (!s && now < retry_us) {
                    st.cv.wait_for_us(lk, retry_us - now, has_work);
                    continue;
                }
                idle = false;
                quiet = true;
            } else if (st.hold && s && sounding) {
                // Half a second after the last data the sound has ended: silence from then.
                const std::uint64_t until = last_data_us + kSoundGapUs;
                const std::uint64_t now = mono_us();
                if (now < until) st.cv.wait_for_us(lk, until - now, has_work);
                idle = false;
                if (!st.stop && st.queue.empty()) {
                    lk.unlock();
                    sound_line("ended (silence until the next)");
                    sounding = false;
                    continue;
                }
            } else {
                // Close an open stream after a second without data; wait as long as needed otherwise.
                idle = !st.cv.wait_for_us(lk, s ? 1000000u : 3600000000u, has_work);
            }
            if (st.stop || idle) {
                const bool stopping = st.stop;
                const std::uint64_t dropped = st.dropped;
                lk.unlock();
                if (s) {
                    if (sounding)
                        sound_line(stopping && mono_us() - last_data_us < kSoundGapUs ? "cut by the session end"
                                                                                       : "ended");
                    sounding = false;
                    // Drain only when idle: on stop the destructor (the main loop) is waiting.
                    int err = 0;
                    if (!stopping) pa.drain(s, &err);
                    pa.close(s);
                    s = nullptr;
                    std::fprintf(stderr, "[aa-bridge] audio id=%u pulse stream '%s' closed after %llu bytes"
                                 " (%llu chunks dropped, longest write %.1f ms)\n", st.id, st.name.c_str(),
                                 (unsigned long long) bytes, (unsigned long long) dropped, longest_us / 1e3);
                }
                if (stopping) {
                    std::lock_guard<std::mutex> done_lk(st.mutex);
                    st.done = true;
                    st.cv.notify_all();
                    return;
                }
                continue;
            }
            if (!quiet) {
                chunk.swap(st.queue.front());
                st.queue.pop_front();
                arrived = st.arrived.front();
                st.arrived.pop_front();
                st.queued -= chunk.size();
            }
        }
        if (!pa.ok) continue; // logged once in api()
        if (!s) {
            if (!quiet && mono_us() < retry_us) {
                std::lock_guard<std::mutex> lk(st.mutex);
                ++st.dropped; // still backing off after a failed open: this chunk is lost
                continue;
            }
            const PaSampleSpec ss = {kPaSampleS16LE, st.out_rate, static_cast<std::uint8_t>(st.channels)};
            // The server's buffer: buffer_ms().
            const PaBufferAttr attr = {kPaDefault, st.out_rate * st.channels * 2 / 1000 * buffer_ms(), kPaDefault,
                                       kPaDefault, kPaDefault};
            int err = 0;
            s = pa.open(nullptr, "aa-bridge", kPaStreamPlayback, nullptr, st.name.c_str(), &ss, nullptr,
                        &attr, &err);
            if (!s) {
                if (open_failures++ == 0) {
                    std::fprintf(stderr, "[aa-bridge] audio id=%u pulse stream '%s' failed: %s; "
                                 "retrying every %llu ms\n", st.id, st.name.c_str(), pa_error(err),
                                 (unsigned long long) (kRetryUs / 1000));
                }
                retry_us = mono_us() + kRetryUs;
                continue; // this chunk is lost; one after the backoff tries again
            }
            if (open_failures > 1) {
                std::fprintf(stderr, "[aa-bridge] audio id=%u pulse stream '%s' opened after %u failed tries\n",
                             st.id, st.name.c_str(), open_failures);
            }
            open_failures = 0;
            notify_shim("open " + st.name); // the shim registers it for the source's volume
            bytes = 0;
            longest_us = win_bytes = win_longest_us = 0;
            win_start = mono_us();
            sounding = false;
            if (st.hold)
                std::fprintf(stderr, "[aa-bridge] audio id=%u pulse stream '%s' open for the session (%uHz/%uch%s;"
                             " silence between sounds)\n", st.id, st.name.c_str(), st.rate, st.channels,
                             st.up ? ", upsampled to 48000Hz here" : "");
        }
        if (quiet) {
            int err = 0;
            if (pa.write(s, silence.data(), silence.size(), &err) < 0) {
                std::fprintf(stderr, "[aa-bridge] audio id=%u pulse write failed: %s; reopening\n", st.id,
                             pa_error(err));
                pa.close(s);
                s = nullptr;
            }
            continue;
        }
        const std::uint64_t now = arrived;
        if (sounding && now - last_data_us >= kSoundGapUs) {
            // On a stream that stays open between sounds (the next came within its idle second).
            sound_line("ended");
            sounding = false;
        }
        if (sounding) {
            const std::uint64_t wait = now - last_data_us;
            if (wait > longest_wait_us) longest_wait_us = wait;
            if (wait > buffer_us) ++long_waits;
        }
        last_data_us = now;
        if (!sounding) {
            // A new sound: on a held stream the first after silence, else the stream's first or
            // the first after a pause.
            sounding = true;
            sound_in = longest_wait_us = 0;
            long_waits = 0;
            peak = 0;
            sound_start_us = now;
            longest_us = win_bytes = win_longest_us = 0;
            win_start = mono_us();
            if (st.up) {
                st.up->reset();
                st.up->overs();
            }
            std::fprintf(stderr, "[aa-bridge] audio id=%u PLAYING (%uHz/%uch -> pulse stream '%s'%s, %u ms buffer)\n",
                         st.id, st.rate, st.channels, st.name.c_str(),
                         st.up ? ", upsampled to 48000Hz here" : "", buffer_ms());
        }
        sound_in += chunk.size();
        const int p = peak_of(chunk);
        if (p > peak) peak = p;
        if (st.up) chunk = st.up->process(chunk.data(), chunk.size());
        int err = 0;
        const std::uint64_t w0 = mono_us();
        const int wr = pa.write(s, chunk.data(), chunk.size(), &err);
        const std::uint64_t took = mono_us() - w0;
        if (took > win_longest_us) win_longest_us = took;
        if (took > longest_us) longest_us = took;
        if (wr >= 0) win_bytes += chunk.size();
        if (mono_us() - win_start >= 2000000) {
            std::fprintf(stderr, "[aa-bridge] audio id=%u pulse: %llu bytes in %.1f s, longest write %.1f ms\n",
                         st.id, (unsigned long long) win_bytes, (mono_us() - win_start) / 1e6, win_longest_us / 1e3);
            win_start = mono_us();
            win_bytes = win_longest_us = 0;
        }
        if (wr < 0) {
            std::fprintf(stderr, "[aa-bridge] audio id=%u pulse write failed: %s; reopening\n", st.id,
                         pa_error(err));
            pa.close(s);
            s = nullptr;
            continue;
        }
        bytes += chunk.size();
    }
}

} // namespace aa
