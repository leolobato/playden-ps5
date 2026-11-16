#include "runtime/tzst_archive.h"
#include "tzst_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace playden::runtime;
namespace fx = tzst_fixture;

namespace {

struct Listing {
  bool ok = false;
  std::string reason;
  std::map<std::string, std::string> files;  // path -> content
  std::map<std::string, char> types;
};

Listing List(const std::string& path) {
  Listing listing;
  std::string current;
  listing.ok = ReadTzst(
      path,
      [&](const TarEntry& entry) {
        current = entry.path;
        listing.types[entry.path] = entry.type;
        if (entry.type == '0') listing.files[entry.path];
        return true;
      },
      [&](const char* data, std::size_t size) {
        listing.files[current].append(data, size);
        return true;
      },
      &listing.reason);
  return listing;
}

}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / ("playden-tzst-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  const auto archive = (root / "a.tzst").string();

  const std::string long_dir = "home/xuser/.wine/drive_c/Program Files (x86)/Steam/bin/some/deeper/folder/that/is";
  const std::string long_path = long_dir + "/long-enough-to-need-a-prefix.dll";
  CHECK(long_path.size() > 100);
  const std::string big(300000, 'z');  // spans several raw blocks

  // ustar short name, a directory, a prefix path, a pax path, a GNU long name, a big file.
  fx::WriteBytes(archive, fx::Frame(fx::Directory("bin/") + fx::File("steam.exe", "MZsteam") +
                                    fx::File("long-enough-to-need-a-prefix.dll", "prefixed", long_dir) +
                                    fx::PaxFile(long_dir + "/pax-path.dll", "pax") +
                                    fx::GnuLongFile(long_dir + "/gnu-path.dll", "gnu") + fx::File("big.bin", big) +
                                    fx::End()));
  auto listing = List(archive);
  CHECK(listing.ok);
  CHECK(listing.types.at("bin") == '5');  // trailing slash dropped
  CHECK(listing.files.at("steam.exe") == "MZsteam");
  CHECK(listing.files.at(long_path) == "prefixed");
  CHECK(listing.files.at(long_dir + "/pax-path.dll") == "pax");
  CHECK(listing.files.at(long_dir + "/gnu-path.dll") == "gnu");
  CHECK(listing.files.at("big.bin") == big);
  CHECK(listing.files.size() == 5);

  // Two frames back to back form one stream.
  const std::string tar = fx::File("one.txt", "1") + fx::File("two.txt", "2") + fx::End();
  fx::WriteBytes(archive, fx::Frame(tar.substr(0, 700)) + fx::Frame(tar.substr(700)));
  listing = List(archive);
  CHECK(listing.ok);
  CHECK(listing.files.at("one.txt") == "1" && listing.files.at("two.txt") == "2");

  // Links and devices are refused.
  fx::WriteBytes(archive, fx::Frame(fx::Header("link", '2', 0, {}, "steam.exe") + fx::End()));
  listing = List(archive);
  CHECK(!listing.ok);
  CHECK(!listing.reason.empty());
  fx::WriteBytes(archive, fx::Frame(fx::Header("hard", '1', 0, {}, "steam.exe") + fx::End()));
  CHECK(!List(archive).ok);
  fx::WriteBytes(archive, fx::Frame(fx::Header("dev", '3', 0) + fx::End()));
  CHECK(!List(archive).ok);

  // A truncated stream, a damaged header checksum and plain garbage are refused.
  const auto whole = fx::Frame(fx::File("steam.exe", std::string(4000, 'x')) + fx::End());
  fx::WriteBytes(archive, whole.substr(0, whole.size() / 2));
  CHECK(!List(archive).ok);
  auto damaged_tar = fx::File("steam.exe", "x") + fx::End();
  damaged_tar[0] = 'S';
  fx::WriteBytes(archive, fx::Frame(damaged_tar));
  CHECK(!List(archive).ok);
  fx::WriteBytes(archive, "not an archive at all");
  CHECK(!List(archive).ok);
  // A stream that ends before the end-of-archive marker is refused too.
  fx::WriteBytes(archive, fx::Frame(fx::File("steam.exe", "x")));
  CHECK(!List(archive).ok);
  CHECK(!List((root / "missing.tzst").string()).ok);

  // A callback can stop the read.
  fx::WriteBytes(archive, fx::Frame(fx::File("a", "1") + fx::File("b", "2") + fx::End()));
  std::string reason;
  int seen = 0;
  CHECK(!ReadTzst(archive, [&](const TarEntry&) { return ++seen < 2; }, [](const char*, std::size_t) { return true; },
                  &reason));
  CHECK(seen == 2);

  // The real Ubisoft archive, when the pack is at hand.
  if (const char* pack = std::getenv("PLAYDEN_PUBLISHER_ARCHIVES")) {
    listing = List(std::string(pack) + "/playden-ubisoft-20261003.11.tzst");
    CHECK(listing.ok);
    for (const char* name : {"pdstub-ubi.exe", "uplay_r264.dll", "uplay_r2.dll", "ubiorbitapi_r2.dll", "uplay_r1.dll",
                             "uplay_aux_r1.dll"})
      CHECK(listing.files.count(name) == 1);
    listing = List(std::string(pack) + "/playden-steamhost-20261003.3.tzst");
    CHECK(listing.ok);
    std::printf("steam host archive: %zu files\n", listing.files.size());
  } else {
    std::printf("PLAYDEN_PUBLISHER_ARCHIVES unset: real archives not read\n");
  }

  fs::remove_all(root);
  std::printf("runtime_tzst_archive: ok\n");
  return 0;
}
