#pragma once
// Builds small .tzst archives for tests: tar bytes in zstd frames made of raw (stored) blocks,
// so no compressor is needed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace tzst_fixture {

inline void Octal(char* field, std::size_t width, std::uint64_t value) {
  std::snprintf(field, width, "%0*llo", static_cast<int>(width - 1), static_cast<unsigned long long>(value));
}

// One 512-byte ustar header. name and prefix are written as given (no splitting).
inline std::string Header(const std::string& name, char type, std::uint64_t size, const std::string& prefix = {},
                          const std::string& link = {}) {
  char block[512] = {};
  std::memcpy(block, name.data(), name.size() < 100 ? name.size() : 100);
  Octal(block + 100, 8, type == '5' ? 0755 : 0644);
  Octal(block + 108, 8, 501);
  Octal(block + 116, 8, 20);
  Octal(block + 124, 12, size);
  Octal(block + 136, 12, 1760000000);
  block[156] = type;
  std::memcpy(block + 157, link.data(), link.size() < 100 ? link.size() : 100);
  std::memcpy(block + 257, "ustar", 6);
  block[263] = '0';
  block[264] = '0';
  std::memcpy(block + 345, prefix.data(), prefix.size() < 155 ? prefix.size() : 155);
  std::memset(block + 148, ' ', 8);
  unsigned sum = 0;
  for (unsigned char c : block) sum += c;
  std::snprintf(block + 148, 8, "%06o", sum);
  block[155] = ' ';
  return std::string(block, 512);
}

inline std::string Pad(const std::string& data) {
  return data + std::string((512 - data.size() % 512) % 512, '\0');
}

inline std::string File(const std::string& name, const std::string& data, const std::string& prefix = {}) {
  return Header(name, '0', data.size(), prefix) + Pad(data);
}

inline std::string Directory(const std::string& name) { return Header(name, '5', 0); }

// A pax extended header carrying path=, followed by the entry itself (with a truncated name).
inline std::string PaxFile(const std::string& path, const std::string& data) {
  std::string record = " path=" + path + "\n";
  std::size_t length = record.size();
  for (;;) {
    const auto total = std::to_string(length).size() + record.size();
    if (total == length) break;
    length = total;
  }
  record = std::to_string(length) + record;
  return Header("PaxHeaders/x", 'x', record.size()) + Pad(record) + File(path.substr(0, 99), data);
}

// A GNU long-name entry ('L') followed by the entry.
inline std::string GnuLongFile(const std::string& path, const std::string& data) {
  const auto name = path + '\0';
  return Header("././@LongLink", 'L', name.size()) + Pad(name) + File(path.substr(0, 99), data);
}

inline std::string End() { return std::string(1024, '\0'); }

// A zstd frame of raw blocks holding bytes.
inline std::string Frame(const std::string& bytes) {
  std::string out("\x28\xB5\x2F\xFD", 4);
  out += '\x00';  // no checksum, no dictionary, no content size, not single segment
  out += '\x38';  // window 128 KiB
  constexpr std::size_t kBlock = 128 * 1024;
  std::size_t at = 0;
  do {
    const auto size = bytes.size() - at < kBlock ? bytes.size() - at : kBlock;
    const bool last = at + size == bytes.size();
    const std::uint32_t header = (last ? 1u : 0u) | (0u << 1) | static_cast<std::uint32_t>(size << 3);
    out += static_cast<char>(header & 0xff);
    out += static_cast<char>((header >> 8) & 0xff);
    out += static_cast<char>((header >> 16) & 0xff);
    out.append(bytes, at, size);
    at += size;
  } while (at < bytes.size());
  return out;
}

inline void WriteBytes(const std::string& path, const std::string& bytes) {
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
  }
}

}  // namespace tzst_fixture
