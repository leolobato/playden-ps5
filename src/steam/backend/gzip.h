#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace playden::steam {

inline constexpr std::size_t kMaxGunzipBytes = 4 * 1024 * 1024;
// InflateRaw's own bound: an inflated depot manifest (kMaxManifestBytes); callers cap below it.
inline constexpr std::size_t kMaxInflateRawBytes = 128 * 1024 * 1024;

// Decodes one RFC 1952 gzip member whose uncompressed size is declared in advance
// (CMsgMulti.size_unzipped). Returns false on any malformed, truncated or oversized input.
bool Gunzip(const std::uint8_t* data, std::size_t size, std::size_t expected_size,
            std::vector<std::uint8_t>* out);

// Raw deflate (no zlib/gzip framing) into exactly expected_size bytes, e.g. a zip entry.
bool InflateRaw(const std::uint8_t* data, std::size_t size, std::size_t expected_size,
                std::vector<std::uint8_t>* out);

}  // namespace playden::steam
