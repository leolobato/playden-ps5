#include "runtime/publisher_detect.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

namespace {
void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
PublisherDetection Detect(const fs::path& root, std::vector<std::string> targets = {"game.exe"}) {
  return DetectPublisher(root.string(), targets);
}
}  // namespace

int main() {
  const auto base = fs::temp_directory_path() / ("playden-detect-" + std::to_string(::getpid()));
  fs::remove_all(base);
  int n = 0;
  const auto fresh = [&] {
    const auto root = base / std::to_string(++n);
    fs::create_directories(root);
    Write(root / "game.exe", "MZ");
    return root;
  };

  // An ordinary game.
  auto root = fresh();
  Write(root / "steam_api64.dll", "MZ");
  CHECK(!Detect(root).found);

  // EA by launch URI (any case) or by its installer metadata.
  root = fresh();
  auto found = Detect(root, {"LINK2EA://launchgame/123"});
  CHECK(found.found && found.publisher == Publisher::kEa);
  CHECK(Detect(root, {"steam2ea://x"}).publisher == Publisher::kEa);
  Write(root / "__Installer/installerdata.xml", "<DiPManifest/>");
  found = Detect(root);
  CHECK(found.found && found.publisher == Publisher::kEa);
  // EA comes first even with Social Club beside it.
  Write(root / "socialclub.dll", "MZ");
  CHECK(Detect(root).publisher == Publisher::kEa);

  // Rockstar: title.rgl with its magic, in a child folder; the folder is reported.
  root = fresh();
  Write(root / "GTAIV/title.rgl", "RGLM....");
  found = Detect(root);
  CHECK(found.found && found.publisher == Publisher::kRockstar);
  CHECK(found.title_directory == "GTAIV");
  // Without the magic it doesn't count; Social Club in the root does (any case).
  root = fresh();
  Write(root / "title.rgl", "nope");
  CHECK(!Detect(root).found);
  Write(root / "SocialClub64.DLL", "MZ");
  found = Detect(root);
  CHECK(found.found && found.publisher == Publisher::kRockstar && found.title_directory.empty());
  // Hidden folders and grandchildren are not searched.
  root = fresh();
  Write(root / ".hidden/socialclub.dll", "MZ");
  Write(root / "a/b/socialclub.dll", "MZ");
  CHECK(!Detect(root).found);
  // Child folders in case-insensitive order: "alpha" before "Beta".
  root = fresh();
  Write(root / "Beta/socialclub.dll", "MZ");
  Write(root / "alpha/title.rgl", "RGLM");
  CHECK(Detect(root).title_directory == "alpha");

  // Ubisoft: a loader in the root, or beside the launch program (either separator).
  root = fresh();
  Write(root / "UPC_R2_LOADER64.dll", "MZ");
  found = Detect(root);
  CHECK(found.found && found.publisher == Publisher::kUbisoft);
  root = fresh();
  Write(root / "bin/farcry3_d3d11.exe", "MZ");
  Write(root / "bin/uplay_r1_loader.dll", "MZ");
  CHECK(!Detect(root).found);
  CHECK(Detect(root, {"bin\\farcry3_d3d11.exe"}).publisher == Publisher::kUbisoft);
  CHECK(Detect(root, {"bin/farcry3_d3d11.exe"}).publisher == Publisher::kUbisoft);

  // Links are not followed.
  root = fresh();
  Write(base / "elsewhere/socialclub.dll", "MZ");
  fs::create_directory_symlink(base / "elsewhere", root / "linked");
  fs::create_symlink(base / "elsewhere/socialclub.dll", root / "socialclub64.dll");
  CHECK(!Detect(root).found);
  // A launch target that climbs out of the game folder is ignored.
  Write(base / "uplay_r1_loader.dll", "MZ");
  CHECK(!Detect(root, {"../x.exe"}).found);

  // A missing folder is simply not a publisher game.
  CHECK(!Detect(base / "missing").found);

  fs::remove_all(base);
  std::printf("runtime_publisher_detect: ok\n");
  return 0;
}
