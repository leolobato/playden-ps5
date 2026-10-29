// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/zip.h"

#include <array>

namespace playden::web {
namespace {

constexpr std::uint64_t kMaxArchiveBytes = 0x7fffffffu;

const std::array<std::uint32_t, 256>& CrcTable() {
  static const auto table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      values[n] = c;
    }
    return values;
  }();
  return table;
}

void Put16(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void Put32(std::string& out, std::uint32_t value) {
  Put16(out, value & 0xffff);
  Put16(out, value >> 16);
}

// MS-DOS date and time for a Unix time, without the platform's time zone support (zip readers
// show it as local time; the archive says UTC in its summary).
void DosTime(std::int64_t unix_seconds, std::uint16_t* time, std::uint16_t* date) {
  if (unix_seconds < 315532800) unix_seconds = 315532800;  // 1980-01-01, the earliest DOS date
  std::int64_t days = unix_seconds / 86400;
  const std::int64_t seconds = unix_seconds % 86400;
  // Civil from days (Howard Hinnant's algorithm).
  days += 719468;
  const std::int64_t era = days / 146097;
  const std::int64_t doe = days - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  std::int64_t year = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
  if (month <= 2) ++year;
  if (year > 2107) year = 2107;
  *date = static_cast<std::uint16_t>(((year - 1980) << 9) | (month << 5) | day);
  *time = static_cast<std::uint16_t>(((seconds / 3600) << 11) | (((seconds / 60) % 60) << 5) | ((seconds % 60) / 2));
}

}  // namespace

std::uint32_t Crc32(std::uint32_t crc, const void* data, std::size_t size) {
  const auto& table = CrcTable();
  const auto* bytes = static_cast<const unsigned char*>(data);
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

StoredZip::StoredZip(Write write, std::int64_t modified) : write_(std::move(write)) {
  DosTime(modified, &dos_time_, &dos_date_);
}

bool StoredZip::Put(const std::string& bytes) {
  if (failed_) return false;
  if (!write_(bytes.data(), bytes.size())) failed_ = true;
  offset_ += bytes.size();
  return !failed_;
}

bool StoredZip::Add(std::string_view name, std::string_view data) {
  if (failed_ || finished_ || name.empty() || name.size() > 0xffff || entries_.size() >= 0xffff) return false;
  // The entry and the central directory record that follows must still fit.
  if (offset_ + 30 + name.size() + data.size() + 46 + name.size() + 22 > kMaxArchiveBytes) {
    failed_ = true;
    return false;
  }
  Entry entry{std::string(name), Crc32(0, data.data(), data.size()), static_cast<std::uint32_t>(data.size()),
              static_cast<std::uint32_t>(offset_)};
  std::string header;
  Put32(header, 0x04034b50);
  Put16(header, 20);         // version needed: 2.0
  Put16(header, 1u << 11);   // names are UTF-8
  Put16(header, 0);          // stored
  Put16(header, dos_time_);
  Put16(header, dos_date_);
  Put32(header, entry.crc);
  Put32(header, entry.size);
  Put32(header, entry.size);
  Put16(header, static_cast<std::uint32_t>(name.size()));
  Put16(header, 0);
  header.append(name.data(), name.size());
  if (!Put(header)) return false;
  if (!data.empty()) {
    if (!write_(data.data(), data.size())) failed_ = true;
    offset_ += data.size();
  }
  if (failed_) return false;
  entries_.push_back(std::move(entry));
  return true;
}

bool StoredZip::Finish() {
  if (failed_ || finished_) return false;
  finished_ = true;
  const auto directory_offset = static_cast<std::uint32_t>(offset_);
  std::string directory;
  for (const auto& entry : entries_) {
    Put32(directory, 0x02014b50);
    Put16(directory, 20);  // made by: 2.0, MS-DOS attributes
    Put16(directory, 20);
    Put16(directory, 1u << 11);
    Put16(directory, 0);
    Put16(directory, dos_time_);
    Put16(directory, dos_date_);
    Put32(directory, entry.crc);
    Put32(directory, entry.size);
    Put32(directory, entry.size);
    Put16(directory, static_cast<std::uint32_t>(entry.name.size()));
    Put16(directory, 0);  // extra
    Put16(directory, 0);  // comment
    Put16(directory, 0);  // disk
    Put16(directory, 0);  // internal attributes
    Put32(directory, 0);  // external attributes
    Put32(directory, entry.offset);
    directory += entry.name;
  }
  const auto directory_size = static_cast<std::uint32_t>(directory.size());
  Put32(directory, 0x06054b50);
  Put16(directory, 0);
  Put16(directory, 0);
  Put16(directory, static_cast<std::uint32_t>(entries_.size()));
  Put16(directory, static_cast<std::uint32_t>(entries_.size()));
  Put32(directory, directory_size);
  Put32(directory, directory_offset);
  Put16(directory, 0);
  return Put(directory);
}

}  // namespace playden::web
