// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/zip.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace playden::web;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
std::uint32_t Read32(const std::string& bytes, std::size_t at) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8 |
         static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16 |
         static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24;
}
std::uint16_t Read16(const std::string& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) | static_cast<unsigned char>(bytes[at + 1]) << 8);
}
}  // namespace

int main() {
  // Known CRC-32 values.
  CHECK(Crc32(0, "", 0) == 0);
  CHECK(Crc32(0, "123456789", 9) == 0xcbf43926u);
  CHECK(Crc32(Crc32(0, "1234", 4), "56789", 5) == 0xcbf43926u);

  std::string archive;
  std::size_t writes = 0;
  StoredZip zip([&](const char* data, std::size_t size) { archive.append(data, size); ++writes; return true; },
                1'791'601'941);  // 2026-10-10 03:12:21 UTC
  const std::string summary = "| Field | Value |\n| --- | --- |\n| collected | 2026-10-28 15:00 UTC |\n";
  std::string big(300 * 1024, 'x');
  for (std::size_t i = 0; i < big.size(); i += 97) big[i] = static_cast<char>('a' + i % 26);
  CHECK(zip.Add("summary.md", summary));
  CHECK(zip.Add("logs/runtime.log", big));
  CHECK(zip.Add("logs/empty.log", ""));
  CHECK(zip.Finish());
  CHECK(!zip.Add("late.txt", "x") && !zip.Finish());
  CHECK(zip.bytes() == archive.size());

  // Local header of the first entry: stored, UTF-8 names, the CRC and sizes in place.
  CHECK(Read32(archive, 0) == 0x04034b50u && Read16(archive, 8) == 0 && Read16(archive, 6) == (1u << 11));
  CHECK(Read32(archive, 14) == Crc32(0, summary.data(), summary.size()));
  CHECK(Read32(archive, 18) == summary.size() && Read32(archive, 22) == summary.size());
  CHECK(archive.compare(30, 10, "summary.md") == 0 && archive.compare(40, summary.size(), summary) == 0);
  // DOS date and time: 2026-10-10, 03:12:20 (two-second steps).
  CHECK(Read16(archive, 12) == ((46u << 9) | (10u << 5) | 10u));
  CHECK(Read16(archive, 10) == ((3u << 11) | (12u << 5) | 10u));
  // End of central directory: three entries, the directory right after the data.
  const std::size_t end = archive.size() - 22;
  CHECK(Read32(archive, end) == 0x06054b50u && Read16(archive, end + 10) == 3);
  const std::uint32_t directory_size = Read32(archive, end + 12), directory_offset = Read32(archive, end + 16);
  CHECK(directory_offset + directory_size == end);
  CHECK(Read32(archive, directory_offset) == 0x02014b50u);

  // The archive opens in a real unzip and every CRC matches.
  const std::string path =
      (std::filesystem::temp_directory_path() / ("playden-web-zip-" + std::to_string(::getpid()) + ".zip")).string();
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    CHECK(std::fwrite(archive.data(), 1, archive.size(), file) == archive.size());
    std::fclose(file);
  }
  const std::string unzip = "/usr/bin/unzip";
  if (::access(unzip.c_str(), X_OK) == 0) CHECK(std::system((unzip + " -tq '" + path + "' >/dev/null").c_str()) == 0);
  const std::string python = "python3 -I -c \"import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); "
                             "sys.exit(0 if z.testzip() is None and z.namelist() == ['summary.md', 'logs/runtime.log', "
                             "'logs/empty.log'] and len(z.read('logs/runtime.log')) == 307200 else 1)\" '" + path + "'";
  CHECK(std::system(python.c_str()) == 0);
  std::remove(path.c_str());

  // A client gone: nothing more is written.
  std::size_t attempts = 0;
  StoredZip cut([&](const char*, std::size_t) { ++attempts; return false; }, 0);
  CHECK(!cut.Add("a", "b") && !cut.Add("c", "d") && !cut.Finish() && attempts == 1);
  CHECK(!StoredZip([](const char*, std::size_t) { return true; }, 0).Add("", "x"));
  std::puts("web_zip: ok");
}
