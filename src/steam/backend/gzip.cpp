#include "steam/backend/gzip.h"

#include <climits>
#include <utility>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#define STBI_NO_JPEG
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_LINEAR
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace playden::steam {
namespace {

constexpr std::size_t kFixedHeaderSize = 10;
constexpr std::size_t kTrailerSize = 8;
constexpr std::uint8_t kFlagHcrc = 0x02;
constexpr std::uint8_t kFlagExtra = 0x04;
constexpr std::uint8_t kFlagName = 0x08;
constexpr std::uint8_t kFlagComment = 0x10;
constexpr std::uint8_t kFlagReserved = 0xE0;

std::uint32_t ReadLe32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

// Advances `pos` past a zero-terminated field; false when no terminator before `end`.
bool SkipCString(const std::uint8_t* data, std::size_t end, std::size_t* pos) {
  while (*pos < end) {
    if (data[(*pos)++] == 0) return true;
  }
  return false;
}

}  // namespace

bool Gunzip(const std::uint8_t* data, std::size_t size, std::size_t expected_size,
            std::vector<std::uint8_t>* out) {
  if (out == nullptr || data == nullptr) return false;
  out->clear();
  if (expected_size == 0 || expected_size > kMaxGunzipBytes) return false;
  if (size < kFixedHeaderSize + kTrailerSize) return false;
  if (data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) return false;
  const std::uint8_t flags = data[3];
  if ((flags & kFlagReserved) != 0) return false;

  const std::size_t body_end = size - kTrailerSize;
  std::size_t pos = kFixedHeaderSize;
  if ((flags & kFlagExtra) != 0) {
    if (body_end - pos < 2) return false;
    const std::size_t xlen = static_cast<std::size_t>(data[pos]) |
                             (static_cast<std::size_t>(data[pos + 1]) << 8U);
    pos += 2;
    if (body_end - pos < xlen) return false;
    pos += xlen;
  }
  if ((flags & kFlagName) != 0 && !SkipCString(data, body_end, &pos)) return false;
  if ((flags & kFlagComment) != 0 && !SkipCString(data, body_end, &pos)) return false;
  if ((flags & kFlagHcrc) != 0) {
    if (body_end - pos < 2) return false;
    pos += 2;
  }
  if (pos >= body_end) return false;

  const std::uint32_t expected_crc = ReadLe32(data + body_end);
  const std::uint32_t isize = ReadLe32(data + body_end + 4);
  if (isize != expected_size) return false;

  const std::size_t compressed_size = body_end - pos;
  if (compressed_size > static_cast<std::size_t>(INT_MAX)) return false;
  if (expected_size > static_cast<std::size_t>(INT_MAX)) return false;

  std::vector<std::uint8_t> result(expected_size);
  const int written = stbi_zlib_decode_noheader_buffer(
      reinterpret_cast<char*>(result.data()), static_cast<int>(expected_size),
      reinterpret_cast<const char*>(data + pos), static_cast<int>(compressed_size));
  if (written < 0 || static_cast<std::size_t>(written) != expected_size) return false;
  if (Crc32(result.data(), result.size()) != expected_crc) return false;
  *out = std::move(result);
  return true;
}

bool InflateRaw(const std::uint8_t* data, std::size_t size, std::size_t expected_size,
                std::vector<std::uint8_t>* out) {
  if (!data || !out || expected_size == 0 || expected_size > kMaxInflateRawBytes ||
      size > static_cast<std::size_t>(INT_MAX) || expected_size > static_cast<std::size_t>(INT_MAX))
    return false;
  std::vector<std::uint8_t> result(expected_size);
  const int written = stbi_zlib_decode_noheader_buffer(
      reinterpret_cast<char*>(result.data()), static_cast<int>(expected_size),
      reinterpret_cast<const char*>(data), static_cast<int>(size));
  if (written < 0 || static_cast<std::size_t>(written) != expected_size) return false;
  *out = std::move(result);
  return true;
}

}  // namespace playden::steam
