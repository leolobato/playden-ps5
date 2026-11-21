// SPDX-License-Identifier: GPL-3.0-or-later
// Also built as runtime_publisher_pack_install_native_policy (PLAYDEN_PLATFORM_PS5=1): the console's
// ManagedDirectory backend, which opens every folder by absolute path.
#include "runtime/artifact_hash.h"
#include "runtime/publisher_pack.h"
#include "runtime/publisher_pack_install.h"
#include "tzst_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
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

std::string Sha(const std::string& bytes) { return HashArtifactBytes(bytes).value(); }

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

mode_t Mode(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 ? info.st_mode & 07777 : 0;
}

}  // namespace

int main() {
  const auto root = fs::canonical(fs::temp_directory_path()) / ("playden-pack-install-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  const auto pack = root / "publisher";
  const auto cache = root / "publisher-work" / "cache";
  const auto downloads = root / "downloads";
  const std::string version = "2.test";

  // A Rockstar archive and the pin that describes it.
  const std::string tar = fx::File("pdstub-sc.exe", "MZsc") + fx::End();
  const std::string bytes = fx::Frame(tar);
  PublisherRelease release{Publisher::kRockstar, "playden-rgschost-2.test.tzst", Sha(bytes),
                           PublisherTreeDigest({{"pdstub-sc.exe", Sha("MZsc")}}), {"pdstub-sc.exe"}};
  const auto archive = downloads / release.archive;
  Write(archive, bytes);

  std::vector<std::string> logged;
  int root_calls = 0;
  PublisherPackInstall install;
  install.pack_root = pack.string();
  install.version = version;
  install.release = release;
  install.cache_root = cache.string();
  install.as_root = [&](const std::function<bool()>& work) {
    ++root_calls;
    return work();
  };
  install.log = [&](const std::string& line) { logged.push_back(line); };

  // The URL is the builds page's, under its packs folder.
  CHECK(PublisherArchiveUrl(version, release) == "https://builds.playden.club/packs/2.test/playden-rgschost-2.test.tzst");

  // A download that isn't the pinned archive is refused before anything is touched.
  std::string reason;
  Write(downloads / "wrong.tzst", bytes + "x");
  CHECK(!InstallPublisherPack(install, (downloads / "wrong.tzst").string(), &reason));
  CHECK(reason.find("does not match its pin") != std::string::npos);
  CHECK(!fs::exists(pack) && root_calls == 0);

  // A fresh console: the pack folder is created, the archive and Playden's release.json go in, read back.
  fs::create_directories(root / "publisher-work");
  reason.clear();
  CHECK(InstallPublisherPack(install, archive.string(), &reason));
  CHECK(root_calls == 1);
  CHECK(Read(pack / version / release.archive) == bytes);
  CHECK(Mode(pack) == 0755 && Mode(pack / version) == 0755);
  CHECK(Mode(pack / version / release.archive) == 0644 && Mode(pack / version / "release.json") == 0644);
  CHECK(!fs::exists(pack / ("." + version + ".download")));
  auto status = CheckPublisherPack(pack.string(), version, {release});
  CHECK(status.version_matches && PublisherSupported(status, Publisher::kRockstar));
  // Playden's release.json is the trimmed one: this archive only.
  CHECK(Read(pack / version / "release.json") == PublisherReleaseJson(version, {release}));
  CHECK(Read(pack / version / "release.json").find("vendor") == std::string::npos);

  // Over an older install: the old version, leftovers and other unpacked copies go; the rest stays.
  Write(pack / "1.old" / "release.json", "{}");
  Write(pack / "1.old" / "playden-rgschost-1.old.tzst", "old");
  Write(pack / ("." + version + ".download") / "half.tzst", "half");
  Write(pack / ("." + version + ".upload") / "busy.tzst", "kept: the upload tool may be writing it");
  Write(pack / "notes.txt", "kept: not a pack");
  Write(pack / "folder-without-release" / "a", "kept");
  Write(pack / "cache" / "release.json", "kept: CheckPublisherPack's reserved name");
  Write(cache / "playden-rgschost-1.old" / "pdstub-sc.exe", "old");
  Write(cache / "playden-rgschost-2.test" / "pdstub-sc.exe", "kept");
  Write(cache / ".installing-x" / "a", "left to PreparePublisherRelease");
  Write(cache / "other" / "a", "kept: not an archive's");
  // This version's folder is damaged and holds archives this build doesn't pin: it is replaced whole.
  Write(pack / version / release.archive, bytes + "damaged");
  Write(pack / version / "playden-ubisoft-2.test.tzst", "parked");
  CHECK(!PublisherSupported(CheckPublisherPack(pack.string(), version, {release}), Publisher::kRockstar));
  reason.clear();
  CHECK(InstallPublisherPack(install, archive.string(), &reason));
  CHECK(PublisherSupported(CheckPublisherPack(pack.string(), version, {release}), Publisher::kRockstar));
  CHECK(!fs::exists(pack / version / "playden-ubisoft-2.test.tzst"));
  CHECK(!fs::exists(pack / "1.old") && !fs::exists(pack / ("." + version + ".download")));
  CHECK(Read(pack / ("." + version + ".upload") / "busy.tzst") == "kept: the upload tool may be writing it");
  CHECK(!fs::exists(pack / ("." + version + ".replaced")));
  CHECK(fs::exists(pack / "notes.txt") && fs::exists(pack / "folder-without-release") && fs::exists(pack / "cache"));
  CHECK(!fs::exists(cache / "playden-rgschost-1.old"));
  CHECK(Read(cache / "playden-rgschost-2.test" / "pdstub-sc.exe") == "kept");
  CHECK(fs::exists(cache / ".installing-x") && fs::exists(cache / "other"));
  bool logged_removal = false;
  for (const auto& line : logged) logged_removal = logged_removal || line == "publisher pack: removed version 1.old";
  CHECK(logged_removal);
  // The download itself is left to the caller.
  CHECK(fs::exists(archive));

  // An install that stopped after setting this version aside: the copy set aside is put back, then replaced.
  fs::rename(pack / version, pack / ("." + version + ".replaced"));
  CHECK(!fs::exists(pack / version));
  CHECK(InstallPublisherPack(install, archive.string(), &reason));
  CHECK(PublisherSupported(CheckPublisherPack(pack.string(), version, {release}), Publisher::kRockstar));
  CHECK(!fs::exists(pack / ("." + version + ".replaced")));

  // Root can't be given up after the placement: reported as a failure, and nothing more is done (no cache cleanup).
  {
    Write(cache / "playden-rgschost-0.left" / "a", "kept: cleanup stops");
    auto stuck = install;
    stuck.as_root = [](const std::function<bool()>& work) {
      work();
      return false;
    };
    reason.clear();
    CHECK(!InstallPublisherPack(stuck, archive.string(), &reason));
    CHECK(reason.find("root") != std::string::npos);
    CHECK(fs::exists(cache / "playden-rgschost-0.left"));
    fs::remove_all(cache / "playden-rgschost-0.left");
  }

  // A cache folder reached through a link (itself, or a folder above it) is not cleaned: nothing is removed where the
  // link points.
  {
    const auto target = root / "work-target";
    Write(target / "cache" / "playden-rgschost-9.other" / "a", "kept: not reached through a link");
    fs::create_directory_symlink(target, root / "work-link");
    fs::create_directory_symlink(target / "cache", root / "cache-link");
    for (const auto& linked_cache : {root / "work-link" / "cache", root / "cache-link"}) {
      auto through = install;
      through.cache_root = linked_cache.string();
      CHECK(InstallPublisherPack(through, archive.string(), &reason));
      CHECK(fs::exists(target / "cache" / "playden-rgschost-9.other"));
    }
  }

  // A link in the pack folder's path is refused (ManagedDirectory), and nothing is written through it.
  const auto elsewhere = root / "elsewhere";
  fs::create_directories(elsewhere / "real");
  fs::create_directory_symlink(elsewhere / "real", root / "linked");
  auto linked = install;
  linked.pack_root = (root / "linked" / "publisher").string();
  reason.clear();
  CHECK(!InstallPublisherPack(linked, archive.string(), &reason));
  CHECK(!reason.empty());
  CHECK(!fs::exists(elsewhere / "real" / "publisher"));
  // So is a pack folder that is a link.
  fs::create_directory_symlink(elsewhere / "real", root / "publisher-link");
  linked.pack_root = (root / "publisher-link").string();
  CHECK(!InstallPublisherPack(linked, archive.string(), &reason));
  CHECK(fs::is_empty(elsewhere / "real"));

  // Without as_root the work runs as is.
  auto plain = install;
  plain.as_root = nullptr;
  plain.cache_root.clear();
  CHECK(InstallPublisherPack(plain, archive.string(), &reason));
  CHECK(PublisherSupported(CheckPublisherPack(pack.string(), version, {release}), Publisher::kRockstar));

  fs::remove_all(root);
  std::printf("runtime_publisher_pack_install: ok\n");
  return 0;
}
