// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the feedproto framer — a direct port of the Rust tests in
// livi-host-proto/src/feed.rs, so we can prove the C++ reader agrees with the Rust
// writer byte-for-byte (independent of GStreamer / the device).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/feedproto.h"

static int g_failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

using feedproto::encode;
using feedproto::Framer;
using feedproto::Record;

static void a_record_comes_back_out() {
    std::printf("test: a_record_comes_back_out\n");
    Framer f;
    std::uint8_t p[] = {1, 2, 3};
    auto e = encode(feedproto::KIND_VIDEO, 0x7a000001u, 42, p, 3);
    f.push(e.data(), e.size());
    Record r;
    CHECK(f.next_record(r));
    CHECK(r.kind == feedproto::KIND_VIDEO);
    CHECK(r.id == 0x7a000001u);
    CHECK(r.ts == 42);
    CHECK(r.payload == std::vector<std::uint8_t>({1, 2, 3}));
    CHECK(!f.next_record(r));
}

static void a_record_split_across_chunks_is_reassembled() {
    std::printf("test: a_record_split_across_chunks_is_reassembled\n");
    std::vector<std::uint8_t> payload(40, 9);
    auto msg = encode(feedproto::KIND_AUDIO, 7, 0, payload.data(), payload.size());
    Framer f;
    for (size_t i = 0; i < msg.size(); i += 5) {
        size_t n = (i + 5 <= msg.size()) ? 5 : msg.size() - i;
        f.push(msg.data() + i, n);
    }
    Record r;
    CHECK(f.next_record(r));
    CHECK(r.payload.size() == 40);
}

static void several_records_in_one_chunk_come_out_in_order() {
    std::printf("test: several_records_in_one_chunk_come_out_in_order\n");
    std::uint8_t zero = 0;
    auto buf = encode(feedproto::KIND_VIDEO_START, 10, 0, &zero, 1);
    const char *nal = "nal";
    auto v = encode(feedproto::KIND_VIDEO, 10, 1, reinterpret_cast<const std::uint8_t *>(nal), 3);
    buf.insert(buf.end(), v.begin(), v.end());
    Framer f;
    f.push(buf.data(), buf.size());
    Record r;
    CHECK(f.next_record(r));
    CHECK(r.kind == feedproto::KIND_VIDEO_START);
    CHECK(f.next_record(r));
    CHECK(r.payload == std::vector<std::uint8_t>({'n', 'a', 'l'}));
    CHECK(!f.next_record(r));
}

static void a_payload_too_short_for_a_header_is_skipped() {
    std::printf("test: a_payload_too_short_for_a_header_is_skipped\n");
    // A record whose declared len (3) is < HEAD_BYTES (13) must be dropped, then the next
    // valid record read.
    std::uint32_t bad = 3;
    std::vector<std::uint8_t> buf(reinterpret_cast<std::uint8_t *>(&bad),
                                 reinterpret_cast<std::uint8_t *>(&bad) + 4);
    buf.insert(buf.end(), {9, 9, 9});
    const char *ok = "ok";
    auto good = encode(feedproto::KIND_VIDEO, 5, 0, reinterpret_cast<const std::uint8_t *>(ok), 2);
    buf.insert(buf.end(), good.begin(), good.end());
    Framer f;
    f.push(buf.data(), buf.size());
    Record r;
    CHECK(f.next_record(r));
    CHECK(r.id == 5);
}

static void an_empty_payload_is_still_a_record() {
    std::printf("test: an_empty_payload_is_still_a_record\n");
    Framer f;
    auto e = encode(feedproto::KIND_VIDEO, 99, 5, nullptr, 0);
    f.push(e.data(), e.size());
    Record r;
    CHECK(f.next_record(r));
    CHECK(r.kind == feedproto::KIND_VIDEO);
    CHECK(r.id == 99);
    CHECK(r.ts == 5);
    CHECK(r.payload.empty());
}

static void a_video_start_carries_the_codec_byte() {
    std::printf("test: a_video_start_carries_the_codec_byte\n");
    // session.rs encodes KIND_VIDEO_START with a single codec byte (h264=0 / h265=1).
    std::uint8_t h264 = feedproto::CODEC_H264;
    auto e = encode(feedproto::KIND_VIDEO_START, 0x7a000001u, 0, &h264, 1);
    Framer f;
    f.push(e.data(), e.size());
    Record r;
    CHECK(f.next_record(r));
    CHECK(r.kind == feedproto::KIND_VIDEO_START);
    CHECK(r.payload.size() == 1);
    CHECK(r.payload[0] == feedproto::CODEC_H264);
}

// The framer only drops and counts: the bytes after a bad length are not a record boundary
// (here, the tail of a valid record that was cut by the corruption), so the bridge ends the
// session on corrupt() rather than parse on from there.
static void an_absurd_length_drops_the_buffer_and_is_counted() {
    std::printf("an_absurd_length_drops_the_buffer_and_is_counted\n");
    feedproto::Framer f;
    feedproto::Record r;
    const std::uint8_t p[] = {9, 9, 9, 9};
    const std::vector<std::uint8_t> ok = feedproto::encode(feedproto::KIND_VIDEO, 1, 2, p, sizeof p);
    std::vector<std::uint8_t> stream = {0xfc, 0xff, 0xff, 0xff, 1, 2, 3, 4};
    stream.insert(stream.end(), ok.begin() + 6, ok.end()); // a record whose head was lost
    f.push(stream.data(), stream.size());
    CHECK(!f.next_record(r));
    CHECK(f.buffered() == 0);
    CHECK(f.corrupt() == 1);
}

// The limit is checked before the length is added to anything: 0xFFFFFFFF does not wrap the
// size check on 32-bit (it used to, and read past the buffer).
static void the_largest_length_is_refused_not_wrapped() {
    std::printf("the_largest_length_is_refused_not_wrapped\n");
    feedproto::Framer f;
    feedproto::Record r;
    const std::uint8_t bad[] = {0xff, 0xff, 0xff, 0xff, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    f.push(bad, sizeof bad);
    CHECK(!f.next_record(r));
    CHECK(f.corrupt() == 1);
}

// Just over the limit is refused; at the limit the framer waits for the rest.
static void the_limit_itself_is_a_record() {
    std::printf("the_limit_itself_is_a_record\n");
    feedproto::Framer over, at;
    feedproto::Record r;
    const std::uint32_t too_long = static_cast<std::uint32_t>(feedproto::MAX_RECORD_BYTES + 1);
    const std::uint32_t longest = static_cast<std::uint32_t>(feedproto::MAX_RECORD_BYTES);
    std::uint8_t head[4];
    std::memcpy(head, &too_long, 4);
    over.push(head, 4);
    CHECK(!over.next_record(r));
    CHECK(over.corrupt() == 1);
    std::memcpy(head, &longest, 4);
    at.push(head, 4);
    CHECK(!at.next_record(r));
    CHECK(at.corrupt() == 0);
    CHECK(at.buffered() == 4);
}

int main() {
    an_absurd_length_drops_the_buffer_and_is_counted();
    the_largest_length_is_refused_not_wrapped();
    the_limit_itself_is_a_record();
    a_record_comes_back_out();
    a_record_split_across_chunks_is_reassembled();
    several_records_in_one_chunk_come_out_in_order();
    a_payload_too_short_for_a_header_is_skipped();
    an_empty_payload_is_still_a_record();
    a_video_start_carries_the_codec_byte();

    if (g_failures == 0) {
        std::printf("\nfeedproto_test: ALL PASS\n");
        return 0;
    }
    std::printf("\nfeedproto_test: %d CHECK(s) FAILED\n", g_failures);
    return 1;
}
