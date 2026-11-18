#include "runtime/artifact_hash.h"
#include "runtime/publisher_pack.h"
#include "tzst_fixture.h"
#include "steam/backend/json.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

// A release whose pins describe exactly these files (tar paths -> contents).
struct Built {
  PublisherRelease release;
  std::string archive_bytes;
};

Built Build(Publisher publisher, const std::string& name, const std::string& tar,
            const std::map<std::string, std::string>& files, std::vector<std::string> required) {
  std::map<std::string, std::string> digests;
  for (const auto& [path, data] : files) digests[path] = Sha(data);
  Built built;
  built.archive_bytes = fx::Frame(tar);
  built.release = {publisher, name, Sha(built.archive_bytes), PublisherTreeDigest(digests), std::move(required)};
  return built;
}

std::string ReleaseJson(const std::string& version, const std::vector<PublisherRelease>& releases) {
  std::string artifacts, trees;
  for (const auto& r : releases) {
    artifacts += std::string(artifacts.empty() ? "" : ",") + "\"" + r.archive + "\":\"" + r.sha256 + "\"";
    trees += std::string(trees.empty() ? "" : ",") + "\"" + r.archive + "\":\"" + r.tree_sha256 + "\"";
  }
  return "{\"version\":\"" + version + "\",\"artifacts\":{" + artifacts + "},\"tree_sha256\":{" + trees + "}}";
}

}  // namespace

int main(int argc, char** argv) {
  const auto root = fs::canonical(fs::temp_directory_path()) / ("playden-publisher-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const auto pack = root / "publisher";
  const auto cache = pack / "cache";

  // The tree digest is the macOS one: sorted "name\0sha\0" pairs, hashed once.
  CHECK(PublisherTreeDigest({{"b", "2"}, {"a", "1"}}) == Sha(std::string("a\0001\000b\0002\000", 8)));

  // Steam host: its files under the Wine path, with the empty "bin\\shaders" quirk directory.
  const std::string nested = "home/xuser/.wine/drive_c/Program Files (x86)/Steam";
  const std::map<std::string, std::string> steam_files = {
      {"steam.exe", "MZsteam"}, {"steamclient64.dll", "MZclient"}, {"bin/SteamService.exe", "MZservice"}};
  std::string steam_tar = fx::Directory(nested) + fx::Directory(nested + "/bin") +
                          fx::Directory(nested + "/bin\\shaders");
  for (const auto& [path, data] : steam_files) steam_tar += fx::PaxFile(nested + "/" + path, data);
  steam_tar += fx::End();
  auto steam = Build(Publisher::kSteamHost, "playden-steamhost-1.tzst", steam_tar, steam_files,
                     {"steam.exe", "steamclient64.dll"});
  // Ubisoft: files at the archive root.
  const std::map<std::string, std::string> ubi_files = {{"pdstub-ubi.exe", "MZubi"}, {"uplay_r264.dll", "MZuplay"}};
  auto ubi = Build(Publisher::kUbisoft, "playden-ubisoft-1.tzst",
                   fx::File("pdstub-ubi.exe", "MZubi") + fx::File("uplay_r264.dll", "MZuplay") + fx::End(), ubi_files,
                   {"pdstub-ubi.exe", "uplay_r264.dll"});
  const std::vector<PublisherRelease> releases = {steam.release, ubi.release};
  const std::string version = "1.test";

  // No pack at all.
  auto status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(!status.present);
  CHECK(!PublisherSupported(status, Publisher::kUbisoft));
  CHECK(!status.message.empty());

  // Another version only: present, but not this build's.
  Write(pack / "0.old/release.json", ReleaseJson("0.old", releases));
  status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(status.present && !status.version_matches);
  CHECK(status.found_version == "0.old");
  CHECK(status.message.find("0.old") != std::string::npos && status.message.find(version) != std::string::npos);
  fs::remove_all(pack / "0.old");

  // The right version with a damaged Ubisoft archive: Steam host verified, Ubisoft not.
  Write(pack / version / "release.json", ReleaseJson(version, releases));
  Write(pack / version / steam.release.archive, steam.archive_bytes);
  Write(pack / version / ubi.release.archive, ubi.archive_bytes + "x");
  status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(status.present && status.version_matches);
  CHECK(status.verified.at(Publisher::kSteamHost));
  CHECK(!status.verified.at(Publisher::kUbisoft));
  CHECK(!PublisherSupported(status, Publisher::kUbisoft));
  std::string directory, reason;
  CHECK(!PreparePublisherRelease(pack.string(), version, cache.string(), ubi.release, &directory, &reason));
  CHECK(!reason.empty());

  // Both good: Ubisoft is supported (it needs the Steam host too), EA is not part of this pack.
  Write(pack / version / ubi.release.archive, ubi.archive_bytes);
  status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(PublisherSupported(status, Publisher::kUbisoft));
  CHECK(!PublisherSupported(status, Publisher::kEa));
  // A release.json that disagrees with the pins does not verify.
  auto wrong = releases;
  wrong[1].sha256 = std::string(64, '0');
  Write(pack / version / "release.json", ReleaseJson(version, wrong));
  CHECK(!CheckPublisherPack(pack.string(), version, releases).verified.at(Publisher::kUbisoft));
  Write(pack / version / "release.json", ReleaseJson(version, releases));

  // Unpack: Ubisoft at the root, the Steam host from its nested folder without the quirk directory.
  fs::create_directories(cache / ".installing-stale");
  CHECK(PreparePublisherRelease(pack.string(), version, cache.string(), ubi.release, &directory, &reason));
  CHECK(directory == (cache / "playden-ubisoft-1").string());
  CHECK(Read(fs::path(directory) / "pdstub-ubi.exe") == "MZubi");
  CHECK(PublisherCacheValid(directory, ubi.release));
  CHECK(!fs::exists(cache / ".installing-stale"));
  CHECK(PreparePublisherRelease(pack.string(), version, cache.string(), steam.release, &directory, &reason));
  CHECK(Read(fs::path(directory) / "bin/SteamService.exe") == "MZservice");
  CHECK(!fs::exists(fs::path(directory) / "bin\\shaders"));
  CHECK(!fs::exists(fs::path(directory) / "home"));
  CHECK(PublisherCacheValid(directory, steam.release));
  struct stat info {};
  CHECK(::stat((fs::path(directory) / "steam.exe").c_str(), &info) == 0 && (info.st_mode & 0777) == 0644);

  // A changed file invalidates the cache; preparing again repairs it.
  Write(fs::path(directory) / "steam.exe", "MZchanged");
  CHECK(!PublisherCacheValid(directory, steam.release));
  CHECK(PreparePublisherRelease(pack.string(), version, cache.string(), steam.release, &directory, &reason));
  CHECK(Read(fs::path(directory) / "steam.exe") == "MZsteam");
  // An extra file invalidates it too.
  Write(fs::path(directory) / "extra.dll", "MZ");
  CHECK(!PublisherCacheValid(directory, steam.release));

  // Path rules: each archive below is refused and leaves no cache behind.
  const std::vector<std::pair<std::string, std::string>> refused = {
      {"dotdot", fx::File("../evil.exe", "x") + fx::End()},
      {"drive", fx::File("C:/evil.exe", "x") + fx::End()},
      {"backslash", fx::File("bin\\evil.exe", "x") + fx::End()},
      {"absolute", fx::File("/evil.exe", "x") + fx::End()},
      {"case", fx::File("Evil.exe", "x") + fx::File("evil.EXE", "y") + fx::End()},
      {"missing", fx::File("other.exe", "x") + fx::End()},
  };
  for (const auto& [label, tar] : refused) {
    auto bad = Build(Publisher::kUbisoft, "playden-ubisoft-" + label + ".tzst", tar, {{"pdstub-ubi.exe", "x"}},
                     {"pdstub-ubi.exe"});
    Write(pack / version / bad.release.archive, bad.archive_bytes);
    reason.clear();
    CHECK(!PreparePublisherRelease(pack.string(), version, cache.string(), bad.release, &directory, &reason));
    CHECK(!reason.empty());
    CHECK(!fs::exists(cache / ("playden-ubisoft-" + label)));
  }
  // An archive whose files don't match the pinned tree hash is refused.
  auto lying = Build(Publisher::kUbisoft, "playden-ubisoft-lying.tzst", fx::File("pdstub-ubi.exe", "evil") + fx::End(),
                     {{"pdstub-ubi.exe", "good"}}, {"pdstub-ubi.exe"});
  Write(pack / version / lying.release.archive, lying.archive_bytes);
  CHECK(!PreparePublisherRelease(pack.string(), version, cache.string(), lying.release, &directory, &reason));
  for (const auto& entry : fs::directory_iterator(cache))
    CHECK(entry.path().filename().string().rfind(".installing", 0) != 0);

  // The pinned releases: four archives, each with its required files.
  CHECK(PinnedPublisherReleases().size() == 4);
  CHECK(std::string(kPublisherPackVersion) == "20261120.1");
  // dependencies.lock.json (read by the upload tool) pins the same pack.
  if (argc > 1) {
    playden::steam::json::Limits limits;
    limits.max_bytes = 4 * 1024 * 1024;
    const auto lock = playden::steam::json::Parse(Read(argv[1]), limits);
    CHECK(lock && lock->is_object());
    const auto* pinned = lock->Find("publisher_pack");
    CHECK(pinned && pinned->Find("version") && pinned->Find("version")->text() == kPublisherPackVersion);
    const auto* archives = pinned->Find("archives");
    CHECK(archives && archives->object().size() == PinnedPublisherReleases().size());
    for (const auto& release : PinnedPublisherReleases()) {
      const auto* entry = archives->Find(release.archive);
      CHECK(entry && entry->Find("sha256")->text() == release.sha256);
      CHECK(entry->Find("tree_sha256")->text() == release.tree_sha256);
    }
  }

  // The real pack, when at hand: every archive verifies and unpacks to its pinned tree hash.
  if (const char* real = std::getenv("PLAYDEN_PUBLISHER_ARCHIVES")) {
    const auto real_pack = root / "real";
    fs::create_directories(real_pack / kPublisherPackVersion);
    for (const auto& entry : fs::directory_iterator(real))
      fs::copy_file(entry.path(), real_pack / kPublisherPackVersion / entry.path().filename());
    status = CheckPublisherPack(real_pack.string());
    CHECK(status.version_matches);
    for (const auto& release : PinnedPublisherReleases()) {
      CHECK(status.verified.at(release.publisher));
      reason.clear();
      const bool ok = PreparePublisherRelease(real_pack.string(), (real_pack / "cache").string(), release, &directory,
                                              &reason);
      if (!ok) std::fprintf(stderr, "%s: %s\n", release.archive.c_str(), reason.c_str());
      CHECK(ok);
      CHECK(PublisherCacheValid(directory, release));
    }
    CHECK(PublisherSupported(status, Publisher::kUbisoft));
    std::printf("real publisher pack: all four archives match their pins\n");
  } else {
    std::printf("PLAYDEN_PUBLISHER_ARCHIVES unset: real archives not checked\n");
  }

  fs::remove_all(root);
  std::printf("runtime_publisher_pack: ok\n");
  return 0;
}
