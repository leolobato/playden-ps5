// SPDX-License-Identifier: GPL-3.0-or-later
// #868: everything Playden leaves under its data roots can be deleted over FTP. LAN tools run as
// root, which the console gives no permission bypass on /data, so every folder of the app user's
// must be 0777 and every file readable by others, except the named secrets, game-launch.json (the
// title shell wants it 0600) and the bottles of live games (the title shell wants them private
// until a runtime kit with publication-open-modes).
#include "homebrew/runtime_host.hpp"
#include "open_modes_walk.h"
#include "runtime_host_fixture.h"
#include "runtime/artifact_hash.h"
#include "runtime/playden_removal.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace playden::homebrew;
using namespace playden::runtime;
using namespace playden::ps5;
using namespace playden::test::runtime_host;
using playden::test::OpenModesProblems;
using playden::test::OpenModesRules;
namespace fs = std::filesystem;

namespace {
RuntimePrepareRequest Hl2() { return {220, "/data/homebrew/FAKE77031/games/220", "internal", "Half-Life 2"}; }

bool LiveBottle(const fs::path& relative) {
  // games/<install id> under /data/playden: a bottle a title uses. <id>.kept and <id>.saves have no title.
  auto it = relative.begin();
  if (relative.empty() || *it != "games" || ++it == relative.end() || std::next(it) != relative.end()) return false;
  return it->string().find('.') == std::string::npos;
}

bool AllowedPrivate(const fs::path& relative) {
  return relative.filename() == "game-launch.json" ||
         relative.string().find("AppData/Local/Steam") != std::string::npos;
}

void Report(const std::vector<std::string>& problems, const char* stage) {
  for (const auto& problem : problems) std::fprintf(stderr, "%s: FTP can't delete %s\n", stage, problem.c_str());
}

// Prepare to Ready, then uninstall keeping saves: the state, title and kept bottle are all open.
void PrepareAndKeepLeaveOpenModes() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  config.uuid = []() -> std::optional<std::string> { return std::string("00000000-0000-4000-8000-000000000001"); };
  config.unregister_title = [&fixture](const std::string&, const std::string&) {
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0666);
  CHECK(chmod(game.c_str(), 0777) == 0);
  const auto data = fixture.root / "data";
  const OpenModesRules rules{LiveBottle, AllowedPrivate};

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220) && done.installs.size() == 1);
  // A save the game wrote (Wine's umask is 077).
  const auto bottle = data / "playden/games/steam-220";
  const auto saves = bottle / "library/prefixes/game/drive_c/users/steamuser/Saved";
  Write(saves / "slot1.sav", "player-save", 0600);
  CHECK(chmod(saves.c_str(), 0700) == 0);
  const auto token = bottle / "library/prefixes/game/drive_c/users/prospero/AppData/Local/Steam/local.vdf";
  Write(token, "token", 0600);
  Write(token.parent_path() / "htmlcache/Cookies", "cookies", 0600);
  CHECK(chmod((token.parent_path() / "htmlcache").c_str(), 0700) == 0);

  bool clean = true;
  for (const auto* root : {"runtime-state", "playden", "homebrew/FAKE78001"}) {
    const auto problems = OpenModesProblems(data / root, rules);
    Report(problems, root);
    clean = clean && problems.empty();
  }
  CHECK(clean);

  const auto before = host.Snapshot().removals_handled;
  host.Remove(220, false);
  const auto removed = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.removals_handled > before; }, 30);
  if (removed.removal_failures.count(220))
    std::fprintf(stderr, "Remove failed: %s\n", removed.removal_failures.at(220).c_str());
  CHECK(!removed.removal_failures.count(220) && removed.installs.empty());
  CHECK(fs::exists(data / "playden/games/steam-220.kept"));
  for (const auto* root : {"runtime-state", "playden"}) {
    const auto problems = OpenModesProblems(data / root, rules);
    Report(problems, root);
    clean = clean && problems.empty();
  }
  CHECK(clean);
  // Launcher files kept with the bottle stay owner-only; their folder can still be emptied over FTP.
  // (Keeping drops the Steam sign-in itself: DropOnlineSession.)
  struct stat info{};
  const auto steam = data / "playden/games/steam-220.kept/library/prefixes/game/drive_c/users/prospero/AppData/Local/Steam";
  CHECK(stat((steam / "htmlcache/Cookies").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
  CHECK(stat((steam / "htmlcache").c_str(), &info) == 0 && (info.st_mode & 0777) == 0777);
  CHECK(stat(steam.c_str(), &info) == 0 && (info.st_mode & 0777) == 0777);
  host.Shutdown();
}

// #568: a recursive chmod over FTP opens a live bottle, which the title shell refuses. Repair gives the
// bottle its modes back first, instead of failing on it ("the bottle is not this install's private directory").
void RepairHealsAnOpenedBottleFirst() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  config.uuid = []() -> std::optional<std::string> { return std::string("00000000-0000-4000-8000-000000000001"); };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0666);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);
  CHECK(ready.installs[0].state == PreparationState::kReady);
  const auto bottle = fixture.root / "data/playden/games/steam-220";
  for (const auto* folder : {"", "library", "library/prefixes", "library/prefixes/game", "library/profiles",
                             "library/input", ".playden-control"})
    CHECK(chmod((bottle / folder).c_str(), 0777) == 0);
  const auto before = host.Snapshot().maintenance_handled;
  host.Maintain(220, RuntimeMaintenance::kRepair);
  const auto repaired = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  if (!repaired.maintenance.at(220).ok) std::fprintf(stderr, "Repair: %s\n", repaired.maintenance.at(220).message.c_str());
  CHECK(repaired.maintenance.at(220).ok);
  struct stat info{};
  CHECK(stat(bottle.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
  CHECK(stat((bottle / ".playden-control").c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
  host.Shutdown();
}

// Uninstall Playden (#868): every game's PS5 title goes, then every file Playden made. With keep_saves only the kept
// bottles stay. A sentinel beside the roots and the target of a link planted in them survive.
void RemovePlaydenLeavesOnlyKeptSaves(bool keep_saves, bool resumed = false) {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.state_root = fixture.root / "data/playden/runtime-state";
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  config.uuid = []() -> std::optional<std::string> { return std::string("00000000-0000-4000-8000-000000000001"); };
  config.unregister_title = [&fixture](const std::string&, const std::string&) {
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  unsigned raises = 0;
  config.as_root = [&raises](const std::function<bool()>& work) { ++raises; return work(); };
  const auto data = fixture.root / "data";
  Write(data / "playden-games/220/hl2.exe", "existing-game", 0666);
  Write(data / "other-app/keep.txt", "not Playden's", 0644);
  Write(data / "playden/kits/playden-kit/manifest.json", "{}", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare({220, "/data/playden-games/220", "internal", "Half-Life 2"});
  const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (ready.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", ready.refusals.at(220).c_str());
  CHECK(!ready.refusals.count(220));
  Write(data / "playden/games/steam-220/library/prefixes/game/drive_c/users/steamuser/save.dat", "player-save", 0600);
  fs::create_symlink(data / "other-app", data / "playden/user-link");
  // A running game refuses it, and nothing is removed.
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  host.RemovePlayden(keep_saves);
  auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.removing_playden && s.playden_removal; }, 60);
  CHECK(!done.playden_removal->complete && done.playden_removal->message.find("Close every game") == 0);
  CHECK(fs::exists(data / "homebrew/FAKE78001") && fs::exists(data / "playden/runtime-state/installs.json"));
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  if (resumed) {
    // The console went off just after the removal began: the next start finds its journal and finishes it.
    host.Shutdown();
    CHECK(SaveRemovalJournal(data / "playden", {RemovalStep::kTitles, keep_saves}));
    RuntimeHost restarted(config);
    CHECK(restarted.Start());
    done = WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.playden_removals_handled > 0; }, 60);
    restarted.Shutdown();
  } else {
    host.RemovePlayden(keep_saves);
    done = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.removing_playden && s.playden_removal; }, 60);
    host.Shutdown();
  }
  for (const auto& failure : done.playden_removal->failed) std::fprintf(stderr, "failed: %s\n", failure.c_str());
  if (!done.playden_removal->message.empty()) std::fprintf(stderr, "removal: %s\n", done.playden_removal->message.c_str());
  CHECK(done.playden_removal->complete);
  CHECK(!fs::exists(data / "homebrew/FAKE78001") && !fs::exists(data / "playden-games"));
  CHECK(Read(data / "other-app/keep.txt") == "not Playden's");
  CHECK(fixture.registry_mutations >= 2);  // registered once, then the title removed
  if (keep_saves) {
    std::vector<std::string> left;
    for (const auto& entry : fs::recursive_directory_iterator(data / "playden"))
      if (!entry.is_directory()) left.push_back(entry.path().lexically_relative(data / "playden").string());
    for (const auto& path : left)
      CHECK(path.rfind("games/steam-220.kept/", 0) == 0 || path.rfind("logs/", 0) == 0);
    CHECK(Read(data / "playden/games/steam-220.kept/library/prefixes/game/drive_c/users/steamuser/save.dat") ==
          "player-save");
    // Opened: FTP can delete the kept saves too.
    CHECK(OpenModesProblems(data / "playden/games/steam-220.kept", {{}, AllowedPrivate}).empty());
  } else {
    // Logs a thread writes after the removal can make logs/ again; nothing else of Playden's stays.
    std::vector<std::string> left;
    if (fs::exists(data / "playden"))
      for (const auto& entry : fs::recursive_directory_iterator(data / "playden"))
        if (!entry.is_directory()) left.push_back(entry.path().string());
    for (const auto& path : left) std::fprintf(stderr, "left: %s\n", path.c_str());
    CHECK(left.empty());
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) g_runtime_dir = argv[1];
  PrepareAndKeepLeaveOpenModes();
  RepairHealsAnOpenedBottleFirst();
  RemovePlaydenLeavesOnlyKeptSaves(true);
  RemovePlaydenLeavesOnlyKeptSaves(false);
  RemovePlaydenLeavesOnlyKeptSaves(true, true);
  std::puts("open_data_modes: ok");
  return 0;
}
