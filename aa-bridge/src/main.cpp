// SPDX-License-Identifier: GPL-3.0-or-later
//
// aa-bridge — reads the Android Auto video feed that the LIVI Rust receiver emits on a
// unix socket (feedproto framing) and pushes it through GStreamer to a hardware H.264
// decoder and onto the AA compositor surface.
//
//   LIVI receiver (FeedWriter, a socket CLIENT)  --feedproto-->  aa-bridge (socket SERVER)
//                                                                     |
//                                      appsrc ! h264parse ! <decoder> ! [videoconvert] <sink>
//
// We are the SERVER: FeedWriter::open() in LIVI does UnixStream::connect(), so the bridge
// binds + listens on the feed path and the receiver connects to it. (Confirmed against
// livi-session-io/src/feed.rs and livi-gst-host/src/feed.rs, which also binds.)
//
// On the device the decoder is `ducatih264dec` (IVA-HD, libgstducati/libdce) and the sink
// `waylandsink2`, whose ILM surface (20000000 + our pid) the CarLife shim shows. For the
// off-hardware smoke test, build with the software defaults (avdec_h264 / fakesink) — see
// build.sh.
//
// Exit status, for procguard (svp/bin/procguard): 1 for a startup failure (bad option, the
// feed socket cannot be bound), which procguard does not retry; 0 otherwise (SIGTERM or
// SIGINT, a failed accept, the watchdog), and a crash dies by its signal: both restarted.
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "audio.h"
#include "feedproto.h"
#include "gst_min.h"
#include "ilm_crop.h"
#include "mic.h"
#include "pipeline.h"
#include "video.h"
#include "watchdog.h"

#ifndef AA_DEFAULT_SOCKET
#define AA_DEFAULT_SOCKET "/tmp/aa.video.sock"
#endif
#ifndef AA_DEFAULT_DECODER
#define AA_DEFAULT_DECODER "avdec_h264"
#endif
#ifndef AA_DEFAULT_SINK
#define AA_DEFAULT_SINK "autovideosink"
#endif
// Audio out + mic in. The device build (build.sh) sets ALSA elements; the desktop smoke
// build leaves audio on autoaudiosink and the mic off. All overridable at runtime by flag or
// env (AA_AUDIO_SINK / AA_MIC_SRC / AA_MIC_SOCK) so audio routing can be tuned without a
// rebuild. "none" disables that half.
#ifndef AA_DEFAULT_AUDIO_SINK
#define AA_DEFAULT_AUDIO_SINK "autoaudiosink"
#endif
#ifndef AA_DEFAULT_MIC_SRC
#define AA_DEFAULT_MIC_SRC "none"
#endif
#ifndef AA_DEFAULT_MIC_SOCKET
#define AA_DEFAULT_MIC_SOCKET "/tmp/aa.mic.sock"
#endif
// Per-feed-id sink overrides, "ID=ELEMENT[,ID=ELEMENT...]". Empty = every id uses the audio
// sink. The device build routes the AA alert streams (speech/system/telephony) through the
// system PulseAudio mixer so they can overlap media instead of fighting it for the non-dmix
// hw:MIB2HiFi ALSA default.
#ifndef AA_DEFAULT_AUDIO_SINKS
#define AA_DEFAULT_AUDIO_SINKS ""
#endif

// procguard (svp/bin/procguard) re-execs a service 50ms after it exits with status 0 or is
// killed by a signal, but gives up once the raw wait() status is > 255, i.e. on any non-zero
// exit code. Startup failures therefore exit non-zero so they are not retried 20x/second.
constexpr int kExitFatal = 1;

namespace {

// SIGTERM / SIGINT: the signal number. The loops look at it at least every second.
volatile sig_atomic_t g_stop = 0;

void on_stop_signal(int sig) { g_stop = sig; }

struct Opts {
    std::string socket = AA_DEFAULT_SOCKET;
    std::string decoder = AA_DEFAULT_DECODER;
    std::string sink = AA_DEFAULT_SINK;
    std::string forced_codec; // "", "h264", or "h265"
    bool once = false;
    bool verbose = false;

    // Audio out (feed-socket KIND_AUDIO -> PCM playback).
    std::string audio_sink = AA_DEFAULT_AUDIO_SINK; // "none" disables audio playback
    std::map<std::uint32_t, aa::AudioFormat> audio_formats; // feed id -> format (defaults below)
    std::map<std::uint32_t, std::string> audio_sinks;       // feed id -> sink override

    // Mic in (device capture -> KIND_MIC -> mic socket -> session -> phone).
    std::string mic_sock = AA_DEFAULT_MIC_SOCKET;
    std::string mic_src = AA_DEFAULT_MIC_SRC; // "alsa:<device>", or "none"
    std::uint32_t mic_rate = 16000;
    std::uint32_t mic_channels = 1;
};

// Default feed-id -> format table, matching livi-aa-headless audio_feed_id().
void set_default_audio_formats(Opts &o) {
    o.audio_formats[101] = aa::AudioFormat{48000, 2}; // media
    o.audio_formats[102] = aa::AudioFormat{16000, 1}; // speech
    o.audio_formats[103] = aa::AudioFormat{16000, 1}; // system
    o.audio_formats[104] = aa::AudioFormat{16000, 1}; // telephony
}

// Apply an env override only when the var is set and non-empty.
void env_override(const char *name, std::string &dst) {
    const char *v = std::getenv(name);
    if (v && *v) {
        dst = v;
    }
}

// Parse "ID=ELEMENT[,ID=ELEMENT...]" into `out`. The element may carry properties
// ("pulsesink stream-properties=..."), but not commas.
bool parse_audio_sinks(const std::string &spec, std::map<std::uint32_t, std::string> &out) {
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(pos, end - pos);
        pos = end + 1;
        if (item.empty()) continue;
        const std::size_t eq = item.find('=');
        const unsigned long id = std::strtoul(item.c_str(), nullptr, 10);
        if (eq == std::string::npos || eq + 1 >= item.size() || id == 0) {
            std::fprintf(stderr, "[aa-bridge] audio sink wants ID=ELEMENT (got %s)\n", item.c_str());
            return false;
        }
        out[static_cast<std::uint32_t>(id)] = item.substr(eq + 1);
    }
    return true;
}

void usage(const char *argv0) {
    std::printf(
        "aa-bridge — AA video feed (feedproto unix socket) -> GStreamer -> decoder/surface\n\n"
        "Usage: %s [options]\n"
        "  --socket PATH       feed socket to bind+listen on (default %s)\n"
        "  --decoder NAME      gst decoder element (default %s)\n"
        "  --sink NAME         gst sink element (default %s)\n"
        "  --codec h264|h265   force codec instead of reading it from KIND_VIDEO_START\n"
        "  --audio-sink NAME   gst sink for PCM audio playback (default %s; env AA_AUDIO_SINK)\n"
        "  --audio ID:RATE:CH  override/add a feed-id audio format (repeatable)\n"
        "  --audio-sink-for ID=ELEM[,ID=ELEM]  per-feed-id sink (default \"%s\"; env AA_AUDIO_SINKS);\n"
        "                      pasimple[:NAME]: a PulseAudio stream, open while sound plays;\n"
        "                      pasimple-open[:NAME]: one open for the whole session\n"
        "  --no-audio          drop audio (video only)\n"
        "  --mic-sock PATH     mic socket to feed KIND_MIC into (default %s; env AA_MIC_SOCK)\n"
        "  --mic-src SRC       mic: alsa:<device> (libasound), or none (default %s; env AA_MIC_SRC)\n"
        "  --mic-rate HZ       mic sample rate (default %u)\n"
        "  --mic-channels N    mic channels (default %u)\n"
        "  --no-mic            do not capture the microphone\n"
        "  --once              serve a single session then exit (used by the smoke test)\n"
        "  --verbose           log every record\n"
        "  --debug-output MODE accepted and ignored (added by svp_start_service)\n"
        "  --help              this help\n\n"
        "Video:  appsrc ! h264parse ! <decoder> ! [videoconvert ! ] <sink>\n"
        "Audio:  appsrc(S16LE) ! queue ! audioconvert ! audioresample ! <audio-sink>  (one per feed id),\n"
        "        or pasimple: libpulse-simple on its own thread\n"
        "Mic:    alsa:<device> -> snd_pcm_readi -> KIND_MIC -> mic sock\n"
        "Environment: AA_SHIM_NOTIFY (default /tmp/run/aa/notify.sock), AA_WATCHDOG_S (10),\n"
        "AA_VIDEO_REPORT_S (60), AA_VIDEO_REORDER, AA_VIDEO_AUD, AA_VIDEO_DISPLAY_DELAY,\n"
        "AA_WL_DISPATCH_MS.\n",
        argv0, AA_DEFAULT_SOCKET, AA_DEFAULT_DECODER, AA_DEFAULT_SINK, AA_DEFAULT_AUDIO_SINK,
        AA_DEFAULT_AUDIO_SINKS, AA_DEFAULT_MIC_SOCKET, AA_DEFAULT_MIC_SRC, 16000u, 1u);
}

bool parse_args(int argc, char **argv, Opts &o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[aa-bridge] %s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else if (a == "--socket") {
            const char *v = next("--socket");
            if (!v) return false;
            o.socket = v;
        } else if (a == "--decoder") {
            const char *v = next("--decoder");
            if (!v) return false;
            o.decoder = v;
        } else if (a == "--sink") {
            const char *v = next("--sink");
            if (!v) return false;
            o.sink = v;
        } else if (a == "--codec") {
            const char *v = next("--codec");
            if (!v) return false;
            o.forced_codec = v;
        } else if (a == "--audio-sink") {
            const char *v = next("--audio-sink");
            if (!v) return false;
            o.audio_sink = v;
        } else if (a == "--audio") {
            const char *v = next("--audio");
            if (!v) return false;
            unsigned id = 0, rate = 0, ch = 0;
            if (std::sscanf(v, "%u:%u:%u", &id, &rate, &ch) != 3 || id == 0 || rate == 0 || ch == 0) {
                std::fprintf(stderr, "[aa-bridge] --audio wants ID:RATE:CH (got %s)\n", v);
                return false;
            }
            o.audio_formats[id] = aa::AudioFormat{rate, ch};
        } else if (a == "--audio-sink-for") {
            const char *v = next("--audio-sink-for");
            if (!v || !parse_audio_sinks(v, o.audio_sinks)) return false;
        } else if (a == "--no-audio") {
            o.audio_sink = "none";
        } else if (a == "--mic-sock") {
            const char *v = next("--mic-sock");
            if (!v) return false;
            o.mic_sock = v;
        } else if (a == "--mic-src") {
            const char *v = next("--mic-src");
            if (!v) return false;
            o.mic_src = v;
        } else if (a == "--mic-rate") {
            const char *v = next("--mic-rate");
            if (!v) return false;
            o.mic_rate = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--mic-channels") {
            const char *v = next("--mic-channels");
            if (!v) return false;
            o.mic_channels = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--no-mic") {
            o.mic_src = "none";
        } else if (a == "--once") {
            o.once = true;
        } else if (a == "--verbose") {
            o.verbose = true;
        } else if (a == "--debug-output") {
            // svp_start_service (svp/etc/func_startstop.sh) appends `--debug-output <mode>` to
            // every service it launches. Accept and ignore it; we log to stderr regardless.
            if (!next("--debug-output")) return false;
        } else {
            // Never die on a flag the platform adds: warn and carry on.
            std::fprintf(stderr, "[aa-bridge] ignoring unknown option: %s\n", a.c_str());
        }
    }
    return true;
}

aa::Codec codec_from_byte(std::uint8_t b) {
    return (b == feedproto::CODEC_H265) ? aa::Codec::H265 : aa::Codec::H264;
}

const char *codec_name(aa::Codec c) { return c == aa::Codec::H265 ? "h265" : "h264"; }

// True when another feed connection is waiting on the listening socket: the receiver started a
// new session, so the one being served is over even if its connection never closed (a session
// the receiver lost track of held the feed and the next one waited behind it, step 3 run 12).
bool newer_feed_waiting(int lfd) {
    struct pollfd p = {lfd, POLLIN, 0};
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

// One accepted connection = one AA session; it ends when the peer closes, a newer feed
// connection arrives on `lfd`, the stream turns out corrupt, or a stop signal comes. A video
// error does not end it: video is rebuilt in place (video.h) and audio goes on.
void serve_session(int cfd, int lfd, const Opts &o) {
    // A short recv timeout lets the loop poll the GStreamer buses even while the feed is idle.
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250 * 1000;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    feedproto::Framer framer;
    aa::AudioOut audio(o.audio_sink);
    for (const auto &kv : o.audio_formats) {
        audio.set_format(kv.first, kv.second);
    }
    for (const auto &kv : o.audio_sinks) {
        audio.set_sink(kv.first, kv.second);
    }
    audio.open_held();
    aa::VideoOut video(o.decoder, o.sink);
    if (!o.forced_codec.empty()) {
        video.set_codec(o.forced_codec == "h265" ? aa::Codec::H265 : aa::Codec::H264);
    }
    feedproto::Record rec;
    std::uint64_t video_frames = 0, audio_frames = 0, other = 0;
    bool superseded = false; // a newer feed connection is waiting: no drain, hand over at once
    bool corrupt = false;

    std::uint8_t buf[65536];
    std::uint64_t reads = 0;
    for (;;) {
        aa::watchdog_tick("feed");
        if (g_stop) {
            break;
        }
        ssize_t n = recv(cfd, buf, sizeof(buf), 0);
        if (n == 0) {
            std::fprintf(stderr, "[aa-bridge] feed peer closed\n");
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (newer_feed_waiting(lfd)) {
                    std::fprintf(stderr, "[aa-bridge] a newer feed connection is waiting: ending this session\n");
                    superseded = true;
                    break;
                }
                video.poll();
                audio.poll();
                continue; // idle; keep waiting for data
            }
            std::fprintf(stderr, "[aa-bridge] recv: %s\n", std::strerror(errno));
            break;
        }

        if (++reads % 64 == 0 && newer_feed_waiting(lfd)) {
            std::fprintf(stderr, "[aa-bridge] a newer feed connection is waiting: ending this session\n");
            superseded = true;
            break;
        }
        framer.push(buf, static_cast<std::size_t>(n));
        const unsigned corrupt_before = framer.corrupt();
        while (framer.next_record(rec)) {
            switch (rec.kind) {
            case feedproto::KIND_VIDEO_START: {
                aa::Codec codec = aa::Codec::H264;
                if (!o.forced_codec.empty()) {
                    codec = (o.forced_codec == "h265") ? aa::Codec::H265 : aa::Codec::H264;
                } else if (!rec.payload.empty()) {
                    codec = codec_from_byte(rec.payload[0]);
                }
                std::fprintf(stderr, "[aa-bridge] VIDEO_START id=%u codec=%s\n", rec.id,
                             codec_name(codec));
                video.set_codec(codec);
                break;
            }
            case feedproto::KIND_VIDEO:
                video.push(rec.payload.data(), rec.payload.size());
                ++video_frames;
                if (o.verbose) {
                    std::fprintf(stderr, "[aa-bridge]  video id=%u ts=%llu %zuB\n", rec.id,
                                 (unsigned long long) rec.ts, rec.payload.size());
                }
                break;
            case feedproto::KIND_AUDIO:
                audio.push(rec.id, rec.payload.data(), rec.payload.size());
                ++audio_frames;
                if (o.verbose) {
                    std::fprintf(stderr, "[aa-bridge]  audio id=%u ts=%llu %zuB\n", rec.id,
                                 (unsigned long long) rec.ts, rec.payload.size());
                }
                break;
            default:
                ++other;
                break;
            }
        }
        if (framer.corrupt() != corrupt_before) {
            // What follows the bad length starts somewhere inside a record, and the stream has
            // no marker to find the next one: end the session (the receiver reconnects).
            std::fprintf(stderr, "[aa-bridge] feed: a record longer than %zu bytes, the stream is corrupt; "
                         "ending the session\n", feedproto::MAX_RECORD_BYTES);
            corrupt = true;
            break;
        }
        video.poll();
        audio.poll();
    }

    video.finish(!superseded && !corrupt && !g_stop);
    aa::watchdog_tick("audio stop");
    audio.stop_all();

    unsigned rss_kb, fds;
    aa::process_usage(rss_kb, fds);
    std::printf("[aa-bridge] session over: video=%llu audio=%llu other=%llu%s%s (rss %u KiB, fds %u)\n",
                (unsigned long long) video_frames, (unsigned long long) audio_frames,
                (unsigned long long) other, corrupt ? " feed corrupt" : "",
                (video.errors() || corrupt) ? " (ERROR)" : "", rss_kb, fds);
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--ilm-crop") == 0) return ilm_crop_main(argc, argv);
    // Our output goes to a pipe (the launcher's relay to syslog): one write per line, so the
    // lines of stdout (ours, and the unit's alsa-lib "alsa: snd_pcm_open ..." traces) keep their
    // place among those of stderr.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Opts o;
    set_default_audio_formats(o);
    // env < flags: seed from env first, then let explicit flags win in parse_args.
    env_override("AA_AUDIO_SINK", o.audio_sink);
    env_override("AA_MIC_SRC", o.mic_src);
    env_override("AA_MIC_SOCK", o.mic_sock);
    std::string sinks_spec = AA_DEFAULT_AUDIO_SINKS;
    env_override("AA_AUDIO_SINKS", sinks_spec);
    if (!parse_audio_sinks(sinks_spec, o.audio_sinks) || !parse_args(argc, argv, o)) {
        return kExitFatal;
    }
    std::signal(SIGPIPE, SIG_IGN);
    // No SA_RESTART: a blocked recv or poll returns EINTR on the thread that takes the signal;
    // the others see g_stop within a second (the recv timeout, the accept poll).
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // gst_init consumes its own args; we hand it none (we parsed our own).
    int gargc = 1;
    char *gargv0 = argv[0];
    char **gargv = &gargv0;
    gst_init(&gargc, &gargv);

    if (o.socket.size() >= sizeof(((struct sockaddr_un *) nullptr)->sun_path)) {
        std::fprintf(stderr, "[aa-bridge] socket path too long: %s\n", o.socket.c_str());
        return kExitFatal;
    }

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        std::fprintf(stderr, "[aa-bridge] socket: %s\n", std::strerror(errno));
        return kExitFatal;
    }
    unlink(o.socket.c_str()); // mirror FeedListener: drop any stale socket first

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, o.socket.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(lfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[aa-bridge] bind %s: %s\n", o.socket.c_str(), std::strerror(errno));
        close(lfd);
        return kExitFatal;
    }
    chmod(o.socket.c_str(), 0666); // let the receiver connect regardless of uid
    if (listen(lfd, 1) < 0) {
        std::fprintf(stderr, "[aa-bridge] listen: %s\n", std::strerror(errno));
        close(lfd);
        return kExitFatal;
    }

    // Microphone capture runs for the life of the process; it connects to the mic socket
    // only while the phone has asked for the mic (see mic.cpp).
    aa::MicCapture mic(o.mic_sock, o.mic_src, o.mic_rate, o.mic_channels);
    mic.start();

    std::printf("[aa-bridge] listening on %s (decoder=%s sink=%s)\n", o.socket.c_str(), o.decoder.c_str(),
                o.sink.c_str());
    std::printf("[aa-bridge] audio=%s  mic=%s%s (sock=%s %uHz/%uch)\n",
                o.audio_sink == "none" ? "off" : o.audio_sink.c_str(),
                o.mic_src == "none" ? "off" : o.mic_src.c_str(),
                o.mic_src == "none" ? "" : " on", o.mic_sock.c_str(), o.mic_rate, o.mic_channels);
    ilm_crop_serve(); // the CarLife shim's crop requests (AA_BRIDGE_CTL)
    aa::watchdog_start();

    while (!g_stop) {
        aa::watchdog_idle();
        struct pollfd p = {lfd, POLLIN, 0};
        const int r = poll(&p, 1, 1000);
        if (r < 0 && errno != EINTR) {
            std::fprintf(stderr, "[aa-bridge] poll: %s\n", std::strerror(errno));
            break;
        }
        if (r <= 0) {
            continue;
        }
        aa::watchdog_tick("accept");
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) {
                continue;
            }
            std::fprintf(stderr, "[aa-bridge] accept: %s\n", std::strerror(errno));
            break;
        }
        std::fprintf(stderr, "[aa-bridge] feed connection accepted\n");
        serve_session(cfd, lfd, o);
        close(cfd);
        if (o.once) {
            break;
        }
    }
    if (g_stop) {
        std::fprintf(stderr, "[aa-bridge] signal %d: stopping\n", static_cast<int>(g_stop));
    }

    aa::watchdog_tick("exit");
    mic.stop();
    close(lfd);
    unlink(o.socket.c_str());
    return 0;
}
