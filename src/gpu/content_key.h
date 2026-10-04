#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace dusk_gpu {
// Two independent word-wise hashes keep cache identity small even for large
// texture buffers. Size is part of the key; no filenames or editor indices are
// trusted to identify uploaded content.
struct content_key {
    uint64_t first = 0, second = 0;
    size_t bytes = 0;
    bool operator==(const content_key& other) const {
        return first == other.first && second == other.second && bytes == other.bytes;
    }
    bool operator!=(const content_key& other) const { return !(*this == other); }
};
inline content_key identify_content(const void* data, size_t bytes) {
    content_key result{0x243f6a8885a308d3ull, 0x13198a2e03707344ull, bytes};
    const auto* source = static_cast<const unsigned char*>(data);
    auto mix = [&](uint64_t word) {
        result.first = (result.first ^ word) * 0x9e3779b185ebca87ull;
        result.first = (result.first << 27) | (result.first >> 37);
        result.second = (result.second + word) * 0xc2b2ae3d27d4eb4full;
        result.second = (result.second << 31) | (result.second >> 33);
    };
    while (bytes >= sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, source, sizeof(word));
        mix(word);
        source += sizeof(word);
        bytes -= sizeof(word);
    }
    if (bytes) {
        uint64_t word = 0;
        std::memcpy(&word, source, bytes);
        mix(word);
    }
    return result;
}
} // namespace dusk_gpu
