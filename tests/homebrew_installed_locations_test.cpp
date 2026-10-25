// An imported game played in place reaches the controller as an external location (import-games
// Task 2.5) and never reaches Steam Cloud (Task 2.6), against a real RuntimeHost.
#include "homebrew/controller.hpp"
#include "homebrew/runtime_host.hpp"
#include "runtime_host_fixture.h"
#include "stores/local/library_source.h"
#include "fake_library_source.h"
#include "platform/managed_directory.h"
#include "platform/ps5/title_allow_list.h"
#include "platform/storage.h"
#include "runtime/layout_migration.h"
#include "runtime/settings_storage.h"
#include "runtime/artifact_hash.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <unistd.h>

using namespace playden::homebrew;
using namespace playden::runtime;
using namespace playden::ps5;
using namespace playden::test::runtime_host;

namespace fs = std::filesystem;
namespace {
unsigned Mode(const fs::path& path) {
  struct stat info{};
  CHECK(lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}

std::uint32_t NumberOf(const RuntimeSnapshot& s, const playden::GameId& game) {
  for (const auto& [number, id] : s.runtime_games)
    if (id == game) return number;
  return 0;
}
}  // namespace

namespace {
// Steam for the controller: a saved session restores (signed in) or there is none (signed out).
class MemorySessions final : public playden::SessionRepository {
 public:
  std::optional<playden::StoredSession> value;
  playden::Result<std::optional<playden::StoredSession>> Load() const override {
    return playden::Result<std::optional<playden::StoredSession>>::Success(value);
  }
  playden::Result<void> Save(const playden::StoredSession&) const override { return playden::Result<void>::Success(); }
  playden::Result<void> Clear() const override { return playden::Result<void>::Success(); }
};
class RestoringBackend final : public playden::steam::AuthBackend {
 public:
  playden::steam::AuthResult AuthenticateQr(const std::atomic<bool>&, const playden::steam::ChallengeCallback&) override {
    return {playden::steam::AuthError::kCanceled, std::nullopt};
  }
  playden::steam::AuthResult Restore(const playden::StoredSession& session, const std::atomic<bool>&) override {
    return {playden::steam::AuthError::kNone, session};
  }
};
class NoDownloads final : public playden::steam::SessionInstallRunner {
 public:
  using SessionInstallRunner::SessionInstallRunner;
  Result InstallApp(const playden::StoredSession&, std::uint32_t, const std::string&, const std::atomic<bool>&,
                    const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                    const std::function<void(playden::steam::InstallStep)>&) override {
    return {false, false, "no downloads here", 0};
  }
  std::string Remove(const std::string&) override { return {}; }
};
// Records every game the Cloud is asked about.
class RecordingCloud final : public playden::steam::CloudRunner {
 public:
  std::mutex mutex;
  std::vector<playden::GameId> seen;
  playden::steam::CloudReport Check(const playden::StoredSession&, const playden::steam::CloudLocation& location,
                                    const std::atomic<bool>&) override {
    Record({location});
    return {playden::steam::CloudState::kUpToDate};
  }
  playden::steam::CloudReport Apply(const playden::StoredSession&, const playden::steam::CloudLocation& location,
                                    playden::steam::CloudResolution, bool, const std::atomic<bool>&) override {
    Record({location});
    return {playden::steam::CloudState::kUpToDate};
  }
  std::vector<playden::steam::CloudDrift> QuickCheck(const playden::StoredSession&,
                                                     const std::vector<playden::steam::CloudLocation>& locations,
                                                     const std::atomic<bool>&) override {
    Record(locations);
    return std::vector<playden::steam::CloudDrift>(locations.size(), playden::steam::CloudDrift::kUnchanged);
  }
  bool Saw(const playden::GameId& id) {
    std::lock_guard<std::mutex> lock(mutex);
    return std::find(seen.begin(), seen.end(), id) != seen.end();
  }

 private:
  void Record(const std::vector<playden::steam::CloudLocation>& locations) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& location : locations) seen.push_back(location.id);
  }
};
}  // namespace

// An imported game played in place is not Playden's install: it reaches the controller as an
// external location. Prepared before a restart and still waiting for the PS5 to register it, it is
// checked again at the next start, like a download (it stayed waiting without the location). It is
// never listed among Playden's installs, and with Steam signed in or out its saves never reach Steam
// Cloud, while the Steam game next to it is checked.
void ExternalGameRegistersAfterRestartWithoutCloud() {
  using playden::GameId;
  using playden::installs::InstalledLocation;
  namespace hb = playden::homebrew;
  const auto local = GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
  Fixture fixture;
  fixture.registers = false;
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
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-0000000000") + (uuids < 10 ? "0" : "") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  const std::string console_root = "/data/playden-import/Imported";
  Write(fixture.root / "data/playden-import/Imported/Bin/Game.exe", pe, 0644);
  CHECK(chmod((fixture.root / "data/playden-import/Imported").c_str(), 0755) == 0);
  // Playden's own Steam download, for the Cloud check to have a game.
  const auto steam_games = fixture.root / "steam-games";
  Write(steam_games / ".playden-games", "220\tinternal\t4096\n", 0644);
  Write(steam_games / "220/.playden-install.json", "{\"install_bytes\":4096}", 0644);

  std::uint32_t number = 0;
  {
    // Prepared earlier: the title waits for the PS5, which registers it once Playden quits.
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    number = *host.RuntimeNumberFor(local);
    RuntimePrepareRequest request{number, console_root, "usb0", "Imported"};
    request.launches = {{"", "", "Bin/Game.exe", "", ""}};
    request.game = local;
    host.Prepare(request);
    const auto waiting = WaitFor(host, [number](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(number) || (!s.installs.empty() && s.installs[0].awaiting_registration));
    }, 60);
    if (waiting.refusals.count(number)) std::fprintf(stderr, "Prepare refused: %s\n", waiting.refusals.at(number).c_str());
    CHECK(!waiting.refusals.count(number) && waiting.installs[0].awaiting_registration);
    host.Shutdown();
  }
  fixture.registered = true;  // Playden quit, and ShadowMountPlus registered the title

  const auto run = [&](bool signed_in, bool with_location, const std::function<bool(const RuntimeSnapshot&, hb::Simulation&)>& done,
                       std::chrono::milliseconds at_least) {
    hb::Simulation model({}, {}, hb::ModelMode::Live);
    MemorySessions sessions;
    if (signed_in) sessions.value = playden::StoredSession{12345, "Synthetic account", "refresh", "access", {}};
    RestoringBackend backend;
    playden::stores::Registry registry;
    registry.Add(std::make_unique<playden::stores::SteamAccount>(backend, sessions));
    auto imported = std::make_unique<playden::test::FakeLibrarySource>();
    imported->games = {playden::test::ImportedGame("0f8fad5b-d9cb-469f-a165-70867728950e", "Imported")};
    registry.Add(std::move(imported));
    NoDownloads downloads(sessions);
    playden::steam::InstallService installs(downloads, steam_games.string());
    RecordingCloud cloud_runner;
    playden::steam::CloudService cloud(cloud_runner, sessions);
    RuntimeHost host(config);
    CHECK(host.Start());
    hb::Controller controller(model, registry, nullptr, &installs, &cloud, &host);
    if (with_location)
      controller.SetExternalLocations([&] {
        return std::vector<InstalledLocation>{{local, console_root, "usb0", true, InstalledLocation::Ownership::kExternal}};
      });
    // Automatic Cloud sync on, so every automatic path would run.
    for (int i = 0; i < 6 && model.state().navigation.route != hb::Route::Settings; ++i) model.action("r1");
    CHECK(model.action("focus:settings-cloud-saves") && model.action("cross") && model.state().cloud_saves_auto);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(60);
    for (;;) {
      controller.Tick();
      const auto snapshot = host.Snapshot();
      if (done(snapshot, model) && std::chrono::steady_clock::now() - started >= at_least) break;
      CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Only Playden's own installs are listed; the imported game has no Cloud saves anywhere.
    const auto& state = model.state();
    CHECK(std::none_of(state.installations.begin(), state.installations.end(),
                       [&](const hb::Installation& item) { return item.game_id == local; }));
    CHECK(std::none_of(state.jobs.begin(), state.jobs.end(), [&](const hb::Job& job) { return job.game_id == local; }));
    CHECK(!state.cloud.count(local) && !cloud_runner.Saw(local));
    for (const auto& game : cloud.Snapshot().games) CHECK(game.id != local);
    CHECK(installs.Snapshot().jobs.empty());
    const bool steam_checked = cloud_runner.Saw(GameId::Steam(220));
    controller.Shutdown();
    return steam_checked;
  };
  const auto ready = [&](const RuntimeSnapshot& s) {
    return s.status == RuntimeHostStatus::kReady && !s.installs.empty() && s.installs[0].game == local &&
           s.installs[0].state == PreparationState::kReady && !s.installs[0].awaiting_registration;
  };
  // Without its location the controller doesn't know the game: it stays waiting.
  run(true, false, [](const RuntimeSnapshot& s, hb::Simulation& model) {
    return s.status == RuntimeHostStatus::kReady && model.state().runtime == hb::RuntimeState::Ready;
  }, std::chrono::milliseconds(500));
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(started.installs.size() == 1 && started.installs[0].awaiting_registration);
    host.Shutdown();
  }
  // With it, the startup check finds the title registered: the game is ready to play. Signed in,
  // the startup Cloud check covers the Steam game only.
  CHECK(run(true, true, [&](const RuntimeSnapshot& s, hb::Simulation& model) {
    const auto game = model.state().runtime_games.find(local);
    return ready(s) && game != model.state().runtime_games.end() && game->second.ready;
  }, std::chrono::milliseconds(0)));
  // Signed out, a preparation the player asks for ends without Cloud too.
  bool asked = false;
  run(false, true, [&](const RuntimeSnapshot& s, hb::Simulation& model) {
    if (!asked && ready(s) && model.state().runtime_games.count(local)) { model.RequestRuntimePrepare(local); asked = true; }
    return asked && ready(s) && s.prepares_handled > 0 && !s.preparing_app;
  }, std::chrono::milliseconds(300));
}

// Preparing a game from another store asks Steam nothing: without a program it is refused with a
// pointer to Game settings, never "Sign in to Steam", and its PS5 title carries no store's name even
// with "Store name in PS5 titles" on.
void ImportedGamePreparesWithoutSteam() {
  using playden::GameId;
  using playden::installs::InstalledLocation;
  namespace hb = playden::homebrew;
  const auto local = GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
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
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-0000000000") + (uuids < 10 ? "0" : "") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  std::atomic<bool> program_chosen{false};
  config.settings_record = [&](const GameId& game) -> std::optional<GameSettingsRecord> {
    CHECK(game == local);
    if (!program_chosen) return std::nullopt;
    GameSettingsRecord record;
    record.overrides["program"] = "Bin/Game.exe";
    return record;
  };
  std::mutex log_mutex;
  std::vector<std::string> lines;
  config.log = [&](const std::string& line) { std::lock_guard<std::mutex> lock(log_mutex); lines.push_back(line); };
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  const std::string console_root = "/data/playden-import/Second";
  Write(fixture.root / "data/playden-import/Second/Bin/Game.exe", pe, 0644);
  CHECK(chmod((fixture.root / "data/playden-import/Second").c_str(), 0755) == 0);

  hb::Simulation model({}, {}, hb::ModelMode::Live);
  MemorySessions sessions;  // signed out of Steam
  RestoringBackend backend;
  playden::stores::Registry registry;
  registry.Add(std::make_unique<playden::stores::SteamAccount>(backend, sessions));
  auto imported = std::make_unique<playden::test::FakeLibrarySource>();
  registry.Add(std::make_unique<playden::test::FakeLibrarySource>(playden::Store::kSteam));  // signed out: empty
  imported->games = {playden::test::ImportedGame("7c9e6679-7425-40de-944b-e07fc1f90ae7", "Second import")};
  registry.Add(std::move(imported));
  RuntimeHost host(config);
  CHECK(host.Start());
  hb::Controller controller(model, registry, nullptr, nullptr, nullptr, &host);
  controller.SetExternalLocations([&] {
    return std::vector<InstalledLocation>{{local, console_root, "usb0", true, InstalledLocation::Ownership::kExternal}};
  });
  const auto tick_until = [&](const std::function<bool(const RuntimeSnapshot&)>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
      controller.Tick();
      const auto snapshot = host.Snapshot();
      if (done(snapshot)) return snapshot;
      if (std::chrono::steady_clock::now() >= deadline)
        std::fprintf(stderr, "auth=%d loading=%d status=%d runtime=%d games=%zu handled=%llu refusals=%zu\n", (int)model.state().account().authentication, (int)model.state().library_loading, (int)snapshot.status,
                     (int)model.state().runtime, model.state().games.size(), (unsigned long long)snapshot.prepares_handled,
                     snapshot.refusals.size());
      CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };
  tick_until([&](const RuntimeSnapshot& s) {
    return s.status == RuntimeHostStatus::kReady && model.state().runtime == hb::RuntimeState::Ready &&
           std::any_of(model.state().games.begin(), model.state().games.end(), [&](const hb::Game& game) { return game.id == local; });
  });
  for (int i = 0; i < 6 && model.state().navigation.route != hb::Route::Settings; ++i) model.action("r1");
  CHECK(model.action("focus:settings-title-store") && model.action("cross") && model.state().title_store_suffix);
  model.RequestRuntimePrepare(local);
  auto refused = tick_until([](const RuntimeSnapshot& s) { return s.prepares_handled > 0 && !s.preparing_app; });
  const auto number = NumberOf(refused, local);
  CHECK(number && refused.refusals.count(number));
  CHECK(refused.refusals.at(number) == "Choose the program that starts this game in Game settings.");
  // With a program chosen it is prepared, and its title is named without a store.
  program_chosen = true;
  model.RequestRuntimePrepare(local);
  tick_until([&](const RuntimeSnapshot& s) {
    return s.prepares_handled > 1 && !s.preparing_app && !s.installs.empty() && s.installs[0].game == local;
  });
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    const auto named = "title metadata app=" + std::to_string(number) + " name=";
    const auto line = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.rfind(named, 0) == 0; });
    CHECK(line != lines.end() && line->rfind(named + "Second import icon=", 0) == 0);
  }
  controller.Shutdown();
  host.Shutdown();
}

// The application thread never waits for the runtime worker. A Local game that never got a number
// this session needs one written to installs.json under the effective-UID hold, which the worker
// keeps through a batch: Prepare for it must not freeze the frame. Each tick returns at once, the
// request stays and the preparation happens once the hold is released.
void FirstPrepareOfAnImportedGameNeverWaitsForTheWorker() {
  using playden::GameId;
  using playden::installs::InstalledLocation;
  namespace hb = playden::homebrew;
  const auto local = GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
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
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-0000000000") + (uuids < 10 ? "0" : "") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.settings_record = [&](const GameId&) -> std::optional<GameSettingsRecord> {
    GameSettingsRecord record;
    record.overrides["program"] = "Bin/Game.exe";
    return record;
  };
  config.hold_effective_uid = playden::ps5::HoldEffectiveUid;
  config.try_hold_effective_uid = [](bool* busy) {
    auto held = playden::ps5::TryHoldEffectiveUid();
    *busy = !held;
    return held;
  };
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  const std::string console_root = "/data/playden-import/Second";
  Write(fixture.root / "data/playden-import/Second/Bin/Game.exe", pe, 0644);
  CHECK(chmod((fixture.root / "data/playden-import/Second").c_str(), 0755) == 0);

  hb::Simulation model({}, {}, hb::ModelMode::Live);
  MemorySessions sessions;
  RestoringBackend backend;
  playden::stores::Registry registry;
  registry.Add(std::make_unique<playden::stores::SteamAccount>(backend, sessions));
  auto imported = std::make_unique<playden::test::FakeLibrarySource>();
  registry.Add(std::make_unique<playden::test::FakeLibrarySource>(playden::Store::kSteam));
  imported->games = {playden::test::ImportedGame("7c9e6679-7425-40de-944b-e07fc1f90ae7", "Second import")};
  registry.Add(std::move(imported));
  RuntimeHost host(config);
  CHECK(host.Start());
  hb::Controller controller(model, registry, nullptr, nullptr, nullptr, &host);
  controller.SetExternalLocations([&] {
    return std::vector<InstalledLocation>{{local, console_root, "usb0", true, InstalledLocation::Ownership::kExternal}};
  });
  // One tick, timed: the frame must not wait on the worker's hold.
  std::chrono::steady_clock::duration slowest{};
  const auto tick = [&] {
    const auto before = std::chrono::steady_clock::now();
    controller.Tick();
    slowest = std::max(slowest, std::chrono::steady_clock::now() - before);
  };
  const auto tick_until = [&](const std::function<bool(const RuntimeSnapshot&)>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
      tick();
      const auto snapshot = host.Snapshot();
      if (done(snapshot)) return snapshot;
      CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };
  tick_until([&](const RuntimeSnapshot& s) {
    return s.status == RuntimeHostStatus::kReady && model.state().runtime == hb::RuntimeState::Ready &&
           std::any_of(model.state().games.begin(), model.state().games.end(), [&](const hb::Game& game) { return game.id == local; });
  });
  CHECK(NumberOf(host.Snapshot(), local) == 0);  // never numbered this session
  // Another thread (the worker, in a batch) holds the effective UID.
  std::mutex hold_mutex;
  std::condition_variable hold_changed;
  bool holding = false, release = false;
  std::thread holder([&] {
    auto held = playden::ps5::HoldEffectiveUid();
    std::unique_lock<std::mutex> lock(hold_mutex);
    holding = true;
    hold_changed.notify_all();
    hold_changed.wait(lock, [&] { return release; });
  });
  {
    std::unique_lock<std::mutex> lock(hold_mutex);
    CHECK(hold_changed.wait_for(lock, std::chrono::seconds(10), [&] { return holding; }));
  }
  model.RequestRuntimePrepare(local);
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  while (std::chrono::steady_clock::now() < until) {
    tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(slowest < std::chrono::milliseconds(200));  // none of those ticks waited for the hold
  CHECK(host.Snapshot().prepares_handled == 0 && NumberOf(host.Snapshot(), local) == 0);
  {
    std::lock_guard<std::mutex> lock(hold_mutex);
    release = true;
  }
  hold_changed.notify_all();
  holder.join();
  // The hold released, the request that waited goes through, once.
  const auto done = tick_until([](const RuntimeSnapshot& s) { return s.prepares_handled > 0 && !s.preparing_app; });
  CHECK(NumberOf(done, local) != 0);
  CHECK(done.prepares_handled == 1);
  controller.Shutdown();
  host.Shutdown();
}

// The Local store end to end (import-games Task 3.3): a folder on a USB drive is found, added and
// listed with its title; Details offers Play, which prepares it from its folder with the program
// chosen at add (written into Game settings first) under a runtime number from 0xF0000000. An
// unplugged drive leaves it listed but unavailable; a folder name the PS5 can't open never plays in
// place. Removing it keeps its files and its bottle (kept data); adding it again brings back its ID
// and settings. Signed out of Steam, Playden opens on Home; signed in, the game is still there.
void ImportedGamePlaysInPlace() {
  using playden::GameId;
  namespace hb = playden::homebrew;
  namespace local = playden::stores::local;
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
  // Replaced by a newer kit for Update runtime, at the end.
  auto kit = SyntheticKit(fixture.root / "kit");
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-0000000000") + (uuids < 10 ? "0" : "") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  // Game settings as the console keeps them: one file, read by the worker at each preparation.
  fs::create_directories(fixture.root / "user");
  std::string settings_reason;
  const auto prepared_settings = PrepareGameSettingsStorage(fixture.root / "user", &settings_reason);
  if (!prepared_settings) std::fprintf(stderr, "settings: %s\n", settings_reason.c_str());
  CHECK(prepared_settings);
  const auto settings_file = *prepared_settings;
  config.settings_record = [settings_file](const GameId& game) -> std::optional<GameSettingsRecord> {
    std::map<GameId, GameSettingsRecord> records;
    std::string reason;
    if (!LoadGameSettingsFile(settings_file.string(), &records, &reason)) return std::nullopt;
    const auto found = records.find(game);
    return found == records.end() ? std::nullopt : std::optional<GameSettingsRecord>(found->second);
  };
  // Play starts the prepared title (on the application thread, from the controller's Tick). Nothing
  // runs it here: without the wrapper's evidence the session soon ends, as a title closed at once would.
  config.launch_evidence_wait_ms = 300;
  // Removing the game removes its PS5 title through the title remover.
  config.unregister_title = [&fixture](const std::string&, const std::string&) {
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  std::vector<std::string> launched;
  config.launch_title = [&launched](const std::string& title, const std::vector<std::string>& arguments) {
    if (arguments.size() == 1 && arguments[0].rfind("session=", 0) == 0) launched.push_back(title);
    return TitleResult{true, 0, "launch"};
  };
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  const auto usb = fixture.root / "mnt/usb0/playden/import";
  Write(usb / "Rally/Bin/Rally.exe", pe + std::string(4096, 'x'), 0644);
  Write(usb / "Rally/Redist/vcredist_x64.exe", pe, 0644);
  Write(usb / "Rally/data.pak", "data", 0644);
  Write(usb / "Caf\xc3\xa9/Game.exe", pe, 0644);
  CHECK(chmod((usb / "Rally").c_str(), 0755) == 0);
  const std::string console_root = "/mnt/usb0/playden/import/Rally";
  std::atomic<bool> usb_present{true};
  // The drop folder's games: FTP leaves them root-owned, simulated by telling the walk the app user
  // is someone else; the fake root raise runs the work and is counted.
  std::atomic<unsigned> drop_raises{0};
  std::atomic<unsigned> drop_opens{0};
  std::atomic<bool> hold_opens{false};
  // imports.json sits next to installs.json, in the folder the runtime host makes private at its start.
  playden::Storage import_storage((fixture.root / "data/runtime-state").string());
  std::string id;

  const auto run = [&](bool signed_in, const std::function<void(hb::Simulation&, hb::Controller&, RuntimeHost&,
                                                                local::LocalLibrarySource&,
                                                                const std::function<void(const std::function<bool()>&)>&)>& body) {
    hb::Simulation model({}, {}, hb::ModelMode::Live);
    model.SetGameSettingsFile(settings_file);
    MemorySessions sessions;
    if (signed_in) sessions.value = playden::StoredSession{12345, "Synthetic account", "refresh", "access", {}};
    RestoringBackend backend;
    local::ImportStore imports(import_storage);
    local::LocalLibraryConfig local_config;
    local_config.volumes = {{"internal", "/data", "playden-import", false}, {"usb0", "/mnt/usb0", "playden/import", true}};
    local_config.host_prefix = fixture.root.string();
    local_config.present = [&usb_present](const local::ImportVolume& volume) {
      return volume.volume != "usb0" || usb_present.load();
    };
    local::DropFolderAccessOptions drop_access;
    drop_access.namespace_root = fixture.root;
    drop_access.app_uid = ::geteuid() + 1;
    drop_access.as_root = [&drop_raises](const std::function<bool()>& work) { ++drop_raises; return work(); };
    // Counted, and held while hold_opens is set (an opening that takes too long).
    local_config.open_drop_folder = [&drop_opens, &hold_opens, opener = local::MakeDropFolderOpener(drop_access)](
                                        const std::string& path) {
      while (hold_opens.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      ++drop_opens;
      return opener(path);
    };
    RuntimeHost host(config);
    CHECK(host.Start());
    WaitFor(host, Started);
    playden::stores::Registry registry;
    registry.Add(std::make_unique<playden::stores::SteamAccount>(backend, sessions));
    registry.Add(std::make_unique<playden::test::FakeLibrarySource>(playden::Store::kSteam));
    auto source = std::make_unique<local::LocalLibrarySource>(imports, local_config);
    auto* local_library = source.get();
    registry.Add(std::move(source));
    hb::Controller controller(model, registry, nullptr, nullptr, nullptr, &host);
    controller.SetLocalLibrary(local_library);
    const auto tick_until = [&](const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      for (;;) {
        controller.Tick();
        if (done()) return;
        if (std::chrono::steady_clock::now() >= deadline) {
          const auto local_snapshot = local_library->Snapshot();
          std::fprintf(stderr, "runtime=%d scanning=%d candidates=%zu games=%zu imports=%zu external=%zu outcomes=%zu\n",
                       (int)model.state().runtime, (int)model.state().import_scanning, model.state().import_candidates.size(),
                       model.state().games.size(), local_snapshot->games.size(), model.state().external_games.size(),
                       local_snapshot->outcomes.size());
        }
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    };
    tick_until([&] {
      return host.Snapshot().status == RuntimeHostStatus::kReady && model.state().runtime == hb::RuntimeState::Ready &&
             !model.state().import_scanning && model.state().import_candidates.size() >= 2;  // Sky joins later
    });
    body(model, controller, host, *local_library, tick_until);
    controller.Shutdown();
    host.Shutdown();
  };
  const auto game_of = [](const hb::Simulation& model, const GameId& game) -> const hb::Game* {
    for (const auto& value : model.state().games) if (value.id == game) return &value;
    return nullptr;
  };
  const auto external_of = [](const hb::Simulation& model, const GameId& game) -> const hb::LiveExternalGame* {
    for (const auto& value : model.state().external_games) if (value.game_id == game) return &value;
    return nullptr;
  };
  const auto row_of = [](const hb::Simulation& model, const std::string& key) -> const hb::ViewRow* {
    for (const auto& row : model.view().rows) if (row.key == key) return &row;
    return nullptr;
  };
  const auto open_details = [](hb::Simulation& model, const GameId& game) {
    for (int i = 0; i < 8 && model.state().navigation.route != hb::Route::Library; ++i) model.action("r1");
    CHECK(model.state().navigation.route == hb::Route::Library);
    CHECK(model.action("focus:game-" + playden::ToKey(game)) && model.action("cross"));
    CHECK(model.state().navigation.route == hb::Route::Details);
  };

  // Signed out of Steam.
  run(false, [&](hb::Simulation& model, hb::Controller&, RuntimeHost& host, local::LocalLibrarySource& source,
                 const std::function<void(const std::function<bool()>&)>& tick_until) {
    // The two folders on the drive are candidates: Rally with its game first and the redistributable
    // flagged, and the one whose name the PS5 can't play from.
    const auto& candidates = model.state().import_candidates;
    const auto rally = std::find_if(candidates.begin(), candidates.end(), [](const hb::LiveImportCandidate& c) { return c.title == "Rally"; });
    CHECK(rally != candidates.end() && rally->volume_id == "usb0" && rally->folder == "playden/import/Rally");
    CHECK(rally->ascii_ok && !rally->imported.valid() && rally->programs.size() == 2);
    CHECK(rally->programs[0].path == "Bin/Rally.exe" && !rally->programs[0].helper && rally->programs[1].helper);
    CHECK(rally->total_bytes > 4096);
    const auto cafe = std::find_if(candidates.begin(), candidates.end(), [](const hb::LiveImportCandidate& c) { return !c.ascii_ok; });
    CHECK(cafe != candidates.end() && cafe->folder == "playden/import/Caf\xc3\xa9");

    // Add: the game is in the library with its folder's name, and plays in place.
    model.RequestImport({hb::ImportRequestKind::Add, {}, "usb0", "Rally", "Bin/Rally.exe"});
    tick_until([&] {
      const auto snapshot = source.Snapshot();
      if (snapshot->games.empty()) return false;
      id = snapshot->games.front().id;
      const auto* external = external_of(model, GameId::Local(id));
      return game_of(model, GameId::Local(id)) && external && external->available &&
             model.state().runtime_games.count(GameId::Local(id));
    });
    const auto game = GameId::Local(id);
    CHECK(game_of(model, game)->title == "Rally");
    CHECK(model.state().navigation.route == hb::Route::Home);
    // Never one of Playden's installs.
    CHECK(model.state().installations.empty());
    // A bad program or an unknown drive is refused, and nothing is added.
    const auto refused = source.Add("usb0", "Rally", "Bin/Missing.exe");
    const auto unknown = source.Add("usb9", "Rally", "Bin/Rally.exe");
    tick_until([&] {
      const auto snapshot = source.Snapshot();
      const auto outcome = [&](std::uint64_t request) {
        return std::any_of(snapshot->outcomes.begin(), snapshot->outcomes.end(), [&](const local::ImportOutcome& o) { return o.request == request && !o.ok; });
      };
      return outcome(refused) && outcome(unknown) && snapshot->games.size() == 1;
    });

    // Details offers Play; it prepares the game from its folder with the chosen program, then starts it.
    open_details(model, game);
    const auto* play = row_of(model, "details-play");
    CHECK(play && play->enabled && play->action == "runtime-play" && play->title == "Play");
    CHECK(model.action("focus:details-play") && model.action("cross"));
    tick_until([&] { return !launched.empty(); });
    CHECK(launched.front() == "FAKE78001");
    tick_until([&] {
      const auto s = host.Snapshot();
      return s.launch && s.launch->game == game && s.launch->state != LaunchState::kReadyToHandoff &&
             s.launch->state != LaunchState::kLaunching;
    });
    {
      const auto snapshot = host.Snapshot();
      const auto number = NumberOf(snapshot, game);
      CHECK(number >= 0xF0000000u && !snapshot.refusals.count(number));
      const auto view = std::find_if(snapshot.installs.begin(), snapshot.installs.end(), [&](const RuntimeInstallView& v) { return v.game == game; });
      CHECK(view->install_path == console_root);
      CHECK(model.state().game_settings.at(game).overrides.at("program") == "Bin/Rally.exe");
      std::map<GameId, GameSettingsRecord> saved;
      std::string reason;
      CHECK(LoadGameSettingsFile(settings_file.string(), &saved, &reason) && saved.at(game).overrides.at("program") == "Bin/Rally.exe");
    }
  });

  // Started again: drives come and go, and the game leaves the library.
  run(false, [&](hb::Simulation& model, hb::Controller&, RuntimeHost& host, local::LocalLibrarySource& source,
                 const std::function<void(const std::function<bool()>&)>& tick_until) {
    const auto game = GameId::Local(id);
    tick_until([&] {
      const auto snapshot = host.Snapshot();
      return game_of(model, game) && model.state().runtime_games.count(game) &&
             (!snapshot.launch || (snapshot.launch->state != LaunchState::kLaunching &&
                                   snapshot.launch->state != LaunchState::kRunning));
    });
    open_details(model, game);

    // Unplugged: still listed, unavailable, and Details says to connect the drive.
    usb_present = false;
    source.CheckVolumes();
    tick_until([&] {
      const auto* external = external_of(model, game);
      return external && !external->available && !model.state().runtime_games.count(game) &&
             model.state().import_candidates.empty();
    });
    CHECK(game_of(model, game));
    CHECK(row_of(model, "details-installed") && row_of(model, "details-installed")->title == "Storage disconnected");
    CHECK(!row_of(model, "details-play"));
    usb_present = true;
    source.CheckVolumes();
    tick_until([&] {
      const auto* external = external_of(model, game);
      return external && external->available && model.state().runtime_games.count(game);
    });

    // The folder the PS5 can't open can be added, but never plays in place.
    model.RequestImport({hb::ImportRequestKind::Add, {}, "usb0", "playden/import/Caf\xc3\xa9", "Game.exe"});
    tick_until([&] { return source.Snapshot()->games.size() == 2 && model.state().external_games.size() == 2; });
    const auto cafe_game = GameId::Local(source.Snapshot()->games[1].id);
    CHECK(!external_of(model, cafe_game)->available && external_of(model, cafe_game)->reason == local::kNotAsciiReason);
    CHECK(!model.state().runtime_games.count(cafe_game));
    model.RequestImport({hb::ImportRequestKind::Remove, cafe_game, {}, {}, {}});
    tick_until([&] { return source.Snapshot()->games.size() == 1 && !game_of(model, cafe_game); });

    // Removed: out of the library, its files where they were, its bottle kept.
    for (int i = 0; i < 4 && model.state().navigation.route != hb::Route::Home; ++i) model.action("circle");
    model.RequestImport({hb::ImportRequestKind::Remove, game, {}, {}, {}});
    tick_until([&] {
      const auto snapshot = host.Snapshot();
      return !game_of(model, game) && source.Snapshot()->games.empty() &&
             std::any_of(snapshot.kept_bottles.begin(), snapshot.kept_bottles.end(), [&](const auto& kept) { return kept.game == game; });
    });
    CHECK(fs::is_regular_file(usb / "Rally/Bin/Rally.exe") && fs::is_regular_file(usb / "Rally/data.pak"));
    CHECK(model.state().external_games.empty());

    // Added again: the same ID, its settings and the program chosen before.
    model.RequestImport({hb::ImportRequestKind::Add, {}, "usb0", "Rally", "Bin/Rally.exe"});
    tick_until([&] { return game_of(model, game) && external_of(model, game) && external_of(model, game)->available; });
    CHECK(source.Snapshot()->games.front().id == id);
    CHECK(model.state().game_settings.at(game).overrides.at("program") == "Bin/Rally.exe");
  });

  // Signed in to Steam, after a restart: the imported game is listed from imports.json and Details offers Play.
  run(true, [&](hb::Simulation& model, hb::Controller&, RuntimeHost&, local::LocalLibrarySource&,
                const std::function<void(const std::function<bool()>&)>& tick_until) {
    const auto game = GameId::Local(id);
    tick_until([&] {
      return model.state().account().authentication == hb::AuthState::SignedIn && game_of(model, game) &&
             model.state().runtime_games.count(game);
    });
    const auto rally = std::find_if(model.state().import_candidates.begin(), model.state().import_candidates.end(),
                                    [](const hb::LiveImportCandidate& c) { return c.title == "Rally"; });
    CHECK(rally != model.state().import_candidates.end() && rally->imported == game);
    open_details(model, game);
    const auto* play = row_of(model, "details-play");
    CHECK(play && play->enabled);
  });

  // A game copied over FTP into the drop folder: its files are opened when it is added and again
  // before each Play; a link out of its folder stops Play with the folder named.
  const auto sky = fixture.root / "data/playden-import/Sky";
  run(false, [&](hb::Simulation& model, hb::Controller& controller, RuntimeHost& host, local::LocalLibrarySource& source,
                 const std::function<void(const std::function<bool()>&)>& tick_until) {
    Write(sky / "Game.exe", pe + std::string(4096, 'x'), 0755);
    Write(sky / "Data/sky.ini", "[sky]", 0644);
    CHECK(chmod((sky / "Data").c_str(), 0755) == 0 && chmod(sky.c_str(), 0755) == 0);
    // Added to play in place: it is prepared without Play, its files opened first.
    const auto opens_at_add = drop_opens.load();
    const auto launches_at_add = launched.size();
    model.RequestImport({hb::ImportRequestKind::Add, {}, "internal", "Sky", "Game.exe", true});
    std::string sky_id;
    tick_until([&] {
      for (const auto& game : source.Snapshot()->games)
        if (game.volume == "internal") sky_id = game.id;
      const auto* external = sky_id.empty() ? nullptr : external_of(model, GameId::Local(sky_id));
      return external && external->available && model.state().runtime_games.count(GameId::Local(sky_id)) &&
             game_of(model, GameId::Local(sky_id));
    });
    const auto game = GameId::Local(sky_id);
    tick_until([&] {
      const auto found = model.state().runtime_games.find(game);
      return !host.Snapshot().preparing_app && found != model.state().runtime_games.end() &&
             (found->second.prepared || found->second.awaiting_registration);
    });
    CHECK(drop_opens.load() == opens_at_add + 2 && launched.size() == launches_at_add);  // at the add, then before preparing
    CHECK(source.Snapshot()->games.size() == 2);
    CHECK(Mode(sky) == 0777 && Mode(sky / "Data") == 0777 && Mode(sky / "Data/sky.ini") == 0666 &&
          Mode(sky / "Game.exe") == 0777 && drop_raises > 0);

    // FTP replaced the .ini (root-owned 0644 again): Play opens it before the game starts.
    CHECK(chmod((sky / "Data/sky.ini").c_str(), 0644) == 0);
    const auto launches = launched.size();
    open_details(model, game);
    CHECK(model.action("focus:details-play") && model.action("cross"));
    tick_until([&] { return launched.size() > launches; });
    CHECK(Mode(sky / "Data/sky.ini") == 0666);
    {
      const auto snapshot = host.Snapshot();
      const auto view = std::find_if(snapshot.installs.begin(), snapshot.installs.end(), [&](const RuntimeInstallView& v) { return v.game == game; });
      CHECK(view != snapshot.installs.end() && view->install_path == "/data/playden-import/Sky");
    }
    tick_until([&] {
      const auto s = host.Snapshot();
      return !s.launch || (s.launch->state != LaunchState::kReadyToHandoff && s.launch->state != LaunchState::kLaunching &&
                           s.launch->state != LaunchState::kRunning);
    });

    // A link out of the game's folder: Play stops and says which.
    fs::create_directories(fixture.root / "outside");
    CHECK(symlink("../../../outside", (sky / "Data/out").c_str()) == 0);
    const auto before = launched.size();
    // The synthetic title never starts, so that session ended with a "Can't play" of its own.
    tick_until([&] { return !model.state().navigation.modals.empty(); });
    CHECK(model.action("focus:alert-ok") && model.action("cross"));
    open_details(model, game);
    CHECK(model.action("focus:details-play") && model.action("cross"));
    tick_until([&] {
      const auto& modals = model.state().navigation.modals;
      if (modals.empty()) return false;
      if (modals.back().body.find("/data/playden-import/Sky/Data/out") != std::string::npos)
        return modals.back().title == "Can't play";
      // The earlier session's own "could not start" may still come up first.
      CHECK(model.action("focus:alert-ok") && model.action("cross"));
      return false;
    });
    CHECK(launched.size() == before);
    CHECK(unlink((sky / "Data/out").c_str()) == 0);
    const auto dismiss = [&] {
      for (int i = 0; i < 8 && !model.state().navigation.modals.empty(); ++i)
        if (!(model.action("focus:alert-ok") && model.action("cross"))) model.action("circle");
      CHECK(model.state().navigation.modals.empty());
    };
    dismiss();

    // Recreate bottle prepares the game again only once its files are opened.
    CHECK(chmod((sky / "Data/sky.ini").c_str(), 0644) == 0);
    const auto opens_before = drop_opens.load();
    open_details(model, game);
    CHECK(model.action("focus:details-more") && model.action("cross"));
    CHECK(model.action("focus:game-maintenance") && model.action("cross"));
    // The sheet asks whether the game runs first.
    tick_until([&] { const auto* row = row_of(model, "maintenance-recreate"); return row && row->enabled; });
    CHECK(model.action("focus:maintenance-recreate") && model.action("cross"));
    CHECK(model.action("focus:confirm-destructive") && model.action("cross"));
    tick_until([&] {
      const auto found = model.state().runtime_games.find(game);
      return found != model.state().runtime_games.end() && found->second.maintenance == hb::MaintenanceKind::RecreateBottle &&
             !found->second.maintenance_running;
    });
    {
      const auto& recreated = model.state().runtime_games.at(game);
      if (!recreated.maintenance_ok) std::fprintf(stderr, "Recreate: %s\n", recreated.maintenance_message.c_str());
      CHECK(recreated.maintenance_ok);
    }
    CHECK(drop_opens.load() > opens_before && Mode(sky / "Data/sky.ini") == 0666);
    dismiss();

    // An opening with no outcome in time stops Play with a sentence instead of waiting forever.
    for (int i = 0; i < 8 && model.state().navigation.route != hb::Route::Home; ++i) model.action("circle");
    hold_opens = true;
    controller.SetDropFolderOpenTimeout(std::chrono::milliseconds(200));
    const auto plays_before = launched.size();
    open_details(model, game);
    CHECK(model.action("focus:details-play") && model.action("cross"));
    tick_until([&] {
      const auto& modals = model.state().navigation.modals;
      if (modals.empty()) return false;
      if (modals.back().body.find("Playden couldn't open the game's folder in time.") != std::string::npos) return true;
      // The recreated title never starts either: its own "Can't play" may come up first.
      CHECK(model.action("focus:alert-ok") && model.action("cross"));
      return false;
    });
    hold_opens = false;
    controller.SetDropFolderOpenTimeout(std::chrono::seconds(60));
    for (int i = 0; i < 50; ++i) { controller.Tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CHECK(launched.size() == plays_before);
  });

  // A newer kit with 120 Hz output: Sky's title keeps the older runtime until it is updated. 120 Hz output and Frame
  // limit 120 chosen now wait for that; Maintenance › Update runtime moves the title to the kit's runtime without
  // starting the game, and publishes them.
  kit = SyntheticKit(fixture.root / "kit-display", "native-2", "kit-1", 'c');
  kit.capabilities.push_back("display-120hz");
  std::sort(kit.capabilities.begin(), kit.capabilities.end());
  run(false, [&](hb::Simulation& model, hb::Controller& controller, RuntimeHost& host, local::LocalLibrarySource& source,
                 const std::function<void(const std::function<bool()>&)>& tick_until) {
    std::string sky_id;
    for (const auto& imported : source.Snapshot()->games)
      if (imported.volume == "internal") sky_id = imported.id;
    CHECK(!sky_id.empty());
    const auto game = GameId::Local(sky_id);
    const auto runtime_of = [&]() -> const hb::LiveRuntimeGame* {
      const auto found = model.state().runtime_games.find(game);
      return found == model.state().runtime_games.end() ? nullptr : &found->second;
    };
    tick_until([&] {
      const auto* runtime = runtime_of();
      const auto snapshot = host.Snapshot();
      return game_of(model, game) && runtime && runtime->status == "Kit update available" && !runtime->runtime_current &&
             (!snapshot.launch || (snapshot.launch->state != LaunchState::kLaunching &&
                                   snapshot.launch->state != LaunchState::kRunning));
    });
    fs::path profile;
    for (const auto& entry : fs::recursive_directory_iterator(fixture.root / "data/playden/games"))
      if (entry.path().extension() == ".profile" && entry.path().string().find(sky_id) != std::string::npos) profile = entry.path();
    CHECK(!profile.empty());
    const auto plays_before = launched.size();
    open_details(model, game);
    CHECK(model.action("focus:details-more") && model.action("cross"));
    CHECK(model.action("focus:game-settings") && model.action("cross"));
    CHECK(model.action("focus:section-compatibility") && model.action("cross"));
    // Sky's recipe leaves Graphics automatic; 120 Hz output is for DXVK and OpenGL.
    tick_until([&] { const auto* row = row_of(model, "setting-graphics"); return row && row->enabled; });
    CHECK(model.action("focus:setting-graphics") && model.action("cross"));
    CHECK(model.action("focus:choice-2") && model.action("cross"));
    tick_until([&] { const auto* row = row_of(model, "setting-refresh"); return row && row->enabled; });
    CHECK(row_of(model, "setting-refresh")->subtitle.rfind(kAppliesAfterKitUpdate, 0) == 0);
    CHECK(model.action("focus:setting-refresh") && model.action("cross"));
    CHECK(model.action("focus:choice-1") && model.action("cross"));
    CHECK(model.action("focus:setting-frame_limit") && model.action("cross"));
    CHECK(model.action("focus:choice-1") && model.action("cross"));
    CHECK(model.state().game_settings.at(game).overrides.at("refresh") == "120" &&
          model.state().game_settings.at(game).overrides.at("frame_limit") == "120");
    CHECK(model.action("circle"));  // closing the sheet applies the settings
    tick_until([&] {
      const auto* runtime = runtime_of();
      return runtime && runtime->maintenance == hb::MaintenanceKind::ApplySettings && !runtime->maintenance_running &&
             runtime->settings_revision == model.state().game_settings.at(game).revision;
    });
    CHECK(Read(profile).find("frame_rate") == std::string::npos && Read(profile).find("refresh = 120") == std::string::npos);

    // Not while the game runs.
    fixture.activity->store(static_cast<int>(NativeActivity::kActive));
    CHECK(model.action("focus:game-maintenance") && model.action("cross"));
    tick_until([&] {
      const auto* row = row_of(model, "maintenance-update");
      return row && !row->enabled && row->subtitle == "Quit Sky to change this.";
    });
    CHECK(model.action("circle"));
    fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
    CHECK(model.action("focus:game-maintenance") && model.action("cross"));
    tick_until([&] { const auto* row = row_of(model, "maintenance-update"); return row && row->enabled; });
    CHECK(model.action("focus:maintenance-update") && model.action("cross"));
    CHECK(model.action("focus:confirm-destructive") && model.action("cross"));
    tick_until([&] {
      const auto* runtime = runtime_of();
      return runtime && runtime->maintenance == hb::MaintenanceKind::UpdateRuntime && !runtime->maintenance_running;
    });
    {
      const auto& updated = *runtime_of();
      if (!updated.maintenance_ok) std::fprintf(stderr, "Update runtime: %s\n", updated.maintenance_message.c_str());
      CHECK(updated.maintenance_ok && updated.maintenance_message == "Runtime updated.");
      CHECK(updated.runtime_current && updated.status != "Kit update available");
    }
    CHECK(Read(profile).find("\nrefresh = 120\nframe_rate = 120\n") != std::string::npos);
    const auto* update = row_of(model, "maintenance-update");
    CHECK(update && !update->enabled && update->subtitle == "Runtime updated.");
    // The game does not start.
    for (int i = 0; i < 50; ++i) { controller.Tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CHECK(launched.size() == plays_before);
  });
}

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_runtime_dir = argv[1];
  ExternalGameRegistersAfterRestartWithoutCloud();
  ImportedGamePreparesWithoutSteam();
  FirstPrepareOfAnImportedGameNeverWaitsForTheWorker();
  ImportedGamePlaysInPlace();
  std::puts("installed locations tests passed");
  return 0;
}
