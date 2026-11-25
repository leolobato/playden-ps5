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

  // Rockstar: files at the archive root, one in a subfolder.
  const std::map<std::string, std::string> rockstar_files = {
      {"pdstub-sc.exe", "MZsc"}, {"pdlaunch-sc.dll", "MZlaunch"}, {"signin/index.html", "<html>"}};
  auto rockstar = Build(Publisher::kRockstar, "playden-rgschost-1.tzst",
                        fx::File("pdstub-sc.exe", "MZsc") + fx::File("pdlaunch-sc.dll", "MZlaunch") +
                            fx::Directory("signin") + fx::PaxFile("signin/index.html", "<html>") + fx::End(),
                        rockstar_files, {"pdstub-sc.exe", "pdlaunch-sc.dll", "signin/index.html"});
  const std::vector<PublisherRelease> releases = {rockstar.release};
  const std::string version = "1.test";

  // No pack at all.
  auto status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(!status.present);
  CHECK(!PublisherSupported(status, Publisher::kRockstar));
  CHECK(!status.message.empty());

  // Another version only: present, but not this build's.
  Write(pack / "0.old/release.json", ReleaseJson("0.old", releases));
  status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(status.present && !status.version_matches);
  CHECK(status.found_version == "0.old");
  CHECK(status.message.find("0.old") != std::string::npos && status.message.find(version) != std::string::npos);
  fs::remove_all(pack / "0.old");

  // The right version without the archive: not installed.
  Write(pack / version / "release.json", ReleaseJson(version, releases));
  status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(status.present && status.version_matches);
  CHECK(!status.verified.at(Publisher::kRockstar));
  CHECK(!PublisherSupported(status, Publisher::kRockstar));
  CHECK(status.message.find("Rockstar not installed") != std::string::npos);

  // A damaged archive: not verified, and not unpacked.
  Write(pack / version / rockstar.release.archive, rockstar.archive_bytes + "x");
  status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(!status.verified.at(Publisher::kRockstar));
  CHECK(!PublisherSupported(status, Publisher::kRockstar));
  CHECK(status.message.find("Rockstar damaged") != std::string::npos);
  std::string directory, reason;
  CHECK(!PreparePublisherRelease(pack.string(), version, cache.string(), rockstar.release, &directory, &reason));
  CHECK(!reason.empty());

  // A good archive: supported, unless the version doesn't match.
  Write(pack / version / rockstar.release.archive, rockstar.archive_bytes);
  status = CheckPublisherPack(pack.string(), version, releases);
  CHECK(status.verified.at(Publisher::kRockstar));
  CHECK(PublisherSupported(status, Publisher::kRockstar));
  CHECK(status.message.find("is ready") != std::string::npos);
  {
    auto other = status;
    other.version_matches = false;
    CHECK(!PublisherSupported(other, Publisher::kRockstar));
  }

  // release.json may be the trimmed one (the builds page's and Playden's own: the Rockstar archive alone) or the
  // full helper-tools one, which also lists archives this build doesn't pin and a vendor block.
  Write(pack / version / "release.json",
        "{\"version\":\"" + version + "\",\"source_revision\":\"abc\",\"ea_properties_included\":true," +
            "\"artifacts\":{\"playden-eahost-1.tzst\":\"" + std::string(64, 'e') + "\",\"" + rockstar.release.archive +
            "\":\"" + rockstar.release.sha256 + "\",\"playden-ubisoft-1.tzst\":\"" + std::string(64, 'u') + "\"}," +
            "\"tree_sha256\":{\"playden-eahost-1.tzst\":\"" + std::string(64, 'f') + "\",\"" + rockstar.release.archive +
            "\":\"" + rockstar.release.tree_sha256 + "\"},\"vendor\":{\"steam\":{\"manifest\":\"x\"}}}");
  CHECK(PublisherSupported(CheckPublisherPack(pack.string(), version, releases), Publisher::kRockstar));
  Write(pack / version / "release.json",
        "{\"version\":\"" + version + "\",\"artifacts\":{\"" + rockstar.release.archive + "\":\"" +
            rockstar.release.sha256 + "\"}}");  // no tree_sha256: the archive's own hash is enough
  CHECK(PublisherSupported(CheckPublisherPack(pack.string(), version, releases), Publisher::kRockstar));
  // A release.json that disagrees with the pins does not verify.
  auto wrong = releases;
  wrong[0].sha256 = std::string(64, '0');
  Write(pack / version / "release.json", ReleaseJson(version, wrong));
  CHECK(!CheckPublisherPack(pack.string(), version, releases).verified.at(Publisher::kRockstar));
  wrong = releases;
  wrong[0].tree_sha256 = std::string(64, '0');
  Write(pack / version / "release.json", ReleaseJson(version, wrong));
  CHECK(!CheckPublisherPack(pack.string(), version, releases).verified.at(Publisher::kRockstar));
  Write(pack / version / "release.json", ReleaseJson("2.other", releases));
  CHECK(!CheckPublisherPack(pack.string(), version, releases).verified.at(Publisher::kRockstar));
  Write(pack / version / "release.json", ReleaseJson(version, releases));

  // Unpack, clearing a staging folder left behind.
  fs::create_directories(cache / ".installing-stale");
  CHECK(PreparePublisherRelease(pack.string(), version, cache.string(), rockstar.release, &directory, &reason));
  CHECK(directory == (cache / "playden-rgschost-1").string());
  CHECK(Read(fs::path(directory) / "pdstub-sc.exe") == "MZsc");
  CHECK(Read(fs::path(directory) / "signin/index.html") == "<html>");
  CHECK(PublisherCacheValid(directory, rockstar.release));
  CHECK(!fs::exists(cache / ".installing-stale"));
  struct stat info {};
  CHECK(::stat((fs::path(directory) / "pdstub-sc.exe").c_str(), &info) == 0 && (info.st_mode & 0777) == 0644);

  // A changed file invalidates the cache; preparing again repairs it.
  Write(fs::path(directory) / "pdstub-sc.exe", "MZchanged");
  CHECK(!PublisherCacheValid(directory, rockstar.release));
  CHECK(PreparePublisherRelease(pack.string(), version, cache.string(), rockstar.release, &directory, &reason));
  CHECK(Read(fs::path(directory) / "pdstub-sc.exe") == "MZsc");
  // An extra file invalidates it too.
  Write(fs::path(directory) / "extra.dll", "MZ");
  CHECK(!PublisherCacheValid(directory, rockstar.release));

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
    auto bad = Build(Publisher::kRockstar, "playden-rgschost-" + label + ".tzst", tar, {{"pdstub-sc.exe", "x"}},
                     {"pdstub-sc.exe"});
    Write(pack / version / bad.release.archive, bad.archive_bytes);
    reason.clear();
    CHECK(!PreparePublisherRelease(pack.string(), version, cache.string(), bad.release, &directory, &reason));
    CHECK(!reason.empty());
    CHECK(!fs::exists(cache / ("playden-rgschost-" + label)));
  }
  // An archive whose files don't match the pinned tree hash is refused.
  auto lying = Build(Publisher::kRockstar, "playden-rgschost-lying.tzst", fx::File("pdstub-sc.exe", "evil") + fx::End(),
                     {{"pdstub-sc.exe", "good"}}, {"pdstub-sc.exe"});
  Write(pack / version / lying.release.archive, lying.archive_bytes);
  CHECK(!PreparePublisherRelease(pack.string(), version, cache.string(), lying.release, &directory, &reason));
  for (const auto& entry : fs::directory_iterator(cache))
    CHECK(entry.path().filename().string().rfind(".installing", 0) != 0);

  // Earlier packs: while this build's isn't there, the newest earlier one still on the console, from its unpacked
  // cache or its archive. Nothing else counts: a damaged one is passed over.
  {
    const auto earlier_root = root / "earlier";
    const auto earlier_pack = earlier_root / "publisher";
    const auto earlier_cache = earlier_root / "cache";
    const auto files_v1 = std::map<std::string, std::string>{{"pdstub-sc.exe", "MZv1"}};
    const auto files_v2 = std::map<std::string, std::string>{{"pdstub-sc.exe", "MZv2"}};
    auto v1 = Build(Publisher::kRockstar, "playden-rgschost-0.1.tzst", fx::File("pdstub-sc.exe", "MZv1") + fx::End(),
                    files_v1, {"pdstub-sc.exe"});
    auto v2 = Build(Publisher::kRockstar, "playden-rgschost-0.2.tzst", fx::File("pdstub-sc.exe", "MZv2") + fx::End(),
                    files_v2, {"pdstub-sc.exe"});
    const std::vector<PublisherPackPin> pins = {{"0.2", v2.release}, {"0.1", v1.release}};
    std::string found, at;
    CHECK(!PrepareEarlierPublisherRelease(earlier_pack.string(), earlier_cache.string(), Publisher::kRockstar, pins,
                                          &found, &at));
    CHECK(found.empty() && at.empty());
    // The older archive alone: unpacked and used.
    Write(earlier_pack / "0.1" / v1.release.archive, v1.archive_bytes);
    CHECK(PrepareEarlierPublisherRelease(earlier_pack.string(), earlier_cache.string(), Publisher::kRockstar, pins,
                                         &found, &at));
    CHECK(found == "0.1" && at == (earlier_cache / "playden-rgschost-0.1").string());
    CHECK(Read(fs::path(at) / "pdstub-sc.exe") == "MZv1");
    // A damaged newer archive is passed over.
    Write(earlier_pack / "0.2" / v2.release.archive, v2.archive_bytes + "x");
    CHECK(PrepareEarlierPublisherRelease(earlier_pack.string(), earlier_cache.string(), Publisher::kRockstar, pins,
                                         &found, &at));
    CHECK(found == "0.1");
    // The newer one's unpacked cache, still matching its pins, wins even without its archive.
    Write(earlier_pack / "0.2" / v2.release.archive, v2.archive_bytes);
    CHECK(PrepareEarlierPublisherRelease(earlier_pack.string(), earlier_cache.string(), Publisher::kRockstar, pins,
                                         &found, &at));
    CHECK(found == "0.2" && Read(fs::path(at) / "pdstub-sc.exe") == "MZv2");
    fs::remove_all(earlier_pack / "0.2");
    CHECK(PrepareEarlierPublisherRelease(earlier_pack.string(), earlier_cache.string(), Publisher::kRockstar, pins,
                                         &found, &at));
    CHECK(found == "0.2");
    // A changed file in that cache: back to the older pack.
    Write(earlier_cache / "playden-rgschost-0.2" / "pdstub-sc.exe", "MZevil");
    CHECK(PrepareEarlierPublisherRelease(earlier_pack.string(), earlier_cache.string(), Publisher::kRockstar, pins,
                                         &found, &at));
    CHECK(found == "0.1" && Read(fs::path(at) / "pdstub-sc.exe") == "MZv1");
  }
  // This build's earlier pins: newest first, all older than its own pack, never its own archive.
  {
    const auto& earlier = EarlierPublisherPacks();
    CHECK(!earlier.empty());
    // "20261125.5": a date and that day's build.
    const auto order = [](const std::string& version) {
      const auto dot = version.find('.');
      CHECK(dot == 8);
      return std::make_pair(std::stoul(version.substr(0, dot)), std::stoul(version.substr(dot + 1)));
    };
    auto previous = order(kPublisherPackVersion);
    for (const auto& pin : earlier) {
      CHECK(order(pin.version) < previous);
      previous = order(pin.version);
      CHECK(pin.release.archive == "playden-rgschost-" + pin.version + ".tzst");
      CHECK(pin.release.archive != PinnedPublisherRelease(pin.release.publisher).archive);
      CHECK(pin.release.sha256.size() == 64 && pin.release.tree_sha256.size() == 64 && pin.release.size > 0);
    }
  }

  // The pinned releases: the Rockstar archive alone.
  CHECK(PinnedPublisherReleases().size() == 1);
  CHECK(PinnedPublisherRelease(Publisher::kRockstar).archive == "playden-rgschost-20261125.5.tzst");
  CHECK(std::string(kPublisherPackVersion) == "20261125.5");
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
      CHECK(entry->Find("size") && entry->Find("size")->AsUint64() == release.size && release.size > 0);
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
      CHECK(fs::file_size(real_pack / kPublisherPackVersion / release.archive) == release.size);
      reason.clear();
      const bool ok = PreparePublisherRelease(real_pack.string(), (real_pack / "cache").string(), release, &directory,
                                              &reason);
      if (!ok) std::fprintf(stderr, "%s: %s\n", release.archive.c_str(), reason.c_str());
      CHECK(ok);
      CHECK(PublisherCacheValid(directory, release));
    }
    CHECK(PublisherSupported(status, Publisher::kRockstar));
    std::printf("real publisher pack: the archive matches its pin\n");
  } else {
    std::printf("PLAYDEN_PUBLISHER_ARCHIVES unset: real archives not checked\n");
  }
  // The earlier packs, when at hand (a folder of <version>/<archive>, as builds.playden.club's packs/): each one
  // still found under its pins.
  if (const char* real = std::getenv("PLAYDEN_EARLIER_PUBLISHER_PACKS")) {
    for (const auto& pin : EarlierPublisherPacks()) {
      CHECK(fs::file_size(fs::path(real) / pin.version / pin.release.archive) == pin.release.size);
      const auto cache_root = root / "earlier-real" / pin.version;
      std::string found, at;
      CHECK(PrepareEarlierPublisherRelease(real, cache_root.string(), pin.release.publisher, {pin}, &found, &at));
      CHECK(found == pin.version && PublisherCacheValid(at, pin.release));
    }
    std::printf("earlier publisher packs: each archive matches its pin\n");
  }

  fs::remove_all(root);
  std::printf("runtime_publisher_pack: ok\n");
  return 0;
}
