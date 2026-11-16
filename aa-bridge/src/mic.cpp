// SPDX-License-Identifier: GPL-3.0-or-later
#include "mic.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "feedproto.h"

namespace aa {

static std::uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

static bool write_all(int fd, const std::uint8_t *data, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(w);
    }
    return true;
}

static void sleep_ms(long ms) {
    struct timespec t {ms / 1000, (ms % 1000) * 1000 * 1000};
    nanosleep(&t, nullptr);
}

// The loudest sample in a block of S16LE.
static int pcm_peak(const std::uint8_t *data, std::size_t n) {
    int peak = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        const int v = static_cast<std::int16_t>(data[i] | (data[i + 1] << 8));
        if (v > peak) peak = v;
        else if (-v > peak) peak = -v;
    }
    return peak;
}

// One block of S16LE capture to the phone, counting bytes and the loudest sample.
static bool send_pcm(int fd, const std::uint8_t *data, std::size_t n, std::uint64_t &bytes, int &peak) {
    bytes += n;
    const int p = pcm_peak(data, n);
    if (p > peak) peak = p;
    std::vector<std::uint8_t> rec = feedproto::encode(feedproto::KIND_MIC, 0, now_ns(), data, n);
    return write_all(fd, rec.data(), rec.size());
}

// True once the receiver has closed its end of the mic socket (it never writes to it).
static bool peer_closed(int fd) {
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0) return false;
    char b[64];
    const ssize_t n = recv(fd, b, sizeof(b), MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

// libasound, loaded at run time (the desktop build has no ALSA to link against).
namespace {
struct Alsa {
    int (*open)(void **pcm, const char *name, int stream, int mode);
    int (*set_params)(void *pcm, int format, int access, unsigned channels, unsigned rate,
                      int soft_resample, unsigned latency_us);
    int (*start)(void *pcm);
    int (*wait)(void *pcm, int timeout_ms);
    long (*readi)(void *pcm, void *buf, unsigned long frames);
    int (*recover)(void *pcm, int err, int silent);
    int (*close)(void *pcm);
    const char *(*strerror)(int err);
    bool ok;
};

const Alsa &alsa() {
    static const Alsa a = [] {
        Alsa x = {};
        void *lib = dlopen("libasound.so.2", RTLD_LAZY);
        if (!lib) {
            std::fprintf(stderr, "[aa-bridge] mic: libasound.so.2: %s\n", dlerror());
            return x;
        }
        x.open = reinterpret_cast<decltype(x.open)>(dlsym(lib, "snd_pcm_open"));
        x.set_params = reinterpret_cast<decltype(x.set_params)>(dlsym(lib, "snd_pcm_set_params"));
        x.start = reinterpret_cast<decltype(x.start)>(dlsym(lib, "snd_pcm_start"));
        x.wait = reinterpret_cast<decltype(x.wait)>(dlsym(lib, "snd_pcm_wait"));
        x.readi = reinterpret_cast<decltype(x.readi)>(dlsym(lib, "snd_pcm_readi"));
        x.recover = reinterpret_cast<decltype(x.recover)>(dlsym(lib, "snd_pcm_recover"));
        x.close = reinterpret_cast<decltype(x.close)>(dlsym(lib, "snd_pcm_close"));
        x.strerror = reinterpret_cast<decltype(x.strerror)>(dlsym(lib, "snd_strerror"));
        x.ok = x.open && x.set_params && x.start && x.wait && x.readi && x.recover && x.close && x.strerror;
        if (!x.ok) std::fprintf(stderr, "[aa-bridge] mic: libasound.so.2 lacks snd_pcm_*\n");
        return x;
    }();
    return a;
}

const int kCapture = 1;           // SND_PCM_STREAM_CAPTURE
const int kS16LE = 2;             // SND_PCM_FORMAT_S16_LE
const int kRwInterleaved = 3;     // SND_PCM_ACCESS_RW_INTERLEAVED
} // namespace

void MicCapture::start() {
    if (!enabled()) {
        return;
    }
    if (src_.compare(0, 5, "alsa:") != 0 || src_.size() == 5) {
        std::fprintf(stderr, "[aa-bridge] mic: source '%s' is not alsa:<device>; no microphone\n", src_.c_str());
        return;
    }
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] { run(); });
}

void MicCapture::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MicCapture::run() {
    while (running_.load()) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            struct timespec t {0, 200 * 1000 * 1000};
            nanosleep(&t, nullptr);
            continue;
        }
        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
            // Socket not up yet (phone hasn't asked for the mic): retry quietly.
            close(fd);
            struct timespec t {0, 200 * 1000 * 1000};
            nanosleep(&t, nullptr);
            continue;
        }
        // A receiver that stops reading must not keep this thread, and the capture device, in
        // write() for good: a second without progress ends the capture as a close would.
        struct timeval tv = {1, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        std::fprintf(stderr, "[aa-bridge] mic: connected to %s, capturing (%uHz/%uch)\n",
                     sock_.c_str(), rate_, channels_);
        const bool go_on = capture_alsa(fd, src_.substr(5));
        close(fd);
        if (!go_on) {
            break; // hard stop requested
        }
        std::fprintf(stderr, "[aa-bridge] mic: session ended, waiting for next request\n");
        // The receiver keeps the socket up while the phone wants the mic: do not spin on it
        // when a capture fails at once.
        sleep_ms(300);
    }
}

// The unit's own CarLife service records like this (CMicrophone::RecordPcmInit): snd_pcm_open
// on MIB2BtOptIn, S16_LE interleaved at the rate and channels the phone asked for; the device
// is a plug over hw:MIB2BtOpt (48 kHz), so ALSA converts. GStreamer's alsasrc on the unit
// failed every time with "not-negotiated" (step 3 run 10). The device may not be ready the
// moment the phone asks (the ASR source is requested at the same time), so opening is
// retried until it works or the phone is done.
bool MicCapture::capture_alsa(int fd, const std::string &device) {
    const Alsa &a = alsa();
    std::uint64_t bytes = 0;
    int peak = 0, tries = 0;
    void *pcm = nullptr;
    while (running_.load() && !peer_closed(fd)) {
        if (!a.ok) {
            sleep_ms(500);
            continue;
        }
        int r = a.open(&pcm, device.c_str(), kCapture, 0);
        if (r == 0) r = a.set_params(pcm, kS16LE, kRwInterleaved, channels_, rate_, 1, 100000);
        if (r == 0) r = a.start(pcm);
        if (r == 0) break;
        if (tries++ == 0) {
            std::fprintf(stderr, "[aa-bridge] mic: %s: %s; retrying\n", device.c_str(), a.strerror(r));
        }
        if (pcm) a.close(pcm);
        pcm = nullptr;
        sleep_ms(300);
    }
    if (pcm) {
        std::fprintf(stderr, "[aa-bridge] mic: %s open (%uHz/%uch S16LE)%s\n", device.c_str(), rate_,
                     channels_, tries ? " after retrying" : "");
        const unsigned long frames = rate_ / 50; // 20 ms
        std::vector<std::uint8_t> buf(frames * channels_ * 2);
        int quiet_ms = 0;
        // A line a second while capturing (frames read, loudest sample, recoveries from
        // overruns or errors): if the unit resets during voice, the last line shows what the mic
        // was doing (the owner suspects the mic or PulseAudio).
        std::uint64_t sec_start = now_ns();
        unsigned long sec_frames = 0;
        int sec_peak = 0, sec_recovers = 0, sec_last_err = 0;
        while (running_.load() && !peer_closed(fd)) {
            const std::uint64_t t = now_ns();
            if (t - sec_start >= 1000000000ULL) {
                std::fprintf(stderr, "[aa-bridge] mic: %.1fs %lu frames, peak %d, recovered %d%s%s\n",
                             (t - sec_start) / 1e9, sec_frames, sec_peak, sec_recovers,
                             sec_last_err ? ", last error " : "", sec_last_err ? a.strerror(sec_last_err) : "");
                sec_start = t;
                sec_frames = 0;
                sec_peak = sec_recovers = sec_last_err = 0;
            }
            int r = a.wait(pcm, 200);
            if (r == 0) {
                if ((quiet_ms += 200) == 2000) {
                    std::fprintf(stderr, "[aa-bridge] mic: %s gave no data for 2 s\n", device.c_str());
                }
                continue;
            }
            long n = r < 0 ? r : a.readi(pcm, buf.data(), frames);
            if (n < 0) {
                ++sec_recovers;
                sec_last_err = static_cast<int>(n);
                if (a.recover(pcm, static_cast<int>(n), 1) < 0) {
                    std::fprintf(stderr, "[aa-bridge] mic: %s: %s\n", device.c_str(), a.strerror(static_cast<int>(n)));
                    break;
                }
                continue;
            }
            quiet_ms = 0;
            sec_frames += static_cast<unsigned long>(n);
            if (n > 0) {
                const int p = pcm_peak(buf.data(), static_cast<std::size_t>(n) * channels_ * 2);
                if (p > sec_peak) sec_peak = p;
            }
            if (n > 0 && !send_pcm(fd, buf.data(), static_cast<std::size_t>(n) * channels_ * 2, bytes, peak)) {
                break; // the receiver closed the socket, or stopped reading it (SO_SNDTIMEO)
            }
        }
        a.close(pcm);
    }
    std::fprintf(stderr, "[aa-bridge] mic: captured %llu bytes (%.1f s), peak level %d of 32767\n",
                 (unsigned long long) bytes, bytes / (2.0 * rate_ * channels_), peak);
    return running_.load();
}

} // namespace aa
