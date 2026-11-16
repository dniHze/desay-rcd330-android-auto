// SPDX-License-Identifier: GPL-3.0-or-later
#include "feedproto.h"

#include <cstring>

namespace feedproto {

namespace {
// Native-endian load/store — matches Rust from_ne_bytes/to_ne_bytes on our LE targets.
std::uint32_t load_u32(const std::uint8_t *p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
std::uint64_t load_u64(const std::uint8_t *p) {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
} // namespace

void Framer::push(const std::uint8_t *data, std::size_t n) {
    buf_.insert(buf_.end(), data, data + n);
}

bool Framer::next_record(Record &out) {
    for (;;) {
        if (buf_.size() < LEN_BYTES) {
            return false;
        }
        const std::size_t len = static_cast<std::size_t>(load_u32(buf_.data()));
        if (len > MAX_RECORD_BYTES) {
            buf_.clear();
            ++corrupt_;
            return false;
        }
        if (buf_.size() < LEN_BYTES + len) {
            return false; // record not fully arrived yet
        }
        if (len < HEAD_BYTES) {
            // Malformed/short record — drop it and keep scanning (mirrors Rust).
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(LEN_BYTES + len));
            continue;
        }
        const std::uint8_t *head = buf_.data() + LEN_BYTES;
        out.kind = head[0];
        out.id = load_u32(head + 1);
        out.ts = load_u64(head + 5);
        out.payload.assign(buf_.begin() + static_cast<std::ptrdiff_t>(LEN_BYTES + HEAD_BYTES),
                           buf_.begin() + static_cast<std::ptrdiff_t>(LEN_BYTES + len));
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(LEN_BYTES + len));
        return true;
    }
}

std::vector<std::uint8_t> encode(std::uint8_t kind, std::uint32_t id, std::uint64_t ts,
                                 const std::uint8_t *payload, std::size_t n) {
    const std::uint32_t len = static_cast<std::uint32_t>(HEAD_BYTES + n);
    std::vector<std::uint8_t> out;
    out.reserve(LEN_BYTES + len);
    out.resize(LEN_BYTES + HEAD_BYTES);
    std::memcpy(out.data(), &len, sizeof(len));
    out[LEN_BYTES + 0] = kind;
    std::memcpy(out.data() + LEN_BYTES + 1, &id, sizeof(id));
    std::memcpy(out.data() + LEN_BYTES + 5, &ts, sizeof(ts));
    if (n) {
        out.insert(out.end(), payload, payload + n);
    }
    return out;
}

} // namespace feedproto
