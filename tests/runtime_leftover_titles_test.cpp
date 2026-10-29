// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/leftover_titles.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace playden::runtime;
namespace fs = std::filesystem;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
  std::exit(1); } } while (false)

namespace {
constexpr uid_t kOwner = 1234;
NativeActivity Stopped(const std::string&) { return NativeActivity::kStopped; }

LeftoverTitleCandidate Candidate(std::string id = "FAKE78001") {
  return {std::move(id), true, false, kOwner, true, "Half-Life 2", 42, 3, 99};
}

std::vector<LeftoverTitle> Detect(const std::vector<LeftoverTitleCandidate>& candidates,
                                 const std::vector<RuntimeInstall>& records = {},
                                 const LeftoverTitleActivity& activity = Stopped) {
  return FindLeftoverTitles("/data/homebrew", true, records, activity, kOwner,
      [&](const fs::path&, uid_t, const std::unordered_set<std::string>&) { return candidates; });
}

void FindsOnlyUnrecordedOwnedStoppedTitles() {
  const auto found = Detect({Candidate()});
  CHECK(found.size() == 1);
  CHECK(found[0].title_id == "FAKE78001");
  CHECK(found[0].title_path == "/data/homebrew/FAKE78001");
  CHECK(found[0].name == "Half-Life 2");
  CHECK(found[0].bytes == 42);
  CHECK(found[0].device == 3 && found[0].inode == 99);

  auto foreign = Candidate(); foreign.owner = 999;
  auto symlink = Candidate(); symlink.is_symlink = true;
  auto file = Candidate(); file.is_directory = false;
  auto unreadable = Candidate(); unreadable.readable = false;
  CHECK(Detect({foreign, symlink, file, unreadable}).empty());
  CHECK(Detect({Candidate("FAKE77031"), Candidate("PPSA97031"), Candidate("FAKE79001"),
                Candidate("FAKE7800"), Candidate("FAKE780001"), Candidate("FAKE78a01"),
                Candidate("../FAKE78001")}).empty());
  CHECK(Detect({Candidate()}, {}, [](const std::string&) { return NativeActivity::kActive; }).empty());
  CHECK(Detect({Candidate()}, {}, [](const std::string&) { return NativeActivity::kUnknown; }).empty());
  CHECK(Detect({Candidate()}, {}, {}).empty());

  for (const auto state : {PreparationState::kDownloaded, PreparationState::kPreparing,
       PreparationState::kRegistering, PreparationState::kReady, PreparationState::kFailed,
       PreparationState::kUnavailable, PreparationState::kRemoving}) {
    RuntimeInstall record; record.title_id = "FAKE78001"; record.state = state;
    CHECK(Detect({Candidate()}, {record}).empty());
    record.journal = PreparationJournal{"remove-op", "remove", "unregister"};
    CHECK(Detect({Candidate()}, {record}).empty());
  }
}

void DoesNotScanAnUnreadRepositoryOrUseFailedListings() {
  bool scanned = false;
  CHECK(FindLeftoverTitles("/data/homebrew", false, {}, Stopped, kOwner,
      [&](const fs::path&, uid_t, const std::unordered_set<std::string>&) {
        scanned = true; return std::vector<LeftoverTitleCandidate>{Candidate()};
      }).empty());
  CHECK(!scanned);
  CHECK(FindLeftoverTitles("/data/homebrew", true, {}, Stopped, kOwner,
      [](const fs::path&, uid_t, const std::unordered_set<std::string>&) { return std::nullopt; }).empty());
}

struct TemporaryDirectory {
  TemporaryDirectory() {
    auto pattern = (fs::temp_directory_path() / "playden-leftovers-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back('\0');
    const auto made = ::mkdtemp(writable.data()); CHECK(made);
    path = fs::canonical(made);
  }
  ~TemporaryDirectory() { fs::remove_all(path); }
  fs::path path;
};

void ReadsRealDirectoriesWithoutFollowingLinks() {
  TemporaryDirectory temp;
  const auto title = temp.path / "homebrew/FAKE78001";
  fs::create_directories(title / "sce_sys");
  const std::string metadata = "{\"localizedParameters\":{\"en-US\":{\"titleName\":\"Half-Life 2\"}}}";
  std::ofstream(title / "sce_sys/param.json") << metadata;
  std::ofstream(title / "eboot.bin") << "game";
  fs::create_directories(temp.path / "outside");
  std::ofstream(temp.path / "outside/data") << "not counted";
  fs::create_directory_symlink(temp.path / "outside", title / "link");
  fs::create_symlink(temp.path / "outside/data", title / "linked-file");
  fs::create_directory_symlink(title, temp.path / "homebrew/FAKE78002");
  fs::create_directories(temp.path / "homebrew/FAKE77031");
  const auto found = FindLeftoverTitles(temp.path / "homebrew", true, {}, Stopped, ::geteuid());
  CHECK(found.size() == 1);
  CHECK(found[0].title_id == "FAKE78001");
  CHECK(found[0].name == "Half-Life 2");
  CHECK(found[0].bytes == metadata.size() + 4);
  CHECK(found[0].inode != 0);
  const auto excluded = ListLeftoverTitleCandidates(temp.path / "homebrew", ::geteuid(), {"FAKE78001"});
  CHECK(excluded && excluded->empty());

  fs::create_directory_symlink(temp.path / "homebrew", temp.path / "root-link");
  CHECK(FindLeftoverTitles(temp.path / "root-link", true, {}, Stopped, ::geteuid()).empty());
  CHECK(FindLeftoverTitles(temp.path / "missing", true, {}, Stopped, ::geteuid()).empty());
  fs::remove(title / "sce_sys/param.json");
  fs::create_symlink(temp.path / "outside/data", title / "sce_sys/param.json");
  const auto no_name = FindLeftoverTitles(temp.path / "homebrew", true, {}, Stopped, ::geteuid());
  CHECK(no_name.size() == 1 && no_name[0].name.empty());
  CHECK(no_name[0].bytes == 4);
  // A partial tree cannot produce a trustworthy size. Root can bypass modes
  // on host filesystems, so exercise the permission failure as an ordinary user.
  if (::geteuid() != 0) {
    CHECK(::chmod((title / "sce_sys").c_str(), 0000) == 0);
    const auto unreadable = FindLeftoverTitles(temp.path / "homebrew", true, {}, Stopped, ::geteuid());
    CHECK(::chmod((title / "sce_sys").c_str(), 0700) == 0);
    CHECK(unreadable.empty());
  }
}
}  // namespace

int main() {
  FindsOnlyUnrecordedOwnedStoppedTitles();
  DoesNotScanAnUnreadRepositoryOrUseFailedListings();
  ReadsRealDirectoriesWithoutFollowingLinks();
}
