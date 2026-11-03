// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::web {

std::uint32_t Crc32(std::uint32_t crc, const void* data, std::size_t size);

// A zip archive written with the store method (no compression, CRC-32 only), so no zlib is needed.
// Each entry goes out whole as it is added; the central directory follows in Finish. Zip64 is not
// written: an archive that would pass 2 GB, or 65535 entries, is refused.
class StoredZip {
 public:
  using Write = std::function<bool(const char* data, std::size_t size)>;
  // modified: the time stamped on every entry, in seconds since 1970 (UTC).
  StoredZip(Write write, std::int64_t modified);
  bool Add(std::string_view name, std::string_view data);
  // An entry written in pieces: its CRC and size follow it in a data descriptor. Nothing else may be added between
  // BeginEntry and EndEntry. Each piece counts against the archive limit, with the directory that will follow it.
  bool BeginEntry(std::string_view name);
  bool EntryData(const char* data, std::size_t size);
  bool EndEntry();
  bool Finish();
  // What an entry with this name adds besides its data: its headers, descriptor and directory record.
  static std::uint64_t EntryOverhead(std::size_t name_size);
  std::uint64_t bytes() const { return offset_; }

 private:
  struct Entry {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
    bool descriptor = false;
  };
  bool Put(const std::string& bytes);
  bool Fits(std::uint64_t more) const;
  Write write_;
  std::uint16_t dos_time_ = 0;
  std::uint16_t dos_date_ = 0;
  std::uint64_t offset_ = 0;
  std::uint64_t directory_bytes_ = 22;  // the end record, and each entry's directory record so far
  std::vector<Entry> entries_;
  Entry open_;
  bool entry_open_ = false;
  bool failed_ = false;
  bool finished_ = false;
};

}  // namespace playden::web
