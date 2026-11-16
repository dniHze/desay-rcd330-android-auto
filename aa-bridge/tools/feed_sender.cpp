// SPDX-License-Identifier: GPL-3.0-or-later
//
// feed_sender — a stand-in for the LIVI receiver's FeedWriter, for the off-hardware smoke
// test. Connects to aa-bridge's feed socket (as a CLIENT, exactly like FeedWriter does),
// sends a KIND_VIDEO_START (codec byte), then streams a raw Annex-B .h264 elementary
// stream as KIND_VIDEO records split on NAL start codes, or with --au one record per access
// unit as Android Auto sends it (--gap-ms N waits between video records, like a phone whose
// screen changes now and then). --frames N sends only the first N video records (0: just the
// VIDEO_START, like a session that dies at once); --corrupt-after N writes a record header with
// an impossible length after N video records, then carries on. Build it for the host; see
// build.sh / smoke_test.sh.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../src/feedproto.h"

namespace {

std::vector<std::uint8_t> read_file(const char *path) {
    std::vector<std::uint8_t> data;
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "feed_sender: open %s: %s\n", path, std::strerror(errno));
        return data;
    }
    std::uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        data.insert(data.end(), chunk, chunk + n);
    }
    std::fclose(f);
    return data;
}

// Finds the offset of the next Annex-B start code (00 00 01 or 00 00 00 01) at or after
// `from`, returning data.size() if none.
size_t next_start_code(const std::vector<std::uint8_t> &d, size_t from) {
    for (size_t i = from; i + 3 <= d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            return i;
        }
    }
    return d.size();
}

// The NAL starting at the start code at `sc`: its type, and whether it is a coded slice that
// begins a picture (first_mb_in_slice == 0, whose ue(v) code is a single 1 bit).
struct Nal {
    unsigned type;
    bool vcl;
    bool first_slice;
};

Nal nal_at(const std::vector<std::uint8_t> &d, size_t sc) {
    const size_t h = sc + 3;
    Nal n = {h < d.size() ? d[h] & 0x1fu : 0u, false, false};
    n.vcl = n.type == 1 || n.type == 5;
    n.first_slice = n.vcl && h + 1 < d.size() && (d[h + 1] & 0x80);
    return n;
}

// Splits the stream into access units: a new one starts at a non-slice NAL that follows a slice,
// or at a slice that begins a picture once the current unit has one.
std::vector<std::pair<size_t, size_t>> access_units(const std::vector<std::uint8_t> &d) {
    std::vector<std::pair<size_t, size_t>> aus;
    size_t start = next_start_code(d, 0), pos = start;
    bool has_slice = false;
    while (pos < d.size()) {
        const Nal n = nal_at(d, pos);
        if (pos != start && has_slice && (!n.vcl || n.first_slice)) {
            aus.push_back(std::make_pair(start, pos));
            start = pos;
            has_slice = false;
        }
        has_slice = has_slice || n.vcl;
        pos = next_start_code(d, pos + 3);
    }
    if (start < d.size()) aus.push_back(std::make_pair(start, d.size()));
    return aus;
}

bool send_all(int fd, const std::uint8_t *p, size_t n) {
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "feed_sender: send: %s\n", std::strerror(errno));
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::string sock = "/tmp/aa.video.sock";
    std::string file;
    std::uint8_t codec = feedproto::CODEC_H264;
    std::uint32_t id = 1;
    std::vector<std::uint32_t> audio_ids; // --audio-ids 101,102: also send PCM silence per id
    bool per_au = false;
    unsigned gap_ms = 0, hold_ms = 300;
    long max_frames = -1, corrupt_after = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) {
            sock = argv[++i];
        } else if (a == "--file" && i + 1 < argc) {
            file = argv[++i];
        } else if (a == "--codec" && i + 1 < argc) {
            codec = (std::string(argv[++i]) == "h265") ? feedproto::CODEC_H265 : feedproto::CODEC_H264;
        } else if (a == "--id" && i + 1 < argc) {
            id = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--au") {
            per_au = true;
        } else if (a == "--hold-ms" && i + 1 < argc) {
            hold_ms = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--gap-ms" && i + 1 < argc) {
            gap_ms = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--frames" && i + 1 < argc) {
            max_frames = std::strtol(argv[++i], nullptr, 10);
        } else if (a == "--corrupt-after" && i + 1 < argc) {
            corrupt_after = std::strtol(argv[++i], nullptr, 10);
        } else if (a == "--audio-ids" && i + 1 < argc) {
            for (char *p = argv[++i]; *p;) {
                char *end = nullptr;
                audio_ids.push_back(static_cast<std::uint32_t>(std::strtoul(p, &end, 10)));
                p = (*end == ',') ? end + 1 : end;
            }
        } else {
            std::fprintf(stderr, "usage: feed_sender --file X.h264 [--socket P] [--codec h264|h265] "
                                 "[--au] [--gap-ms N] [--hold-ms N] [--audio-ids 101,102] [--frames N] "
                                 "[--corrupt-after N]\n");
            return 2;
        }
    }
    if (file.empty()) {
        std::fprintf(stderr, "feed_sender: --file is required\n");
        return 2;
    }

    std::vector<std::uint8_t> h264 = read_file(file.c_str());
    if (h264.empty()) {
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "feed_sender: socket: %s\n", std::strerror(errno));
        return 1;
    }
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "feed_sender: connect %s: %s\n", sock.c_str(), std::strerror(errno));
        close(fd);
        return 1;
    }

    // 1) KIND_VIDEO_START carrying the codec byte (ts=0), exactly as session.rs emits.
    std::vector<std::uint8_t> start = feedproto::encode(feedproto::KIND_VIDEO_START, id, 0, &codec, 1);
    if (!send_all(fd, start.data(), start.size())) {
        close(fd);
        return 1;
    }

    // 2) KIND_VIDEO records, one per NAL (or per access unit), with an increasing
    //    (nanosecond) timestamp.
    std::vector<std::pair<size_t, size_t>> records;
    if (per_au) {
        records = access_units(h264);
    } else {
        for (size_t pos = next_start_code(h264, 0); pos < h264.size();) {
            const size_t end = next_start_code(h264, pos + 3);
            records.push_back(std::make_pair(pos, end));
            pos = end;
        }
    }
    if (max_frames >= 0 && records.size() > static_cast<size_t>(max_frames)) {
        records.resize(static_cast<size_t>(max_frames));
    }
    std::uint64_t ts = 0;
    size_t frames = 0;
    for (const auto &r : records) {
        if (corrupt_after >= 0 && frames == static_cast<size_t>(corrupt_after)) {
            // A length no record has (over the bridge's 8 MiB limit), then part of a header.
            const std::uint8_t bad[] = {0xf0, 0xff, 0xff, 0xff, feedproto::KIND_VIDEO, 1, 0, 0, 0};
            if (!send_all(fd, bad, sizeof bad)) {
                close(fd);
                return 1;
            }
        }
        if (gap_ms && frames) usleep(gap_ms * 1000);
        std::vector<std::uint8_t> rec =
            feedproto::encode(feedproto::KIND_VIDEO, id, ts, &h264[r.first], r.second - r.first);
        if (!send_all(fd, rec.data(), rec.size())) {
            close(fd);
            return 1;
        }
        ++frames;
        ts += 33333333ULL; // ~30 fps spacing
    }

    std::printf("feed_sender: sent VIDEO_START(codec=%u) + %zu video records (%zu bytes) to %s\n",
                codec, frames, h264.size(), sock.c_str());

    // 3) Optional KIND_AUDIO: 20 x 20 ms of S16LE silence per id, interleaved so the streams
    //    overlap (media 101 is 48k/2ch, everything else 16k/1ch, as the bridge table expects).
    for (int chunk = 0; chunk < 20 && !audio_ids.empty(); ++chunk) {
        for (std::uint32_t aid : audio_ids) {
            const std::vector<std::uint8_t> pcm(aid == 101 ? 3840 : 640, 0);
            std::vector<std::uint8_t> rec = feedproto::encode(
                feedproto::KIND_AUDIO, aid, chunk * 20000000ULL, pcm.data(), pcm.size());
            if (!send_all(fd, rec.data(), rec.size())) {
                close(fd);
                return 1;
            }
        }
    }
    if (!audio_ids.empty()) {
        std::printf("feed_sender: sent 20 PCM records for each of %zu audio ids\n", audio_ids.size());
    }
    // Give the bridge a moment to push everything before we drop the connection (--hold-ms
    // keeps it open longer, like a session the receiver lost track of).
    usleep(hold_ms * 1000);
    close(fd);
    return 0;
}
