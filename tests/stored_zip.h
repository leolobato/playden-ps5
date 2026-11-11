// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A stored (uncompressed) zip of name -> bytes, made as on Windows (no Unix modes), without miniz: for tests that unpack
// a package of Valve's Steam client.

#include <cstdint>
#include <map>
#include <string>

namespace playden::test {

inline std::uint32_t StoredZipCrc32(const std::string& bytes) {
  std::uint32_t crc = 0xffffffffu;
  for (const unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline std::string StoredZip(const std::map<std::string, std::string>& files) {
  const auto le = [](std::string* out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out->push_back(static_cast<char>(value >> (8 * i)));
  };
  std::string zip, central;
  std::uint16_t count = 0;
  for (const auto& [name, bytes] : files) {
    const auto offset = zip.size();
    const auto crc = StoredZipCrc32(bytes);
    le(&zip, 0x04034b50, 4), le(&zip, 20, 2), le(&zip, 0, 2), le(&zip, 0, 2), le(&zip, 0, 4);
    le(&zip, crc, 4), le(&zip, bytes.size(), 4), le(&zip, bytes.size(), 4), le(&zip, name.size(), 2), le(&zip, 0, 2);
    zip += name + bytes;
    le(&central, 0x02014b50, 4), le(&central, 20, 2), le(&central, 20, 2), le(&central, 0, 2), le(&central, 0, 2);
    le(&central, 0, 4), le(&central, crc, 4), le(&central, bytes.size(), 4), le(&central, bytes.size(), 4);
    le(&central, name.size(), 2), le(&central, 0, 2), le(&central, 0, 2), le(&central, 0, 2), le(&central, 0, 2);
    le(&central, 0, 4), le(&central, offset, 4);
    central += name;
    ++count;
  }
  const auto start = zip.size();
  zip += central;
  le(&zip, 0x06054b50, 4), le(&zip, 0, 2), le(&zip, 0, 2), le(&zip, count, 2), le(&zip, count, 2);
  le(&zip, central.size(), 4), le(&zip, start, 4), le(&zip, 0, 2);
  return zip;
}

}  // namespace playden::test
