// SPDX-License-Identifier: GPL-3.0-or-later
//
// feedproto — the wire framing the LIVI Rust AA receiver uses on the video/audio
// feed unix socket. Ported *verbatim* from the LIVI source so the integer layout
// is not guessed:
//   - livi-src/native/crates/livi-host-proto/src/feed.rs  (Framer / encode)
//   - livi-src/native/crates/livi-aa/src/session.rs        (KIND_VIDEO_START + codec)
//
// Each record on the socket is:
//
//     [ u32 len ][ u8 kind ][ u32 id ][ u64 ts ][ payload ... ]
//      \__4___/   \_________ 13-byte header ____/  \_ len-13 _/
//
//   * len  (LEN_BYTES=4)  = HEAD_BYTES(13) + payload.len().
//                           It counts the header + payload, NOT the 4 len bytes.
//   * kind (1 byte)       = KIND_* below.
//   * id   (4 bytes)      = feed/stream id (the video feed id from the sink config).
//   * ts   (8 bytes)      = timestamp in nanoseconds (0 for KIND_VIDEO_START).
//   * payload             = for KIND_VIDEO_START: a single codec byte (h264=0, h265=1);
//                           for KIND_VIDEO: raw H.264 Annex-B NAL bytes.
//
// All integers are written with Rust's to_ne_bytes()/from_ne_bytes() — i.e. NATIVE
// endian. The device (armhf) and every build/test host here (x86_64, arm64) are all
// little-endian, so native-endian == little-endian end to end, matching the peer.
// We therefore read/write with memcpy (true native-endian), exactly mirroring Rust.
#ifndef AA_BRIDGE_FEEDPROTO_H
#define AA_BRIDGE_FEEDPROTO_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace feedproto {

constexpr std::size_t LEN_BYTES = 4;
constexpr std::size_t HEAD_BYTES = 13;
// No record comes near this (an 800x480 keyframe is well under 1 MiB): a longer declared
// length means the stream is corrupt (Framer drops what it holds).
constexpr std::size_t MAX_RECORD_BYTES = 8u << 20;

constexpr std::uint8_t KIND_VIDEO = 1;
constexpr std::uint8_t KIND_AUDIO = 2;
constexpr std::uint8_t KIND_VIDEO_START = 3;
constexpr std::uint8_t KIND_MIC = 4;

// Codec byte carried in a KIND_VIDEO_START payload (session.rs: h264=>0, h265=>1).
constexpr std::uint8_t CODEC_H264 = 0;
constexpr std::uint8_t CODEC_H265 = 1;

struct Record {
    std::uint8_t kind = 0;
    std::uint32_t id = 0;
    std::uint64_t ts = 0;
    std::vector<std::uint8_t> payload;
};

// Reassembles records from an arbitrarily-chunked byte stream. Mirrors the Rust
// `livi_host_proto::feed::Framer`: a record whose declared len is shorter than the
// 13-byte header is skipped (malformed), and partial records are buffered until the
// rest of their bytes arrive. A declared length over MAX_RECORD_BYTES drops everything
// buffered (counted in corrupt()): without the limit a bad length made the framer buffer
// without end or, near 4 GiB, wrap the size check on 32-bit and read past the buffer. The
// bytes after a bad length start inside some record and nothing marks where the next begins,
// so a stream cannot be resynchronised: the bridge ends the session on corrupt().
class Framer {
public:
    void push(const std::uint8_t *data, std::size_t n);
    // Pops the next complete record into `out`; returns false if none is buffered yet.
    bool next_record(Record &out);
    std::size_t buffered() const { return buf_.size(); }
    unsigned corrupt() const { return corrupt_; }

private:
    std::vector<std::uint8_t> buf_;
    unsigned corrupt_ = 0;
};

// Encodes one record to its wire bytes (used by the test feed sender).
std::vector<std::uint8_t> encode(std::uint8_t kind, std::uint32_t id, std::uint64_t ts,
                                 const std::uint8_t *payload, std::size_t n);

} // namespace feedproto

#endif // AA_BRIDGE_FEEDPROTO_H
