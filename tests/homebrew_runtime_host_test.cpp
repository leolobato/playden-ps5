#include "homebrew/runtime_host.hpp"
#include "installs/install_record.h"
#include "runtime_host_fixture.h"
#include "msi_fixture.h"
#include "platform/managed_directory.h"
#include "platform/ps5/title_allow_list.h"
#include "platform/storage.h"
#include "runtime/layout_migration.h"
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
#include <future>
#include <memory>
#include <unistd.h>

using namespace playden::homebrew;
using namespace playden::runtime;
using namespace playden::ps5;
using namespace playden::test::runtime_host;

namespace fs = std::filesystem;
namespace {
RuntimePrepareRequest Hl2() { return {220, "/data/homebrew/FAKE77031/games/220", "internal", "Half-Life \"2\""}; }

RuntimeInstall Record() {
  RuntimeInstall record;
  record.install_id = "steam-220"; record.runtime_number = 220; record.default_profile_id = "playden-steam-220";
  record.runtime_bundle_id = "wine-wow64"; record.recipe_id = "steam-220-hl2-v1"; record.recipe_version = "1";
  record.launch_entry_id = "hl2"; record.launch_path = "hl2.exe";
  record.install_path = "/data/homebrew/FAKE77031/games/220"; record.volume_id = "internal";
  return record;
}

// A kit before 0.10: game folders must be the app user's and 0700/0755, except on drives
// that record no owners, which it can't start games from.
RuntimeKit WithoutUnownedRoots(RuntimeKit kit) {
  kit.capabilities.erase(std::remove(kit.capabilities.begin(), kit.capabilities.end(), "game-root-unowned"),
                         kit.capabilities.end());
  return kit;
}
// The same kit with one shell for every game: its own param.json names the template title.
RuntimeKit AnyTitleKit(const fs::path& kit) {
  auto admitted = SyntheticKit(kit);
  const auto param = kit / "titles/variant/sce_sys/param.json";
  Write(param,
        "{\n  \"conceptId\": \"78000\",\n  \"contentId\": \"UP9000-FAKE78000_00-PROSPEROWIN00001\",\n"
        "  \"localizedParameters\": {\n    \"en-US\": {\n      \"titleName\": \"Prospero Win\"\n    }\n  },\n"
        "  \"titleId\": \"FAKE78000\"\n}\n", 0644);
  for (auto& file : admitted.view.files)
    if (file.path == "titles/variant/sce_sys/param.json") {
      file.size = fs::file_size(param);
      file.sha256 = *HashArtifactFile(param);
    }
  admitted.view.variants = {{"any-title", "FAKE78000", "titles/variant", true}};
  admitted.capabilities.push_back("title-any-id");
  return admitted;
}
}  // namespace

// One shell for every game: the game gets a new title of its own (the lowest free FAKE78xxx),
// stamped with its own title ID, and its helper's allow-list names it before the title exists.
// Without a way to write that list nothing is prepared.
void AnyTitleKitMakesTheGameItsOwnTitle() {
  for (const int scenario : {0, 1, 2}) {
    const bool writer = scenario != 0;
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
    const auto kit = AnyTitleKit(fixture.root / "kit");
    config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    unsigned uuids = 0;
    config.uuid = [&uuids]() -> std::optional<std::string> {
      ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
    };
    config.unix_time = [] { return std::int64_t{1760000000}; };
    std::vector<std::string> lists;
    if (writer)
      config.write_title_allow_list = [&lists, scenario](const std::string& contents, std::string*) {
        lists.push_back(contents);
        return scenario != 2 || lists.size() != 2;
      };
    Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) ||
          (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (scenario == 2) {
      CHECK(done.refusals.count(220));
      host.Prepare(Hl2());
      done = WaitFor(host, [](const RuntimeSnapshot& s) {
        return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
      }, 60);
    }
    if (!writer) {
      CHECK(done.refusals.count(220) && done.refusals.at(220) == "This console can't let new PS5 titles start.");
      CHECK(!fs::exists(fixture.root / "data/homebrew/FAKE78001"));
      host.Shutdown();
      continue;
    }
    if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
    CHECK(!done.refusals.count(220) && done.installs.size() == 1);
    CHECK(done.installs[0].state == PreparationState::kReady && done.installs[0].title_id == "FAKE78001");
    // Startup wrote the (empty) list; the reservation listed the new title before staging.
    CHECK(lists.size() == (scenario == 2 ? 3u : 2u) && lists[0].empty() && lists.back() == "FAKE78001\n");
    const auto param = Read(fixture.root / "data/homebrew/FAKE78001/sce_sys/param.json");
    CHECK(param.find(R"("titleId": "FAKE78001")") != std::string::npos);
    CHECK(param.find(R"("conceptId": "78001")") != std::string::npos);
    CHECK(param.find(R"("contentId": "UP9000-FAKE78001_00-PROSPEROWIN00001")") != std::string::npos);
    CHECK(param.find("FAKE78000") == std::string::npos && param.find(R"("titleName": "Half-Life \"2\"")") != std::string::npos);
    CHECK(Read(fixture.root / "data/homebrew/FAKE78001/eboot.bin") == "native");
    CHECK(done.installs[0].title_current && done.installs[0].admission == "ok");
    host.Shutdown();
    std::atomic<bool> list_writable{false};
    std::atomic<unsigned> retries{0};
    config.write_title_allow_list = [&](const std::string&, std::string*) { ++retries; return list_writable.load(); };
    RuntimeHost restarted(config);
    CHECK(restarted.Start());
    WaitFor(restarted, Started);
    restarted.Prepare(Hl2());
    const auto refused = WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.refusals.count(220); });
    CHECK(retries >= 2);
    CHECK(refused.refusals.at(220) != "This game is already prepared.");
    list_writable = true;
    restarted.Prepare(Hl2());
    WaitFor(restarted, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && s.refusals.count(220) && s.refusals.at(220) == "This game is already prepared.";
    });
    restarted.Shutdown();
  }
}

// Without an admitted kit, the composed services start, report why preparation
// is unavailable and refuse Prepare without creating durable records.
void NoKitRefusesPreparationWithoutDurableEffects() {
  Fixture fixture;
  RuntimeHost host(fixture.Config());
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.status == RuntimeHostStatus::kReady);
  CHECK(!started.kit_available && started.kit_reason == "No verified runtime kit is installed.");
  CHECK(started.installs.empty());
  struct stat info{};
  CHECK(stat((fixture.root / "data/runtime-state").c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
  host.Prepare(Hl2());
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(refused.refusals.at(220) == "No verified runtime kit is installed.");
  host.Shutdown();
  CHECK(host.Snapshot().status == RuntimeHostStatus::kStopped);
  CHECK(!fs::exists(fixture.root / "data/runtime-state/installs.json"));
  CHECK(fixture.registry_mutations == 0);
  CHECK(fs::is_empty(fixture.root / "data/homebrew"));
}

// A kit put in place while Playden runs (the first-run kit download) is admitted by ReloadKit: the
// services start again with it, also over a kit already in use (one this version wasn't made for).
void ReloadKitAdmitsAKitInstalledWhileRunning() {
  Fixture fixture;
  auto config = fixture.Config();
  const auto kit = AnyTitleKit(fixture.root / "kit");
  std::atomic<bool> installed{false};
  std::atomic<int> admissions{0};
  config.admit_kit = [&](std::string* reason) -> std::optional<RuntimeKit> {
    ++admissions;
    if (installed) return kit;
    *reason = "No verified runtime kit is installed.";
    return std::nullopt;
  };
  RuntimeHost host(std::move(config));
  CHECK(host.Start());
  CHECK(!WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  installed = true;
  host.ReloadKit();
  const auto reloaded = WaitFor(host, [](const RuntimeSnapshot& s) { return Started(s) && s.kit_available; });
  CHECK(reloaded.status == RuntimeHostStatus::kReady && reloaded.kit_reason.empty());
  CHECK(reloaded.refusals.empty());  // the refusal for want of a kit is gone
  CHECK(admissions == 2);
  // Preparation goes past the kit check now (this fixture has no game folder to prepare).
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.prepares_handled == 2; }, 60);
  CHECK(!done.refusals.count(220) || done.refusals.at(220) != "No verified runtime kit is installed.");
  host.ReloadKit();
  WaitFor(host, [&](const RuntimeSnapshot& s) { return Started(s) && admissions == 3; });
  CHECK(host.Snapshot().kit_available);
  host.Shutdown();
  CHECK(host.Snapshot().status == RuntimeHostStatus::kStopped);
}

// Root is process-wide on the console: while the worker starts up or works through a batch it holds
// the effective UID, so a root raise elsewhere (an update's cleanup) can't fail its ownership checks.
// Waiting for work, it holds nothing.
void HoldsEffectiveUidWhileWorking() {
  Fixture fixture;
  auto config = fixture.Config();
  std::atomic<int> holds{0};
  std::atomic<bool> admitted_while_held{false};
  config.hold_effective_uid = [&holds]() -> std::shared_ptr<void> {
    ++holds;
    return std::shared_ptr<void>(nullptr, [&holds](void*) { --holds; });
  };
  config.admit_kit = [&](std::string* reason) -> std::optional<RuntimeKit> {
    admitted_while_held = holds > 0;
    *reason = "No verified runtime kit is installed.";
    return std::nullopt;
  };
  RuntimeHost host(std::move(config));
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).status == RuntimeHostStatus::kReady);
  CHECK(admitted_while_held);
  host.Prepare(Hl2());
  CHECK(WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; }).refusals.count(220));
  for (int i = 0; i < 200 && holds != 0; ++i) usleep(5000);
  CHECK(holds == 0);  // idle: root may be raised again
  host.Shutdown();
}

// A preparation asked for before the runtime is ready is published as queued until the worker
// takes it, so Play and Recreate bottle wait instead of reporting it lost.
void QueuedPreparationIsPublishedUntilTaken() {
  Fixture fixture;
  RuntimeHost host(fixture.Config());
  host.Prepare(Hl2());
  CHECK(host.Snapshot().queued_prepares == std::vector<std::uint32_t>{220});
  CHECK(host.Start());
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(refused.queued_prepares.empty() && !refused.preparing_app);
  host.Shutdown();
}

RuntimePrepareRequest Game(std::uint32_t app) {
  return {app, "/data/homebrew/FAKE77031/games/" + std::to_string(app), "internal", "Game " + std::to_string(app)};
}

// The order the worker finished preparations in, from its log.
struct PrepareLog {
  std::mutex mutex;
  std::vector<std::uint32_t> order;
  void Attach(RuntimeHostConfig& config) {
    config.log = [this](const std::string& line) {
      constexpr const char* kResult = "prepare result app=";
      if (line.rfind(kResult, 0) != 0) return;
      std::lock_guard<std::mutex> lock(mutex);
      order.push_back(static_cast<std::uint32_t>(std::stoul(line.substr(std::strlen(kResult)))));
    };
  }
  std::vector<std::uint32_t> Order() {
    std::lock_guard<std::mutex> lock(mutex);
    return order;
  }
};

// Preparations run one at a time in the order asked. Prepare next moves a waiting game to the front even
// after the worker began a pass with every request waiting; the others keep their order. Asking again
// for a queued game keeps its place, and a game that is not queued is left alone.
void PrepareNextReordersWaitingPreparations() {
  Fixture fixture;
  auto config = fixture.Config();
  PrepareLog log;
  log.Attach(config);
  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool block = false;
  bool blocked = false;
  config.hold_effective_uid = [&]() -> std::shared_ptr<void> {
    std::unique_lock<std::mutex> lock(gate_mutex);
    if (block) {
      blocked = true;
      gate_changed.notify_all();
      gate_changed.wait(lock, [&] { return !block; });
    }
    return nullptr;
  };
  config.admit_kit = [](std::string* reason) -> std::optional<RuntimeKit> {
    *reason = "No verified runtime kit is installed.";
    return std::nullopt;
  };
  RuntimeHost host(std::move(config));
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).status == RuntimeHostStatus::kReady);
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    block = true;
  }
  host.Prepare(Game(10));
  host.Prepare(Game(20));
  host.Prepare(Game(30));
  host.Prepare(Game(40));
  host.Prepare(Game(20));
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    CHECK(gate_changed.wait_for(lock, std::chrono::seconds(5), [&] { return blocked; }));
  }
  CHECK((host.Snapshot().queued_prepares == std::vector<std::uint32_t>{10, 20, 30, 40}));
  CHECK(host.PrepareNext(30));
  CHECK(host.PrepareNext(30));  // already first: nothing moves
  CHECK(!host.PrepareNext(99));
  CHECK((host.Snapshot().queued_prepares == std::vector<std::uint32_t>{30, 10, 20, 40}));
  // An uninstall takes a waiting game out; the others keep their order.
  CHECK(host.WithdrawPrepare(20) && !host.WithdrawPrepare(20));
  CHECK((host.Snapshot().queued_prepares == std::vector<std::uint32_t>{30, 10, 40}));
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    block = false;
  }
  gate_changed.notify_all();
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.prepares_handled == 3; });
  CHECK(done.queued_prepares.empty() && !done.preparing_app);
  CHECK((log.Order() == std::vector<std::uint32_t>{30, 10, 40}));
  CHECK(!host.PrepareNext(30));  // handled: no longer queued
  host.Shutdown();
}

// A quit to Home holds the queue: queued preparations wait until the hold is released.
void HeldPreparationsWait() {
  Fixture fixture;
  RuntimeHost host(fixture.Config());
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).status == RuntimeHostStatus::kReady);
  host.HoldPreparations(true);
  host.Prepare(Hl2());
  usleep(100000);
  const auto held = host.Snapshot();
  CHECK(held.prepares_handled == 0 && held.queued_prepares == std::vector<std::uint32_t>{220});
  host.HoldPreparations(false);
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(refused.queued_prepares.empty());
  host.Shutdown();
}

// Startup reads durable records only. A journaled record is reported as needing
// recovery; it is not resumed, re-registered or rewritten.
void StartupReportsJournalsWithoutResuming() {
  Fixture fixture;
  auto config = fixture.Config();
  fs::create_directories(config.state_root); CHECK(chmod(config.state_root.c_str(), 0700) == 0);
  {
    playden::Storage storage(config.state_root.string());
    RuntimeInstallRepository repository(storage, "installs.json", [](const std::string&) {
      return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree};
    });
    CHECK(repository.Load().ok());
    const auto record = Record();
    ReserveRequest request;
    request.install_id = record.install_id; request.runtime_number = record.runtime_number;
    request.default_profile_id = record.default_profile_id; request.runtime_bundle_id = record.runtime_bundle_id;
    request.recipe_id = record.recipe_id; request.recipe_version = record.recipe_version;
    request.launch_entry_id = record.launch_entry_id; request.launch_path = record.launch_path;
    request.install_path = record.install_path; request.volume_id = record.volume_id;
    request.kit_title_variants = {"FAKE78001"};
    auto reserved = repository.Reserve(request); CHECK(reserved.ok());
    auto journaled = reserved.value();
    journaled.state = PreparationState::kRegistering;
    journaled.journal = PreparationJournal{"11111111-2222-4333-8444-555555555555", "settings-publication", "register"};
    CHECK(repository.Update(journaled).ok());
  }
  const auto before = Read(config.state_root / "installs.json");
  CHECK(!before.empty());
  const auto database_reads_before = fixture.database_reads;
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.status == RuntimeHostStatus::kReady && started.installs.size() == 1);
  const auto& view = started.installs[0];
  CHECK(view.runtime_number == 220 && view.title_id == "FAKE78001" && view.state == PreparationState::kRegistering);
  CHECK(view.recovery_pending);
  host.Shutdown();
  CHECK(Read(config.state_root / "installs.json") == before);
  CHECK(fixture.registry_mutations == 0 && fixture.database_reads == database_reads_before);
}

// A game from another store gets a runtime number from the reserved range on first use, from any
// thread; its preparation is asked for with that number, and the snapshot names the game back.
// Steam games keep their app ID and add nothing to installs.json.
void LocalGameGetsRuntimeNumber() {
  const auto local_a = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
  const auto local_b = playden::GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
  Fixture fixture;
  auto config = fixture.Config();
  // A Local game with a record from an earlier run: its view carries the game.
  fs::create_directories(config.state_root); CHECK(chmod(config.state_root.c_str(), 0700) == 0);
  {
    playden::Storage storage(config.state_root.string());
    RuntimeInstallRepository repository(storage, "installs.json", [](const std::string&) {
      return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree};
    });
    CHECK(repository.Load().ok());
    const auto number = repository.RuntimeNumberFor(local_a);
    CHECK(number.ok() && number.value() == 0xF0000000u);
    const auto record = Record();
    ReserveRequest request;
    request.install_id = playden::ToInstallId(local_a); request.runtime_number = number.value(); request.game = local_a;
    request.default_profile_id = playden::ProfileId(local_a); request.runtime_bundle_id = record.runtime_bundle_id;
    request.recipe_id = playden::GeneratedRecipeId(local_a); request.recipe_version = record.recipe_version;
    request.launch_entry_id = record.launch_entry_id; request.launch_path = record.launch_path;
    request.install_path = "/data/playden-import/Game"; request.volume_id = record.volume_id;
    request.kit_title_variants = {"FAKE78001"};
    CHECK(repository.Reserve(request).ok());
  }
  std::mutex log_mutex;
  std::vector<std::string> lines;
  config.log = [&](const std::string& line) { std::lock_guard<std::mutex> lock(log_mutex); lines.push_back(line); };
  RuntimeHost host(config);
  CHECK(!host.RuntimeNumberFor(local_b));  // not started: no records to allocate from
  CHECK(host.RuntimeNumberFor(playden::GameId::Steam(220)) == 220u);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.status == RuntimeHostStatus::kReady && started.installs.size() == 1);
  CHECK(started.installs[0].game == local_a && started.installs[0].runtime_number == 0xF0000000u);
  CHECK(started.runtime_games.at(0xF0000000u) == local_a);
  CHECK(host.RuntimeNumberFor(local_a) == 0xF0000000u);
  CHECK(host.RuntimeNumberFor(playden::GameId::Steam(220)) == 220u);

  // Several threads asking at once: one number per game, never one number for two games.
  std::vector<std::thread> threads;
  std::vector<std::optional<std::uint32_t>> answers(8);
  for (std::size_t i = 0; i < answers.size(); ++i)
    threads.emplace_back([&, i] { answers[i] = host.RuntimeNumberFor(i % 2 ? local_b : local_a); });
  for (auto& thread : threads) thread.join();
  for (std::size_t i = 0; i < answers.size(); ++i)
    CHECK(answers[i] == (i % 2 ? 0xF0000001u : 0xF0000000u));
  const auto allocated = host.Snapshot();
  CHECK(allocated.runtime_games.at(0xF0000001u) == local_b && !allocated.runtime_games.count(220));
  CHECK(Read(config.state_root / "installs.json").find("\"next_runtime_number\":4026531842") != std::string::npos);

  // The request reaches the worker with its number; the refusal maps back to the game.
  RuntimePrepareRequest prepare{*host.RuntimeNumberFor(local_b), "/data/playden-import/Other", "internal", "Other"};
  prepare.game = local_b;
  CHECK(prepare.runtime_number >= 0xF0000000u);
  host.Prepare(prepare);
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(0xF0000001u) != 0; });
  CHECK(refused.runtime_games.at(0xF0000001u) == local_b);
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    CHECK(std::find(lines.begin(), lines.end(), "runtime number for local-7c9e6679-7425-40de-944b-e07fc1f90ae7 is 4026531841") !=
          lines.end());
  }
  host.Shutdown();
  CHECK(!host.RuntimeNumberFor(playden::GameId::Local("9a1d1f2e-0000-4000-8000-000000000001")));
}

// A public or foreign state root is refused rather than taken over.
void PublicStateRootIsUnavailable() {
  Fixture fixture;
  auto config = fixture.Config();
  fs::create_directories(config.state_root); CHECK(chmod(config.state_root.c_str(), 0755) == 0);
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.status == RuntimeHostStatus::kUnavailable && !started.reason.empty());
  host.Prepare(Hl2());
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(refused.refusals.at(220) == started.reason);
  host.Shutdown();
  struct stat info{};
  CHECK(stat(config.state_root.c_str(), &info) == 0 && (info.st_mode & 0777) == 0755);
}

// ShadowMountPlus registers a submitted title only once no title runs (Playden
// included): preparation ends as soon as the registration is submitted, with the
// game waiting for the console and no refusal. A later check only asks the console:
// once the title is registered it becomes Ready, never submitted again.
void PreparationEndsAwaitingRegistration() {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  const auto began = std::chrono::steady_clock::now();
  host.Prepare(Hl2());
  const auto waiting = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (s.refusals.count(220) || s.installs[0].awaiting_registration);
  }, 60);
  if (waiting.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", waiting.refusals.at(220).c_str());
  CHECK(!waiting.refusals.count(220) && waiting.installs[0].awaiting_registration);
  CHECK(waiting.installs[0].state != PreparationState::kReady && waiting.installs[0].recovery_pending);
  CHECK(waiting.prepared.empty());  // not done until the PS5 registers it
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(20));  // no polling for the console
  const auto submissions = fixture.registry_mutations;
  CHECK(submissions > 0 && fixture.submitted && !fixture.registered);

  // Asked again while the console still has not registered it: still waiting, nothing resubmitted.
  host.Prepare(Hl2());
  auto again = WaitFor(host, [](const RuntimeSnapshot& s) { return s.preparing_app.has_value(); });
  again = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app; }, 30);
  CHECK(!again.refusals.count(220) && again.installs[0].awaiting_registration);
  CHECK(fixture.registry_mutations == submissions);

  // The player left Playden and ShadowMountPlus registered it: the check finds it Ready.
  fixture.registered = true;
  host.Prepare(Hl2());
  const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 30);
  CHECK(!ready.refusals.count(220) && ready.installs[0].state == PreparationState::kReady);
  CHECK(!ready.installs[0].awaiting_registration && !ready.installs[0].recovery_pending);
  CHECK(fixture.registry_mutations == submissions);
  // Registered: now it is listed as done, and Clear completed takes it off.
  const auto listed = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.prepared.empty(); });
  CHECK(listed.prepared.size() == 1 && listed.prepared[0].runtime_number == 220);
  host.ClearPrepared();
  CHECK(WaitFor(host, [](const RuntimeSnapshot& s) { return s.prepared.empty(); }).prepared.empty());
  host.Shutdown();
}

// An existing download is prepared in place: the host stages its own title,
// copies a private bottle, publishes settings through OWNER, registers once and
// commits Ready. Game files, DLLs and saves are preserved byte for byte.
void PreparesExistingDownloadToReady() {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.title_icon = [](const playden::GameId& game) -> std::optional<std::string> {
    return game == playden::GameId::Steam(220) ? std::optional<std::string>("game-icon-png") : std::nullopt;
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  Write(game / "bin/steam_api.dll", "unchanged-original-dll", 0644);
  Write(game / "hl2/save/quick.sav", "existing-player-save", 0644);
  CHECK(chmod(game.c_str(), 0777) == 0);  // console downloads are world-writable

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
  if (done.refusals.count(220)) std::fprintf(stderr, "refusal: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220));
  CHECK(done.installs.size() == 1);
  const auto& ready = done.installs[0];
  CHECK(ready.state == PreparationState::kReady && ready.title_id == "FAKE78001" && !ready.recovery_pending);
  CHECK(fixture.registry_mutations == 2 && uuids == 1);
  // Game files stay at the recorded download root and are untouched.
  CHECK(Read(game / "hl2.exe") == "existing-game");
  CHECK(Read(game / "bin/steam_api.dll") == "unchanged-original-dll");
  CHECK(Read(game / "hl2/save/quick.sav") == "existing-player-save");
  CHECK(!fs::exists(fixture.root / "data/homebrew/FAKE78001/games"));
  // The game folder stays world-writable (mods over FTP): the kit admits it as it is.
  struct stat game_info{};
  CHECK(stat(game.c_str(), &game_info) == 0 && (game_info.st_mode & 07777) == 0777);
  const auto metadata = Read(fixture.root / "data/playden/games/steam-220/library/profiles/.playden-publication");
  CHECK(metadata.find("schema=3\n") != std::string::npos && metadata.find("game_root.unowned=1\n") != std::string::npos);
  // The staged title is readable by ShadowMount but not writable by others.
  struct stat title_info{};
  for (const auto* relative : {"data/homebrew/FAKE78001", "data/homebrew/FAKE78001/sce_sys"})
    CHECK(stat((fixture.root / relative).c_str(), &title_info) == 0 && (title_info.st_mode & 07777) == 0755);
  // The PS5 title shows the game's own name and icon, not the kit's.
  const auto param = Read(fixture.root / "data/homebrew/FAKE78001/sce_sys/param.json");
  CHECK(param.find(R"("titleName": "Half-Life \"2\"")") != std::string::npos);
  CHECK(param.find("Prospero Win") == std::string::npos && param.find("FAKE78001") != std::string::npos);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/sce_sys/icon0.png") == "game-icon-png");
  CHECK(stat((fixture.root / "data/homebrew/FAKE78001/sce_sys/icon0.png").c_str(), &title_info) == 0 &&
        (title_info.st_mode & 07777) == 0644);
  CHECK(done.installs[0].title_current);
  // The descriptor binds the existing root; the bottle is private.
  const auto descriptor = Read(fixture.root / "data/homebrew/FAKE78001/game-launch.json");
  CHECK(descriptor.find(R"("schema_version":2)") != std::string::npos);
  CHECK(descriptor.find(R"("game_root":"/data/homebrew/FAKE77031/games/220")") != std::string::npos);
  struct stat info{};
  CHECK(stat((fixture.root / "data/playden/games/steam-220/library/prefixes/game").c_str(), &info) == 0 &&
        (info.st_mode & 0777) == 0700);
  // The finished publication's request is no longer named by any journal: an
  // explicit cleanup removes it.
  const auto request = fixture.root / "data/runtime-state/requests/settings-request-00000000-0000-4000-8000-000000000001.v1";
  CHECK(fs::is_regular_file(request));
  CHECK(WaitFor(host, [](const RuntimeSnapshot& s) { return s.leftover_requests == 1; }).leftover_requests == 1);
  host.CleanUpRequests();
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.leftover_requests == 0; });
  CHECK(!fs::exists(request));
  // A second request does not redo anything.
  host.Prepare(Hl2());
  const auto again = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
  CHECK(again.refusals.at(220) == "This game is already prepared.");
  CHECK(fixture.registry_mutations == 2);
  // Queue lists the finished preparation once; the refused second request adds nothing.
  CHECK((again.prepared == std::vector<PreparedGame>{{220, 1760000000}}));
  host.Shutdown();

  // The list survives a restart. A dismissal names its finish time; another time leaves it listed.
  {
    RuntimeHost restarted(config);
    CHECK(restarted.Start());
    // The list is published after the status turns ready: wait for it, not for the status alone.
    const std::vector<PreparedGame> listed{{220, 1760000000}};
    CHECK(WaitFor(restarted, [&](const RuntimeSnapshot& s) { return Started(s) && s.prepared == listed; }, 60).prepared == listed);
    restarted.DismissPrepared(220, 1759999999);
    restarted.DismissPrepared(220, 1760000000);
    CHECK(WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.prepared.empty(); }).prepared.empty());
    restarted.Shutdown();
  }
  {
    RuntimeHost restarted(config);
    CHECK(restarted.Start());
    CHECK(WaitFor(restarted, Started).prepared.empty());
    restarted.Shutdown();
  }
  // Past kPreparedDays a preparation is no longer listed.
  Write(fixture.root / "data/runtime-state/prepared-history", "220\t1760000000\n", 0600);
  config.unix_time = [] { return std::int64_t{1760000000} + kPreparedDays * 24 * 60 * 60 + 1; };
  {
    RuntimeHost restarted(config);
    CHECK(restarted.Start());
    CHECK(WaitFor(restarted, Started).prepared.empty());
    restarted.Shutdown();
  }
}

// A folder made over FTP belongs to root and is 0755, so the app user can't write into it
// (#70): Prepare makes it 0777 as root first. Here 0555 stands in for it.
RuntimeHostConfig WritableFoldersConfig(Fixture& fixture, const RuntimeKit& kit) {
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
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  config.uuid = [] { return std::optional<std::string>("00000000-0000-4000-8000-000000000001"); };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.writable_folders = {"/data/homebrew", "/data/playden", "/data/playden-games", "/data/playden/user",
                             "/data/playden/logs"};
  return config;
}

void PrepareFixesRootOwnedHomebrewFolder() {
  if (geteuid() == 0) return;  // root writes anywhere
  Fixture fixture;
  const auto kit = SyntheticKit(fixture.root / "kit");
  auto config = WritableFoldersConfig(fixture, kit);
  std::atomic<int> raises{0};
  config.as_root = [&raises](const std::function<bool()>& work) { ++raises; return work(); };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  const auto homebrew = fixture.root / "data/homebrew";
  CHECK(chmod(homebrew.c_str(), 0555) == 0);

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
  if (done.refusals.count(220)) std::fprintf(stderr, "refusal: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220));
  CHECK(done.installs.size() == 1 && done.installs[0].state == PreparationState::kReady);
  CHECK(raises == 1);  // only the folder that failed its probe
  struct stat info{};
  CHECK(stat(homebrew.c_str(), &info) == 0 && (info.st_mode & 07777) == 01777);
  CHECK(fs::is_regular_file(fixture.root / "data/homebrew/FAKE78001/game-launch.json"));
  host.Shutdown();
}

void PrepareNamesUnfixableFolder() {
  if (geteuid() == 0) return;  // root writes anywhere
  Fixture fixture;
  const auto kit = SyntheticKit(fixture.root / "kit");
  auto config = WritableFoldersConfig(fixture, kit);
  std::atomic<int> raises{0};
  config.as_root = [&raises](const std::function<bool()>&) { ++raises; return false; };  // root refused
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  const auto homebrew = fixture.root / "data/homebrew";
  CHECK(chmod(homebrew.c_str(), 0555) == 0);

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app && s.refusals.count(220); }, 60);
  CHECK(done.refusals.at(220) == "Playden can't write to " + homebrew.string() +
                                 ". Make that folder writable over FTP (777 on the folder only, not its contents), then try again.");
  CHECK(raises == 1);
  CHECK(done.installs.empty());
  struct stat info{};
  CHECK(stat(homebrew.c_str(), &info) == 0 && (info.st_mode & 07777) == 0555);
  CHECK(!fs::exists(fixture.root / "data/homebrew/FAKE78001"));
  CHECK(!fs::exists(fixture.root / "data/playden"));
  CHECK(fixture.registry_mutations == 0);
  host.Shutdown();
  CHECK(chmod(homebrew.c_str(), 0755) == 0);  // so the fixture can clean up
}

// No free title is rarely the truth: the refusal names the source that could not answer.
void TitleRefusalNamesTheSourceThatCannotAnswer() {
  // ShadowMountPlus silent, answering with bad data, app.db unreadable, and the only title truly in use.
  for (const int scenario : {0, 1, 2, 3}) {
    Fixture fixture;
    const auto kit = SyntheticKit(fixture.root / "kit");
    auto config = WritableFoldersConfig(fixture, kit);
    if (scenario == 0)
      config.registry_transport = [](const std::string&, const std::string&) -> std::optional<std::string> { return {}; };
    else if (scenario == 1)
      config.registry_transport = [](const std::string&, const std::string&) -> std::optional<std::string> {
        return std::string(R"({"status":0,"games":[{"title_id":"odd"}]})");
      };
    else
      config.native.database = [scenario](const std::string&) {
        return TitleDatabaseObservation{scenario == 2 ? TitleAvailability::kUnknown : TitleAvailability::kOccupied, {}};
      };
    Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
    config.writable_folders.clear();
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app && s.refusals.count(220); }, 60);
    const auto& refusal = done.refusals.at(220);
    if (scenario == 0)
      CHECK(refusal == "ShadowMountPlus isn't answering. Load it again (or restart the PS5 and jailbreak it again), then try again.");
    else if (scenario == 3)
      CHECK(refusal == "Every PS5 title this runtime kit offers is in use. Uninstall a game, then try again.");
    else
      CHECK(refusal == "Playden could not check that a PS5 title is free for this game. Try again; if it still fails, "
                       "open Logs in Settings for details.");
    CHECK(done.installs.empty() && fixture.registry_mutations == 0);
    host.Shutdown();
  }
}

// A capture refused after control setup (the console's world-writable game
// folder) leaves a settings-control journal. An explicit Prepare resumes the
// saved request and reaches Ready with one registration.
void ResumesInterruptedCaptureFromSavedRequest(bool tamper_backup = false) {
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
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  const auto staged = fixture.root / "data/homebrew/FAKE78001";
  unsigned uuids = 0;
  // Called just before capture: the first attempt sees a world-writable PS5 title folder,
  // which the capture's preflight refuses (game folders may be anything).
  config.uuid = [&uuids, staged]() -> std::optional<std::string> {
    if (++uuids == 1) CHECK(chmod(staged.c_str(), 0777) == 0);
    return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(game / "hl2.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto failed = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app && s.refusals.count(220); }, 60);
  CHECK(failed.installs.size() == 1 && failed.installs[0].state != PreparationState::kReady);
  CHECK(failed.installs[0].recovery_pending);
  CHECK(chmod(staged.c_str(), 0755) == 0);
  // The saved request the journal names is in use: never leftover, never removed.
  CHECK(failed.leftover_requests == 0);
  host.CleanUpRequests();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(fs::is_regular_file(fixture.root / "data/runtime-state/requests/settings-request-00000000-0000-4000-8000-000000000001.v1"));
  const auto backup = fixture.root / "data/playden/games/steam-220/.playden-control" /
                      "registry-backup-00000000-0000-4000-8000-000000000001.v1";
  CHECK(fs::is_regular_file(backup));
  if (tamper_backup) {  // Same size, different bytes: not this capture's backup.
    auto bytes = Read(backup); bytes[0] = bytes[0] == 'X' ? 'Y' : 'X';
    Write(backup, bytes, 0600);
  }
  if (!tamper_backup) {
    // While the console cannot tell whether the title runs, the resume stops before it
    // rebuilds a title folder the player deleted (ShadowMount would put it back on Home).
    CHECK(failed.installs[0].state == PreparationState::kPreparing ||
          failed.installs[0].state == PreparationState::kRegistering);
    const auto title = fixture.root / "data/homebrew/FAKE78001", aside = fixture.root / "title-aside";
    fs::rename(title, aside);
    fixture.activity->store(static_cast<int>(NativeActivity::kUnknown));
    const auto asked = host.Snapshot().revision;
    host.Prepare(Hl2());
    WaitFor(host, [asked](const RuntimeSnapshot& s) {
      return s.revision > asked && !s.preparing_app && s.refusals.count(220) &&
             s.refusals.at(220).find("Restart the PS5") != std::string::npos;
    });
    CHECK(!fs::exists(title) && fixture.registry_mutations == 0);
    fs::rename(aside, title);
    fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  }
  // Waits for this request's own answer: a refusal can come back before a poll sees the old one cleared.
  const auto before = host.Snapshot().prepares_handled;
  host.Prepare(Hl2());
  WaitFor(host, [before](const RuntimeSnapshot& s) { return s.prepares_handled > before; });
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() &&
        (s.installs[0].state == PreparationState::kReady || s.refusals.count(220));
  }, 60);
  if (tamper_backup) {
    CHECK(done.refusals.count(220) && done.refusals.at(220).find("orphan") != std::string::npos);
    CHECK(done.installs[0].state != PreparationState::kReady && fixture.registry_mutations == 0);
    host.Shutdown();
    return;
  }
  if (done.refusals.count(220)) std::fprintf(stderr, "Resume refused: %s\n", done.refusals.at(220).c_str());
  CHECK(done.installs[0].state == PreparationState::kReady && !done.installs[0].recovery_pending);
  CHECK(fixture.registry_mutations == 2 && uuids == 1);
  host.Shutdown();
}

// A respun kit with the same id, runtime and recipe (for example a new wrapper
// build) updates the installed title in place: changed files and owner markers
// only, no new registration, publication or bottle changes. A different kit id
// is refused.
void RefreshesInstalledTitleToRespunKit() {
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
  // Every publication transaction takes a UUID: counting them shows that an Update which changes nothing the
  // settings write starts none.
  auto transactions = std::make_shared<int>(0);
  config.uuid = [transactions]() -> std::optional<std::string> {
    ++*transactions;
    return std::string("00000000-0000-4000-8000-000000000001");
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  {
    const auto kit = SyntheticKit(fixture.root / "kit");
    auto first = config;
    first.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    RuntimeHost host(first);
    CHECK(host.Start());
    host.Prepare(Hl2());
    const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
    }, 60);
    CHECK(ready.installs[0].title_current);
    host.Shutdown();
  }
  const auto bottle_reg = fixture.root / "data/playden/games/steam-220/library/prefixes/game/user.reg";
  const auto registry_before = Read(bottle_reg);
  const auto installs_before = Read(fixture.root / "data/runtime-state/installs.json");
  const int transactions_before = *transactions;
  CHECK(fixture.registry_mutations == 2);
  {  // A different kit id is not a respin; it needs republication.
    const auto other = SyntheticKit(fixture.root / "kit-other", "native", "kit-2", 'b');
    auto second = config;
    second.admit_kit = [other](std::string*) { return std::optional<RuntimeKit>(other); };
    RuntimeHost host(second);
    CHECK(host.Start());
    CHECK(!WaitFor(host, Started).installs[0].title_current);
    host.Prepare(Hl2());
    const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app && s.refusals.count(220); });
    CHECK(refused.refusals.at(220).find("compatible kit") != std::string::npos);
    host.Shutdown();
  }
  const auto respun = SyntheticKit(fixture.root / "kit-respun", "native-2", "kit-1", 'c');
  auto third = config;
  third.admit_kit = [respun](std::string*) { return std::optional<RuntimeKit>(respun); };
  RuntimeHost host(third);
  CHECK(host.Start());
  CHECK(!WaitFor(host, Started).installs[0].title_current);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (s.installs[0].title_current || s.refusals.count(220));
  }, 60);
  if (done.refusals.count(220)) std::fprintf(stderr, "Refresh refused: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);
  CHECK(*transactions == transactions_before && !done.installs[0].recovery_pending);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/eboot.bin") == "native-2");
  CHECK(Read(bottle_reg) == registry_before);
  CHECK(Read(fixture.root / "data/runtime-state/installs.json") == installs_before);
  CHECK(fixture.registry_mutations == 2);
  host.Shutdown();

  // A rebuilt runtime (same artifacts, new bytes) upgrades the title in place:
  // the new runtime reaches the title, the record keeps the new hashes, the
  // registration and bottle registry stay.
  const auto rebuilt = SyntheticKit(fixture.root / "kit-rebuilt", "native-2", "kit-1", 'd', "runtime-neutral", "rev-neutral");
  auto fourth = config;
  fourth.admit_kit = [rebuilt](std::string*) { return std::optional<RuntimeKit>(rebuilt); };
  RuntimeHost upgraded(fourth);
  CHECK(upgraded.Start());
  CHECK(!WaitFor(upgraded, Started).installs[0].title_current);
  upgraded.Prepare(Hl2());
  const auto up = WaitFor(upgraded, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (s.installs[0].title_current || s.refusals.count(220));
  }, 60);
  if (up.refusals.count(220)) std::fprintf(stderr, "Upgrade refused: %s\n", up.refusals.at(220).c_str());
  CHECK(!up.refusals.count(220) && up.installs[0].state == PreparationState::kReady);
  CHECK(*transactions == transactions_before);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/win/wine/lib/core.prx") == "runtime-neutral");
  const auto upgraded_installs = Read(fixture.root / "data/runtime-state/installs.json");
  CHECK(upgraded_installs != installs_before &&
        upgraded_installs.find(*HashArtifactBytes("runtime-neutral")) != std::string::npos);
  CHECK(Read(bottle_reg) == registry_before);
  CHECK(fixture.registry_mutations == 2);
  upgraded.Shutdown();
  // A restart keeps the upgraded title current.
  RuntimeHost again(fourth);
  CHECK(again.Start());
  CHECK(WaitFor(again, Started).installs[0].title_current);
  again.Shutdown();
}

// A preparation that failed under one kit and is retried under a rebuilt kit binds
// the new kit's artifacts, so validation compares the title with the kit it came from.
void RetryAfterKitRespinBindsTheNewArtifacts() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  config.uuid = []() -> std::optional<std::string> { return std::string("00000000-0000-4000-8000-000000000001"); };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  {
    const auto kit = SyntheticKit(fixture.root / "kit");
    auto first = config;
    first.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    first.package_operations.free_bytes = [](const fs::path&) { return std::optional<std::uint64_t>(0); };
    RuntimeHost host(first);
    CHECK(host.Start());
    host.Prepare(Hl2());
    const auto failed = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kFailed;
    }, 60);
    CHECK(failed.installs[0].state == PreparationState::kFailed);
    CHECK(failed.installs[0].failure_code == "space-unavailable");
    CHECK(failed.installs[0].failure == "insufficient verified free space for package copy");
    host.Shutdown();
  }
  const auto rebuilt = SyntheticKit(fixture.root / "kit-rebuilt", "native", "kit-1", 'd', "runtime-neutral", "rev-neutral");
  auto second = config;
  second.admit_kit = [rebuilt](std::string*) { return std::optional<RuntimeKit>(rebuilt); };
  second.package_operations.free_space_unverifiable = true;
  RuntimeHost host(second);
  CHECK(host.Start());
  WaitFor(host, Started);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() &&
           (s.installs[0].state == PreparationState::kReady || s.refusals.count(220) ||
            (s.installs[0].state == PreparationState::kFailed &&
             s.installs[0].failure.find("free space") == std::string::npos));
  }, 60);
  if (done.refusals.count(220)) std::fprintf(stderr, "Retry refused: %s\n", done.refusals.at(220).c_str());
  if (done.installs[0].state != PreparationState::kReady)
    std::fprintf(stderr, "Retry state=%d step=%s failure=%s\n", static_cast<int>(done.installs[0].state),
                 done.installs[0].step.c_str(), done.installs[0].failure.c_str());
  CHECK(done.installs[0].state == PreparationState::kReady);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/win/wine/lib/core.prx") == "runtime-neutral");
  CHECK(Read(fixture.root / "data/runtime-state/installs.json").find(*HashArtifactBytes("runtime-neutral")) !=
        std::string::npos);
  host.Shutdown();
}

// A preparation that stopped after its title and bottle were made, then a kit swap: the title is the
// earlier kit's, so every Try again would fail the same way. The snapshot says so, Prepare refuses
// with the way out instead of retrying, and Recreate bottle (remove, then prepare) finishes it.
// The same stop under the installed kit is not flagged: Try again finishes that one.
void SetupFromAnEarlierKitAsksForRecreate() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  // The bottle fails its check at first: the preparation stops with the title and bottle made.
  auto bottle_valid = std::make_shared<std::atomic<bool>>(false);
  config.package_operations.validate_bottle = [bottle_valid](const fs::path& bottle) {
    return *bottle_valid && fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-0000000000") + (uuids < 10 ? "0" : "") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.removal_confirm_polls = 3;
  config.removal_poll_interval = std::chrono::milliseconds(1);
  config.unregister_title = [&fixture](const std::string&, const std::string&) {
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  const auto prepare = [](RuntimeHost& host) {
    const auto before = host.Snapshot().prepares_handled;
    host.Prepare(Hl2());
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return !s.preparing_app && s.prepares_handled > before; }, 60);
  };
  const auto kit = SyntheticKit(fixture.root / "kit");
  {
    auto first = config;
    first.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    RuntimeHost host(first);
    CHECK(host.Start());
    WaitFor(host, Started);
    const auto failed = prepare(host);
    CHECK(failed.installs.size() == 1 && failed.installs[0].state == PreparationState::kFailed);
    CHECK(fs::is_directory(fixture.root / "data/homebrew/FAKE78001"));
    // Stopped under the installed kit: nothing from another kit, and Try again goes on as before.
    CHECK(!failed.installs[0].setup_other_kit);
    const auto again = prepare(host);
    CHECK(!again.installs[0].setup_other_kit && again.installs[0].state == PreparationState::kFailed);
    CHECK(!again.refusals.count(220) || again.refusals.at(220).find("another runtime kit") == std::string::npos);
    host.Shutdown();
  }
  *bottle_valid = true;
  const auto save = fixture.root / "data/playden/games/steam-220/library/prefixes/game/drive_c/users/steamuser/save.dat";
  Write(save, "player-save", 0600);
  const auto other = SyntheticKit(fixture.root / "kit-other", "native", "kit-2", 'b');
  auto second = config;
  second.admit_kit = [other](std::string*) { return std::optional<RuntimeKit>(other); };
  RuntimeHost host(second);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.installs.size() == 1 && started.installs[0].setup_other_kit);
  const auto records_before = Read(fixture.root / "data/runtime-state/installs.json");
  const auto refused = prepare(host);
  CHECK(refused.refusals.count(220) && refused.refusals.at(220).find("another runtime kit") != std::string::npos &&
        refused.refusals.at(220).find("Recreate bottle") != std::string::npos);
  CHECK(refused.installs[0].setup_other_kit && refused.installs[0].state == PreparationState::kFailed);
  // Nothing was rebound or retried.
  CHECK(Read(fixture.root / "data/runtime-state/installs.json") == records_before);

  // Recreate bottle: the title goes through the normal removal, keeping saves, then the game is prepared again.
  const auto removals = host.Snapshot().removals_handled;
  host.Remove(220, false);
  const auto removed = WaitFor(host, [removals](const RuntimeSnapshot& s) { return s.removals_handled > removals; }, 30);
  if (removed.removal_failures.count(220)) std::fprintf(stderr, "Remove failed: %s\n", removed.removal_failures.at(220).c_str());
  CHECK(!removed.removal_failures.count(220) && removed.installs.empty());
  const auto done = prepare(host);
  if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220) && done.installs.size() == 1 && done.installs[0].state == PreparationState::kReady);
  CHECK(done.installs[0].title_current && !done.installs[0].setup_other_kit);
  CHECK(Read(save) == "player-save");
  host.Shutdown();
}

// A game run that leaves the bottle registry wider than admission accepts
// (Wine saving with umask 0) is reported at startup, and Prepare repairs it.
void RepairsBottleRegistryModes() {
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
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    host.Prepare(Hl2());
    WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
    }, 60);
    host.Shutdown();
  }
  const auto user_reg = fixture.root / "data/playden/games/steam-220/library/prefixes/game/user.reg";
  CHECK(chmod(user_reg.c_str(), 0666) == 0);
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.installs[0].admission.find("stage 7") != std::string::npos);
  host.Prepare(Hl2());
  const auto repaired = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].admission == "ok";
  }, 60);
  CHECK(!repaired.refusals.count(220));
  struct stat info{};
  CHECK(stat(user_reg.c_str(), &info) == 0 && (info.st_mode & 07777) == 0600);
  CHECK(fixture.registry_mutations == 2);
  host.Shutdown();

  // A title prepared before per-game names keeps the kit's name and icon: it
  // reports an update, and Update gives it the game's own without registering
  // again.
  std::error_code ignored;
  fs::remove_all(fixture.root / "data/playden/games/steam-220/title-metadata", ignored);
  const auto title_param = fixture.root / "data/homebrew/FAKE78001/sce_sys/param.json";
  CHECK(chmod(title_param.c_str(), 0644) == 0);
  Write(title_param, Read(fixture.root / "kit/titles/variant/sce_sys/param.json"), 0644);
  auto named = config;
  named.title_icon = [](const playden::GameId&) -> std::optional<std::string> { return std::string("game-icon-png"); };
  RuntimeHost upgraded(named);
  CHECK(upgraded.Start());
  CHECK(!WaitFor(upgraded, Started).installs[0].title_current);
  upgraded.Prepare(Hl2());
  const auto renamed = WaitFor(upgraded, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].title_current;
  }, 60);
  CHECK(!renamed.refusals.count(220));
  CHECK(Read(title_param).find("Half-Life") != std::string::npos);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/sce_sys/icon0.png") == "game-icon-png");
  CHECK(fixture.registry_mutations == 2);
  // Asking for the store name in titles makes the title out of date; Update
  // renames it, still without registering again.
  upgraded.SetTitleNames({{220, "Half-Life \"2\" (Steam)"}});
  WaitFor(upgraded, [](const RuntimeSnapshot& s) { return !s.installs.empty() && !s.installs[0].title_current; }, 10);
  auto suffixed = Hl2();
  suffixed.title_name = "Half-Life \"2\" (Steam)";
  upgraded.Prepare(suffixed);
  WaitFor(upgraded, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].title_current;
  }, 60);
  CHECK(Read(title_param).find(R"(Half-Life \"2\" (Steam))") != std::string::npos);
  CHECK(fixture.registry_mutations == 2);
  upgraded.Shutdown();
}

// Play from Playden: the worker checks the Ready title and prepares a session;
// only Handoff on the application thread starts it, once, with the session
// argument. The wrapper's exit record ends the session and allows the next Play.
void PlaysReadyTitleThroughHandoff() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.session_data_root = fixture.root / "data";
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
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  std::vector<std::pair<std::string, std::vector<std::string>>> launches;
  config.launch_title = [&launches](const std::string& title, const std::vector<std::string>& arguments) {
    launches.emplace_back(title, arguments);
    return TitleResult{true, 0, "launch"};
  };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  CHECK(!host.Handoff());  // nothing prepared
  host.Prepare(Hl2());
  WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);

  // A kit update adds media metadata after this bottle was made. The next Play merges it before handoff.
  const auto media = kit.view.root / kit.view.runtime.path / "share/wine/media-build/media-registry.json";
  const auto hive = fixture.root / "data/playden/games/steam-220/library/prefixes/game/system.reg";
  CHECK(Read(hive).find("winegstreamer") == std::string::npos);
  Write(media, R"({"schema":"playden-media-registry/1","values":[
    {"key":"Software\\Classes\\CLSID\\media\\InprocServer32","name":"","data":"\"winegstreamer.dll\""},
    {"key":"Software\\Classes\\Wow6432Node\\CLSID\\media\\InprocServer32","name":"","data":"\"winegstreamer.dll\""}]})", 0644);
  // The bottle predates the media DLLs as well; the same Play copies them in.
  const auto windows = fixture.root / "data/playden/games/steam-220/library/prefixes/game/drive_c/windows";
  for (const auto* arch : {"x86_64-windows", "i386-windows"})
    Write(kit.view.root / kit.view.runtime.path / "lib/wine" / arch / "winegstreamer.dll", std::string("dll ") + arch, 0644);
  for (const auto* folder : {"system32", "syswow64"}) std::filesystem::create_directories(windows / folder);

  host.Play(220);
  const auto prepared = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  const auto upgraded_hive = Read(hive);
  CHECK(upgraded_hive.find("@=\"winegstreamer.dll\"") != std::string::npos &&
        upgraded_hive.find("Wow6432Node") != std::string::npos);
  // The backup is the hive before the merge: the Visual C++ runtime key the preparation wrote, no media values.
  const auto media_backup = Read(hive.string() + ".playden-backup");
  CHECK(media_backup.find("winegstreamer") == std::string::npos && media_backup.find("14.50.35719.0") != std::string::npos);
  CHECK(Read(windows / "system32/winegstreamer.dll") == "dll x86_64-windows");
  CHECK(Read(windows / "syswow64/winegstreamer.dll") == "dll i386-windows");
  CHECK(prepared.launch && prepared.launch->runtime_number == 220 && prepared.launch->title_id == "FAKE78001");
  // Cloud saves under Windows folders use this game's private bottle.
  CHECK(prepared.installs[0].prefix_root == "/data/playden/games/steam-220/library/prefixes/game");
  CHECK(launches.empty());
  // Another thread cannot start the title.
  std::thread other([&host] { CHECK(host.Handoff()); });
  other.join();
  CHECK(launches.empty());
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff());
  CHECK(launches.size() == 1 && launches[0].first == "FAKE78001");
  const std::string session = "00000000-0000-4000-8000-000000000002";
  CHECK(launches[0].second == std::vector<std::string>{"session=" + session});
  CHECK(!host.Handoff());
  CHECK(host.Snapshot().launch->state == LaunchState::kLaunching);
  // A second Play while the session is undecided is refused and stays visible
  // for that game; the pending session is untouched.
  const auto handled = host.Snapshot().plays_handled;
  host.Play(220);
  const auto refused = WaitFor(host, [handled](const RuntimeSnapshot& s) { return s.plays_handled > handled; }, 30);
  CHECK(refused.play_refusals.count(220) && refused.launch && refused.launch->state == LaunchState::kLaunching);
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // one more poll
  CHECK(host.Snapshot().play_refusals.count(220) && launches.size() == 1);

  // The game cannot be updated or repaired while its session is undecided.
  host.Prepare(Hl2());
  const auto held = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; }, 30);
  CHECK(held.refusals.at(220).find("Close the game") != std::string::npos);
  // The wrapper records the session in the private game data root.
  const auto sessions = fixture.root / "data/playden/games/steam-220/logs/sessions";
  fs::create_directories(sessions);
  CHECK(chmod((fixture.root / "data/playden/games/steam-220/logs").c_str(), 0700) == 0);
  CHECK(chmod(sessions.c_str(), 0700) == 0);
  const auto record = [&](const std::string& id, const std::string& phase) {
    return R"({"schema_version":1,"session_id":")" + id +
           R"(","install_id":"steam-220","title_id":"FAKE78001","origin":"playden","settings_revision":0,)"
           R"("runtime_bundle_id":"wine-wow64","phase":")" + phase + "\"}\n";
  };
  // Many earlier Home sessions do not hide this one.
  for (int i = 0; i < 300; ++i) {
    char id[37];
    std::snprintf(id, sizeof(id), "11111111-1111-4111-8111-%012d", i);
    Write(sessions / (std::string(id) + ".json"), record(id, "exited"), 0600);
  }
  Write(sessions / (session + ".json"), record(session, "running"), 0600);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kRunning; }, 30);
  Write(sessions / (session + ".json"), record(session, "exited"), 0600);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kExited; }, 30);
  // The ended session no longer blocks Play, and an accepted Play clears the
  // earlier refusal.
  host.Play(220);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff());
  CHECK(Read(hive) == upgraded_hive);
  CHECK(launches.size() == 2 && launches[1].second[0] != launches[0].second[0]);
  CHECK(!host.Snapshot().play_refusals.count(220));
  // The system closes Playden when the game starts. If the game is then closed
  // from the system menu, its record still says running; the restarted host
  // ends that session once the title is seen stopped, and Play works again.
  const std::string third = "00000000-0000-4000-8000-000000000003";
  CHECK(launches[1].second[0] == "session=" + third);
  host.Shutdown();
  Write(sessions / (third + ".json"), record(third, "running"), 0600);
  RuntimeHost restarted(config);
  CHECK(restarted.Start());
  const auto ended = WaitFor(restarted, [](const RuntimeSnapshot& s) {
    return s.launch && s.launch->state == LaunchState::kExited;
  }, 30);
  CHECK(ended.launch->code == "title-ended-without-record");
  restarted.Play(220);
  WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(restarted.Handoff() && launches.size() == 3);
  // Evidence that does not belong to this session fails it, but while the title
  // runs the session stays pending and Play stays refused.
  const std::string fourth = "00000000-0000-4000-8000-000000000004";
  CHECK(launches[2].second[0] == "session=" + fourth);
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  // Cloud asks the console through the worker: a running title answers active,
  // and every answer carries a newer serial.
  const auto serial = restarted.Snapshot().activity_serial;
  restarted.CheckActivity(220);
  const auto checked = WaitFor(restarted, [serial](const RuntimeSnapshot& s) { return s.activity_serial > serial; }, 10);
  CHECK(checked.activity.at(220).first == NativeActivity::kActive && checked.activity.at(220).second > serial);
  restarted.CheckActivity(999);  // no prepared title: nothing of it can run
  const auto unprepared = WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.activity.count(999) != 0; }, 10);
  CHECK(unprepared.activity.at(999).first == NativeActivity::kStopped);
  // Another thread (the Cloud worker) can wait for a fresh answer.
  CHECK(restarted.ActivityNow(220, std::chrono::seconds(10)) == NativeActivity::kActive);
  CHECK(restarted.ActivityNow(999, std::chrono::seconds(10)) == NativeActivity::kStopped);
  auto foreign = record(fourth, "running");
  foreign.replace(foreign.find("\"playden\""), 9, "\"home\"");
  Write(sessions / (fourth + ".json"), foreign, 0600);
  WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kFailed; }, 30);
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  const auto handled_before = restarted.Snapshot().plays_handled;
  restarted.Play(220);
  const auto refused_while_alive = WaitFor(restarted, [handled_before](const RuntimeSnapshot& s) {
    return s.plays_handled > handled_before;
  }, 30);
  CHECK(refused_while_alive.play_refusals.count(220) && launches.size() == 3);
  // Once the title is seen stopped, the failed session ends and Play works again.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  restarted.Play(220);
  WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(restarted.Handoff() && launches.size() == 4);
  restarted.Shutdown();
}

// A game Playden ships no recipe for is prepared from Steam's launch options; the
// generated recipe is saved and read back after a restart.
// A kit replaced while Playden runs (an upload with --replace) leaves the admitted paths behind:
// Play, preparation and maintenance say to restart Playden instead of failing on a missing file.
void ReplacedKitAsksForRestart() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.session_data_root = fixture.root / "data";
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  auto kit = SyntheticKit(fixture.root / "kit");
  Write(fixture.root / "kit/manifest.json", "{\"kit\":\"first\"}", 0644);
  kit.manifest_sha256 = *HashArtifactFile(fixture.root / "kit/manifest.json");
  config.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  std::vector<std::string> launches;
  config.launch_title = [&launches](const std::string& title, const std::vector<std::string>&) {
    launches.push_back(title);
    return TitleResult{true, 0, "launch"};
  };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);

  Write(fixture.root / "kit/manifest.json", "{\"kit\":\"second\"}", 0644);
  const auto handled = host.Snapshot().plays_handled;
  host.Play(220);
  const auto refused = WaitFor(host, [handled](const RuntimeSnapshot& s) { return s.plays_handled > handled; }, 30);
  CHECK(refused.play_refusals.count(220) &&
        refused.play_refusals.at(220).find("Quit Playden and start it again") != std::string::npos);
  CHECK(!refused.handoff_ready && launches.empty());
  host.Prepare(Hl2());
  const auto held = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; }, 30);
  CHECK(held.refusals.at(220).find("changed while Playden was running") != std::string::npos);
}

void PreparesGameWithoutShippedRecipe() {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  std::vector<std::pair<std::string, std::vector<std::string>>> launches;
  config.launch_title = [&launches](const std::string& title, const std::vector<std::string>& arguments) {
    launches.emplace_back(title, arguments);
    return TitleResult{true, 0, "launch"};
  };
  std::map<std::string, std::string> title_logs;
  config.title_log = [&title_logs](const std::string& title, const std::string& text) { title_logs[title] = text; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/4242";
  // A 64-bit PE header is all the generator reads.
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(game / "Bin/Game.exe", pe, 0644);
  // The game's own Steam library (a 64-bit PE that asks for one interface).
  const std::string own_api = pe + std::string("\0SteamUser021\0", 14);
  Write(game / "Bin/steam_api64.dll", own_api, 0644);
  Write(game / ".playden-steam", "depots=4243\ndlc=4250\n", 0644);
  std::map<std::string, std::string> saved_settings;
  config.settings_record = [&saved_settings](const playden::GameId&) -> std::optional<GameSettingsRecord> {
    if (saved_settings.empty()) return std::nullopt;
    GameSettingsRecord record;
    record.overrides = saved_settings;
    return record;
  };
  CHECK(chmod(game.c_str(), 0755) == 0);
  RuntimePrepareRequest request{4242, "/data/homebrew/FAKE77031/games/4242", "internal", "Example Game"};

  RuntimeHost host(config);
  host.SetSteamAccount("player", 76561198000000001ull);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  // Without Steam's launch options there is nothing to start.
  request.launch_reason = "Steam details are unavailable right now.";
  host.Prepare(request);
  auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(4242) != 0; });
  CHECK(refused.refusals.at(4242) == "Steam details are unavailable right now.");
  CHECK(refused.installs.empty() && fixture.registry_mutations == 0);

  request.launch_reason.clear();
  request.launches = {{"", "", "bin\\game.exe", "", "-windowed"}};
  host.Prepare(request);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && ((s.refusals.count(4242) && s.refusals.at(4242) != "Steam details are unavailable right now.") ||
           (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(4242)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(4242).c_str());
  CHECK(done.installs.size() == 1 && done.installs[0].state == PreparationState::kReady);
  CHECK(done.generated_recipes.size() == 1 && done.generated_recipes[0].id == "steam-4242-auto");
  CHECK(done.generated_recipes[0].architecture == "pe64");
  CHECK(done.generated_recipes[0].launch_entries[0].executable == "Bin/Game.exe");
  CHECK(fs::is_regular_file(fixture.root / "data/runtime-state/recipe-steam-4242-auto.json"));
  const auto descriptor = Read(fixture.root / "data/homebrew/FAKE78001/game-launch.json");
  CHECK(descriptor.find("steam-4242-auto") != std::string::npos);
  // Steamworks: the emulator replaced the game's library, which is kept, and steam_settings/ was written.
  const auto emulator = Read(fs::path(g_runtime_dir) / "steamworks/steam_api64.dll");
  CHECK(Read(game / "Bin/steam_api64.dll") == emulator && Read(game / "Bin/steam_api64.dll.orig") == own_api);
  CHECK(Read(game / "Bin/steam_settings/configs.user.ini").find("account_name=player\n") != std::string::npos);
  CHECK(Read(game / "Bin/steam_settings/configs.app.ini").find("4250=dlc4250\n") != std::string::npos);
  CHECK(Read(game / "Bin/steam_settings/steam_interfaces.txt") == "SteamUser021\n");
  // The bottle's owner marker names the generated recipe, so the game's sessions are read.
  const auto marker = Read(fixture.root / "data/playden/games/steam-4242/library/prefixes/game/.playden-template-owner");
  CHECK(marker.find("\nrecipe_id=steam-4242-auto\n") != std::string::npos);
  // The player asks for the game's own Steam files: Play puts them back before the game starts.
  saved_settings = {{"steam_features", "game"}};
  host.Play(4242);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff() && launches.size() == 1 && launches[0].first == "FAKE78001");
  CHECK(Read(game / "Bin/steam_api64.dll") == own_api);
  const auto session = launches[0].second[0].substr(std::strlen("session="));
  const auto sessions = fixture.root / "data/playden/games/steam-4242/logs/sessions";
  fs::create_directories(sessions);
  CHECK(chmod((fixture.root / "data/playden/games/steam-4242/logs").c_str(), 0700) == 0);
  CHECK(chmod(sessions.c_str(), 0700) == 0);
  const auto record = [&](const std::string& phase) {
    return R"({"schema_version":1,"session_id":")" + session +
           R"(","install_id":"steam-4242","title_id":"FAKE78001","origin":"playden","settings_revision":0,)"
           R"("runtime_bundle_id":"wine-wow64","phase":")" + phase + "\"}\n";
  };
  Write(sessions / (session + ".json"), record("running"), 0600);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kRunning; }, 30);
  Write(sessions / (session + ".json"), record("exited"), 0600);
  const auto ended = WaitFor(host, [](const RuntimeSnapshot& s) {
    return s.launch && s.launch->state != LaunchState::kRunning && s.launch->state != LaunchState::kLaunching;
  }, 30);
  CHECK(ended.launch->state == LaunchState::kExited);
  host.Shutdown();

  // After a restart the saved recipe is found without Steam, and the title log carries the
  // session report followed by the engine's own log from the bottle.
  const auto data = fixture.root / "data/playden/games/steam-4242";
  Write(data / "library/logs/session-0.log", "PW_REPORT/1 stopped\n", 0600);
  Write(data / "library/prefixes/game/drive_c/users/prospero/AppData/LocalLow/Studio/Game/Player.log",
        "NullReferenceException at Boot\n", 0600);
  RuntimeHost restarted(config);
  CHECK(restarted.Start());
  const auto reloaded = WaitFor(restarted, [](const RuntimeSnapshot& s) { return Started(s) && !s.generated_recipes.empty(); });
  CHECK(reloaded.generated_recipes[0].id == "steam-4242-auto");
  CHECK(title_logs.count("FAKE78001"));
  const auto& title_log = title_logs.at("FAKE78001");
  CHECK(title_log.find("== session-0.log\nPW_REPORT/1 stopped") != std::string::npos);
  CHECK(title_log.find("AppData/LocalLow/Studio/Game/Player.log\nNullReferenceException at Boot") != std::string::npos);
  restarted.Prepare({4242, "/data/homebrew/FAKE77031/games/4242", "internal", "Example Game"});
  const auto prepared = WaitFor(restarted, [](const RuntimeSnapshot& s) { return s.refusals.count(4242) != 0; });
  CHECK(prepared.refusals.at(4242) == "This game is already prepared.");
  restarted.Shutdown();

  // Unity 5 writes output_log.txt beside the program instead; the newer log is the one shown.
  Write(game / "Bin/Game_Data/output_log.txt", "Initialize engine version: 5.3.6p5\n", 0600);
  std::filesystem::last_write_time(game / "Bin/Game_Data/output_log.txt",
                                   std::filesystem::file_time_type::clock::now() + std::chrono::seconds(10));
  title_logs.clear();
  RuntimeHost legacy(config);
  CHECK(legacy.Start());
  WaitFor(legacy, [](const RuntimeSnapshot& s) { return Started(s) && !s.generated_recipes.empty(); });
  CHECK(title_logs.count("FAKE78001"));
  CHECK(title_logs.at("FAKE78001").find("== Game_Data/output_log.txt\nInitialize engine version: 5.3.6p5") !=
        std::string::npos);
  legacy.Prepare({4242, "/data/homebrew/FAKE77031/games/4242", "internal", "Example Game"});
  const auto again = WaitFor(legacy, [](const RuntimeSnapshot& s) { return s.refusals.count(4242) != 0; });
  CHECK(again.refusals.at(4242) == "This game is already prepared.");
  legacy.Shutdown();
}

// An update changed a Ready game's files (its marker in the game's folder): Play refuses until Prepare has
// made the launch again from the new files and Steam's options, as a fresh preparation would, and published
// it; then the marker goes. A program that is gone with no other to start keeps the marker and says why.
void UpdatedFilesSetUpTheLaunchAgain() {
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
  config.session_data_root = fixture.root / "data";
  config.launch_title = [](const std::string&, const std::vector<std::string>&) { return TitleResult{true, 0, "launch"}; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/4243";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(game / "Bin/Game.exe", pe, 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  GameSettingsRecord saved;
  bool has_record = false;
  config.settings_record = [&](const playden::GameId&) -> std::optional<GameSettingsRecord> {
    if (!has_record) return std::nullopt;
    return saved;
  };
  RuntimePrepareRequest request{4243, "/data/homebrew/FAKE77031/games/4243", "internal", "Updated Game"};
  request.launches = {{"", "", "bin\\game.exe", "", ""}};
  const auto profile = fixture.root / "data/playden/games/steam-4243/library/profiles/playden-steam-4243.profile";
  const auto marker = game / playden::installs::kFilesChangedName;

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(request);
  const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(4243) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (ready.refusals.count(4243)) std::fprintf(stderr, "Prepare refused: %s\n", ready.refusals.at(4243).c_str());
  CHECK(ready.installs.size() == 1 && ready.installs[0].state == PreparationState::kReady && !ready.installs[0].files_changed);
  CHECK(ready.installs[0].launch_path == "Bin/Game.exe" && Read(profile).find("Game.exe\n") != std::string::npos);

  // The update renamed the program and left its marker.
  fs::remove(game / "Bin/Game.exe");
  Write(game / "Bin/Game2.exe", pe, 0644);
  Write(marker, "v1\n", 0644);
  host.RecheckInstalls();
  WaitFor(host, [](const RuntimeSnapshot& s) { return !s.installs.empty() && s.installs[0].files_changed; });
  host.Play(4243);
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.play_refusals.count(4243) != 0; });
  CHECK(refused.play_refusals.at(4243).find("was updated") != std::string::npos && !refused.handoff_ready);

  // Prepare with Steam's options for the new version: the launch is made again and published.
  request.launches = {{"", "", "bin\\game2.exe", "", ""}};
  const auto prepare = [&] {
    const auto before = host.Snapshot().prepares_handled;
    host.Prepare(request);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.prepares_handled > before && !s.preparing_app; }, 60);
  };
  auto done = prepare();
  if (done.refusals.count(4243)) std::fprintf(stderr, "Refresh refused: %s\n", done.refusals.at(4243).c_str());
  CHECK(!done.refusals.count(4243));
  CHECK(done.installs[0].launch_path == "Bin/Game2.exe" && !done.installs[0].files_changed && !done.installs[0].recovery_pending);
  CHECK(done.generated_recipes.size() == 1 && done.generated_recipes[0].launch_entries[0].executable == "Bin/Game2.exe");
  CHECK(Read(profile).find("Game2.exe\n") != std::string::npos && !fs::exists(marker));

  // The program is gone and Steam names no other: refused as a fresh preparation is, and the marker stays.
  fs::remove(game / "Bin/Game2.exe");
  Write(marker, "v1\n", 0644);
  host.RecheckInstalls();
  WaitFor(host, [](const RuntimeSnapshot& s) { return !s.installs.empty() && s.installs[0].files_changed; });
  done = prepare();
  CHECK(done.refusals.count(4243) && done.installs[0].files_changed && fs::exists(marker));
  CHECK(done.installs[0].launch_path == "Bin/Game2.exe");

  // It is back: the program stays, and the marker goes.
  Write(game / "Bin/Game2.exe", pe, 0644);
  done = prepare();
  if (done.refusals.count(4243)) std::fprintf(stderr, "Second refresh refused: %s\n", done.refusals.at(4243).c_str());
  CHECK(!done.installs[0].files_changed && done.installs[0].launch_path == "Bin/Game2.exe" && !fs::exists(marker));

  // A program the player chose in Game settings stays the one that starts while the update kept it, though
  // Steam's options name another now.
  Write(game / "Tool.exe", pe, 0644);
  has_record = true;
  saved.revision = 1;
  saved.overrides = {{"program", "Tool.exe"}, {"architecture", "pe64"}};
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  const auto maintained = host.Snapshot().maintenance_handled;
  host.Maintain(4243, RuntimeMaintenance::kApplySettings);
  WaitFor(host, [maintained](const RuntimeSnapshot& s) { return s.maintenance_handled > maintained; }, 60);
  CHECK(host.Snapshot().installs[0].launch_path == "Tool.exe");
  Write(marker, "v1\n", 0644);
  host.RecheckInstalls();
  WaitFor(host, [](const RuntimeSnapshot& s) { return !s.installs.empty() && s.installs[0].files_changed; });
  done = prepare();
  if (done.refusals.count(4243)) std::fprintf(stderr, "Program refresh refused: %s\n", done.refusals.at(4243).c_str());
  CHECK(!done.installs[0].files_changed && !fs::exists(marker));
  CHECK(done.installs[0].launch_entry_id == kProgramEntryId && done.installs[0].launch_path == "Tool.exe");
  CHECK(Read(profile).find("Tool.exe\n") != std::string::npos);
  // Play goes ahead now.
  host.Play(4243);
  CHECK(WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30).handoff_ready);
  CHECK(host.Handoff());
  host.Shutdown();
}

// Run a program once: Play with a program from the game folder publishes it, with the Mouse preset, for one start. The
// game's own settings come back with the next publication: when Playden sees the run end, on its next start, or on
// the next Play.
void RunsSetupProgramOnce() {
  Fixture fixture;
  auto config = fixture.Config();
  // A shipped recipe that binds buttons, one of them locked: the run has the Mouse preset's instead.
  const auto runtime = fixture.root / "runtime";
  Write(runtime / "presets.json", Read(fs::path(g_runtime_dir) / "presets.json"), 0644);
  for (const auto& entry : fs::directory_iterator(fs::path(g_runtime_dir) / "input"))
    Write(runtime / "input" / entry.path().filename(), Read(entry.path()), 0644);
  Write(runtime / "recipes/steam-4300-setuptest-v1.json", R"({
  "recipe_id": "steam-4300-setuptest-v1", "recipe_version": 1, "app_id": 4300, "name": "Setup Game",
  "launch_entries": [{"id": "game", "label": "Setup Game", "executable": "Binaries/Game.exe"}],
  "arguments": "", "dll_overrides": "", "architecture": "pe32", "runtime": "wine-wow64", "prefix": "game",
  "graphics": "dxvk", "defaults": {"button.options": "escape", "button.cross": "space"}, "locked": ["button.cross"]
})", 0644);
  Write(runtime / "recipes/recipes.lst", "steam-4300-setuptest-v1.json\n", 0644);
  config.runtime_dir = runtime.string();
  config.steamworks_dir = (fs::path(g_runtime_dir) / "steamworks").string();
  config.host_namespace_prefix = fixture.root;
  config.session_data_root = fixture.root / "data";
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
    char id[37];
    std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012u", ++uuids);
    return std::string(id);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  std::vector<std::vector<std::string>> launches;
  config.launch_title = [&launches](const std::string&, const std::vector<std::string>& arguments) {
    launches.push_back(arguments);
    return TitleResult{true, 0, "launch"};
  };
  GameSettingsRecord saved;
  saved.overrides = {{"controller_preset", "gamepad"}, {"button.square", "e"}};
  config.settings_record = [&](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/4300";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x4c; pe[0x45] = 0x01; pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x01;
  Write(game / "Binaries/Game.exe", pe, 0644);
  Write(game / "Binaries/Launcher.exe", pe, 0644);
  RuntimePrepareRequest request{4300, "/data/homebrew/FAKE77031/games/4300", "internal", "Setup Game"};
  request.launches = {{"", "", "Binaries\\Game.exe", "", ""}};
  const auto profile = fixture.root / "data/playden/games/steam-4300/library/profiles/playden-steam-4300.profile";
  const auto installs = config.state_root / "installs.json";
  const auto sessions = fixture.root / "data/playden/games/steam-4300/logs/sessions";
  // The session record the title shell writes for a run.
  const auto session = [&](const std::string& argument, const char* phase) {
    const auto id = argument.substr(argument.find('=') + 1);
    fs::create_directories(sessions);
    CHECK(chmod(sessions.parent_path().c_str(), 0700) == 0 && chmod(sessions.c_str(), 0700) == 0);
    Write(sessions / (id + ".json"),
          R"({"schema_version":1,"session_id":")" + id + R"(","install_id":"steam-4300","title_id":"FAKE78001",)"
          R"("origin":"playden","settings_revision":0,"runtime_bundle_id":"wine-wow64","phase":")" + phase + "\"}\n", 0600);
  };
  const auto ended = [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kExited; };
  const auto handed_off = [&](RuntimeHost& host) {
    CHECK(WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30).handoff_ready);
    CHECK(host.Handoff());
  };
  const auto game_published = [&] {
    const auto text = Read(profile);
    return text.find("\\Binaries\\Game.exe\n") != std::string::npos && text.find("preset = gamepad") != std::string::npos &&
           text.find("cross = space\n") != std::string::npos && text.find("options = escape\n") != std::string::npos &&
           text.find("square = e\n") != std::string::npos && Read(installs).find("setup_program") == std::string::npos;
  };
  const auto launcher_published = [&] {
    const auto text = Read(profile);
    return text.find("\\Binaries\\Launcher.exe\n") != std::string::npos && text.find("preset = mouse") != std::string::npos &&
           text.find("cross = ") == std::string::npos && text.find("options = ") == std::string::npos &&
           text.find("square = ") == std::string::npos &&
           Read(installs).find("\"setup_program\":\"Binaries/Launcher.exe\"") != std::string::npos;
  };
  const auto restored = [](const RuntimeSnapshot& s) {
    return !s.installs.empty() && s.installs[0].launch_path == "Binaries/Game.exe";
  };
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(request);
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(4300) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (done.refusals.count(4300)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(4300).c_str());
    CHECK(!done.refusals.count(4300) && game_published());
    fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
    // The launcher starts with the Mouse preset alone; the game's program and every button binding stay out.
    host.Play(4300, "Binaries/Launcher.exe");
    handed_off(host);
    CHECK(launcher_published() && host.Snapshot().installs[0].launch_path == "Binaries/Launcher.exe" && launches.size() == 1);
    // Closed from the PS5 menu: its record still says running when the system ends Playden.
    session(launches[0][0], "running");
    host.Shutdown();
  }
  {
    // Playden starts again and sees the title stopped: once the session ends, the game's settings are back.
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, [&](const RuntimeSnapshot& s) { return restored(s) && ended(s); }, 60).installs[0].launch_path ==
          "Binaries/Game.exe");
    CHECK(game_published());
    // The program quits by itself this time, while Playden is closed: its next start puts the game back.
    host.Play(4300, "Binaries/Launcher.exe");
    handed_off(host);
    CHECK(launcher_published() && launches.size() == 2);
    session(launches[1][0], "exited");
    WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kExited; }, 30);
    host.Shutdown();
  }
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, restored, 60).installs[0].launch_path == "Binaries/Game.exe" && game_published());

  // The next Play puts the game back before it starts, too.
  host.Play(4300, "Binaries/Launcher.exe");
  handed_off(host);
  CHECK(launcher_published() && launches.size() == 3);
  host.Play(4300);
  const auto busy = WaitFor(host, [](const RuntimeSnapshot& s) { return s.play_refusals.count(4300) != 0; }, 30);
  CHECK(busy.play_refusals.count(4300) && launcher_published());  // not while the program's session is open
  session(launches[2][0], "exited");
  WaitFor(host, ended, 30);
  CHECK(WaitFor(host, restored, 30).installs[0].launch_path == "Binaries/Game.exe" && game_published());
  host.Play(4300);
  handed_off(host);
  CHECK(game_published() && launches.size() == 4);
  session(launches[3][0], "exited");
  WaitFor(host, ended, 30);

  // A program that isn't there is refused, and the game's settings stay.
  const auto handled = host.Snapshot().plays_handled;
  host.Play(4300, "Binaries/Missing.exe");
  const auto refused = WaitFor(host, [handled](const RuntimeSnapshot& s) { return s.plays_handled > handled; }, 30);
  CHECK(refused.play_refusals.count(4300) && refused.play_refusals.at(4300).find("missing") != std::string::npos);
  CHECK(game_published() && launches.size() == 4);
  host.Shutdown();
}

// Settings saved while the game is being prepared, after its profile was written, reach the title
// as soon as it is ready instead of waiting for the next Playden start.
void SettingsSavedDuringPrepareReachReadyTitle() {
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
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  std::int64_t now = 1760000000;
  config.unix_time = [&now] { return now; };
  config.session_data_root = fixture.root / "data";
  std::mutex mutex;
  GameSettingsRecord saved;
  bool applied_logged = false;
  config.settings_record = [&](const playden::GameId&) -> std::optional<GameSettingsRecord> {
    std::lock_guard<std::mutex> lock(mutex);
    return saved;
  };
  // The player changes a setting right after the profile is written.
  config.log = [&](const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    if (line.rfind("prepare step app=220 from=publish-profile", 0) == 0) {
      saved.revision = 1;
      saved.overrides = {{"resolution", "1280x720"}};
    }
    if (line == "prepared settings app=220 applied") applied_logged = true;
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  const auto profile = fixture.root / "data/playden/games/steam-220/library/profiles/playden-steam-220.profile";

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);
  CHECK(done.installs[0].settings_revision == 1 && Read(profile).find("desktop = 1280x720") != std::string::npos);
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(applied_logged);
  }
  host.Shutdown();
}

// Settings saved after preparation reach the Ready title through a journaled update, from
// maintenance or from Play, only while the title is stopped; maintenance clears the shader
// cache, backs up the bottle's user folders and repairs the bottle.
void MaintainsReadyTitle() {
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
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "game-root-unowned", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  bool loosen_next = false;  // the next armed update's capture sees a world-writable PS5 title folder
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  const auto staged = fixture.root / "data/homebrew/FAKE78001";
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  // Logged once the update is armed and before its capture: the seam for an interruption.
  unsigned full_inspections = 0, ownership_inspections = 0;
  config.log = [&loosen_next, staged, &full_inspections, &ownership_inspections](const std::string& line) {
    if (line.rfind("package inspect ownership ", 0) == 0) ++ownership_inspections;
    else if (line.rfind("package inspect ", 0) == 0) ++full_inspections;
    if (loosen_next && line.rfind("settings update request", 0) == 0 && line.find(" state=0 ") != std::string::npos) {
      loosen_next = false;
      CHECK(chmod(staged.c_str(), 0777) == 0);
    }
  };
  std::int64_t now = 1760000000;
  config.unix_time = [&now] { return now; };
  config.session_data_root = fixture.root / "data";
  std::vector<std::string> launches;
  config.launch_title = [&launches](const std::string& title, const std::vector<std::string>&) {
    launches.push_back(title);
    return TitleResult{true, 0, "launch"};
  };
  GameSettingsRecord saved;
  config.settings_record = [&saved](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  const auto data = fixture.root / "data/playden/games/steam-220";
  const auto profile = data / "library/profiles/playden-steam-220.profile";

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  CHECK(!done.refusals.count(220) && done.installs[0].settings_revision == 0 && done.installs[0].launch_entry_id == "hl2");
  CHECK(Read(profile).find("desktop = 1920x1080") != std::string::npos && !fs::exists(game / "dxvk.conf"));

  const auto maintain = [&host](RuntimeMaintenance action) {
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(220, action);
    CHECK(host.Snapshot().maintenance.at(220).running);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  };
  // Saved while the game runs: nothing changes until it is stopped.
  saved.revision = 1;
  saved.overrides = {{"resolution", "1280x720"}, {"frame_limit", "30"}};
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  auto result = maintain(RuntimeMaintenance::kApplySettings);
  CHECK(!result.maintenance.at(220).ok && result.maintenance.at(220).message == "Changes apply the next time Playden starts the game.");
  CHECK(result.installs[0].settings_revision == 0 && Read(profile).find("desktop = 1920x1080") != std::string::npos);
  fixture.activity->store(static_cast<int>(NativeActivity::kUnknown));
  CHECK(!maintain(RuntimeMaintenance::kClearShaderCache).maintenance.at(220).ok);
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  full_inspections = ownership_inspections = 0;
  result = maintain(RuntimeMaintenance::kApplySettings);
  CHECK(result.maintenance.at(220).ok && result.maintenance.at(220).message == "Settings applied.");
  // The update checks the title's ownership once, before it changes anything, and never
  // walks its files and folders (14 full walks, then 11 ownership checks, before).
  CHECK(full_inspections == 0 && ownership_inspections == 1);
  CHECK(result.installs[0].settings_revision == 1 && !result.installs[0].recovery_pending);
  CHECK(Read(profile).find("desktop = 1280x720") != std::string::npos);
  CHECK(Read(game / "dxvk.conf").find("maxFrameRate = 30") != std::string::npos);
  CHECK(maintain(RuntimeMaintenance::kApplySettings).maintenance.at(220).message == "Settings are up to date.");

  // The shader cache of the launch program goes; nothing else in the folder does.
  Write(game / "hl2.dxvk-cache", "pipelines", 0644);
  CHECK(maintain(RuntimeMaintenance::kClearShaderCache).maintenance.at(220).ok);
  CHECK(!fs::exists(game / "hl2.dxvk-cache") && Read(game / "hl2.exe") == "existing-game");

  // Backups copy the Windows user folders; the newest three are kept.
  // A file with another link (here the bottle's registry linked into the profile) is not copied.
  Write(data / "library/prefixes/game/drive_c/users/prospero/Documents/save.dat", "progress", 0600);
  const auto linked = data / "library/prefixes/game/drive_c/users/prospero/Documents/linked.reg";
  CHECK(link((data / "library/prefixes/game/user.reg").c_str(), linked.c_str()) == 0);
  for (int index = 0; index < 4; ++index, ++now) CHECK(maintain(RuntimeMaintenance::kBackupSaves).maintenance.at(220).ok);
  const auto backups = fixture.root / "data/playden/games/steam-220.saves";
  CHECK(Read(backups / "1760000003/users/prospero/Documents/save.dat") == "progress");
  CHECK(!fs::exists(backups / "1760000003/users/prospero/Documents/linked.reg"));
  CHECK(unlink(linked.c_str()) == 0);
  CHECK(!fs::exists(backups / "1760000000") && fs::exists(backups / "1760000001"));
  result = host.Snapshot();
  CHECK(result.installs[0].save_backups == 3 && result.installs[0].latest_save_backup == 1760000003);

  CHECK(chmod((data / "library/prefixes/game/user.reg").c_str(), 0666) == 0);
  result = maintain(RuntimeMaintenance::kRepair);
  CHECK(result.maintenance.at(220).ok);
  struct stat info{};
  CHECK(stat((data / "library/prefixes/game/user.reg").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
  // A kit without bottle-profile-shared keeps the bottle private: its way and its saves.
  for (const auto* folder : {"", "library", "library/prefixes", "library/prefixes/game", "library/prefixes/game/drive_c/users"})
    CHECK(lstat((data / folder).c_str(), &info) == 0 && (info.st_mode & 07777) == 0700);
  CHECK(lstat((data / "library/prefixes/game/drive_c/users/prospero/Documents/save.dat").c_str(), &info) == 0 &&
        (info.st_mode & 07777) == 0600);
  CHECK(lstat(backups.c_str(), &info) == 0 && (info.st_mode & 07777) == 0700);

  // An update interrupted after its arm keeps its journal: Play waits, and Resume (Prepare)
  // finishes the saved request instead of making a new one.
  saved.revision = 2;
  saved.overrides = {{"resolution", "1280x720"}, {"frame_limit", "60"}};
  loosen_next = true;
  result = maintain(RuntimeMaintenance::kApplySettings);
  CHECK(!result.maintenance.at(220).ok && result.installs[0].recovery_pending && result.installs[0].settings_revision == 1);
  const auto plays = host.Snapshot().plays_handled;
  host.Play(220);
  result = WaitFor(host, [plays](const RuntimeSnapshot& s) { return s.plays_handled > plays; }, 30);
  CHECK(result.play_refusals.count(220) && launches.empty());
  CHECK(chmod(staged.c_str(), 0755) == 0);
  host.Prepare(Hl2());
  result = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (!s.installs[0].recovery_pending || s.refusals.count(220));
  }, 60);
  if (result.refusals.count(220)) std::fprintf(stderr, "Resume refused: %s\n", result.refusals.at(220).c_str());
  CHECK(!result.refusals.count(220) && !result.installs[0].recovery_pending && result.installs[0].settings_revision == 2);
  CHECK(Read(game / "dxvk.conf").find("maxFrameRate = 60") != std::string::npos);

  // Play publishes newer settings before the game starts, so Home starts the same profile.
  saved.revision = 3;
  saved.overrides = {{"resolution", "2560x1440"}};
  full_inspections = ownership_inspections = 0;
  host.Play(220);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff() && launches.size() == 1);
  // Play's checks: one full inspection for the launch (two before, plus a separate start
  // check), the apply's one ownership check and a marker-only native query.
  CHECK(full_inspections == 1 && ownership_inspections == 2);
  CHECK(host.Snapshot().installs[0].settings_revision == 3);
  CHECK(Read(profile).find("desktop = 2560x1440") != std::string::npos && !fs::exists(game / "dxvk.conf"));
  host.Shutdown();
}

// #73: files a player adds or changes over FTP (a mod, a world-writable folder) are theirs.
// A kit that admits unowned game folders gets the folder as it is: Play, a settings update
// and Repair all work, and the added files and modes stay.
void ModdedGameFolderStillPlays() {
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
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "game-root-unowned", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  std::vector<std::string> launches;
  config.launch_title = [&launches](const std::string& title, const std::vector<std::string>&) {
    launches.push_back(title);
    return TitleResult{true, 0, "launch"};
  };
  GameSettingsRecord saved;
  config.settings_record = [&saved](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  config.as_root = [](const std::function<bool()>& work) { return work(); };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);  // prepared by an older Playden, which tightened it
  const auto mode = [](const fs::path& path) {
    struct stat info{};
    CHECK(lstat(path.c_str(), &info) == 0);
    return unsigned(info.st_mode & 07777);
  };

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);

  // The mod, as an FTP client leaves it: world-writable folders and files.
  Write(game / "GInput.asi", "mod-plugin", 0666);
  Write(game / "mods/GInput.ini", "mod-settings", 0666);
  CHECK(chmod((game / "mods").c_str(), 0777) == 0);
  CHECK(chmod(game.c_str(), 0777) == 0);
  const auto mod_kept = [&] {
    CHECK(Read(game / "GInput.asi") == "mod-plugin" && Read(game / "mods/GInput.ini") == "mod-settings");
    CHECK(mode(game) == 0777 && mode(game / "mods") == 0777 && mode(game / "GInput.asi") == 0666);
    CHECK(Read(game / "hl2.exe") == "existing-game");
  };

  const auto maintain = [&host](RuntimeMaintenance action) {
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(220, action);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  };
  // A settings update republishes into the opened-up folder. One the app user can't write
  // (root's 0755 after an FTP upload; 0555 stands in for it) is opened up first.
  if (geteuid() != 0) CHECK(chmod(game.c_str(), 0555) == 0);
  saved.revision = 1;
  saved.overrides = {{"frame_limit", "30"}};
  auto result = maintain(RuntimeMaintenance::kApplySettings);
  if (!result.maintenance.at(220).ok) std::fprintf(stderr, "Apply refused: %s\n", result.maintenance.at(220).message.c_str());
  CHECK(result.maintenance.at(220).ok && result.installs[0].settings_revision == 1);
  CHECK(Read(game / "dxvk.conf").find("maxFrameRate = 30") != std::string::npos);
  mod_kept();
  // Repair passes the start check and keeps the mod.
  result = maintain(RuntimeMaintenance::kRepair);
  if (!result.maintenance.at(220).ok) std::fprintf(stderr, "Repair refused: %s\n", result.maintenance.at(220).message.c_str());
  CHECK(result.maintenance.at(220).ok);
  mod_kept();
  // Prepare again (Update) is not refused for the folder either.
  host.Prepare(Hl2());
  done = WaitFor(host, [](const RuntimeSnapshot& s) { return !s.preparing_app && s.refusals.count(220); });
  CHECK(done.refusals.at(220) == "This game is already prepared.");
  mod_kept();
  // A dxvk.conf hard-linked by hand fails the title's start check; Repair makes it a copy of its own.
  const auto dxvk = Read(game / "dxvk.conf");
  CHECK(link((game / "dxvk.conf").c_str(), (game / "dxvk-copy.conf").c_str()) == 0);
  result = maintain(RuntimeMaintenance::kRepair);
  if (!result.maintenance.at(220).ok) std::fprintf(stderr, "Repair refused: %s\n", result.maintenance.at(220).message.c_str());
  CHECK(result.maintenance.at(220).ok);
  struct stat linked{};
  CHECK(lstat((game / "dxvk.conf").c_str(), &linked) == 0 && linked.st_nlink == 1);
  CHECK(Read(game / "dxvk.conf") == dxvk && Read(game / "dxvk-copy.conf") == dxvk);
  mod_kept();
  // Play publishes newer settings first, then starts the game.
  saved.revision = 2;
  saved.overrides = {{"frame_limit", "60"}};
  host.Play(220);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(host.Handoff() && launches.size() == 1);
  CHECK(host.Snapshot().installs[0].settings_revision == 2);
  CHECK(Read(game / "dxvk.conf").find("maxFrameRate = 60") != std::string::npos);
  mod_kept();
  host.Shutdown();
}

// A game an older Playden or kit published owned (schema 2), which the player then opened for a
// mod. With a kit that admits unowned folders, each way back republishes it unowned: the
// startup pass (route 0), or with the title busy at startup, Repair (1), a settings update (2),
// Update (3) or Play (4). Owned 0755 folders that were tightened open up again (route 0), but
// never while the publication stays owned (5: the republication is refused). Such a game
// played before, so it still plays, Repairs and Updates under its owned publication (5).
void OwnedPublicationOpenedUpRecovers(int route) {
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
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    char id[37]; std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012u", ++uuids);
    return std::string(id);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  config.launch_title = [](const std::string&, const std::vector<std::string>&) { return TitleResult{true, 0, "launch"}; };
  GameSettingsRecord saved;
  config.settings_record = [&saved](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  config.log = [&](const std::string& line) { std::lock_guard<std::mutex> lock(lines_mutex); lines.push_back(line); };
  const auto logged = [&](const std::string& prefix) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0777) == 0);
  const auto metadata = fixture.root / "data/playden/games/steam-220/library/profiles/.playden-publication";
  const auto mode = [](const fs::path& path) {
    struct stat info{};
    CHECK(lstat(path.c_str(), &info) == 0);
    return unsigned(info.st_mode & 07777);
  };
  if (route == 5) saved.revision = 1;
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);
    host.Shutdown();
  }
  CHECK(Read(metadata).find("schema=2\n") != std::string::npos && mode(game) == 0755);
  Write(game / "GInput.asi", "mod-plugin", 0666);
  if (route == 5) {
    saved.revision = 0;  // older than the title's: the republication is refused
    kit.capabilities = {"dxvk", "game-root-unowned", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
    RuntimeHost host(config);
    CHECK(host.Start());
    WaitFor(host, Started);
    CHECK(logged("startup unowned app=220 deferred: "));
    CHECK(Read(metadata).find("schema=2\n") != std::string::npos && mode(game) == 0755);
    const auto maintain = [&host](RuntimeMaintenance action) {
      const auto before = host.Snapshot().maintenance_handled;
      host.Maintain(220, action);
      return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60).maintenance.at(220);
    };
    const auto repaired = maintain(RuntimeMaintenance::kRepair);
    if (!repaired.ok) std::fprintf(stderr, "route 5 Repair refused: %s\n", repaired.message.c_str());
    CHECK(repaired.ok && repaired.message.find("could not be published for mods") != std::string::npos);
    const auto before = host.Snapshot().prepares_handled;
    host.Prepare(Hl2());
    const auto updated = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.prepares_handled > before; }, 60);
    CHECK(updated.refusals.count(220) && updated.refusals.at(220) == "This game is already prepared.");
    host.Play(220);
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
    if (done.play_refusals.count(220)) std::fprintf(stderr, "route 5 Play refused: %s\n", done.play_refusals.at(220).c_str());
    CHECK(done.handoff_ready && host.Handoff());
    CHECK(Read(metadata).find("schema=2\n") != std::string::npos && mode(game) == 0755);
    CHECK(Read(game / "GInput.asi") == "mod-plugin");
    host.Shutdown();
    return;
  }
  if (route != 0) CHECK(chmod(game.c_str(), 0777) == 0);  // opened over FTP
  kit.capabilities = {"dxvk", "game-root-unowned", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  if (route != 0) fixture.activity->store(static_cast<int>(NativeActivity::kActive));

  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  if (route == 0) {
    CHECK(logged("startup unowned app=220 republished"));
  } else {
    CHECK(logged("startup unowned app=220 waits: "));
    CHECK(Read(metadata).find("schema=2\n") != std::string::npos);
    fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  }
  const auto maintain = [&host](RuntimeMaintenance action) {
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(220, action);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60).maintenance.at(220);
  };
  bool played = false;
  if (route == 1 || route == 2) {
    if (route == 2) { saved.revision = 1; saved.overrides = {{"frame_limit", "30"}}; }
    const auto result = maintain(route == 1 ? RuntimeMaintenance::kRepair : RuntimeMaintenance::kApplySettings);
    if (!result.ok) std::fprintf(stderr, "route %d refused: %s\n", route, result.message.c_str());
    CHECK(result.ok);
  } else if (route == 3) {
    const auto before = host.Snapshot().prepares_handled;
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.prepares_handled > before; }, 60);
    if (done.refusals.count(220)) std::fprintf(stderr, "Update refused: %s\n", done.refusals.at(220).c_str());
    if (done.refusals.count(220)) std::fprintf(stderr, "refusal: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220));
  } else if (route == 4) {
    host.Play(220);
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
    if (done.play_refusals.count(220)) std::fprintf(stderr, "Play refused: %s\n", done.play_refusals.at(220).c_str());
    CHECK(done.handoff_ready && host.Handoff());
    played = true;
  }
  const auto published = Read(metadata);
  CHECK(published.find("schema=3\n") != std::string::npos && published.find("game_root.unowned=1\n") != std::string::npos);
  CHECK(mode(game) == 0777 && Read(game / "GInput.asi") == "mod-plugin" && mode(game / "GInput.asi") == 0666);
  if (!played) {
    host.Play(220);
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
    if (done.play_refusals.count(220)) std::fprintf(stderr, "Play refused: %s\n", done.play_refusals.at(220).c_str());
    CHECK(done.handoff_ready && host.Handoff());
  }
  host.Shutdown();
}

// A kit without game-root-unowned (before 0.10) keeps the old rule: an owned, world-writable
// game folder is tightened to 0755 and published owned (metadata schema 2). A folder owned by
// another user is refused; that needs root to set up, so it isn't tested here.
void KitWithoutUnownedRootsKeepsTightening() {
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
  const auto kit = WithoutUnownedRoots(SyntheticKit(fixture.root / "kit"));
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  config.uuid = [] { return std::optional<std::string>("00000000-0000-4000-8000-000000000001"); };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0777) == 0);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(Hl2());
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);
  struct stat info{};
  CHECK(stat(game.c_str(), &info) == 0 && (info.st_mode & 07777) == 0755);
  const auto metadata = Read(fixture.root / "data/playden/games/steam-220/library/profiles/.playden-publication");
  CHECK(metadata.find("schema=2\n") != std::string::npos && metadata.find("game_root.unowned") == std::string::npos);
  host.Shutdown();
}

// The test kit plus what a fixture needs for preparations and settings updates.
RuntimeHostConfig DisplayConfig(Fixture& fixture, unsigned* uuids, GameSettingsRecord* saved) {
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
  config.uuid = [uuids]() -> std::optional<std::string> {
    ++*uuids; return "00000000-0000-4000-8000-" + std::string(12 - std::to_string(*uuids).size(), '0') + std::to_string(*uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.settings_record = [saved](const playden::GameId&) -> std::optional<GameSettingsRecord> { return *saved; };
  return config;
}

// A kit with 120 Hz output over a title still running an earlier kit's runtime: until Update, the title's profile
// has no frame_rate (its runtime would refuse the whole profile) and Frame limit stays DXVK's. Update alone
// publishes the saved settings again for the new runtime, and an Update back onto a kit without 120 Hz output
// takes them out again.
void StaleTitleWaitsForUpdateBefore120Hz() {
  Fixture fixture;
  unsigned uuids = 0;
  GameSettingsRecord saved;
  auto config = DisplayConfig(fixture, &uuids, &saved);
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  const auto profile = fixture.root / "data/playden/games/steam-220/library/profiles/playden-steam-220.profile";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  {
    auto kit = SyntheticKit(fixture.root / "kit");
    kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", "wow64_i386"};
    auto first = config;
    first.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    RuntimeHost host(first);
    CHECK(host.Start());
    host.Prepare(Hl2());
    const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
    }, 60);
    CHECK(ready.installs[0].title_current && ready.installs[0].runtime_current);
    host.Shutdown();
  }
  const auto update = [](RuntimeHost& host) {
    host.Prepare(Hl2());
    const auto result = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && (s.installs[0].title_current || s.refusals.count(220));
    }, 60);
    if (result.refusals.count(220)) std::fprintf(stderr, "Update refused: %s\n", result.refusals.at(220).c_str());
    CHECK(!result.refusals.count(220) && result.installs[0].runtime_current);
    return result;
  };
  {
    auto display = SyntheticKit(fixture.root / "kit-display", "native-2", "kit-1", 'c');
    display.capabilities = {"display-120hz", "dxvk", "offline_bottle", "per_game_title", "wow64_i386"};
    auto second = config;
    second.admit_kit = [display](std::string*) { return std::optional<RuntimeKit>(display); };
    RuntimeHost host(second);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(!started.installs[0].title_current && !started.installs[0].runtime_current);
    saved.revision = 1;
    saved.overrides = {{"refresh", "120"}, {"frame_limit", "60"}};
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(220, RuntimeMaintenance::kApplySettings);
    const auto applied = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
    if (!applied.maintenance.at(220).ok) std::fprintf(stderr, "stale apply: %s\n", applied.maintenance.at(220).message.c_str());
    CHECK(applied.maintenance.at(220).ok && applied.installs[0].settings_revision == 1);
    CHECK(Read(profile).find("frame_rate") == std::string::npos && Read(profile).find("refresh") == std::string::npos);
    CHECK(Read(game / "dxvk.conf").find("dxgi.maxFrameRate = 60") != std::string::npos);
    // Update alone rewrites them for the new runtime: the display paces the cap, DXVK no longer does.
    const auto updated = update(host);
    CHECK(updated.installs[0].settings_revision == 1);
    CHECK(Read(profile).find("\nrefresh = 120\nframe_rate = 60\n") != std::string::npos);
    CHECK(!fs::exists(game / "dxvk.conf") || Read(game / "dxvk.conf").find("maxFrameRate") == std::string::npos);
    host.Shutdown();
  }
  // Back to a kit revision without 120 Hz output: Update takes frame_rate out again, and DXVK caps the game.
  auto older = SyntheticKit(fixture.root / "kit-older", "native-3", "kit-1", 'e');
  older.capabilities = {"dxvk", "offline_bottle", "per_game_title", "wow64_i386"};
  config.admit_kit = [older](std::string*) { return std::optional<RuntimeKit>(older); };
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(!WaitFor(host, Started).installs[0].runtime_current);
    update(host);
    CHECK(Read(profile).find("frame_rate") == std::string::npos && Read(profile).find("refresh") == std::string::npos);
    CHECK(Read(game / "dxvk.conf").find("dxgi.maxFrameRate = 60") != std::string::npos);
    host.Shutdown();
  }
  // Title files that match the kit but runtime artifacts bound from another build: not this kit's runtime.
  const auto installs = fixture.root / "data/runtime-state/installs.json";
  auto records = Read(installs);
  const auto runtime_hash = *HashArtifactFile(fixture.root / "kit-older/runtime/wine/rev/lib/core.prx");
  const auto at = records.find(runtime_hash);
  CHECK(at != std::string::npos);
  records.replace(at, runtime_hash.size(), std::string(runtime_hash.size(), runtime_hash[0] == 'f' ? '0' : 'f'));
  Write(installs, records, static_cast<unsigned>(fs::status(installs).permissions()) & 0777u);
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto mismatched = WaitFor(host, Started);
  CHECK(mismatched.installs.size() == 1 && mismatched.installs[0].title_current && !mismatched.installs[0].runtime_current);
  host.Shutdown();
}

// Prepares HL2 on a kit without 120 Hz output and publishes saved 120 Hz settings there (revision 1), then returns
// the kit with 120 Hz output that the title is not updated to yet.
RuntimeKit PrepareStaleFor120Hz(Fixture& fixture, RuntimeHostConfig config, GameSettingsRecord* saved) {
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", "wow64_i386"};
  config.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  RuntimeHost host(config);
  CHECK(host.Start());
  host.Prepare(Hl2());
  WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);
  saved->revision = 1;
  saved->overrides = {{"refresh", "120"}, {"frame_limit", "60"}};
  const auto before = host.Snapshot().maintenance_handled;
  host.Maintain(220, RuntimeMaintenance::kApplySettings);
  const auto applied = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  CHECK(applied.maintenance.at(220).ok && applied.installs[0].settings_revision == 1);
  host.Shutdown();
  auto display = SyntheticKit(fixture.root / "kit-display", "native-2", "kit-1", 'c');
  display.capabilities = {"display-120hz", "dxvk", "offline_bottle", "per_game_title", "wow64_i386"};
  return display;
}

RuntimeSnapshot UpdateHl2(RuntimeHost& host) {
  const auto before = host.Snapshot().prepares_handled;
  host.Prepare(Hl2());
  return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.prepares_handled > before && !s.preparing_app; }, 60);
}

// An Update that fails after the title was refreshed (here its start check) is finished by the next Update, even
// though the title is current by then, after a Playden restart in between.
void UpdateFinishesAFailedRepublish() {
  Fixture fixture;
  unsigned uuids = 0;
  GameSettingsRecord saved;
  auto config = DisplayConfig(fixture, &uuids, &saved);
  const auto display = PrepareStaleFor120Hz(fixture, config, &saved);
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  const auto profile = fixture.root / "data/playden/games/steam-220/library/profiles/playden-steam-220.profile";
  config.admit_kit = [display](std::string*) { return std::optional<RuntimeKit>(display); };
  // A dxvk.conf Playden did not write: the title fails its start check after the refresh.
  const auto published_config = Read(game / "dxvk.conf");
  Write(game / "dxvk.conf", "dxgi.maxFrameRate = 50\n", 0644);
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    const auto failed = UpdateHl2(host);
    CHECK(failed.refusals.count(220) && failed.installs[0].runtime_current);
    CHECK(Read(profile).find("frame_rate") == std::string::npos);
    host.Shutdown();
  }
  Write(game / "dxvk.conf", published_config, 0644);
  const auto transactions = uuids;
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.installs[0].title_current);
  const auto done = UpdateHl2(host);
  if (done.refusals.count(220)) std::fprintf(stderr, "Second Update refused: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220) && !done.installs[0].recovery_pending && done.installs[0].settings_revision == 1);
  CHECK(Read(profile).find("\nrefresh = 120\nframe_rate = 60\n") != std::string::npos);
  // One publication in all: either the saved request resumed, or a new one when the first left none behind.
  CHECK(uuids - transactions <= 1);
  // Another Update has nothing left to publish.
  const auto again_transactions = uuids;
  const auto again = UpdateHl2(host);
  CHECK(again.refusals.count(220) && again.refusals.at(220) == "This game is already prepared.");
  CHECK(uuids == again_transactions && !again.installs[0].recovery_pending);
  host.Shutdown();
}

// Without the game's saved settings (missing or unreadable), Update publishes nothing: Playden defaults would replace
// what the title has.
void UpdateWithoutSavedSettingsKeepsThePublication() {
  Fixture fixture;
  unsigned uuids = 0;
  GameSettingsRecord saved;
  auto config = DisplayConfig(fixture, &uuids, &saved);
  const auto display = PrepareStaleFor120Hz(fixture, config, &saved);
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  const auto profile = fixture.root / "data/playden/games/steam-220/library/profiles/playden-steam-220.profile";
  const auto published = Read(profile);
  const auto config_before = Read(game / "dxvk.conf");
  config.admit_kit = [display](std::string*) { return std::optional<RuntimeKit>(display); };
  config.settings_record = [](const playden::GameId&) -> std::optional<GameSettingsRecord> { return std::nullopt; };
  const auto transactions = uuids;
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto done = UpdateHl2(host);
  CHECK(!done.refusals.count(220) && done.installs[0].runtime_current && done.installs[0].settings_revision == 1);
  CHECK(uuids == transactions && Read(profile) == published && Read(game / "dxvk.conf") == config_before);
  host.Shutdown();
}

// 120 Hz output and Frame limit 120 chosen while the title still runs the older runtime: saving them publishes nothing
// that runtime would refuse (no refresh, no frame_rate, and no DXVK cap at 120), and the Update before Play publishes
// them for the kit's runtime, so that same launch runs at 120 Hz.
void StaleTitleKeeps120HzChoicesForUpdate() {
  Fixture fixture;
  unsigned uuids = 0;
  GameSettingsRecord saved;
  auto config = DisplayConfig(fixture, &uuids, &saved);
  const auto display = PrepareStaleFor120Hz(fixture, config, &saved);
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  const auto profile = fixture.root / "data/playden/games/steam-220/library/profiles/playden-steam-220.profile";
  config.admit_kit = [display](std::string*) { return std::optional<RuntimeKit>(display); };
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(!started.installs[0].title_current && !started.installs[0].runtime_current);
  saved.revision = 2;
  saved.overrides = {{"refresh", "120"}, {"frame_limit", "120"}};
  const auto before = host.Snapshot().maintenance_handled;
  host.Maintain(220, RuntimeMaintenance::kApplySettings);
  const auto applied = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  if (!applied.maintenance.at(220).ok) std::fprintf(stderr, "stale apply: %s\n", applied.maintenance.at(220).message.c_str());
  CHECK(applied.maintenance.at(220).ok && applied.installs[0].settings_revision == 2);
  CHECK(Read(profile).find("frame_rate") == std::string::npos && Read(profile).find("refresh") == std::string::npos);
  CHECK(!fs::exists(game / "dxvk.conf") || Read(game / "dxvk.conf").find("maxFrameRate") == std::string::npos);
  const auto updated = UpdateHl2(host);
  if (updated.refusals.count(220)) std::fprintf(stderr, "Update refused: %s\n", updated.refusals.at(220).c_str());
  CHECK(!updated.refusals.count(220) && updated.installs[0].title_current && updated.installs[0].runtime_current);
  CHECK(updated.installs[0].settings_revision == 2);
  CHECK(Read(profile).find("\nrefresh = 120\nframe_rate = 120\n") != std::string::npos);
  CHECK(!fs::exists(game / "dxvk.conf") || Read(game / "dxvk.conf").find("maxFrameRate") == std::string::npos);
  host.Shutdown();
}

// A game prepared on a kit with 120 Hz output publishes its 120 Hz settings with the first profile.
void FreshPrepareOn120HzKitPublishesIt() {
  Fixture fixture;
  unsigned uuids = 0;
  GameSettingsRecord saved;
  saved.overrides = {{"refresh", "120"}, {"frame_limit", "40"}};
  auto config = DisplayConfig(fixture, &uuids, &saved);
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"display-120hz", "dxvk", "offline_bottle", "per_game_title", "wow64_i386"};
  config.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  RuntimeHost host(config);
  CHECK(host.Start());
  host.Prepare(Hl2());
  const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  CHECK(!ready.refusals.count(220) && ready.installs[0].runtime_current);
  const auto profile = Read(fixture.root / "data/playden/games/steam-220/library/profiles/playden-steam-220.profile");
  CHECK(profile.find("\nrefresh = 120\nframe_rate = 40\n") != std::string::npos);
  host.Shutdown();
}

// A kit respin while a settings update waits for recovery (#5): the title is still this
// install's, so Update first finishes the saved update, and the title only reports that
// it needs Update (not current) until a second Update refreshes it.
void KitRespinKeepsWaitingSettingsUpdate() {
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
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  bool loosen_next = false;  // interrupts the next armed update before its capture
  config.log = [&loosen_next, game](const std::string& line) {
    if (loosen_next && line.rfind("settings update request", 0) == 0 && line.find(" state=0 ") != std::string::npos) {
      loosen_next = false;
      CHECK(chmod(game.c_str(), 0777) == 0);
    }
  };
  GameSettingsRecord saved;
  config.settings_record = [&saved](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  {
    auto kit = SyntheticKit(fixture.root / "kit");
    kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
    auto first = config;
    first.admit_kit = [kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    RuntimeHost host(first);
    CHECK(host.Start());
    host.Prepare(Hl2());
    const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
    }, 60);
    CHECK(ready.installs[0].title_current);
    saved.revision = 1;
    saved.overrides = {{"frame_limit", "30"}};
    loosen_next = true;
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(220, RuntimeMaintenance::kApplySettings);
    const auto interrupted = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
    CHECK(!interrupted.maintenance.at(220).ok && interrupted.installs[0].recovery_pending);
    host.Shutdown();
  }
  CHECK(chmod(game.c_str(), 0755) == 0);
  auto respun = SyntheticKit(fixture.root / "kit-respun", "native-2", "kit-1", 'c');
  respun.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [respun](std::string*) { return std::optional<RuntimeKit>(respun); };
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(!started.installs[0].title_current && started.installs[0].recovery_pending);
  host.Prepare(Hl2());
  auto result = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (!s.installs[0].recovery_pending || s.refusals.count(220));
  }, 60);
  if (result.refusals.count(220)) std::fprintf(stderr, "Resume after respin refused: %s\n", result.refusals.at(220).c_str());
  CHECK(!result.refusals.count(220) && result.installs[0].settings_revision == 1);
  CHECK(Read(game / "dxvk.conf").find("maxFrameRate = 30") != std::string::npos);
  CHECK(!result.installs[0].title_current);  // needs Update, not recovery
  host.Prepare(Hl2());
  result = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && (s.installs[0].title_current || s.refusals.count(220));
  }, 60);
  CHECK(!result.refusals.count(220) && result.installs[0].title_current);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/eboot.bin") == "native-2");
  host.Shutdown();
}

// Uninstall removes the game's PS5 title: the title folder leaves ShadowMount's
// scan root, the title is unregistered through the remover, the bottle is kept
// (or deleted on request) and the record goes once the console no longer lists
// the title. Every step is journaled and continues after a failure or restart.
// A game installed again on another drive (its uninstall stopped part way and left the title):
// Prepare removes the old title, keeping its saves, and prepares the game where it is now.
void PreparesGameInstalledAgainElsewhere() {
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
  config.removal_confirm_polls = 3;
  config.removal_poll_interval = std::chrono::milliseconds(1);
  std::vector<std::string> removed_titles;
  bool remover_works = false;  // silent at first: the console never confirms
  config.unregister_title = [&fixture, &removed_titles, &remover_works](const std::string& title, const std::string&) {
    removed_titles.push_back(title);
    if (!remover_works) return TitleRemoverResult{true, std::nullopt, "no answer"};
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  const auto bottle = fixture.root / "data/playden/games/steam-220";
  const auto save = fs::path("library/prefixes/game/drive_c/users/steamuser/AppData/Roaming/Game/save.dat");
  const auto prepare = [](RuntimeHost& host, const RuntimePrepareRequest& request) {
    const auto before = host.Snapshot().prepares_handled;
    host.Prepare(request);
    const auto done = WaitFor(host, [before](const RuntimeSnapshot& s) {
      return !s.preparing_app && s.prepares_handled > before;
    }, 60);
    if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
    return done;
  };
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  auto first = prepare(host, Hl2());
  CHECK(!first.refusals.count(220) && first.installs.size() == 1 &&
        first.installs[0].install_path == "/data/homebrew/FAKE77031/games/220");
  Write(bottle / save, "player-save", 0600);
  // The download went away with an uninstall that stopped; the game is on the USB drive now.
  fs::remove_all(fixture.root / "data/homebrew/FAKE77031/games/220");
  auto elsewhere = Hl2();
  elsewhere.install_path = "/mnt/usb0/playden/games/220";
  elsewhere.volume_id = "usb0";
  // Not there yet: nothing is removed.
  auto refused = prepare(host, elsewhere);
  CHECK(refused.refusals.count(220) && refused.refusals.at(220).find("missing") != std::string::npos);
  CHECK(removed_titles.empty() && refused.installs.size() == 1);
  const auto usb = fixture.root / "mnt/usb0/playden/games/220";
  Write(usb / "hl2.exe", "existing-game", 0644);
  for (const auto& folder : {fixture.root / "mnt", fixture.root / "mnt/usb0", fixture.root / "mnt/usb0/playden",
                             fixture.root / "mnt/usb0/playden/games", usb})
    CHECK(chmod(folder.c_str(), 0755) == 0);
  // The console doesn't confirm the old title is gone: the move waits, recorded as a move, so the
  // game page offers to finish it rather than an uninstall.
  const auto stopped = prepare(host, elsewhere);
  CHECK(stopped.refusals.count(220) && stopped.refusals.at(220).find("Play tries again") != std::string::npos);
  CHECK(stopped.installs.size() == 1 && stopped.installs[0].state == PreparationState::kRemoving &&
        stopped.installs[0].moving);
  remover_works = true;
  const auto moved = prepare(host, elsewhere);
  CHECK(!moved.refusals.count(220) && moved.installs.size() == 1 && !moved.installs[0].moving);
  CHECK(moved.installs[0].install_path == "/mnt/usb0/playden/games/220");
  CHECK(removed_titles.size() == 2 && removed_titles[1] == first.installs[0].title_id);
  CHECK(Read(bottle / save) == "player-save" && moved.kept_bottles.empty());
  // The move brought the saves back and says so in the bottle.
  CHECK(fs::exists(bottle / ".playden-saves-restored"));
  // A kept copy whose saves never came back (a preparation stopped before that, so no mark): an
  // uninstall keeping saves leaves it as it is and sets the newer bottle aside beside it.
  fs::remove(bottle / ".playden-saves-restored");
  const auto kept = fixture.root / "data/playden/games/steam-220.kept";
  Write(kept / save, "original-save", 0600);
  const auto before = host.Snapshot().removals_handled;
  host.Remove(220, false);
  const auto removed = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.removals_handled > before; }, 30);
  CHECK(!removed.removal_failures.count(220) && removed.installs.empty());
  CHECK(Read(kept / save) == "original-save" && !fs::exists(bottle));
  bool aside = false;
  for (const auto& entry : fs::directory_iterator(fixture.root / "data/playden/games"))
    aside |= entry.path().filename().string().rfind("steam-220.set-aside-", 0) == 0 &&
             Read(entry.path() / save) == "player-save";
  CHECK(aside);
  host.Shutdown();
}

// A game from another store gets its Steamworks app ID from its own steam_appid.txt (the program's folder,
// then the game folder), else from Game settings, else none (it keeps its own Steam DLL). Its runtime number
// is never one, the account follows the Steam session, and Playden's own steam_appid.txt never stands in for
// the game's.
void LocalGameSteamworksAppId() {
  const auto local = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
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
  config.steamworks_dir = (fs::path(g_runtime_dir) / "steamworks").string();
  std::mutex log_mutex;
  std::vector<std::string> lines;
  config.log = [&](const std::string& line) { std::lock_guard<std::mutex> lock(log_mutex); lines.push_back(line); };
  const auto logged = [&](const std::string& prefix) {
    std::lock_guard<std::mutex> lock(log_mutex);
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
  };
  std::mutex settings_mutex;
  GameSettingsRecord saved;
  config.settings_record = [&](const playden::GameId& game) -> std::optional<GameSettingsRecord> {
    std::lock_guard<std::mutex> lock(settings_mutex);
    CHECK(game == local);
    return saved;
  };
  const auto set_field = [&](std::uint32_t app) { std::lock_guard<std::mutex> lock(settings_mutex); saved.steam_app_id = app; };
  const auto game = fixture.root / "data/playden-import/Imported";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(game / "Bin/Game.exe", pe, 0644);
  const std::string own_api = pe + std::string("\0SteamUser021\0", 14);
  Write(game / "Bin/steam_api64.dll", own_api, 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  const auto emulator = Read(fs::path(g_runtime_dir) / "steamworks/steam_api64.dll");
  const auto settings_app = [&] { return Read(game / "Bin/steam_settings/steam_appid.txt"); };
  const auto account = [&] {
    const auto user = Read(game / "Bin/steam_settings/configs.user.ini");
    const auto at = user.find("account_name=");
    return at == std::string::npos ? std::string() : user.substr(at + 13, user.find('\n', at) - at - 13);
  };

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  const auto number = *host.RuntimeNumberFor(local);
  CHECK(number >= 0xF0000000u);
  // No steam_appid.txt and no app ID in Game settings: no emulation, the game's own DLL stays. No Steam
  // session is needed for any of it.
  RuntimePrepareRequest request{number, "/data/playden-import/Imported", "internal", "Imported"};
  request.launches = {{"", "", "Bin/Game.exe", "", ""}};
  request.game = local;
  host.Prepare(request);
  const auto ready = WaitFor(host, [number](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(number) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (ready.refusals.count(number)) std::fprintf(stderr, "Prepare refused: %s\n", ready.refusals.at(number).c_str());
  CHECK(!ready.refusals.count(number) && ready.installs.size() == 1);
  CHECK(Read(game / "Bin/steam_api64.dll") == own_api && !fs::exists(game / "Bin/steam_api64.dll.orig"));
  CHECK(ready.steamworks_apps.at(number) == SteamworksAppView{});
  CHECK(logged("steamworks app=" + std::to_string(number) + " steam_app_id=- source=none"));
  CHECK(logged("steamworks app=" + std::to_string(number) + " mode=game found=1 changed=-"));

  // Each call applies the saved settings again, as Play does before the game starts.
  const auto apply = [&]() {
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(number, RuntimeMaintenance::kApplySettings);
    const auto done = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; });
    if (!done.maintenance.at(number).ok) std::fprintf(stderr, "Apply: %s\n", done.maintenance.at(number).message.c_str());
    CHECK(done.maintenance.at(number).ok);
    return done.steamworks_apps.at(number);
  };
  using Source = SteamworksAppView::Source;
  // The app ID from Game settings only: emulated with it, signed out (the emulator's default account).
  // No steam_appid.txt of Playden's beside the DLL; the emulator's own copy has the app ID.
  set_field(4000);
  CHECK((apply() == SteamworksAppView{4000, Source::kSettings, false}));
  CHECK(Read(game / "Bin/steam_api64.dll") == emulator && Read(game / "Bin/steam_api64.dll.orig") == own_api);
  CHECK(settings_app() == "4000" && !fs::exists(game / "Bin/steam_appid.txt") && account() == "Playden");
  // A new app ID in Game settings reaches the emulator.
  set_field(4001);
  CHECK((apply() == SteamworksAppView{4001, Source::kSettings, false}) && settings_app() == "4001");
  // Signed in, the emulator names the account; signed out again, the default.
  host.SetSteamAccount("player", 76561198000000001ull);
  apply();
  CHECK(account() == "player");
  host.SetSteamAccount({}, 0);
  apply();
  CHECK(account() == "Playden");
  // The game's own file in its folder wins over Game settings, which shows as overridden.
  Write(game / "steam_appid.txt", "99700\n", 0644);
  CHECK((apply() == SteamworksAppView{99700, Source::kGameFile, true}) && settings_app() == "99700");
  CHECK(Read(game / "steam_appid.txt") == "99700\n");  // the game's own stays
  // One in the program's folder comes first; whitespace around the number is fine.
  Write(game / "Bin/steam_appid.txt", " 99701\r\n", 0644);
  CHECK((apply() == SteamworksAppView{99701, Source::kGameFile, true}) && settings_app() == "99701");
  CHECK(Read(game / "Bin/steam_appid.txt") == " 99701\r\n");
  // A malformed one (a leading zero, words, zero, beyond 32 bits) is not an app ID: the next place is asked.
  for (const char* malformed : {"099701", "app 99701", "0", "4294967296", "-5", ""}) {
    Write(game / "Bin/steam_appid.txt", malformed, 0644);
    CHECK((apply() == SteamworksAppView{99700, Source::kGameFile, true}));
  }
  // The same app ID as Game settings is no conflict.
  set_field(99700);
  CHECK((apply() == SteamworksAppView{99700, Source::kGameFile, false}));
  // Neither a file nor the field: the game's own Steam DLL comes back.
  fs::remove(game / "steam_appid.txt");
  set_field(0);
  CHECK(apply() == SteamworksAppView{});
  CHECK(Read(game / "Bin/steam_api64.dll") == own_api);
  CHECK(logged("steamworks app=" + std::to_string(number) + " mode=game found=1 changed=Bin/steam_api64.dll"));
  // The runtime number never reached the emulator.
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    CHECK(std::none_of(lines.begin(), lines.end(), [&](const std::string& line) {
      return line.find("steam_app_id=" + std::to_string(number)) != std::string::npos;
    }));
  }
  host.Shutdown();
}

// Kept bottles are not Steam-only: an imported game's marker names its game, lists after a restart
// and gives the game its runtime number back. A Steam marker from before (fixture) still lists.
// A first number for a Local game is written to installs.json, which takes the process-wide
// effective-UID hold. The worker keeps that hold around its whole startup (and each batch) while
// using the repository: a lookup that took the repository's lock first and then waited for the hold
// stopped both threads for good (Playden froze on its first frame with a copy request in
// dev/install-request.txt). The lookup takes the hold first and waits for the worker, or, asked not
// to wait, says the worker is busy and gives nothing out.
void RuntimeNumberWaitsForTheWorkersHold() {
  const auto local_a = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
  const auto local_b = playden::GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
  Fixture fixture;
  auto config = fixture.Config();
  // The console's hold: one recursive lock shared with every durable write.
  config.hold_effective_uid = playden::ps5::HoldEffectiveUid;
  config.try_hold_effective_uid = [](bool* busy) {
    auto held = playden::ps5::TryHoldEffectiveUid();
    *busy = !held;
    return held;
  };
  // The worker stops inside its startup, hold taken and records shared, until the test lets it go.
  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool gated = true, at_gate = false;
  config.log = [&](const std::string& line) {
    if (line.rfind("startup records loaded", 0) != 0) return;
    std::unique_lock<std::mutex> lock(gate_mutex);
    at_gate = true;
    gate_changed.notify_all();
    gate_changed.wait(lock, [&] { return !gated; });
  };
  RuntimeHost host(std::move(config));
  CHECK(host.Start());
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    CHECK(gate_changed.wait_for(lock, std::chrono::seconds(10), [&] { return at_gate; }));
  }
  // Each lookup runs on its own thread: one that never answers (the lookup inside the repository,
  // the worker waiting for it) fails the test instead of stopping it.
  struct Answer { std::optional<std::uint32_t> number; bool busy = false; };
  const auto ask = [&host](const playden::GameId& game, bool wait) {
    return std::async(std::launch::async, [&host, game, wait] {
      Answer answer;
      answer.number = host.RuntimeNumberFor(game, wait ? nullptr : &answer.busy);
      return answer;
    });
  };
  const auto answered = [](std::future<Answer>& answer, int seconds) {
    return answer.wait_for(std::chrono::seconds(seconds)) == std::future_status::ready;
  };
  // Asked not to wait (the application thread, every frame): busy, nothing given out, no number taken.
  auto at_once = ask(local_a, false);
  CHECK(answered(at_once, 20));
  const auto refused = at_once.get();
  CHECK(!refused.number && refused.busy);
  auto steam = ask(playden::GameId::Steam(220), false);
  CHECK(answered(steam, 20));
  const auto steam_answer = steam.get();
  CHECK(steam_answer.number == 220u && !steam_answer.busy);
  // Asked to wait (the Local store's worker): the number arrives once the worker's startup is over.
  auto waiting = ask(local_a, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(!answered(waiting, 0));  // still waiting
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    gated = false;
  }
  gate_changed.notify_all();
  CHECK(answered(waiting, 20));
  const auto given = waiting.get();
  CHECK(given.number == 0xF0000000u && !given.busy);
  CHECK(WaitFor(host, Started).status == RuntimeHostStatus::kReady);
  CHECK(host.Snapshot().runtime_games.at(0xF0000000u) == local_a);
  bool busy = false;
  CHECK(host.RuntimeNumberFor(local_a, &busy) == 0xF0000000u && !busy);  // known: no hold needed
  // Idle, the worker holds nothing: a new number is given out at once.
  std::optional<std::uint32_t> second;
  for (int i = 0; i < 2500 && !second; ++i) {
    second = host.RuntimeNumberFor(local_b, &busy);
    if (!second) { CHECK(busy); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
  }
  CHECK(second == 0xF0000001u);
  host.Shutdown();
}

void LocalGameKeepsItsBottle() {
  const auto local = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
  const auto other = playden::GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
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
  config.removal_confirm_polls = 3;
  config.removal_poll_interval = std::chrono::milliseconds(1);
  std::vector<playden::GameId> icons;
  config.title_icon = [&icons](const playden::GameId& game) -> std::optional<std::string> {
    icons.push_back(game);
    return std::nullopt;
  };
  config.unregister_title = [&fixture](const std::string&, const std::string&) {
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  const auto game = fixture.root / "data/playden-import/Imported";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(game / "Bin/Game.exe", pe, 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  const auto games = fixture.root / "data/playden/games";
  const auto steam_kept = games / "steam-220.kept";
  Write(steam_kept / "playden-kept.json",
        Read(fs::path(g_runtime_dir) / "../tests/fixtures/identity/playden-kept.json"), 0600);
  const auto kept = games / "local-0f8fad5b-d9cb-469f-a165-70867728950e.kept";
  const auto save = fs::path("library/prefixes/game/drive_c/users/steamuser/save.dat");
  std::uint32_t number = 0;
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(started.kit_available && started.kept_bottles.size() == 1);
    CHECK(started.kept_bottles[0].game == playden::GameId::Steam(220) && started.kept_bottles[0].runtime_number == 220);
    CHECK(started.kept_bottles[0].name == "Half-Life 2" && started.kept_bottles[0].bytes == 4096);
    number = *host.RuntimeNumberFor(local);
    CHECK(number == 0xF0000000u);
    RuntimePrepareRequest request{number, "/data/playden-import/Imported", "internal", "Imported"};
    request.launches = {{"", "", "Bin/Game.exe", "", ""}};
    request.game = local;
    host.Prepare(request);
    const auto ready = WaitFor(host, [number](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(number) ||
          (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (ready.refusals.count(number)) std::fprintf(stderr, "Prepare refused: %s\n", ready.refusals.at(number).c_str());
    CHECK(!ready.refusals.count(number) && ready.installs.size() == 1 && ready.installs[0].game == local);
    CHECK(ready.installs[0].install_id == "local-0f8fad5b-d9cb-469f-a165-70867728950e");
    CHECK(std::find(icons.begin(), icons.end(), local) != icons.end());  // named by game, not by number
    CHECK(ready.generated_recipes.size() == 1 && ready.generated_recipes[0].id == playden::GeneratedRecipeId(local) &&
          playden::runtime::RecipeGame(ready.generated_recipes[0]) == local);
    // A store's name in the title name (any store's) is not the game's: the kept bottle names the game.
    host.SetTitleNames({{number, "Imported (Epic)"}});
    Write(games / "local-0f8fad5b-d9cb-469f-a165-70867728950e" / save, "player-save", 0600);
    const auto before = host.Snapshot().removals_handled;
    host.Remove(number, false);
    const auto removed = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.removals_handled > before; }, 30);
    if (removed.removal_failures.count(number)) std::fprintf(stderr, "Remove: %s\n", removed.removal_failures.at(number).c_str());
    CHECK(removed.installs.empty() && removed.kept_bottles.size() == 2);
    const auto marker = Read(kept / "playden-kept.json");
    CHECK(marker.find("{\"schema\":1,\"install_id\":\"local-0f8fad5b-d9cb-469f-a165-70867728950e\",\"app_id\":4026531840,"
                      "\"store\":\"local\",\"game\":\"0f8fad5b-d9cb-469f-a165-70867728950e\",\"name\":\"Imported\",") == 0);
    host.Shutdown();
  }
  {
    // After a restart the kept bottle lists with its game, which gets its number back.
    RuntimeHost host(config);
    CHECK(!host.RuntimeNumberFor(local));
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(started.kept_bottles.size() == 2);
    const auto found = std::find_if(started.kept_bottles.begin(), started.kept_bottles.end(),
                                    [&](const KeptBottleView& view) { return view.game == local; });
    CHECK(found != started.kept_bottles.end() && found->runtime_number == number && found->name == "Imported");
    CHECK(started.runtime_games.at(number) == local);
    CHECK(host.RuntimeNumberFor(local) == number);
    const auto next = host.RuntimeNumberFor(other);
    CHECK(next && *next != number && *next >= 0xF0000000u);
    CHECK(Read(kept / save) == "player-save");
    // Deleted by its number: only its own folder goes.
    host.DeleteKeptBottle(number);
    const auto deleted = WaitFor(host, [](const RuntimeSnapshot& s) { return s.kept_bottles.size() == 1; });
    CHECK(deleted.kept_bottles[0].game == playden::GameId::Steam(220));
    CHECK(!fs::exists(kept) && fs::exists(steam_kept));
    host.Shutdown();
  }
  // A marker whose game doesn't match its folder, or a Local number outside the range, is not listed.
  Write(kept / "playden-kept.json",
        "{\"schema\":1,\"install_id\":\"local-0f8fad5b-d9cb-469f-a165-70867728950e\",\"app_id\":220,"
        "\"store\":\"local\",\"game\":\"0f8fad5b-d9cb-469f-a165-70867728950e\",\"name\":\"X\",\"kept_at\":1,\"bytes\":1}\n", 0600);
  const auto mismatch = games / "local-7c9e6679-7425-40de-944b-e07fc1f90ae7.kept";
  Write(mismatch / "playden-kept.json",
        "{\"schema\":1,\"install_id\":\"local-7c9e6679-7425-40de-944b-e07fc1f90ae7\",\"app_id\":4026531841,"
        "\"store\":\"local\",\"game\":\"0f8fad5b-d9cb-469f-a165-70867728950e\",\"name\":\"X\",\"kept_at\":1,\"bytes\":1}\n", 0600);
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kept_bottles.size() == 1);
    host.Shutdown();
  }
}

void RemovesTitleKeepingSaves() {
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
  config.removal_confirm_polls = 3;
  config.removal_poll_interval = std::chrono::milliseconds(1);
  // The remover: silent at first (the console never drops the title), then working.
  auto remover_works = std::make_shared<std::atomic<bool>>(false);
  auto remover_calls = std::make_shared<std::atomic<int>>(0);
  config.unregister_title = [&fixture, remover_works, remover_calls](const std::string& title, const std::string& nonce) {
    ++*remover_calls;
    CHECK(title == "FAKE78001" && nonce.size() == 36);
    if (!*remover_works) return TitleRemoverResult{true, std::nullopt, "no answer"};
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  const auto bottle = fixture.root / "data/playden/games/steam-220";
  const auto kept = fixture.root / "data/playden/games/steam-220.kept";
  const auto save = fs::path("library/prefixes/game/drive_c/users/steamuser/AppData/Roaming/Game/save.dat");
  const auto prepare = [](RuntimeHost& host) {
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) ||
          (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
    CHECK(!done.refusals.count(220) && done.installs.size() == 1);
    return done;
  };
  const auto remove = [](RuntimeHost& host, bool delete_saves) {
    const auto before = host.Snapshot().removals_handled;
    host.Remove(220, delete_saves);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.removals_handled > before; }, 30);
  };
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    CHECK(prepare(host).prepared.size() == 1);
    host.SetTitleNames({{220, "Half-Life 2"}});
    Write(bottle / save, "player-save", 0600);
    for (const auto* part : {"library/prefixes/game/drive_c/users", "library/prefixes/game/drive_c/users/steamuser",
                             "library/prefixes/game/drive_c/users/steamuser/AppData",
                             "library/prefixes/game/drive_c/users/steamuser/AppData/Roaming",
                             "library/prefixes/game/drive_c/users/steamuser/AppData/Roaming/Game"})
      CHECK(chmod((bottle / part).c_str(), 0700) == 0);

    // A running title is never removed.
    fixture.activity->store(static_cast<int>(NativeActivity::kActive));
    auto removed = remove(host, false);
    CHECK(removed.removal_failures.at(220) == "Close the game before uninstalling it.");
    CHECK(removed.installs[0].state == PreparationState::kReady && *remover_calls == 0);
    fixture.activity->store(static_cast<int>(NativeActivity::kStopped));

    // The remover is silent: the folder already left the scan root, nothing is
    // recorded as failed by the console, and the record waits at "unregister".
    removed = remove(host, false);
    CHECK(removed.removal_failures.at(220).find("has not confirmed") != std::string::npos);
    CHECK(removed.installs.size() == 1 && removed.installs[0].state == PreparationState::kRemoving);
    CHECK(removed.installs[0].step == "unregister");
    CHECK(removed.prepared.empty());  // no longer Ready
    CHECK(!fs::exists(fixture.root / "data/homebrew/FAKE78001"));
    CHECK(fs::is_directory(fixture.root / "data/runtime-state/trash/FAKE78001-00000000-0000-4000-8000-000000000002"));
    CHECK(*remover_calls == 1);
    // Preparation waits for the removal to finish.
    host.Prepare(Hl2());
    const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(220) != 0; });
    CHECK(refused.refusals.at(220).find("still being removed") != std::string::npos);
    host.Shutdown();
  }
  {
    // After a restart the removal is reported, not resumed, until asked. Asked again,
    // it continues with the first choice (keep saves) even when asked to delete them.
    RuntimeHost host(config);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(started.installs.size() == 1 && started.installs[0].state == PreparationState::kRemoving);
    CHECK(started.installs[0].failure.find("has not confirmed") != std::string::npos);
    CHECK(!fs::exists(fixture.root / "data/runtime-state/trash/FAKE78001-00000000-0000-4000-8000-000000000002"));
    // The PS5 title carries the store name; the kept saves are named for the game.
    host.SetTitleNames({{220, "Half-Life 2 (Steam)"}});
    // A read-only games folder stops the bottle step; the next attempt continues there.
    const auto games = fixture.root / "data/playden/games";
    CHECK(chmod(games.c_str(), 0500) == 0);
    *remover_works = true;
    auto removed = remove(host, true);
    CHECK(chmod(games.c_str(), 0700) == 0);
    CHECK(removed.installs.size() == 1 && removed.installs[0].step == "bottle");
    CHECK(removed.removal_failures.at(220).find("could not be kept") != std::string::npos);
    CHECK(!fixture.registered && *remover_calls == 2);
    removed = remove(host, true);
    CHECK(!removed.removal_failures.count(220) && removed.installs.empty());
    CHECK(*remover_calls == 2);  // the console no longer listed the title: never sent again
    CHECK(!fs::exists(bottle) && Read(kept / save) == "player-save");
    CHECK(removed.kept_bottles.size() == 1 && removed.kept_bottles[0].runtime_number == 220);
    CHECK(removed.kept_bottles[0].name == "Half-Life 2" && removed.kept_bottles[0].kept_at == 1760000000);
    CHECK(removed.kept_bottles[0].bytes > 0);
    CHECK(Read(kept / "playden-kept.json").find("\"install_id\":\"steam-220\"") != std::string::npos);
    CHECK(Read(game / "hl2.exe") == "existing-game");  // the download is the controller's to remove
    // The kept bottle says where it is, and its Windows user folders can be measured (the Saves tab).
    CHECK(removed.kept_bottles[0].data_root.size() > 14 &&
          removed.kept_bottles[0].data_root.compare(removed.kept_bottles[0].data_root.size() - 14, 14, "steam-220.kept") == 0);
    host.MeasureSaves();
    const auto measured = WaitFor(host, [](const RuntimeSnapshot& s) { return s.save_bytes.count(220) > 0; });
    CHECK(measured.save_bytes.at(220) >= std::string("player-save").size());

    // Prepared again, the game gets its saves back and the kept copy goes.
    const auto ready = prepare(host);
    CHECK(ready.kept_bottles.empty() && !fs::exists(kept));
    CHECK(!ready.work_progress);  // the copy's progress ends with the preparation
    CHECK(Read(bottle / save) == "player-save");
    struct stat info{};
    CHECK(stat((bottle / "library/prefixes/game/drive_c/users/steamuser").c_str(), &info) == 0 &&
          (info.st_mode & 0777) == 0700);

    // Deleting saves removes the bottle without following its links.
    const auto outside = fixture.root / "outside";
    Write(outside / "keep.txt", "not the bottle's", 0644);
    fs::create_directories(bottle / "library/prefixes/game/dosdevices");
    fs::create_directory_symlink(outside, bottle / "library/prefixes/game/dosdevices/z:");
    removed = remove(host, true);
    CHECK(!removed.removal_failures.count(220) && removed.installs.empty());
    CHECK(!fs::exists(bottle) && !fs::exists(kept) && removed.kept_bottles.empty());
    CHECK(!removed.work_progress);  // the deletion's progress ends with it
    CHECK(Read(outside / "keep.txt") == "not the bottle's");

    // A kept bottle can be deleted on its own.
    prepare(host);
    removed = remove(host, false);
    CHECK(removed.kept_bottles.size() == 1);
    host.DeleteKeptBottle(220);
    WaitFor(host, [](const RuntimeSnapshot& s) { return s.kept_bottles.empty(); });
    CHECK(!fs::exists(kept));

    // A removal whose record cannot be written (a full drive) changes nothing, and the next
    // one reads the records again instead of taking the empty list for "no PS5 title".
    prepare(host);
    CHECK(fixture.registered);
    const auto records = config.state_root / "installs.json";
    fs::rename(records, config.state_root / "installs.aside");
    fs::create_directory(records);  // the record can be neither replaced nor read
    removed = remove(host, false);
    CHECK(removed.removal_failures.at(220).find("could not be recorded") != std::string::npos);
    CHECK(fixture.registered && fs::is_directory(bottle));
    // The empty list is published as unknown, so an uninstall still asks for the removal.
    CHECK(removed.installs.empty() && !removed.installs_known);
    removed = remove(host, false);
    CHECK(removed.removal_failures.at(220).find("cannot be read") != std::string::npos);
    CHECK(fixture.registered && fs::is_directory(bottle));
    fs::remove(records);
    fs::rename(config.state_root / "installs.aside", records);
    removed = remove(host, false);
    CHECK(!removed.removal_failures.count(220) && removed.installs.empty() && removed.installs_known);
    CHECK(!fixture.registered && !fs::exists(bottle) && fs::is_directory(kept));
    host.Shutdown();
  }
}

// A game on a drive that records no file owners (exFAT) keeps its folder as it
// is. Its publication says so (metadata schema 3), and only a kit that can start
// games from such drives prepares it.
void PreparesGameOnUnownedDrive() {
  playden::platform::SetOwnershipProbeForTesting([](const fs::path&) { return std::optional<bool>(false); });
  for (const bool capable : {false, true}) {
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
    auto kit = capable ? SyntheticKit(fixture.root / "kit") : WithoutUnownedRoots(SyntheticKit(fixture.root / "kit"));
    config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
    unsigned uuids = 0;
    config.uuid = [&uuids]() -> std::optional<std::string> {
      ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
    };
    config.unix_time = [] { return std::int64_t{1760000000}; };
    const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
    Write(game / "hl2.exe", "existing-game", 0644);
    CHECK(chmod(game.c_str(), 0777) == 0);  // what exFAT reports for every folder

    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) ||
          (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    struct stat game_info{};
    CHECK(stat(game.c_str(), &game_info) == 0 && (game_info.st_mode & 07777) == 0777);
    if (!capable) {
      CHECK(done.refusals.count(220) && done.refusals.at(220).find("doesn't keep file owners") != std::string::npos);
      CHECK(done.installs.empty());
    } else {
      if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
      if (done.refusals.count(220)) std::fprintf(stderr, "refusal: %s\n", done.refusals.at(220).c_str());
  CHECK(!done.refusals.count(220));
      CHECK(done.installs.size() == 1 && done.installs[0].state == PreparationState::kReady);
      const auto metadata = Read(fixture.root / "data/playden/games/steam-220/library/profiles/.playden-publication");
      CHECK(metadata.find("schema=3\n") != std::string::npos);
      CHECK(metadata.find("game_root.unowned=1\n") != std::string::npos);
    }
    host.Shutdown();
  }
  playden::platform::SetOwnershipProbeForTesting(nullptr);
}

// Selaco (issue 52): Steam lists only .bat files, so Playden refuses it, but the program the
// player picks in Game settings prepares and starts; later settings updates keep that program.
void SteamEntryWithoutFolderStartsInInstallRoot() {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/646570";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  // Slay the Spire: Steam starts jre/bin/javaw.exe -jar desktop-1.0.jar in the install root.
  Write(game / "jre/bin/javaw.exe", pe, 0644);
  Write(game / "desktop-1.0.jar", "jar", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0 && chmod((game / "jre").c_str(), 0755) == 0 && chmod((game / "jre/bin").c_str(), 0755) == 0);
  GameSettingsRecord saved;
  config.settings_record = [&](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  RuntimePrepareRequest request{646570, "/data/homebrew/FAKE77031/games/646570", "internal", "Slay the Spire"};
  request.launches = {{"", "Play", "jre\\bin\\javaw.exe", "", "-jar desktop-1.0.jar"}};
  const auto profile = fixture.root / "data/playden/games/steam-646570/library/profiles/playden-steam-646570.profile";
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(request);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);
  if (done.refusals.count(646570)) std::fprintf(stderr, "refused: %s\n", done.refusals.at(646570).c_str());
  CHECK(!done.installs.empty() && done.installs[0].launch_in_install_root && done.installs[0].launch_working_directory.empty());
  CHECK(Read(profile).find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\646570\n") != std::string::npos);
  // The program's own folder, chosen in Game settings, reaches the title and the record; "." goes back to the game folder.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  const auto apply = [&](std::uint64_t revision) {
    saved.revision = revision;
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(646570, RuntimeMaintenance::kApplySettings);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  };
  saved.overrides = {{"program_folder", "jre/bin"}};
  auto applied = apply(1);
  if (!applied.maintenance.at(646570).ok) std::fprintf(stderr, "apply: %s\n", applied.maintenance.at(646570).message.c_str());
  CHECK(applied.maintenance.at(646570).ok && !applied.installs[0].launch_in_install_root &&
        applied.installs[0].launch_working_directory == "jre/bin");
  CHECK(Read(profile).find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\646570\\jre\\bin\n") != std::string::npos);
  saved.overrides = {{"program_folder", "."}};
  applied = apply(2);
  CHECK(applied.maintenance.at(646570).ok && applied.installs[0].launch_in_install_root &&
        applied.installs[0].launch_working_directory.empty());
  CHECK(Read(profile).find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\646570\n") != std::string::npos);
  host.Shutdown();
}

// Steam's options all for other branches (Monster Boy lists x64/Launcher64.exe and x86/Launcher.exe
// for a debug branch): preparing waits for the player's choice when two of their programs are in the
// folder, then starts the chosen one as Steam's option. One clear program is used without asking.
void PreparesSteamProgramChosenAfterDownload() {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/449610";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  std::filesystem::create_directories(game / "x64");
  std::filesystem::create_directories(game / "x86");
  Write(game / "x64/Launcher64.exe", pe, 0644);
  Write(game / "x86/Launcher.exe", pe, 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  GameSettingsRecord saved;
  config.settings_record = [&](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  RuntimePrepareRequest request{449610, "/data/homebrew/FAKE77031/games/449610", "internal", "Monster Boy"};
  // Missing.exe (another branch's program) isn't in the download, so it isn't offered.
  request.launches = {{"", "", "x64\\Launcher64.exe", "", ""}, {"", "", "Missing.exe", "", ""},
                      {"", "", "x86\\Launcher.exe", "", "-safe"}};
  request.launches_any_branch = true;

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(request);
  const auto asked = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(449610) != 0; });
  CHECK(asked.refusals.at(449610).find("Choose the program") != std::string::npos && asked.installs.empty());
  const auto& choices = asked.game_setups.at(449610).steam_choices;
  CHECK(choices.size() == 2 && choices[0].path == "x64/Launcher64.exe" && choices[1].path == "x86/Launcher.exe" &&
        !choices[0].helper && !asked.game_setups.at(449610).draft);

  // The player chooses the second: it is prepared as Steam's option, with Steam's arguments.
  saved.steam_launch = "x86/Launcher.exe";
  host.Prepare(request);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);
  if (done.refusals.count(449610)) std::fprintf(stderr, "refused: %s\n", done.refusals.at(449610).c_str());
  CHECK(!done.refusals.count(449610) && done.game_setups.at(449610).steam_choices.empty());
  CHECK(done.generated_recipes.size() == 1 && done.generated_recipes[0].launch_entries[0].executable == "x86/Launcher.exe" &&
        done.generated_recipes[0].launch_entries[0].id != kProgramEntryId &&
        done.generated_recipes[0].launch_entries[0].arguments == "-safe");
  host.Shutdown();
}

void PreparesProgramChosenInSettings() {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/1592280";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(game / "SELACO.exe", pe, 0644);
  Write(game / "Launch.bat", "start Launcher.exe\n", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  GameSettingsRecord saved;
  bool has_record = false;
  config.settings_record = [&](const playden::GameId&) -> std::optional<GameSettingsRecord> {
    if (!has_record) return std::nullopt;
    return saved;
  };
  RuntimePrepareRequest request{1592280, "/data/homebrew/FAKE77031/games/1592280", "internal", "Selaco"};
  request.launches = {{"", "Play", "Launch.bat", "", ""}};
  const auto profile = fixture.root / "data/playden/games/steam-1592280/library/profiles/playden-steam-1592280.profile";

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(request);
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.refusals.count(1592280) != 0; });
  CHECK(refused.refusals.at(1592280).find("no Windows program") != std::string::npos && refused.installs.empty());
  // Game settings gets a draft to start from and the folder's programs to pick.
  const auto& setup = refused.game_setups.at(1592280);
  CHECK(setup.draft && setup.draft->launch_entries.empty() && setup.refusal_soft && setup.scanned);
  CHECK(setup.programs.size() == 1 && setup.programs[0].path == "SELACO.exe" && setup.programs[0].architecture == "pe64");

  // The player picks SELACO.exe: Prepare makes the recipe from it and the title starts it.
  has_record = true;
  saved.overrides = {{"program", "SELACO.exe"}, {"architecture", "pe64"}};
  host.Prepare(request);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && !s.installs.empty() && s.installs[0].state == PreparationState::kReady;
  }, 60);
  CHECK(!done.refusals.count(1592280) && !done.game_setups.at(1592280).draft);
  CHECK(done.generated_recipes.size() == 1 && done.generated_recipes[0].launch_entries[0].id == kProgramEntryId);
  CHECK(Read(profile).find("\\games\\1592280\\SELACO.exe\n") != std::string::npos);
  CHECK(Read(profile).find("\narchitecture = pe64\n") != std::string::npos);

  // A later update applies the program's folder too, without preparing the game again; a folder
  // that isn't there is refused and the title keeps what it had.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  const auto apply = [&](std::uint64_t revision) {
    saved.revision = revision;
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(1592280, RuntimeMaintenance::kApplySettings);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  };
  saved.overrides["program_arguments"] = "-nostartup";
  saved.overrides["program_folder"] = "Elsewhere";
  auto applied = apply(1);
  CHECK(!applied.maintenance.at(1592280).ok &&
        applied.maintenance.at(1592280).message.find("Elsewhere chosen in Game settings is missing") != std::string::npos);
  CHECK(Read(profile).find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\1592280\n") != std::string::npos);
  CHECK(applied.installs[0].launch_working_directory.empty() && applied.installs[0].launch_path == "SELACO.exe" &&
        !applied.installs[0].recovery_pending);
  std::filesystem::create_directory(game / "Elsewhere");
  CHECK(chmod((game / "Elsewhere").c_str(), 0755) == 0);
  applied = apply(2);
  if (!applied.maintenance.at(1592280).ok) std::fprintf(stderr, "apply: %s\n", applied.maintenance.at(1592280).message.c_str());
  CHECK(applied.maintenance.at(1592280).ok);
  CHECK(Read(profile).find("\narguments = -nostartup\n") != std::string::npos);
  CHECK(Read(profile).find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\1592280\\Elsewhere\n") !=
        std::string::npos);
  CHECK(applied.installs[0].launch_working_directory == "Elsewhere");
  struct stat elsewhere{};  // an owned 0755 folder on the way to dxvk.conf opens for FTP
  CHECK(stat((game / "Elsewhere").c_str(), &elsewhere) == 0 && (elsewhere.st_mode & 07777) == 0777);
  // Another program from the folder: the profile and the install record both name it.
  Write(game / "Other.exe", pe, 0644);
  saved.overrides = {{"program", "Other.exe"}, {"architecture", "pe64"}};
  applied = apply(3);
  if (!applied.maintenance.at(1592280).ok) std::fprintf(stderr, "switch: %s\n", applied.maintenance.at(1592280).message.c_str());
  CHECK(applied.maintenance.at(1592280).ok);
  CHECK(Read(profile).find("\\games\\1592280\\Other.exe\n") != std::string::npos);
  CHECK(applied.installs[0].launch_path == "Other.exe" && applied.installs[0].launch_entry_id == kProgramEntryId &&
        applied.installs[0].launch_working_directory.empty() && applied.installs[0].settings_revision == 3);
  // A program that isn't there is refused before anything changes.
  saved.overrides = {{"program", "Missing.exe"}, {"architecture", "pe64"}};
  applied = apply(4);
  CHECK(!applied.maintenance.at(1592280).ok && applied.maintenance.at(1592280).message.find("missing") != std::string::npos);
  CHECK(Read(profile).find("\\games\\1592280\\Other.exe\n") != std::string::npos &&
        applied.installs[0].launch_path == "Other.exe" && applied.installs[0].settings_revision == 3);
  host.Shutdown();
}

// An Unreal Engine 3 (UDK) game asks for its EULA through UnSetup.exe, which the PS5 cannot
// start; preparing it marks the EULA accepted in the bottle's system.reg instead, once.
void PreparesUdkGameWithEulaAccepted() {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  config.log = [&](const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    lines.push_back(line);
  };
  const auto eula_lines = [&] {
    std::lock_guard<std::mutex> lock(lines_mutex);
    std::vector<std::string> found;
    for (const auto& line : lines)
      if (line.rfind("ue3 eula ", 0) == 0) found.push_back(line);
    return found;
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/219890";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x4c; pe[0x45] = 0x01; pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x01;
  Write(game / "Binaries/Win32/UDK.exe", pe, 0644);
  Write(game / "Binaries/UnSetup.exe", "MZ", 0644);
  Write(game / "Binaries/InstallInfo.xml",
        "<?xml version=\"1.0\"?>\r\n<InstallInfo>\r\n  <InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933"
        "</InstallGuidString>\r\n</InstallInfo>", 0644);
  CHECK(chmod(game.c_str(), 0755) == 0);
  RuntimePrepareRequest request{219890, "/data/homebrew/FAKE77031/games/219890", "internal", "Antichamber"};
  request.launches = {{"", "Play", "Binaries\\Win32\\UDK.exe", "", ""}};

  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare(request);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(219890) ||
        (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(219890)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(219890).c_str());
  CHECK(!done.refusals.count(219890) && done.installs.size() == 1);
  const auto bottle = fixture.root / "data/playden/games/steam-219890/library/prefixes/game";
  const auto key = std::string("Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\UDK-dcf140f3-829e-4033-950d-b3be8d76b933]");
  const auto system_reg = Read(bottle / "system.reg");
  CHECK(system_reg.find("[Software\\\\" + key) != std::string::npos);
  CHECK(system_reg.find("[Software\\\\Wow6432Node\\\\" + key) != std::string::npos);
  // The Visual C++ runtime key follows (VcRuntimeStep), so the backup is the hive with the EULA already in it.
  CHECK(system_reg.find("[Software\\\\Microsoft\\\\VisualStudio\\\\14.0\\\\VC\\\\Runtimes\\\\x64]") != std::string::npos);
  CHECK(system_reg.find("\"Version\"=\"14.50.35719.0\"") != std::string::npos);
  CHECK(Read(bottle / "system.reg.playden-backup").find("[Software\\\\" + key) != std::string::npos);
  CHECK(Read(bottle / "system.reg.playden-backup").find("VisualStudio") == std::string::npos);
  auto logged = eula_lines();
  CHECK(!logged.empty() && logged.front() == "ue3 eula app=219890 guid=dcf140f3-829e-4033-950d-b3be8d76b933 written=1");

  // Repair runs the fixups again: the value is already there, so nothing is rewritten.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  const auto before = host.Snapshot().maintenance_handled;
  host.Maintain(219890, RuntimeMaintenance::kRepair);
  const auto repaired = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  if (!repaired.maintenance.at(219890).ok) std::fprintf(stderr, "repair: %s\n", repaired.maintenance.at(219890).message.c_str());
  CHECK(repaired.maintenance.at(219890).ok);
  CHECK(Read(bottle / "system.reg") == system_reg);
  logged = eula_lines();
  CHECK(logged.back() == "ue3 eula app=219890 guid=dcf140f3-829e-4033-950d-b3be8d76b933 written=0");
  host.Shutdown();
}

// A Windows DLL that imports one other DLL: a PE32 or PE32+ header, one section, an import table.
std::string ImportingDll(bool pe64, const std::string& import) {
  std::string bytes(0x600, '\0');
  const auto put16 = [&](std::size_t at, unsigned value) { bytes[at] = char(value & 0xff); bytes[at + 1] = char(value >> 8); };
  const auto put32 = [&](std::size_t at, std::uint32_t value) { put16(at, value & 0xffff); put16(at + 2, value >> 16); };
  const std::size_t optional_size = pe64 ? 0xf0 : 0xe0, optional = 0x58;
  bytes[0] = 'M'; bytes[1] = 'Z'; put32(0x3c, 0x40); bytes[0x40] = 'P'; bytes[0x41] = 'E';
  put16(0x44, pe64 ? 0x8664 : 0x014c);
  put16(0x46, 1);
  put16(0x54, static_cast<unsigned>(optional_size));
  put16(optional, pe64 ? 0x20b : 0x10b);
  put32(optional + (pe64 ? 112 : 96) + 8, 0x1000);  // imports at the section's start
  const std::size_t row = optional + optional_size;
  bytes.replace(row, 5, ".data");
  put32(row + 8, 0x200); put32(row + 12, 0x1000); put32(row + 16, 0x200); put32(row + 20, 0x400);
  put32(0x400 + 12, 0x1100);  // the descriptor's DLL name
  bytes.replace(0x500, import.size(), import);
  return bytes;
}

// A game whose plugin imports Microsoft's MFC gets MFC's files in its bottle from the Visual C++ redistributable.
// An upgrade, not a condition: the preparation that can't download it still ends Ready. Start-up never downloads
// (settings published there leave MFC for Play); a Play that can't download it still starts the game with a notice,
// and the next Play installs it with a progress line; later Plays neither walk the folder nor download again.
void PlayInstallsMfcForGameThatImportsIt() {
  Fixture fixture;
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.session_data_root = fixture.root / "data";
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
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return "00000000-0000-4000-8000-" + std::string(11 - std::to_string(uuids).size(), '0') + "1" + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  std::string session;
  config.launch_title = [&session](const std::string&, const std::vector<std::string>& arguments) {
    session = arguments.at(0).substr(8);  // "session=<UUID>"
    return TitleResult{true, 0, "launch"};
  };
  GameSettingsRecord saved;
  config.settings_record = [&saved](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  config.log = [&](const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    lines.push_back(line);
  };
  const auto logged = [&](const char* prefix) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    std::vector<std::string> found;
    for (const auto& line : lines)
      if (line.rfind(prefix, 0) == 0) found.push_back(line);
    return found;
  };
  // Microsoft's bundles stand in for by synthetic ones, at their own pinned hashes.
  const auto x64_bundle = playden::test::msi::MfcBundle(true), x86_bundle = playden::test::msi::MfcBundle(false);
  config.vc_redists = {
      {"x86", "https://download.example/x86/VC_redist.x86.exe", *HashArtifactBytes(x86_bundle), x86_bundle.size(),
       "vcRuntimeAdditional_x86", "14.44.35211"},
      {"x64", "https://download.example/x64/VC_redist.x64.exe", *HashArtifactBytes(x64_bundle), x64_bundle.size(),
       "vcRuntimeAdditional_x64", "14.44.35211"}};
  config.redist_retry_wait = std::chrono::seconds(1);
  std::atomic<bool> online{false};
  std::atomic<int> fetches{0};
  std::atomic<RuntimeHost*> current_host{nullptr};
  std::atomic<bool> progress_shown{false};
  config.fetch_range = [&](const std::string& url, std::uint64_t offset, std::uint64_t length, const std::atomic<bool>&,
                           std::string* error) -> std::optional<std::string> {
    ++fetches;
    if (auto* host = current_host.load()) {
      const auto progress = host->Snapshot().play_progress;
      if (progress.count(220) && progress.at(220) == kPlayDownloadingMfc) progress_shown = true;
    }
    if (!online) {
      *error = "offline";
      return std::nullopt;
    }
    const auto& bundle = url.find("x64") != std::string::npos ? x64_bundle : x86_bundle;
    return bundle.substr(offset, length);
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  Write(game / "bin/chroma.dll", ImportingDll(true, "MFC140U.dll"), 0644);
  const auto windows = fixture.root / "data/playden/games/steam-220/library/prefixes/game/drive_c/windows";
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
    CHECK(!done.refusals.count(220) && done.installs.size() == 1);
    const auto upgrades = logged("mfc upgrade ");
    CHECK(upgrades.size() == 2);
    CHECK(upgrades[0] == "mfc upgrade app=220 arch=x64 program=bin/chroma.dll skipped: the download failed at byte 0: offline");
    // Three attempts; the preparation's later pass over its steps doesn't wait for the network again.
    CHECK(upgrades[1] == "mfc upgrade app=220 arch=x64 program=bin/chroma.dll skipped: its download failed 0 s ago; "
                         "it is tried again from 1 s");
    CHECK(fetches == 3);
    CHECK(logged("mfc scan app=220 programs=").size() >= 1);
    CHECK(!fs::exists(windows / "system32/mfc140u.dll"));
    host.Shutdown();
  }
  // Settings saved while Playden was closed are published at start-up, which doesn't download.
  saved.revision = 1;
  saved.overrides = {{"resolution", "1280x720"}};
  online = true;
  RuntimeHost host(config);
  current_host = &host;
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  CHECK(!logged("startup settings app=220 applied").empty());
  CHECK(fetches == 3);
  CHECK(logged("mfc upgrade ").back() ==
        "mfc upgrade app=220 arch=x64 program=bin/chroma.dll waits for Play: start-up and background work don't download");
  const auto end_session = [&] {
    const auto sessions = fixture.root / "data/playden/games/steam-220/logs/sessions";
    fs::create_directories(sessions);
    CHECK(chmod((fixture.root / "data/playden/games/steam-220/logs").c_str(), 0700) == 0);
    CHECK(chmod(sessions.c_str(), 0700) == 0);
    Write(sessions / (session + ".json"),
          R"({"schema_version":1,"session_id":")" + session +
              R"(","install_id":"steam-220","title_id":"FAKE78001","origin":"playden","settings_revision":1,)"
              R"("runtime_bundle_id":"wine-wow64","phase":"exited"})" "\n", 0600);
    WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kExited; }, 30);
  };
  // A Play that can't download it still starts the game, and says why it may stop.
  online = false;
  host.Play(220);
  auto played = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(played.play_notices.count(220) && played.play_notices.at(220) == kPlayMfcNotDownloaded);
  CHECK(!played.play_progress.count(220) && progress_shown);
  CHECK(host.Handoff());
  end_session();

  // The game updates with a 32-bit tool that needs MFC as well; the next Play installs both.
  Write(game / "tools/editor.dll", ImportingDll(false, "mfc140.dll"), 0644);
  online = true;
  progress_shown = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // past the retry wait
  host.Play(220);
  played = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(progress_shown && !played.play_progress.count(220) && !played.play_notices.count(220));
  CHECK(Read(windows / "system32/mfc140u.dll") == "MFC64U" && Read(windows / "system32/mfcm140.dll") == "MFCM64");
  CHECK(Read(windows / "syswow64/mfc140u.dll") == "MFC32U");
  CHECK(!fs::exists(windows / "system32/msvcp140.dll") && !fs::exists(windows / "syswow64/msvcp140.dll"));
  CHECK(Read(fixture.root / "data/playden/redist/vc-14.44.35211/x64/VC_redist.x64.exe") == x64_bundle);
  CHECK(Read(fixture.root / "data/playden/redist/vc-14.44.35211/x86/VC_redist.x86.exe") == x86_bundle);
  auto upgrades = logged("mfc upgrade ");
  CHECK(upgrades[upgrades.size() - 2].rfind("mfc upgrade app=220 arch=x86 program=tools/editor.dll installed files=2 from=download ms=", 0) == 0);
  CHECK(upgrades.back().rfind("mfc upgrade app=220 arch=x64 program=bin/chroma.dll installed files=2 from=download ms=", 0) == 0);
  CHECK(host.Handoff());
  end_session();

  // Installed once: the next Play neither walks the game folder nor downloads, even without the kept copy.
  const auto count = logged("mfc ").size();
  const int fetched = fetches;
  fs::remove_all(fixture.root / "data/playden/redist");
  host.Play(220);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(logged("mfc ").size() == count && fetches == fetched);
  CHECK(Read(windows / "system32/mfc140u.dll") == "MFC64U");
  current_host = nullptr;
  host.Shutdown();
}

// Playden ships more recipes than the one the kit carries: a game with its own shipped recipe
// (FF XIII) is prepared by a kit packaged for HL2, with the recipe's bottle values written.
void ShippedRecipePreparesWithAnotherKitsRecipe() {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(fixture.root / "data/homebrew/FAKE77031/games/292120/white_data/prog/win/bin/ffxiiiimg.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare({292120, "/data/homebrew/FAKE77031/games/292120", "internal", "Final Fantasy XIII"});
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(292120) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(292120)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(292120).c_str());
  CHECK(!done.refusals.count(292120) && done.installs.size() == 1 && done.installs[0].runtime_number == 292120);
  CHECK(Read(config.state_root / "installs.json").find("\"recipe_id\":\"steam-292120-ffxiii-v1\"") != std::string::npos);
  const auto game = fixture.root / "data/playden/games/steam-292120";
  CHECK(Read(game / "library/profiles/playden-steam-292120.profile").find("ffxiiiimg.exe") != std::string::npos);
  CHECK(Read(game / "library/prefixes/game/user.reg").find("\"VideoMemorySize\"=\"4096\"") != std::string::npos);
  // A restart resolves the record again through the same rule.
  host.Shutdown();
  RuntimeHost restarted(config);
  CHECK(restarted.Start());
  const auto reloaded = WaitFor(restarted, Started);
  CHECK(reloaded.installs.size() == 1 && reloaded.installs[0].state == PreparationState::kReady);
  restarted.Shutdown();
}

// Batman: Arkham Asylum's shipped recipe writes Language into the game folder's UserEngine.ini at preparation, so
// the game does not start its launcher; a language already there stays.
void ShippedRecipeWritesGameIni(bool language_chosen) {
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/35140";
  Write(game / "Binaries/ShippingPC-BmGame.exe", "existing-game", 0644);
  const std::string shipped = "[Configuration]\r\nBasedOn=..\\BmGame\\Config\\DefaultEngine.ini\r\n\r\n"
                              "[Engine.Engine]\r\nbSubtitlesEnabled=FALSE\r\n";
  const auto ini = shipped + (language_chosen ? "Language=deu\r\n" : "");
  Write(game / "BmGame/Config/UserEngine.ini", ini, 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare({35140, "/data/homebrew/FAKE77031/games/35140", "internal", "Batman: Arkham Asylum GOTY Edition"});
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(35140) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (done.refusals.count(35140)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(35140).c_str());
  CHECK(!done.refusals.count(35140) && done.installs.size() == 1);
  CHECK(Read(config.state_root / "installs.json").find("\"recipe_id\":\"steam-35140-arkhamasylum-v1\"") != std::string::npos);
  CHECK(Read(game / "BmGame/Config/UserEngine.ini") == (language_chosen ? ini : shipped + "Language=int\r\n"));
  host.Shutdown();
}

// A copy of Arkham Asylum prepared before Playden shipped its recipe keeps the recipe Playden generated, and still
// gets the shipped recipe's INI values the next time it is played.
void GeneratedRecipeGetsShippedIni() {
  Fixture fixture;
  auto config = fixture.Config();
  const auto runtime = fixture.root / "runtime";
  Write(runtime / "presets.json", Read(fs::path(g_runtime_dir) / "presets.json"), 0644);
  Write(runtime / "recipes/steam-220-hl2-v1.json", Read(fs::path(g_runtime_dir) / "recipes/steam-220-hl2-v1.json"), 0644);
  Write(runtime / "recipes/recipes.lst", "steam-220-hl2-v1.json\n", 0644);
  config.runtime_dir = runtime.string();
  config.steamworks_dir = (fs::path(g_runtime_dir) / "steamworks").string();
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
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/35140";
  // A 32-bit PE header is all the generator reads.
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x4c; pe[0x45] = 0x01; pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x01;
  Write(game / "Binaries/ShippingPC-BmGame.exe", pe, 0644);
  const std::string shipped = "[Engine.Engine]\r\nbSubtitlesEnabled=FALSE\r\n";
  Write(game / "BmGame/Config/UserEngine.ini", shipped, 0644);
  RuntimePrepareRequest request{35140, "/data/homebrew/FAKE77031/games/35140", "internal", "Batman: Arkham Asylum GOTY Edition"};
  request.launches = {{"", "", "Binaries\\ShippingPC-BmGame.exe", "", ""}};
  const auto prepared = [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(35140) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  };
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(request);
    const auto done = WaitFor(host, prepared, 60);
    if (done.refusals.count(35140)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(35140).c_str());
    CHECK(!done.refusals.count(35140) && done.installs.size() == 1);
    CHECK(Read(config.state_root / "installs.json").find("\"recipe_id\":\"steam-35140-auto\"") != std::string::npos);
    CHECK(Read(game / "BmGame/Config/UserEngine.ini") == shipped);
    host.Shutdown();
  }
  // Playden now ships the recipe: the game keeps its own, and its files get the value.
  Write(runtime / "recipes/steam-35140-arkhamasylum-v1.json",
        Read(fs::path(g_runtime_dir) / "recipes/steam-35140-arkhamasylum-v1.json"), 0644);
  Write(runtime / "recipes/recipes.lst", "steam-220-hl2-v1.json\nsteam-35140-arkhamasylum-v1.json\n", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Play(35140);
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.plays_handled > 0; }, 30);
  CHECK(Read(config.state_root / "installs.json").find("\"recipe_id\":\"steam-35140-auto\"") != std::string::npos);
  CHECK(Read(game / "BmGame/Config/UserEngine.ini") == shipped + "Language=int\r\n");
  host.Shutdown();
}

// A shipped recipe's requirements are checked against the kit's template, as the kit's packager
// checks the kit's own recipe: without PS5 audio in the template, a recipe that needs it is refused.
void ShippedRecipeRequirementsFollowTheKit(bool template_audio_ps5) {
  Fixture fixture;
  auto config = fixture.Config();
  const auto runtime = fixture.root / "runtime";
  Write(runtime / "presets.json", Read(fs::path(g_runtime_dir) / "presets.json"), 0644);
  Write(runtime / "recipes/recipes.lst", Read(fs::path(g_runtime_dir) / "recipes/recipes.lst"), 0644);
  Write(runtime / "recipes/steam-220-hl2-v1.json", Read(fs::path(g_runtime_dir) / "recipes/steam-220-hl2-v1.json"), 0644);
  auto ffxiii = Read(fs::path(g_runtime_dir) / "recipes/steam-292120-ffxiii-v1.json");
  ffxiii.replace(ffxiii.find("\"locked\""), 0, "\"requirements\": {\"audio\": \"ps5\"},\n  ");
  Write(runtime / "recipes/steam-292120-ffxiii-v1.json", ffxiii, 0644);
  config.runtime_dir = runtime.string();
  config.steamworks_dir = (fs::path(g_runtime_dir) / "steamworks").string();
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.view.template_audio_ps5 = template_audio_ps5;
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    ++uuids; return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(uuids);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  Write(fixture.root / "data/homebrew/FAKE77031/games/292120/white_data/prog/win/bin/ffxiiiimg.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  host.Prepare({292120, "/data/homebrew/FAKE77031/games/292120", "internal", "Final Fantasy XIII"});
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(292120) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (template_audio_ps5) {
    if (done.refusals.count(292120)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(292120).c_str());
    CHECK(!done.refusals.count(292120) && done.installs.size() == 1);
    // Swapped for a kit without PS5 audio, Play is refused before the bottle or the game's files change.
    host.Shutdown();
    const auto user_reg = fixture.root / "data/playden/games/steam-292120/library/prefixes/game/user.reg";
    auto marked = Read(user_reg);
    marked.replace(marked.find("\"VideoMemorySize\"=\"4096\""), 24, "\"VideoMemorySize\"=\"1234\"");
    Write(user_reg, marked, 0600);
    kit.view.template_audio_ps5 = false;
    RuntimeHost swapped(config);
    CHECK(swapped.Start());
    CHECK(WaitFor(swapped, Started).kit_available);
    swapped.Play(292120);
    const auto played = WaitFor(swapped, [](const RuntimeSnapshot& s) { return s.plays_handled > 0; }, 30);
    if (played.play_refusals.count(292120)) std::fprintf(stderr, "Play refused: %s\n", played.play_refusals.at(292120).c_str());
    CHECK(played.play_refusals.count(292120) &&
          played.play_refusals.at(292120).find("lacks what this game needs (PS5 audio)") != std::string::npos);
    CHECK(Read(user_reg).find("\"VideoMemorySize\"=\"1234\"") != std::string::npos);
    swapped.Shutdown();
    return;
  } else {
    CHECK(done.refusals.count(292120) && done.installs.empty());
    CHECK(done.refusals.at(292120).find("lacks what this game needs (PS5 audio)") != std::string::npos);
  }
  host.Shutdown();
}

// The 0.6 layout moves /data/homebrew/FAKE77031/games to /data/playden-games in one rename at
// startup. The runtime host then rewrites the records and republishes each moved Ready game, so
// its descriptor, profile and dxvk.conf name the new folder and it starts from Home again.
struct GamesMove {
  std::mutex mutex;
  std::optional<std::pair<std::string, std::string>> journal;  // at "games-moved"
  int done = 0;
  std::vector<std::string> lines;
  void Wire(RuntimeHostConfig& config) {
    config.moved_games_root = [this] { std::lock_guard<std::mutex> lock(mutex); return journal; };
    config.finish_games_move = [this](std::string*) {
      std::lock_guard<std::mutex> lock(mutex);
      journal.reset(); ++done;
      return true;
    };
    config.log = [this](const std::string& line) { std::lock_guard<std::mutex> lock(mutex); lines.push_back(line); };
  }
  bool Logged(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mutex);
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
  }
  int Done() { std::lock_guard<std::mutex> lock(mutex); return done; }
};
constexpr const char* kOldGames = "/data/homebrew/FAKE77031/games";
constexpr const char* kNewGames = "/data/playden-games";

RuntimeHostConfig MoveConfig(Fixture& fixture, const RuntimeKit& kit, unsigned* uuids, GamesMove& move) {
  auto config = fixture.Config();
  config.host_namespace_prefix = fixture.root;
  config.session_data_root = fixture.root / "data";
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.package_operations.hash_file = HashArtifactFile;
  config.package_operations.hash_bytes = HashArtifactBytes;
  config.package_operations.free_space_unverifiable = true;
  config.package_operations.validate_bottle = [](const fs::path& bottle) {
    return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
  };
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  config.uuid = [uuids]() -> std::optional<std::string> {
    char id[37]; std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012u", ++*uuids);
    return std::string(id);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.settings_record = [](const playden::GameId&) -> std::optional<GameSettingsRecord> {
    GameSettingsRecord saved; saved.revision = 1; saved.overrides = {{"resolution", "1280x720"}};
    return saved;
  };
  config.launch_title = [](const std::string&, const std::vector<std::string>&) { return TitleResult{true, 0, "launch"}; };
  move.Wire(config);
  return config;
}

// HL2 prepared under the old games root, with an install script and a Cloud index; then the
// startup rename moves the tree, and the journal says so.
void PrepareThenMoveGames(Fixture& fixture, const RuntimeHostConfig& config, GamesMove& move) {
  const auto old_games = fixture.root / "data/homebrew/FAKE77031/games";
  Write(old_games / "220/hl2.exe", "existing-game", 0644);
  CHECK(chmod((old_games / "220").c_str(), 0755) == 0);
  Write(old_games / "220/installscript.vdf",
        "\"InstallScript\"\n{\n \"Registry\"\n {\n  \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Valve\\\\HL2\"\n  {\n"
        "   \"string\"\n   {\n    \"Installed Path\" \"%INSTALLDIR%\\\\\"\n   }\n  }\n"
        // A redistributable's older Visual C++ runtime key: the runtime key step runs after it and raises it.
        "  \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Microsoft\\\\VisualStudio\\\\14.0\\\\VC\\\\Runtimes\\\\x64\"\n  {\n"
        "   \"dword\"\n   {\n    \"Minor\" \"20\"\n   }\n  }\n }\n}\n", 0644);
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    const auto ready = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (ready.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", ready.refusals.at(220).c_str());
    CHECK(!ready.refusals.count(220) && ready.installs[0].settings_revision == 1);
    host.Shutdown();
  }
  const auto system_reg = Read(fixture.root / "data/playden/games/steam-220/library/prefixes/game/system.reg");
  CHECK(system_reg.find("FAKE77031\\\\games\\\\220") != std::string::npos);
  Write(old_games / ".playden-saves/220/cloud-index.txt",
        "version=1\naccount=a\nchange=7\ngame_root=/data/homebrew/FAKE77031/games/220\n"
        "prefix_root=/data/playden/games/steam-220/library/prefixes/game\nrule\tg\t1\tsave\t*\tsave/\n"
        "file\tg\t3\t5\tsave/game_root=/data/homebrew/FAKE77031/games/220\n", 0666);  // Cloud writes it under umask 0
  fs::rename(old_games, fixture.root / "data/playden-games");
  std::lock_guard<std::mutex> lock(move.mutex);
  move.journal = std::make_pair(std::string(kOldGames), std::string(kNewGames));
}

// The worker owns the actual rename, with every title excluded until records name the new root.
void WorkerMovesGamesUnderPublicationLease() {
  Fixture fixture;
  auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  GamesMove move;
  auto config = MoveConfig(fixture, kit, &uuids, move);
  PrepareThenMoveGames(fixture, config, move);
  const auto old = fixture.root / "data/homebrew/FAKE77031/games";
  const auto target = fixture.root / "data/playden-games";
  fs::rename(target, old);
  config.layout_paths = LayoutPaths{old.parent_path(), fixture.root / "data/playden/user",
      fixture.root / "data/playden/logs", target, config.state_root, fixture.root / "data"};
  const std::string text = "{\"step\":\"games\",\"from\":\"" + old.string() + "\",\"to\":\"" + target.string() + "\"}";
  CHECK(playden::Storage(config.state_root.string()).WriteDurable(kLayoutJournal, playden::Bytes(text.begin(), text.end())).ok());
  const auto lock_path = fixture.root / "data/playden/games/steam-220/.playden-control/publication.lock";
  const auto busy = [&] {
    const int fd = ::open(lock_path.c_str(), O_RDONLY);
    CHECK(fd >= 0);
    const bool held = ::flock(fd, LOCK_SH | LOCK_NB) != 0;
    ::close(fd);
    return held;
  };
  unsigned rename_checks = 0, record_checks = 0;
  config.layout_operations.rename = [&](const fs::path& from, const fs::path& to) -> std::error_code {
    CHECK(from == old && to == target && busy());
    ++rename_checks;
    return ::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : std::error_code{errno, std::generic_category()};
  };
  config.moved_games_root = [&]() -> std::optional<std::pair<std::string, std::string>> {
    const auto journal = LoadLayoutJournal(config.state_root);
    if (!journal || journal->step != "games-moved") return std::nullopt;
    CHECK(busy());  // Called by MoveGameRecords before UpdateAll publishes the new paths.
    ++record_checks;
    return std::make_pair(std::string(kOldGames), std::string(kNewGames));
  };
  config.finish_games_move = {};
  // A Home launch's reader lease prevents the rename even if the activity query says stopped.
  const int reader = ::open(lock_path.c_str(), O_RDONLY);
  CHECK(reader >= 0 && ::flock(reader, LOCK_SH | LOCK_NB) == 0);
  {
    RuntimeHost host(config);
    CHECK(host.Start() && host.WaitForStartup());
    CHECK(fs::exists(old) && !fs::exists(target) && rename_checks == 0 && record_checks == 0);
    host.Shutdown();
  }
  ::close(reader);
  // An active or suspended title also postpones after the writer lease is acquired.
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  {
    RuntimeHost host(config);
    CHECK(host.Start() && host.WaitForStartup());
    CHECK(fs::exists(old) && !fs::exists(target) && rename_checks == 0);
    host.Shutdown();
  }
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  if (geteuid() != 0) {
    CHECK(::chmod(old.c_str(), 0000) == 0);
    RuntimeHost host(config);
    CHECK(host.Start() && host.WaitForStartup());
    CHECK(rename_checks == 0 && record_checks == 0);
    host.Shutdown();
    CHECK(::chmod(old.c_str(), 0755) == 0);
  }
  {
    RuntimeHost host(config);
    CHECK(host.Start() && host.WaitForStartup());
    CHECK(!fs::exists(old) && fs::exists(target) && rename_checks == 1 && record_checks == 1);
    CHECK(!busy());
    CHECK(host.Snapshot().installs.front().install_path == std::string(kNewGames) + "/220");
    host.Shutdown();
  }
}

// What the moved game's published files and record name after the move finished.
void CheckRepublishedAtNewRoot(Fixture& fixture) {
  const auto records = Read(fixture.root / "data/runtime-state/installs.json");
  CHECK(records.find(R"("install_path":"/data/playden-games/220")") != std::string::npos);
  CHECK(records.find("moved_from") == std::string::npos && records.find("FAKE77031") == std::string::npos);
  const auto descriptor = Read(fixture.root / "data/homebrew/FAKE78001/game-launch.json");
  CHECK(descriptor.find(R"("game_root":"/data/playden-games/220")") != std::string::npos);
  CHECK(descriptor.find(R"("settings_revision":1)") != std::string::npos);
  const auto profiles = fixture.root / "data/playden/games/steam-220/library/profiles";
  const auto profile = Read(profiles / "playden-steam-220.profile");
  CHECK(profile.find("playden-games") != std::string::npos && profile.find("FAKE77031") == std::string::npos);
  CHECK(profile.find("desktop = 1280x720") != std::string::npos);  // the saved settings are kept
  // The metadata keeps paths in hex.
  const auto hex = [](const std::string& text) {
    std::string out;
    for (const unsigned char c : text) { char pair[3]; std::snprintf(pair, sizeof(pair), "%02x", c); out += pair; }
    return out;
  };
  const auto metadata = Read(profiles / ".playden-publication");
  CHECK(metadata.find("dxvk.path_hex=" + hex("/data/playden-games/220/dxvk.conf") + "\n") != std::string::npos);
  // The install script's path follows the folder (ApplyPreparationSteps runs in the republication).
  const auto system_reg = Read(fixture.root / "data/playden/games/steam-220/library/prefixes/game/system.reg");
  CHECK(system_reg.find("playden-games\\\\220") != std::string::npos && system_reg.find("FAKE77031") == std::string::npos);
  // The install script's older Visual C++ runtime Minor (20) was raised to the builtin DLLs' 14.50 after it.
  CHECK(system_reg.find("\"Minor\"=dword:00000032") != std::string::npos &&
        system_reg.find("\"Minor\"=dword:00000014") == std::string::npos);
}

void MigrationRewritesInstallPathsAndRepublishes() {
  Fixture fixture;
  GamesMove move;
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  unsigned uuids = 0;
  const auto config = MoveConfig(fixture, kit, &uuids, move);
  PrepareThenMoveGames(fixture, config, move);
  const auto registrations = fixture.registry_mutations;

  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.installs.size() == 1 && started.installs[0].state == PreparationState::kReady);
  CHECK(started.installs[0].admission == "ok" && started.installs[0].settings_revision == 1);
  CHECK(move.Logged("startup moved app=220 republished") && move.Done() == 1);
  CheckRepublishedAtNewRoot(fixture);
  // No new registration: the title stays the one Home lists.
  CHECK(fixture.registry_mutations == registrations);
  // The Cloud index follows the game folder; the bottle (prefix_root) did not move.
  const auto index_path = fixture.root / "data/playden-games/.playden-saves/220/cloud-index.txt";
  const auto index = Read(index_path);
  struct stat index_info{};  // the rewrite keeps the file's mode, whatever the umask
  CHECK(stat(index_path.c_str(), &index_info) == 0 && (index_info.st_mode & 07777) == 0666);
  CHECK(index == "version=1\naccount=a\nchange=7\ngame_root=/data/playden-games/220\n"
                 "prefix_root=/data/playden/games/steam-220/library/prefixes/game\nrule\tg\t1\tsave\t*\tsave/\n"
                 "file\tg\t3\t5\tsave/game_root=/data/homebrew/FAKE77031/games/220\n");
  // Play passes the title's whole start check.
  host.Play(220);
  const auto played = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
  if (played.play_refusals.count(220)) std::fprintf(stderr, "Play refused: %s\n", played.play_refusals.at(220).c_str());
  CHECK(played.handoff_ready);
  host.Shutdown();
  // Nothing is left to do on the next start.
  RuntimeHost again(config);
  CHECK(again.Start());
  WaitFor(again, Started);
  CHECK(move.Done() == 1);
  again.Shutdown();
}

// A game an older Playden prepared (owned publication, schema 2) and the player then opened up
// over FTP (#73): with a kit that admits unowned folders, the move republishes it unowned.
void MovedModdedGameRepublishesUnowned() {
  Fixture fixture;
  GamesMove move;
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  unsigned uuids = 0;
  const auto config = MoveConfig(fixture, kit, &uuids, move);
  PrepareThenMoveGames(fixture, config, move);
  const auto profiles = fixture.root / "data/playden/games/steam-220/library/profiles";
  CHECK(Read(profiles / ".playden-publication").find("schema=2\n") != std::string::npos);
  const auto game = fixture.root / "data/playden-games/220";
  Write(game / "GInput.asi", "mod-plugin", 0666);
  CHECK(chmod(game.c_str(), 0777) == 0);
  kit.capabilities = {"dxvk", "game-root-unowned", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};

  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.installs.size() == 1 && started.installs[0].state == PreparationState::kReady);
  CHECK(started.installs[0].admission == "ok");
  CHECK(move.Logged("startup moved app=220 republished") && move.Done() == 1);
  CheckRepublishedAtNewRoot(fixture);
  const auto metadata = Read(profiles / ".playden-publication");
  CHECK(metadata.find("schema=3\n") != std::string::npos && metadata.find("game_root.unowned=1\n") != std::string::npos);
  struct stat info{};
  CHECK(stat(game.c_str(), &info) == 0 && (info.st_mode & 07777) == 0777 && Read(game / "GInput.asi") == "mod-plugin");
  host.Play(220);
  const auto played = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
  if (played.play_refusals.count(220)) std::fprintf(stderr, "Play refused: %s\n", played.play_refusals.at(220).c_str());
  CHECK(played.handoff_ready);
  host.Shutdown();
}

// The startup republication runs before the Steam account is known: the emulator's files keep
// the account Play wrote, and Play rewrites them.
void MovedGameStartupKeepsSteamAccount() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  auto config = MoveConfig(fixture, kit, &uuids, move);
  config.settings_record = [](const playden::GameId&) -> std::optional<GameSettingsRecord> {
    GameSettingsRecord saved; saved.revision = 1; return saved;
  };
  const auto old_games = fixture.root / "data/homebrew/FAKE77031/games";
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(old_games / "4242/Bin/Game.exe", pe, 0644);
  Write(old_games / "4242/Bin/steam_api64.dll", pe + std::string("\0SteamUser021\0", 14), 0644);
  RuntimePrepareRequest request{4242, std::string(kOldGames) + "/4242", "internal", "Example Game"};
  request.launches = {{"", "", "bin\\game.exe", "", "-windowed"}};
  {
    RuntimeHost host(config);
    host.SetSteamAccount("player", 76561198000000001ull);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(request);
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(4242) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (done.refusals.count(4242)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(4242).c_str());
    CHECK(!done.refusals.count(4242) && done.installs[0].state == PreparationState::kReady);
    host.Shutdown();
  }
  const auto user_ini = [&] { return Read(fixture.root / "data/playden-games/4242/Bin/steam_settings/configs.user.ini"); };
  fs::rename(old_games, fixture.root / "data/playden-games");
  CHECK(user_ini().find("account_name=player\n") != std::string::npos);
  {
    std::lock_guard<std::mutex> lock(move.mutex);
    move.journal = std::make_pair(std::string(kOldGames), std::string(kNewGames));
  }
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  CHECK(move.Logged("startup moved app=4242 republished") && move.Done() == 1);
  CHECK(user_ini().find("account_name=player\n") != std::string::npos);
  CHECK(user_ini().find("account_steamid=76561198000000001\n") != std::string::npos);
  CHECK(move.Logged("steamworks app=4242 mode=emulate waits"));
  host.Shutdown();
}

// A moved game whose publication can't be redone (here its saved settings are older than its
// title's) is not prepared again behind the player's back: Play and Update refuse, name the
// confirmed way out (Recreate bottle), and change nothing.
void StuckMovedGameOffersRecreateBottle() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  auto config = MoveConfig(fixture, kit, &uuids, move);
  auto revision = std::make_shared<std::atomic<std::uint64_t>>(1);
  config.settings_record = [revision](const playden::GameId&) -> std::optional<GameSettingsRecord> {
    GameSettingsRecord saved; saved.revision = revision->load(); saved.overrides = {{"resolution", "1280x720"}};
    return saved;
  };
  bool unregistered = false;
  config.unregister_title = [&unregistered](const std::string&, const std::string&) {
    unregistered = true;
    return TitleRemoverResult{true, 0, "answered"};
  };
  PrepareThenMoveGames(fixture, config, move);
  const auto save = fixture.root / "data/playden/games/steam-220/library/prefixes/game/drive_c/users/steamuser/save.dat";
  Write(save, "player-save", 0600);
  revision->store(0);

  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  CHECK(move.Logged("startup moved app=220 failed: ") && move.Done() == 0);
  const auto records_before = Read(fixture.root / "data/runtime-state/installs.json");
  CHECK(records_before.find("moved_from") != std::string::npos);
  const auto title = host.Snapshot().installs.at(0).title_id;
  host.Play(220);
  const auto played = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
  CHECK(played.play_refusals.count(220) &&
        played.play_refusals.at(220).find("Use Maintenance > Recreate bottle on the game page") != std::string::npos);
  const auto before = host.Snapshot().prepares_handled;
  host.Prepare({220, std::string(kNewGames) + "/220", "internal", "Half-Life \"2\""});
  const auto done = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.prepares_handled > before; }, 60);
  CHECK(done.refusals.count(220) &&
        done.refusals.at(220).find("Use Maintenance > Recreate bottle on the game page") != std::string::npos);
  CHECK(done.installs.size() == 1 && done.installs[0].state == PreparationState::kReady && done.installs[0].title_id == title);
  CHECK(!move.Logged("prepare again app=220") && move.Done() == 0 && !unregistered);
  CHECK(Read(fixture.root / "data/runtime-state/installs.json") == records_before);
  CHECK(Read(save) == "player-save" && !fs::exists(fixture.root / "data/playden/games/steam-220.kept"));
  host.Shutdown();
}

// A moved game whose title was busy at startup is republished by Play first, which says so.
void PlayBeforeRepublishRepublishesFirst() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  auto config = MoveConfig(fixture, kit, &uuids, move);
  std::vector<std::string> progress;
  std::shared_ptr<RuntimeHost*> current = std::make_shared<RuntimeHost*>(nullptr);
  const auto log = config.log;
  config.log = [log, current, &progress](const std::string& line) {
    log(line);
    // Settings update requests run inside Play's republication; its progress text is up meanwhile.
    if (line.rfind("settings update request app=220", 0) == 0 && *current) {
      const auto shared = (*current)->Shared();
      if (const auto found = shared->play_progress.find(220); found != shared->play_progress.end())
        progress.push_back(found->second);
    }
  };
  PrepareThenMoveGames(fixture, config, move);
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  RuntimeHost host(config);
  *current = &host;
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(move.Logged("startup moved app=220 waits: Close the game first.") && move.Done() == 0);
  // The mark is durable; the published descriptor still names the old folder.
  CHECK(Read(fixture.root / "data/runtime-state/installs.json").find(R"("moved_from":"/data/homebrew/FAKE77031/games/220")") !=
        std::string::npos);
  CHECK(started.installs[0].state == PreparationState::kReady);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/game-launch.json").find(kOldGames) != std::string::npos);
  // While it runs, Play says why it can't update the game.
  host.Play(220);
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.play_refusals.count(220) != 0; });
  CHECK(refused.play_refusals.at(220) == "This game needs an update for Playden 0.6 first: Close the game first.");
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  host.Play(220);
  const auto played = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
  CHECK(played.play_progress.empty() && !played.play_refusals.count(220));
  CHECK(progress == std::vector<std::string>{kPlayUpdatingForLayout});
  CHECK(move.Logged("play moved app=220 republished") && move.Done() == 1);
  CheckRepublishedAtNewRoot(fixture);
  CHECK(host.Handoff());
  *current = nullptr;
  host.Shutdown();
}

// USB and M.2 installs, and folders that only share the old root's spelling, keep their paths.
void MigrationSkipsRecordsOutsideOldPrefix() {
  Fixture fixture;
  auto config = fixture.Config();  // no kit: the records are rewritten all the same
  // The real layout journal on state_root (no injected callbacks).
  fs::create_directories(config.state_root); CHECK(chmod(config.state_root.c_str(), 0700) == 0);
  const std::vector<std::pair<std::uint32_t, std::string>> games = {
      {400, "/mnt/usb1/playden-games/400"}, {500, "/mnt/ext1/playden-games/500"},
      {600, "/data/homebrew/FAKE77031/games-old/600"}, {700, "/data/homebrew/FAKE77031/games"},
      {220, "/data/homebrew/FAKE77031/games/220"}};
  {
    playden::Storage storage(config.state_root.string());
    RuntimeInstallRepository repository(storage, "installs.json", [](const std::string&) {
      return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree};
    });
    CHECK(repository.Load().ok());
    int title = 1;
    for (const auto& [app, path] : games) {
      auto record = Record();
      ReserveRequest request;
      request.install_id = "steam-" + std::to_string(app); request.runtime_number = app;
      request.default_profile_id = record.default_profile_id; request.runtime_bundle_id = record.runtime_bundle_id;
      request.recipe_id = record.recipe_id; request.recipe_version = record.recipe_version;
      request.launch_entry_id = record.launch_entry_id; request.launch_path = record.launch_path;
      request.install_path = path; request.volume_id = record.volume_id;
      char id[10]; std::snprintf(id, sizeof(id), "FAKE78%03d", title++);
      request.kit_title_variants = {id};
      CHECK(repository.Reserve(request).ok());
    }
  }
  {
    // A trailing slash on the old root is ignored.
    const std::string journal = std::string(R"({"step":"games-moved","from":")") + kOldGames + R"(/","to":")" + kNewGames +
                                R"(","postponed":0})";
    CHECK(playden::Storage(config.state_root.string()).WriteDurable(kLayoutJournal, playden::Bytes(journal.begin(), journal.end())).ok());
  }
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  host.Shutdown();
  playden::Storage storage(config.state_root.string());
  RuntimeInstallRepository repository(storage, "installs.json", {});
  CHECK(repository.Load().ok());
  std::map<std::uint32_t, RuntimeInstall> records;
  for (const auto& record : repository.Snapshot()) records[record.runtime_number] = record;
  CHECK(records.size() == 5);
  for (const std::uint32_t app : {400u, 500u, 600u}) {
    CHECK(records[app].moved_from.empty());
    CHECK(records[app].install_path == std::find_if(games.begin(), games.end(), [app](const auto& g) { return g.first == app; })->second);
  }
  CHECK(records[700].install_path == kNewGames && records[700].moved_from == "/data/homebrew/FAKE77031/games");
  CHECK(records[220].install_path == "/data/playden-games/220" && records[220].moved_from == "/data/homebrew/FAKE77031/games/220");
  // None was Ready, so nothing waits for a republication; the marks stay for their preparation.
  const auto finished = LoadLayoutJournal(config.state_root);
  CHECK(finished && finished->step == "done");
}

// The mark survives restarts: a start that can't republish (busy title) and one interrupted in
// the middle of the republication both leave the move to the next one, which finishes it.
void RestartDuringRepublishResumes() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  auto config = MoveConfig(fixture, kit, &uuids, move);
  PrepareThenMoveGames(fixture, config, move);
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    WaitFor(host, Started);
    CHECK(move.Logged("startup moved app=220 waits:") && move.Done() == 0);
    host.Shutdown();
  }
  CHECK(Read(fixture.root / "data/runtime-state/installs.json").find("moved_from") != std::string::npos);
  // The next start's republication is cut off after its request is saved: the console stops
  // answering whether the title runs.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  auto interrupted = config;
  const auto log = config.log;
  auto activity = fixture.activity;
  interrupted.log = [log, activity](const std::string& line) {
    log(line);
    if (line.rfind("settings update request app=220", 0) == 0) activity->store(static_cast<int>(NativeActivity::kUnknown));
  };
  {
    RuntimeHost host(interrupted);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(move.Logged("startup moved app=220 failed:") && move.Done() == 0);
    CHECK(started.installs[0].recovery_pending);
    host.Shutdown();
  }
  CHECK(Read(fixture.root / "data/runtime-state/installs.json").find("moved_from") != std::string::npos);
  // Restarted: the saved request is recovery, which Update resumes; Play says what is missing.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  CHECK(move.Done() == 0);
  host.Play(220);
  const auto refused = WaitFor(host, [](const RuntimeSnapshot& s) { return s.play_refusals.count(220) != 0; });
  CHECK(refused.play_refusals.at(220).rfind("This game needs an update for Playden 0.6 first: ", 0) == 0);
  host.Prepare({220, "/data/playden-games/220", "internal", "Half-Life \"2\""});
  const auto resumed = WaitFor(host, [](const RuntimeSnapshot& s) {
    return !s.preparing_app && s.prepares_handled > 0 && !s.installs.empty() && !s.installs[0].recovery_pending;
  }, 60);
  if (resumed.refusals.count(220)) std::fprintf(stderr, "Resume refused: %s\n", resumed.refusals.at(220).c_str());
  CHECK(!resumed.refusals.count(220) && resumed.installs[0].state == PreparationState::kReady);
  CHECK(move.Done() == 1);
  CheckRepublishedAtNewRoot(fixture);
  host.Play(220);
  CHECK(WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30).handoff_ready);
  CHECK(host.Handoff());
  host.Shutdown();
}

// A moved game whose folder is gone from the new place is not published over: it keeps its mark
// and the move stays unfinished.
// A schema-2 (owned) game whose folder is gone (a stale record, an unplugged drive) is not
// republished unowned: nothing is armed, so no journal waits on the record.
void UnownedUpgradeSkippedWhenFolderMissing() {
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
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    char id[37]; std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012u", ++uuids);
    return std::string(id);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  config.launch_title = [](const std::string&, const std::vector<std::string>&) { return TitleResult{true, 0, "launch"}; };
  GameSettingsRecord saved;
  config.settings_record = [&saved](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  config.log = [&](const std::string& line) { std::lock_guard<std::mutex> lock(lines_mutex); lines.push_back(line); };
  const auto logged = [&](const std::string& prefix) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(prefix) != std::string::npos; });
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  CHECK(chmod(game.c_str(), 0777) == 0);
  const auto metadata = fixture.root / "data/playden/games/steam-220/library/profiles/.playden-publication";
  const auto installs = fixture.root / "data/runtime-state/installs.json";
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);
    host.Shutdown();
  }
  CHECK(Read(metadata).find("schema=2\n") != std::string::npos);
  CHECK(Read(installs).find("\"journal\":{") == std::string::npos);
  const auto published = Read(metadata);
  const auto records = Read(installs);
  fs::remove_all(game);
  kit.capabilities = {"dxvk", "game-root-unowned", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  CHECK(logged("unowned upgrade skipped: game folder missing ("));
  CHECK(logged("startup unowned app=220 deferred: "));
  CHECK(!logged("settings update request"));
  CHECK(Read(installs).find("\"journal\":{") == std::string::npos && Read(installs) == records);
  CHECK(Read(metadata) == published);
  host.Play(220);
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
  CHECK(done.play_refusals.count(220) && done.play_refusals.at(220).find("installation volume is unavailable") != std::string::npos);
  CHECK(!done.handoff_ready);
  CHECK(!logged("settings update request"));
  CHECK(Read(installs).find("\"journal\":{") == std::string::npos);
  CHECK(Read(metadata) == published);
  host.Shutdown();
}

// A moved Ready record whose folder is gone (the game was installed again elsewhere) can't be
// republished: it keeps its mark (Uninstall or Recreate bottle clear it), and the move still finishes.
void MovedGameMissingAtNewRootLetsTheMoveFinish() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  const auto config = MoveConfig(fixture, kit, &uuids, move);
  PrepareThenMoveGames(fixture, config, move);
  fs::remove_all(fixture.root / "data/playden-games/220");
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  CHECK(move.Logged("startup moved app=220 failed:") && move.Logged("layout move done") && move.Done() == 1);
  CHECK(Read(fixture.root / "data/runtime-state/installs.json").find(R"("moved_from":"/data/homebrew/FAKE77031/games/220")") !=
        std::string::npos);
  CHECK(Read(fixture.root / "data/homebrew/FAKE78001/game-launch.json").find(kOldGames) != std::string::npos);
  host.Shutdown();
}

// bottle-profile-shared: the bottle's way to its user profile is 0711 and the profile is open to FTP.
RuntimeKit SharedProfileKit(const fs::path& kit) {
  auto admitted = SyntheticKit(kit);
  admitted.capabilities.push_back(kKitBottleProfileShared);
  std::sort(admitted.capabilities.begin(), admitted.capabilities.end());
  return admitted;
}
unsigned ModeOf(const fs::path& path) {
  struct stat info{};
  CHECK(lstat(path.c_str(), &info) == 0);
  return static_cast<unsigned>(info.st_mode & 07777);
}
// The six folders on the way from the data root to the profile.
std::vector<fs::path> ProfileWay(const fs::path& data) {
  const auto bottle = data / "library/prefixes/game";
  return {data, data / "library", data / "library/prefixes", bottle, bottle / "drive_c", bottle / "drive_c/users"};
}

// A prepared HL2 title with a kit that the test can swap, Play from Playden and a working remover.
struct ProfileRun {
  Fixture fixture;
  RuntimeKit kit;
  unsigned uuids = 0;
  std::vector<std::vector<std::string>> launches;
  RuntimeHostConfig config;
  const fs::path data = fixture.root / "data/playden/games/steam-220";
  const fs::path profile = data / "library/prefixes/game/drive_c/users/prospero";
  explicit ProfileRun(bool shared) {
    kit = shared ? SharedProfileKit(fixture.root / "kit") : SyntheticKit(fixture.root / "kit");
    config = fixture.Config();
    config.host_namespace_prefix = fixture.root;
    config.session_data_root = fixture.root / "data";
    config.package_paths.console_data_root = "/data";
    config.package_paths.host_data_root = fixture.root / "data";
    config.package_operations.hash_file = HashArtifactFile;
    config.package_operations.hash_bytes = HashArtifactBytes;
    config.package_operations.free_space_unverifiable = true;
    config.package_operations.validate_bottle = [](const fs::path& bottle) {
      return fs::is_regular_file(bottle / "system.reg") && fs::is_regular_file(bottle / "user.reg");
    };
    config.admit_kit = [this](std::string*) { return std::optional<RuntimeKit>(kit); };
    config.uuid = [this]() -> std::optional<std::string> {
      ++uuids;
      char id[37];
      std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012u", uuids);
      return std::string(id);
    };
    config.unix_time = [] { return std::int64_t{1760000000}; };
    config.launch_title = [this](const std::string&, const std::vector<std::string>& arguments) {
      launches.push_back(arguments);
      return TitleResult{true, 0, "launch"};
    };
    config.removal_confirm_polls = 3;
    config.removal_poll_interval = std::chrono::milliseconds(1);
    config.unregister_title = [this](const std::string&, const std::string&) {
      fixture.registered = false; fixture.submitted = false;
      return TitleRemoverResult{true, 0, "answered"};
    };
    Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  }
  RuntimeSnapshot Prepare(RuntimeHost& host) {
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) ||
          (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
    CHECK(!done.refusals.count(220) && done.installs.size() == 1);
    return done;
  }
  RuntimeSnapshot Maintain(RuntimeHost& host, RuntimeMaintenance action) {
    const auto before = host.Snapshot().maintenance_handled;
    host.Maintain(220, action);
    return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60);
  }
  // Play from Playden up to the handoff; returns the session it starts.
  std::string Play(RuntimeHost& host) {
    const auto handled = host.Snapshot().plays_handled;
    host.Play(220);
    const auto played = WaitFor(host, [handled](const RuntimeSnapshot& s) {
      return s.handoff_ready || s.plays_handled > handled;
    }, 30);
    if (!played.handoff_ready && played.play_refusals.count(220))
      std::fprintf(stderr, "Play refused: %s\n", played.play_refusals.at(220).c_str());
    WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready; }, 30);
    CHECK(host.Handoff());
    return launches.back().at(0).substr(std::string("session=").size());
  }
  // The title's wrapper records the session in the private data root.
  void Session(const std::string& id, const std::string& phase) {
    const auto sessions = data / "logs/sessions";
    fs::create_directories(sessions);
    CHECK(chmod((data / "logs").c_str(), 0700) == 0 && chmod(sessions.c_str(), 0700) == 0);
    Write(sessions / (id + ".json"),
          R"({"schema_version":1,"session_id":")" + id +
              R"(","install_id":"steam-220","title_id":"FAKE78001","origin":"playden","settings_revision":0,)"
              R"("runtime_bundle_id":"wine-wow64","phase":")" + phase + "\"}\n", 0600);
  }
};

// Whether the linked publication verifier (the pinned kit's) admits a 0711 data root. One from
// before bottle-profile-shared refuses it, and with it every start check of a shared bottle:
// those checks wait for the pin that brings the capability.
bool VerifierAdmitsSharedBottle() {
  static const bool admits = [] {
    ProfileRun run(false);
    {
      RuntimeHost host(run.config);
      CHECK(host.Start());
      run.Prepare(host);
      host.Shutdown();
    }
    CHECK(chmod(run.data.c_str(), 0711) == 0);
    FilesystemPackageInstaller installer(run.kit.view, run.config.package_paths, run.config.package_operations);
    auto record = Record();
    record.title_id = "FAKE78001";
    record.data_root = "/data/playden/games/steam-220";
    const bool ok = installer.AdmissionDiagnostic(record) == "ok";
    if (!ok)
      std::fprintf(stderr, "note: the linked publication verifier refuses 0711 bottles; "
                           "bottle-profile-shared start checks are skipped until the kit pin admits them\n");
    return ok;
  }();
  return admits;
}

// A kit with bottle-profile-shared makes the bottle's way 0711 and its profile 0777/0666. Saves
// Wine makes (0600, umask 077) are opened again at Play and when the session ends; Repair keeps
// the modes, backups stay private, and saves kept across an uninstall come back open.
void SharedProfileKitOpensSaves() {
  if (!VerifierAdmitsSharedBottle()) return;
  ProfileRun run(true);
  RuntimeHost host(run.config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  auto ready = run.Prepare(host);
  CHECK(ready.installs[0].admission == "ok");
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0711);
  for (const auto* folder : {".playden-control", "library/profiles", "library/input"}) CHECK(ModeOf(run.data / folder) == 0700);
  CHECK(ModeOf(run.data / "library/prefixes/game/user.reg") == 0600);
  for (const auto* folder : {"", "AppData", "AppData/Roaming", "Documents", "Saved Games"})
    CHECK(ModeOf(run.profile / folder) == 0777);

  // Between sessions (from Home, or over FTP) Wine left a private save: Play opens it first.
  const auto game = run.profile / "AppData/Roaming/Game";
  Write(game / "save.dat", "first", 0600);
  CHECK(chmod(game.c_str(), 0700) == 0);
  const auto session = run.Play(host);
  CHECK(ModeOf(game) == 0777 && ModeOf(game / "save.dat") == 0666);
  // The game saves while it runs; once its session ends the new save is opened too.
  run.Session(session, "running");
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kRunning; }, 30);
  Write(game / "later.dat", "second", 0600);
  run.Session(session, "exited");
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kExited; }, 30);
  CHECK(ModeOf(game / "later.dat") == 0666);
  CHECK(ModeOf(run.data / "logs/sessions") == 0700 && ModeOf(run.data / "library/prefixes/game/user.reg") == 0600);

  // Repair keeps the modes and the start check passes.
  auto result = run.Maintain(host, RuntimeMaintenance::kRepair);
  CHECK(result.maintenance.at(220).ok);
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0711);
  // Backups live in Playden's private backup folder.
  result = run.Maintain(host, RuntimeMaintenance::kBackupSaves);
  CHECK(result.maintenance.at(220).ok);
  const auto backups = run.fixture.root / "data/playden/games/steam-220.saves";
  CHECK(ModeOf(backups) == 0700 && ModeOf(backups / "1760000000/users/prospero/AppData/Roaming/Game/save.dat") == 0600);

  // Uninstalled keeping saves, prepared again: the saves come back open.
  const auto removals = host.Snapshot().removals_handled;
  host.Remove(220, false);
  auto removed = WaitFor(host, [removals](const RuntimeSnapshot& s) { return s.removals_handled > removals; }, 30);
  CHECK(removed.installs.empty() && removed.kept_bottles.size() == 1);
  const auto kept = run.fixture.root / "data/playden/games/steam-220.kept";
  CHECK(ModeOf(kept / "library/prefixes/game/drive_c/users/prospero/AppData/Roaming/Game/save.dat") == 0666);
  ready = run.Prepare(host);
  CHECK(ready.kept_bottles.empty() && Read(game / "save.dat") == "first");
  CHECK(ModeOf(game) == 0777 && ModeOf(game / "save.dat") == 0666 && ModeOf(game / "later.dat") == 0666);
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0711);
  host.Shutdown();
}

// A bottle made before the kit had bottle-profile-shared (all 0700) is opened when Playden starts
// with the new kit, and again by Repair and by the first Play if it was tightened since.
void ExistingBottleOpensWithSharedKit() {
  if (!VerifierAdmitsSharedBottle()) return;
  ProfileRun run(false);
  {
    RuntimeHost host(run.config);
    CHECK(host.Start());
    run.Prepare(host);
    host.Shutdown();
  }
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0700);
  Write(run.profile / "Documents/My Games/settings.ini", "[video]\n", 0600);
  run.kit = SharedProfileKit(run.fixture.root / "kit");
  const auto tighten = [&run] {
    for (const auto& folder : ProfileWay(run.data)) CHECK(chmod(folder.c_str(), 0700) == 0);
    CHECK(chmod((run.profile / "Documents/My Games/settings.ini").c_str(), 0600) == 0);
  };
  RuntimeHost host(run.config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0711);
  CHECK(ModeOf(run.profile / "Documents/My Games") == 0777 && ModeOf(run.profile / "Documents/My Games/settings.ini") == 0666);
  tighten();
  CHECK(run.Maintain(host, RuntimeMaintenance::kRepair).maintenance.at(220).ok);
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0711);
  CHECK(ModeOf(run.profile / "Documents/My Games/settings.ini") == 0666);
  tighten();
  run.Play(host);
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0711);
  CHECK(ModeOf(run.profile / "Documents/My Games/settings.ini") == 0666);
  host.Shutdown();
}

// Without the capability nothing opens: Wine's saves stay as they are after Play and after a
// session. A bottle a capable kit opened (then a kit downgrade) fails the strict start check
// until Repair, or the next Play, puts its way back to 0700; the profile keeps its modes.
void KitWithoutSharedProfileKeepsBottlePrivate() {
  ProfileRun run(false);
  RuntimeHost host(run.config);
  CHECK(host.Start());
  run.Prepare(host);
  const auto save = run.profile / "Saved Games/Game/save.dat";
  Write(save, "save", 0600);
  CHECK(chmod(save.parent_path().c_str(), 0700) == 0);
  const auto session = run.Play(host);
  run.Session(session, "exited");
  WaitFor(host, [](const RuntimeSnapshot& s) { return s.launch && s.launch->state == LaunchState::kExited; }, 30);
  CHECK(ModeOf(save) == 0600 && ModeOf(save.parent_path()) == 0700 && ModeOf(run.profile) == 0700);
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0700);

  const auto open_up = [&run, &save] {
    for (const auto& folder : ProfileWay(run.data)) CHECK(chmod(folder.c_str(), 0711) == 0);
    CHECK(chmod(run.profile.c_str(), 0777) == 0 && chmod(save.c_str(), 0666) == 0);
  };
  open_up();
  auto result = run.Maintain(host, RuntimeMaintenance::kRepair);
  if (!result.maintenance.at(220).ok) std::fprintf(stderr, "Repair: %s\n", result.maintenance.at(220).message.c_str());
  CHECK(result.maintenance.at(220).ok && result.installs[0].admission == "ok");
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0700);
  CHECK(ModeOf(run.profile) == 0777 && ModeOf(save) == 0666);
  open_up();
  run.Play(host);
  for (const auto& folder : ProfileWay(run.data)) CHECK(ModeOf(folder) == 0700);
  CHECK(ModeOf(save) == 0666);
  host.Shutdown();
}

// A game published owned (schema 2) that then had settings updates committed under the same
// kit (revision bumps, with registry edits that leave a registry backup and archived
// operations in its control folder). A capable kit's startup unowned upgrade republishes it at
// the same revision, after those committed operations (scenario 0).
// When the control folder's last operation can't be read nor set aside (1: a link in its place),
// the upgrade's capture is refused with the loader's reason, its request is dropped at once and
// the game keeps playing under its owned publication: no recovery is needed. A last operation
// that is gone (2) is not needed: the receipt and the publication prove the previous state.
// A capture cut off while its request was armed (the lock held, as a game running from Home
// holds it) leaves that request on the record (what an earlier build left on a console).
// The next start drops it and tries the upgrade again (3; with the unreadable operation, 6: the
// upgrade is deferred again and the game plays owned); Update resumes it (5), or, with the
// unreadable operation, drops it and goes on as for any Ready game (4).
// A last operation that no longer loads (7: another build's codec, trailing bytes; 9: its registry
// backup lost its private mode) is set aside under its own name once the committed receipt and the
// published metadata prove the previous publication, and the upgrade goes through; with a receipt
// that doesn't (8), it stays, the upgrade is deferred and its request kept for Resume.
void UnownedUpgradeAfterCommittedSettingsUpdates(int scenario) {
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
  auto kit = SyntheticKit(fixture.root / "kit");
  kit.capabilities = {"dxvk", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  config.admit_kit = [&kit](std::string*) { return std::optional<RuntimeKit>(kit); };
  unsigned uuids = 0;
  config.uuid = [&uuids]() -> std::optional<std::string> {
    char id[37]; std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012u", ++uuids);
    return std::string(id);
  };
  config.unix_time = [] { return std::int64_t{1760000000}; };
  config.session_data_root = fixture.root / "data";
  config.launch_title = [](const std::string&, const std::vector<std::string>&) { return TitleResult{true, 0, "launch"}; };
  GameSettingsRecord saved;
  config.settings_record = [&saved](const playden::GameId&) -> std::optional<GameSettingsRecord> { return saved; };
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  config.log = [&](const std::string& line) { std::lock_guard<std::mutex> lock(lines_mutex); lines.push_back(line); };
  const auto logged = [&](const std::string& text) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
  };
  const auto game = fixture.root / "data/homebrew/FAKE77031/games/220";
  Write(game / "hl2.exe", "existing-game", 0644);
  const auto metadata = fixture.root / "data/playden/games/steam-220/library/profiles/.playden-publication";
  const auto control = fixture.root / "data/playden/games/steam-220/.playden-control";
  const auto installs = fixture.root / "data/runtime-state/installs.json";
  const auto requests = fixture.root / "data/runtime-state/requests";
  const auto play = [&](RuntimeHost& host) {
    host.Play(220);
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
    if (done.play_refusals.count(220)) std::fprintf(stderr, "Play refused: %s\n", done.play_refusals.at(220).c_str());
    CHECK(done.handoff_ready && host.Handoff());
  };
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).kit_available);
    host.Prepare(Hl2());
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return !s.preparing_app && (s.refusals.count(220) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
    }, 60);
    if (done.refusals.count(220)) std::fprintf(stderr, "Prepare refused: %s\n", done.refusals.at(220).c_str());
    CHECK(!done.refusals.count(220) && done.installs[0].state == PreparationState::kReady);
    const auto maintain = [&host](RuntimeMaintenance action) {
      const auto before = host.Snapshot().maintenance_handled;
      host.Maintain(220, action);
      return WaitFor(host, [before](const RuntimeSnapshot& s) { return s.maintenance_handled > before; }, 60).maintenance.at(220);
    };
    // Several committed updates: a registry edit (Windows version) and a DXVK setting.
    saved.revision = 2; saved.overrides = {{"windows_version", "win7"}};
    auto result = maintain(RuntimeMaintenance::kApplySettings);
    if (!result.ok) std::fprintf(stderr, "update 2: %s\n", result.message.c_str());
    CHECK(result.ok && result.message == "Settings applied.");
    saved.revision = 3; saved.overrides = {{"windows_version", "winxp"}, {"frame_limit", "30"}};
    result = maintain(RuntimeMaintenance::kApplySettings);
    if (!result.ok) std::fprintf(stderr, "update 3: %s\n", result.message.c_str());
    CHECK(result.ok && result.message == "Settings applied.");
    saved.revision = 4; saved.overrides = {{"windows_version", "winxp"}, {"frame_limit", "60"}};
    result = maintain(RuntimeMaintenance::kApplySettings);
    if (!result.ok) std::fprintf(stderr, "update 4: %s\n", result.message.c_str());
    CHECK(result.ok && result.message == "Settings applied.");
    CHECK(host.Snapshot().installs[0].settings_revision == 4);
    host.Shutdown();
  }
  const auto owned = Read(metadata);
  CHECK(owned.find("schema=2\n") != std::string::npos);
  CHECK(Read(installs).find("\"journal\":{") == std::string::npos);
  const auto backup = control / "registry-backup-00000000-0000-4000-8000-000000000004.v1";
  CHECK(fs::is_regular_file(control / "settings-operation.v1") && fs::is_regular_file(backup));
  CHECK(fs::is_regular_file(control / "settings-operation-00000000-0000-4000-8000-000000000003.v1"));
  const bool unreadable = scenario == 1 || scenario == 4 || scenario == 6;
  const auto operation = Read(control / "settings-operation.v1");
  if (unreadable) {
    fs::rename(control / "settings-operation.v1", control / "settings-operation.saved.v1");
    fs::create_symlink("settings-operation.saved.v1", control / "settings-operation.v1");
  }
  if (scenario == 2) fs::remove(control / "settings-operation.v1");
  if (scenario == 9) CHECK(chmod(backup.c_str(), 0644) == 0);
  const auto aside = control / "settings-operation.unreadable-1760000000.v1";
  if (scenario == 7 || scenario == 8) Write(control / "settings-operation.v1", operation + "trailing", 0600);
  const auto kept_unreadable = [&] {
    const bool kept = logged("Existing frozen operation is corrupt or unavailable: Operation journal could not be inspected") &&
           logged("(not set aside: ") && !logged("settings operation set aside") && !fs::exists(aside) &&
           fs::is_symlink(control / "settings-operation.v1");
    if (!kept) {
      std::lock_guard<std::mutex> guard(lines_mutex);
      for (const auto& line : lines) if (line.find("220") != std::string::npos) std::fprintf(stderr, "log: %s\n", line.c_str());
      std::fprintf(stderr, "scenario %d: aside=%d symlink=%d\n", scenario, int(fs::exists(aside)), int(fs::is_symlink(control / "settings-operation.v1")));
    }
    return kept;
  };
  const auto receipt = Read(control / "publication-state.v1");
  if (scenario == 8) Write(control / "publication-state.v1", "not a receipt", 0600);
  kit.capabilities = {"dxvk", "game-root-unowned", "offline_bottle", "per_game_title", kKitDxvkConfig, "wow64_i386"};
  const auto request = requests / "settings-request-00000000-0000-4000-8000-000000000005.v1";
  const auto journaled = [&] { return Read(installs).find("\"journal\":{") != std::string::npos; };
  const auto unowned_published = [&] {
    const auto published = Read(metadata);
    return published.find("schema=3\n") != std::string::npos && published.find("game_root.unowned=1\n") != std::string::npos;
  };
  if (scenario >= 3 && scenario <= 6) {
    // The capture finds the lock held: its request stays armed on the record, nothing changed.
    const int lock = open((control / "publication.lock").c_str(), O_RDONLY);
    CHECK(lock >= 0 && flock(lock, LOCK_SH) == 0);
    RuntimeHost host(config);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(logged("startup unowned app=220 deferred: "));
    CHECK(logged("unowned upgrade request 00000000-0000-4000-8000-000000000005 kept: "));
    CHECK(started.installs[0].recovery_pending);
    CHECK(journaled() && Read(installs).find("\"step\":\"capture-update\"") != std::string::npos);
    CHECK(fs::is_regular_file(request) && Read(metadata) == owned);
    if (scenario == 3 || scenario == 6) {
      host.Shutdown();
      CHECK(close(lock) == 0);
      { std::lock_guard<std::mutex> guard(lines_mutex); lines.clear(); }
      RuntimeHost restarted(config);
      CHECK(restarted.Start());
      WaitFor(restarted, Started);
      CHECK(logged("unowned upgrade request 00000000-0000-4000-8000-000000000005 dropped: left by an earlier start"));
      CHECK(!fs::exists(request));
      if (scenario == 6) {
        // The upgrade tried again is deferred again, with its reason, and its request dropped.
        CHECK(kept_unreadable());
        CHECK(logged("unowned upgrade request 00000000-0000-4000-8000-000000000006 dropped: "));
        CHECK(logged("startup unowned app=220 deferred: "));
        CHECK(!journaled() && Read(metadata) == owned);
        CHECK(restarted.Snapshot().leftover_requests == 4);
      } else {
        CHECK(logged("startup unowned app=220 republished"));
        CHECK(!journaled() && unowned_published());
      }
      CHECK(restarted.Snapshot().installs[0].settings_revision == 4 && !restarted.Snapshot().installs[0].recovery_pending);
      play(restarted);
      CHECK(!journaled() && (scenario == 6 ? Read(metadata) == owned : unowned_published()));
      restarted.Shutdown();
      return;
    }
    CHECK(close(lock) == 0);
    // Update (Resume on the game page) takes the request up.
    const auto before = host.Snapshot().prepares_handled;
    host.Prepare(Hl2());
    const auto updated = WaitFor(host, [before](const RuntimeSnapshot& s) { return s.prepares_handled > before && !s.preparing_app; }, 60);
    if (updated.refusals.count(220)) std::fprintf(stderr, "Update: %s\n", updated.refusals.at(220).c_str());
    if (scenario == 5) {
      // Resumed: the saved request completes as unowned.
      CHECK(!updated.refusals.count(220));
      CHECK(logged("publication resume journal=settings-request/capture-update state=0"));
      CHECK(unowned_published());
    } else {
      // Its capture refused again, the request is dropped and the update goes on: the game is as
      // before the upgrade existed.
      CHECK(kept_unreadable());
      CHECK(logged("unowned upgrade request 00000000-0000-4000-8000-000000000005 dropped: "));
      CHECK(updated.refusals.count(220) && updated.refusals.at(220) == "This game is already prepared.");
      CHECK(Read(metadata) == owned && !fs::exists(request));
    }
    CHECK(!journaled() && !updated.installs[0].recovery_pending && updated.installs[0].settings_revision == 4);
    play(host);
    host.Shutdown();
    return;
  }
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  if (scenario == 8) {
    CHECK(logged("Existing frozen operation is corrupt or unavailable: Operation is truncated, malformed, altered or has trailing bytes. (kept: "));
    CHECK(!logged("settings operation set aside"));
    // Without a receipt that proves the committed publication nothing is touched, not even the
    // request: the title can't pass its start check either way, and Resume keeps the evidence.
    CHECK(logged("unowned upgrade request 00000000-0000-4000-8000-000000000005 kept: ") && logged("code=publication-discard-receipt"));
    CHECK(logged("startup unowned app=220 deferred: "));
    CHECK(journaled() && fs::exists(request) && Read(metadata) == owned);
    CHECK(Read(control / "settings-operation.v1") == operation + "trailing" && !fs::exists(aside));
    CHECK(host.Snapshot().installs[0].recovery_pending);
    host.Shutdown();
    return;
  }
  if (scenario == 7 || scenario == 9) {
    CHECK(logged(std::string("settings operation set aside install=steam-220 as settings-operation.unreadable-1760000000.v1: ") +
                 (scenario == 7 ? "Operation is truncated" : "Referenced registry backup is not valid")));
    CHECK(Read(aside) == (scenario == 7 ? operation + "trailing" : operation));
    CHECK(fs::is_regular_file(control / "settings-operation.v1") && Read(control / "settings-operation.v1") != Read(aside));
    CHECK(Read(control / "publication-state.v1") != receipt);
  }
  if (scenario == 1) {
    CHECK(kept_unreadable());
    CHECK(logged("unowned upgrade request 00000000-0000-4000-8000-000000000005 dropped: "));
    CHECK(logged("startup unowned app=220 deferred: "));
    CHECK(!journaled() && !fs::exists(request) && Read(metadata) == owned);
    // The preparation's and the committed updates' requests are the usual leftovers; the dropped
    // one is gone.
    CHECK(host.Snapshot().leftover_requests == 4 && !host.Snapshot().installs[0].recovery_pending);
    // Play tries the upgrade again, drops its request again, and starts the game as before.
    play(host);
    CHECK(logged("unowned upgrade request 00000000-0000-4000-8000-000000000006 dropped: "));
    CHECK(!journaled() && Read(metadata) == owned);
    host.Shutdown();
    return;
  }
  if (!logged("startup unowned app=220 republished")) {
    std::lock_guard<std::mutex> guard(lines_mutex);
    for (const auto& line : lines) if (line.find("app=220") != std::string::npos) std::fprintf(stderr, "log: %s\n", line.c_str());
  }
  CHECK(logged("startup unowned app=220 republished"));
  CHECK(!journaled() && unowned_published());
  CHECK(host.Snapshot().installs[0].settings_revision == 4 && !host.Snapshot().installs[0].recovery_pending);
  play(host);
  host.Shutdown();
}

// A moved game whose last operation no longer decodes (written by another build): the move's
// republish sets it aside, since the committed receipt and the published metadata prove the
// previous publication, and names the new folder.
void MovedGameWithUnreadableOperationRepublishes() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  const auto config = MoveConfig(fixture, kit, &uuids, move);
  PrepareThenMoveGames(fixture, config, move);
  const auto control = fixture.root / "data/playden/games/steam-220/.playden-control";
  const auto operation = Read(control / "settings-operation.v1");
  CHECK(!operation.empty());
  Write(control / "settings-operation.v1", operation + "trailing", 0600);
  RuntimeHost host(config);
  CHECK(host.Start());
  const auto started = WaitFor(host, Started);
  CHECK(started.installs.size() == 1 && started.installs[0].state == PreparationState::kReady);
  CHECK(move.Logged("settings operation set aside install=steam-220 as settings-operation.unreadable-1760000000.v1: Operation is truncated"));
  CHECK(move.Logged("startup moved app=220 republished") && move.Done() == 1);
  CHECK(Read(control / "settings-operation.unreadable-1760000000.v1") == operation + "trailing");
  CHECK(fs::is_regular_file(control / "settings-operation.v1") && Read(control / "settings-operation.v1") != operation + "trailing");
  CheckRepublishedAtNewRoot(fixture);
  host.Play(220);
  const auto played = WaitFor(host, [](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(220); }, 30);
  if (played.play_refusals.count(220)) std::fprintf(stderr, "Play refused: %s\n", played.play_refusals.at(220).c_str());
  CHECK(played.handoff_ready);
  host.Shutdown();
}

// "Copy into Playden" and "Move into Playden" of an imported game already prepared to play from its
// folder (import-games 4.3, 4.4): RuntimeHost::Relocate.
constexpr const char* kImportedId = "0f8fad5b-d9cb-469f-a165-70867728950e";
constexpr const char* kImportedRoot = "/data/playden-games/local-0f8fad5b-d9cb-469f-a165-70867728950e";
constexpr const char* kImportedSave = "data/playden/games/local-0f8fad5b-d9cb-469f-a165-70867728950e/library/prefixes/game/drive_c/users/steamuser/save.dat";

struct Imported {
  std::uint32_t number = 0;
  std::string title_id;
};

void WriteImportedGame(const fs::path& folder) {
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  Write(folder / "Bin/Game.exe", pe, 0644);
  Write(folder / "Data/game.pak", "pak", 0644);
  CHECK(chmod(folder.c_str(), 0777) == 0);
}

RuntimePrepareRequest ImportedRequest(std::uint32_t number, const std::string& folder) {
  RuntimePrepareRequest request{number, folder, folder.rfind("/data/", 0) == 0 ? "internal" : "usb0", "Imported"};
  request.launches = {{"", "", "Bin/Game.exe", "", ""}};
  request.game = playden::GameId::Local(kImportedId);
  return request;
}

// Prepares the imported game at `folder` (a console path) and leaves a save in its bottle.
Imported PrepareImported(Fixture& fixture, const RuntimeHostConfig& config, const std::string& folder) {
  WriteImportedGame(fixture.root / fs::path(folder).relative_path());
  Imported imported;
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).kit_available);
  imported.number = *host.RuntimeNumberFor(playden::GameId::Local(kImportedId));
  host.Prepare(ImportedRequest(imported.number, folder));
  const auto number = imported.number;
  const auto ready = WaitFor(host, [number](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(number) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (ready.refusals.count(number)) std::fprintf(stderr, "Prepare refused: %s\n", ready.refusals.at(number).c_str());
  CHECK(!ready.refusals.count(number) && ready.installs.size() == 1 && ready.installs[0].install_path == folder);
  imported.title_id = ready.installs[0].title_id;
  host.Shutdown();
  Write(fixture.root / kImportedSave, "player-save", 0600);
  return imported;
}

// The relocated game: same title, number and bottle; its record and publication name the new folder.
void CheckRelocated(Fixture& fixture, RuntimeHost& host, const Imported& imported, const std::string& volume) {
  const auto snapshot = host.Snapshot();
  CHECK(snapshot.installs.size() == 1);
  const auto& view = snapshot.installs[0];
  CHECK(view.title_id == imported.title_id && view.runtime_number == imported.number && view.state == PreparationState::kReady);
  CHECK(view.install_path == kImportedRoot && view.admission == "ok" && view.settings_revision == 1);
  const auto records = Read(fixture.root / "data/runtime-state/installs.json");
  CHECK(records.find(std::string("\"install_path\":\"") + kImportedRoot + "\"") != std::string::npos);
  CHECK(records.find("\"volume_id\":\"" + volume + "\"") != std::string::npos && records.find("moved_from") == std::string::npos);
  const auto descriptor = Read(fixture.root / "data/homebrew" / imported.title_id / "game-launch.json");
  CHECK(descriptor.find(std::string("\"game_root\":\"") + kImportedRoot + "\"") != std::string::npos);
  CHECK(Read(fixture.root / kImportedSave) == "player-save");
  // Play passes the title's whole start check.
  host.Play(imported.number);
  const auto number = imported.number;
  const auto played = WaitFor(host, [number](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(number); }, 30);
  if (played.play_refusals.count(number)) std::fprintf(stderr, "Play refused: %s\n", played.play_refusals.at(number).c_str());
  CHECK(played.handoff_ready && host.Handoff());
}

RuntimeRelocationResult RelocateNow(RuntimeHost& host, std::uint32_t number, std::function<std::string()> move_files = {}) {
  RuntimeRelocation request;
  request.runtime_number = number;
  request.to = kImportedRoot;
  request.volume_id = "internal";
  request.move_files = std::move(move_files);
  return host.Relocate(std::move(request), std::chrono::seconds(60));
}

// Copy later: a game prepared on a USB drive, copied into Playden. Its title follows the copy;
// the drive folder stays as it was.
void CopiedGameKeepsItsTitle() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  const auto config = MoveConfig(fixture, kit, &uuids, move);
  const std::string drive = "/mnt/usb0/playden/import/Imported";
  const auto imported = PrepareImported(fixture, config, drive);
  const auto registrations = fixture.registry_mutations;
  fs::create_directories(fixture.root / "data/playden-games");
  fs::copy(fixture.root / fs::path(drive).relative_path(), fixture.root / fs::path(kImportedRoot).relative_path(),
           fs::copy_options::recursive);
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    WaitFor(host, Started);
    const auto result = RelocateNow(host, imported.number);
    if (!result.message.empty()) std::fprintf(stderr, "Relocate: %s\n", result.message.c_str());
    CHECK(result.outcome == RuntimeRelocationResult::Outcome::kDone && !result.prepare_again && result.message.empty());
    CHECK(move.Logged("relocate app=" + std::to_string(imported.number) + " republished"));
    CheckRelocated(fixture, host, imported, "internal");
    CHECK(fixture.registry_mutations == registrations);  // the same title: nothing registered again
    CHECK(fs::is_regular_file(fixture.root / fs::path(drive).relative_path() / "Bin/Game.exe"));
    host.Shutdown();
  }
  // After a restart (the copy runner asks again when it stopped before its install record): done.
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  const auto again = RelocateNow(host, imported.number);
  CHECK(again.outcome == RuntimeRelocationResult::Outcome::kDone && again.message.empty());
  CHECK(host.Snapshot().installs[0].install_path == kImportedRoot);
  host.Shutdown();
}

// A preparation queued for the game names its old folder (it would move the game back there):
// the relocation waits for it, and goes once it's withdrawn.
void RelocationWaitsForQueuedPreparation() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  const auto config = MoveConfig(fixture, kit, &uuids, move);
  const std::string drive = "/mnt/usb0/playden/import/Imported";
  const auto imported = PrepareImported(fixture, config, drive);
  fs::create_directories(fixture.root / "data/playden-games");
  fs::copy(fixture.root / fs::path(drive).relative_path(), fixture.root / fs::path(kImportedRoot).relative_path(),
           fs::copy_options::recursive);
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  host.HoldPreparations(true);
  host.Prepare(ImportedRequest(imported.number, drive));
  unsigned copies = 0;
  const auto refused = RelocateNow(host, imported.number, [&]() -> std::string { ++copies; return {}; });
  CHECK(refused.outcome == RuntimeRelocationResult::Outcome::kBusy &&
        refused.message == "Finish setting up the game first.");
  CHECK(copies == 0 && host.Snapshot().installs[0].install_path == drive);
  CHECK(host.WithdrawPrepare(imported.number));
  host.HoldPreparations(false);
  const auto result = RelocateNow(host, imported.number, [&]() -> std::string { ++copies; return {}; });
  if (!result.message.empty()) std::fprintf(stderr, "Relocate: %s\n", result.message.c_str());
  CHECK(result.outcome == RuntimeRelocationResult::Outcome::kDone && copies == 1);
  CheckRelocated(fixture, host, imported, "internal");
  host.Shutdown();
}

// Move: the drop folder renamed under the title's publication lease; refused while the title runs.
void MovedGameKeepsItsTitle() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  const auto config = MoveConfig(fixture, kit, &uuids, move);
  const std::string drop = "/data/playden-import/Imported";
  const auto imported = PrepareImported(fixture, config, drop);
  fs::create_directories(fixture.root / "data/playden-games");
  const auto from = fixture.root / fs::path(drop).relative_path();
  const auto to = fixture.root / fs::path(kImportedRoot).relative_path();
  const auto lock_path = fixture.root / "data/playden/games" / ("local-" + std::string(kImportedId)) / ".playden-control/publication.lock";
  const auto held = [&] {
    const int fd = ::open(lock_path.c_str(), O_RDONLY);
    CHECK(fd >= 0);
    const bool busy = ::flock(fd, LOCK_SH | LOCK_NB) != 0;
    ::close(fd);
    return busy;
  };
  unsigned renames = 0;
  const auto rename = [&]() -> std::string {
    ++renames;
    CHECK(held());  // the lease is held through the rename
    if (!fs::exists(from) && fs::is_directory(to)) return {};
    return ::rename(from.c_str(), to.c_str()) == 0 ? std::string() : std::string("rename failed");
  };
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  // Its title runs (or is suspended): nothing moves.
  fixture.activity->store(static_cast<int>(NativeActivity::kActive));
  auto refused = RelocateNow(host, imported.number, rename);
  CHECK(refused.outcome == RuntimeRelocationResult::Outcome::kBusy && refused.message == "Close the game first." && renames == 0);
  fixture.activity->store(static_cast<int>(NativeActivity::kUnknown));
  refused = RelocateNow(host, imported.number, rename);
  CHECK(refused.outcome == RuntimeRelocationResult::Outcome::kBusy && renames == 0);
  // A Home launch's reader lease refuses it too, whatever the activity says.
  fixture.activity->store(static_cast<int>(NativeActivity::kStopped));
  const int reader = ::open(lock_path.c_str(), O_RDONLY);
  CHECK(reader >= 0 && ::flock(reader, LOCK_SH | LOCK_NB) == 0);
  refused = RelocateNow(host, imported.number, rename);
  CHECK(refused.outcome == RuntimeRelocationResult::Outcome::kBusy && renames == 0);
  ::close(reader);
  CHECK(fs::exists(from / "Bin/Game.exe") && host.Snapshot().installs[0].install_path == drop);
  // Stopped: renamed, then republished.
  const auto result = RelocateNow(host, imported.number, rename);
  if (!result.message.empty()) std::fprintf(stderr, "Relocate: %s\n", result.message.c_str());
  CHECK(result.outcome == RuntimeRelocationResult::Outcome::kDone && !result.prepare_again && renames == 1);
  CHECK(!fs::exists(from) && fs::is_regular_file(to / "Bin/Game.exe") && !held());
  CheckRelocated(fixture, host, imported, "internal");
  // A game without a PS5 title: the files move at once.
  const auto other = fixture.root / "data/playden-import/Other";
  WriteImportedGame(other);
  unsigned plain = 0;
  RuntimeRelocation unprepared;
  unprepared.to = "/data/playden-games/local-other";
  unprepared.move_files = [&]() -> std::string {
    ++plain;
    return ::rename(other.c_str(), (fixture.root / "data/playden-games/local-other").c_str()) == 0 ? std::string() : "failed";
  };
  const auto moved = host.Relocate(std::move(unprepared), std::chrono::seconds(30));
  CHECK(moved.outcome == RuntimeRelocationResult::Outcome::kDone && plain == 1);
  CHECK(fs::is_regular_file(fixture.root / "data/playden-games/local-other/Bin/Game.exe"));
  host.Shutdown();
}

// Restarts: after the rename (record not written), after the record (republish not done), after
// the republish. Each start leaves the game consistent and the next Relocate finishes it.
void RelocationResumesAfterRestart() {
  const std::string drop = "/data/playden-import/Imported";
  for (const int step : {1, 2}) {
    Fixture fixture;
    GamesMove move;
    const auto kit = SyntheticKit(fixture.root / "kit");
    unsigned uuids = 0;
    const auto config = MoveConfig(fixture, kit, &uuids, move);
    const auto imported = PrepareImported(fixture, config, drop);
    fs::create_directories(fixture.root / "data/playden-games");
    const auto from = fixture.root / fs::path(drop).relative_path();
    const auto to = fixture.root / fs::path(kImportedRoot).relative_path();
    unsigned renames = 0;
    const auto rename = [&]() -> std::string {
      ++renames;
      if (!fs::exists(from) && fs::is_directory(to)) return {};
      return ::rename(from.c_str(), to.c_str()) == 0 ? std::string() : std::string("rename failed");
    };
    if (step == 2) {
      // The record written (moved_from), then Playden stopped before the republish.
      playden::Storage storage(config.state_root.string());
      RuntimeInstallRepository repository(storage, "installs.json", {});
      CHECK(repository.Load().ok());
      auto record = repository.Snapshot().front();
      record.moved_from = record.install_path;
      record.install_path = kImportedRoot;
      CHECK(repository.UpdateAll({record}).ok());
    }
    // Step 1: renamed, then Playden stopped; the record still names the drop folder.
    CHECK(::rename(from.c_str(), to.c_str()) == 0);
    {
      RuntimeHost host(config);
      CHECK(host.Start());
      WaitFor(host, Started);
      const auto logged = move.Logged("startup moved app=" + std::to_string(imported.number) + " republished");
      if (step == 1) {
        // Nothing moves the record by itself: Relocate (the copy runner's resume) finishes it.
        CHECK(!logged && host.Snapshot().installs[0].install_path == drop);
      } else {
        // The next start republishes it like a game the 0.6 move left marked.
        CHECK(logged && host.Snapshot().installs[0].install_path == kImportedRoot);
      }
      const auto result = RelocateNow(host, imported.number, rename);
      if (!result.message.empty()) std::fprintf(stderr, "Relocate: %s\n", result.message.c_str());
      CHECK(result.outcome == RuntimeRelocationResult::Outcome::kDone && !result.prepare_again);
      CHECK(renames == (step == 1 ? 1u : 0u));  // after its record, nothing renames again
      CheckRelocated(fixture, host, imported, "internal");
      host.Shutdown();
    }
    // After the republish: a start changes nothing.
    std::size_t before = 0;
    {
      std::lock_guard<std::mutex> lock(move.mutex);
      before = move.lines.size();
    }
    RuntimeHost host(config);
    CHECK(host.Start());
    WaitFor(host, Started);
    {
      std::lock_guard<std::mutex> lock(move.mutex);
      for (std::size_t index = before; index < move.lines.size(); ++index)
        CHECK(move.lines[index].rfind("startup moved app=", 0) != 0);
    }
    CHECK(host.Snapshot().installs[0].install_path == kImportedRoot);
    host.Shutdown();
  }
}

// The moved publication can't be verified (its descriptor changed): the old title goes, keeping its
// saves, and the game is prepared again at its new folder with them.
void RelocationFallsBackToANewTitle() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  auto config = MoveConfig(fixture, kit, &uuids, move);
  config.removal_confirm_polls = 3;
  config.removal_poll_interval = std::chrono::milliseconds(1);
  config.unregister_title = [&fixture](const std::string&, const std::string&) {
    fixture.registered = false; fixture.submitted = false;
    return TitleRemoverResult{true, 0, "answered"};
  };
  const std::string drop = "/data/playden-import/Imported";
  const auto imported = PrepareImported(fixture, config, drop);
  fs::create_directories(fixture.root / "data/playden-games");
  const auto from = fixture.root / fs::path(drop).relative_path();
  const auto to = fixture.root / fs::path(kImportedRoot).relative_path();
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  const auto descriptor = fixture.root / "data/homebrew" / imported.title_id / "game-launch.json";
  const auto bytes = Read(descriptor);
  CHECK(!bytes.empty());
  fs::permissions(descriptor, fs::perms::owner_read | fs::perms::owner_write);
  std::ofstream(descriptor, std::ios::binary | std::ios::trunc) << bytes << " ";
  const auto result = RelocateNow(host, imported.number, [&]() -> std::string {
    return ::rename(from.c_str(), to.c_str()) == 0 ? std::string() : std::string("rename failed");
  });
  if (!result.message.empty()) std::fprintf(stderr, "Relocate: %s\n", result.message.c_str());
  CHECK(result.outcome == RuntimeRelocationResult::Outcome::kDone && result.prepare_again && result.message.empty());
  const auto removed = host.Snapshot();
  CHECK(removed.installs.empty() && fs::is_regular_file(to / "Bin/Game.exe"));
  const auto kept = std::find_if(removed.kept_bottles.begin(), removed.kept_bottles.end(),
                                 [](const KeptBottleView& view) { return view.game == playden::GameId::Local(kImportedId); });
  CHECK(kept != removed.kept_bottles.end() && kept->runtime_number == imported.number);
  // Play prepares it again where it is now; the saves come back.
  host.Prepare(ImportedRequest(imported.number, kImportedRoot));
  const auto number = imported.number;
  const auto ready = WaitFor(host, [number](const RuntimeSnapshot& s) {
    return !s.preparing_app && (s.refusals.count(number) || (!s.installs.empty() && s.installs[0].state == PreparationState::kReady));
  }, 60);
  if (ready.refusals.count(number)) std::fprintf(stderr, "Prepare refused: %s\n", ready.refusals.at(number).c_str());
  CHECK(!ready.refusals.count(number) && ready.installs[0].install_path == kImportedRoot);
  CHECK(ready.installs[0].runtime_number == imported.number);
  CHECK(Read(fixture.root / kImportedSave) == "player-save");
  host.Shutdown();
}

// A republish refused for a reason that isn't the moved publication failing its check (here the
// saved settings, which a restart or Play gets past): the old title stays, its record marked moved,
// and the next start publishes it at the new folder, as after the 0.6 games move.
void RelocationKeepsTheTitleWhenTheRepublishWaits() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  auto config = MoveConfig(fixture, kit, &uuids, move);
  const std::string drop = "/data/playden-import/Imported";
  const auto imported = PrepareImported(fixture, config, drop);
  fs::create_directories(fixture.root / "data/playden-games");
  const auto from = fixture.root / fs::path(drop).relative_path();
  const auto to = fixture.root / fs::path(kImportedRoot).relative_path();
  auto stale = std::make_shared<std::atomic<bool>>(true);
  const auto saved = config.settings_record;
  config.settings_record = [stale, saved](const playden::GameId& game) -> std::optional<GameSettingsRecord> {
    auto record = saved(game);
    if (record && stale->load()) record->revision = 0;
    return record;
  };
  {
    RuntimeHost host(config);
    CHECK(host.Start());
    WaitFor(host, Started);
    const auto result = RelocateNow(host, imported.number, [&]() -> std::string {
      return ::rename(from.c_str(), to.c_str()) == 0 ? std::string() : std::string("rename failed");
    });
    CHECK(result.outcome == RuntimeRelocationResult::Outcome::kDone && !result.prepare_again && !result.message.empty());
    CHECK(move.Logged("relocate app=" + std::to_string(imported.number) + " republish waits"));
    CHECK(!move.Logged("relocate app=" + std::to_string(imported.number) + " republish failed"));
    const auto snapshot = host.Snapshot();
    CHECK(snapshot.installs.size() == 1 && snapshot.installs[0].title_id == imported.title_id);
    CHECK(snapshot.installs[0].install_path == kImportedRoot);
    CHECK(Read(fixture.root / "data/runtime-state/installs.json").find("moved_from") != std::string::npos);
    host.Shutdown();
  }
  // The next start republishes it like a game the 0.6 move left marked.
  stale->store(false);
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  CHECK(move.Logged("startup moved app=" + std::to_string(imported.number) + " republished"));
  CheckRelocated(fixture, host, imported, "internal");
  host.Shutdown();
}

// Quitting while a relocation waits for the worker: the caller stops waiting at once ("Playden is
// closing"), the relocation never runs, and later ones are refused the same way.
void RelocationStopsWaitingWhenPlaydenCloses() {
  Fixture fixture;
  GamesMove move;
  const auto kit = SyntheticKit(fixture.root / "kit");
  unsigned uuids = 0;
  const auto config = MoveConfig(fixture, kit, &uuids, move);
  const std::string drive = "/mnt/usb0/playden/import/Imported";
  const auto imported = PrepareImported(fixture, config, drive);
  RuntimeHost host(config);
  CHECK(host.Start());
  WaitFor(host, Started);
  // A handoff waiting holds relocations back on the worker.
  host.Play(imported.number);
  const auto number = imported.number;
  const auto played = WaitFor(host, [number](const RuntimeSnapshot& s) { return s.handoff_ready || s.play_refusals.count(number); }, 30);
  CHECK(played.handoff_ready);
  std::atomic<bool> moved{false};
  RuntimeRelocationResult result;
  std::atomic<bool> answered{false};
  std::thread waiting([&] {
    result = RelocateNow(host, imported.number, [&]() -> std::string { moved = true; return {}; });
    answered = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(!answered.load());
  const auto closing = std::chrono::steady_clock::now();
  host.BeginClosing();
  waiting.join();
  CHECK(std::chrono::steady_clock::now() - closing < std::chrono::seconds(5));
  CHECK(result.outcome == RuntimeRelocationResult::Outcome::kBusy && result.message == kRelocationClosing);
  const auto again = RelocateNow(host, imported.number);
  CHECK(again.outcome == RuntimeRelocationResult::Outcome::kBusy && again.message == kRelocationClosing);
  // The handoff goes on; the abandoned relocation never runs.
  CHECK(host.Handoff());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(!moved.load() && host.Snapshot().installs[0].install_path == drive);
  CHECK(!move.Logged("relocate result app="));
  host.Shutdown();
}

namespace {
constexpr const char* kLeftoverOperation = "00000000-0000-4000-8000-000000000001";

RuntimeHostConfig LeftoverConfig(Fixture& fixture) {
  auto config = fixture.Config();
  // Bottles live under the fixture, where Playden looks for them.
  config.host_namespace_prefix = fixture.root;
  config.package_paths.console_data_root = "/data";
  config.package_paths.host_data_root = fixture.root / "data";
  config.uuid = [] { return std::optional<std::string>(kLeftoverOperation); };
  config.removal_confirm_polls = 1;
  config.removal_poll_interval = std::chrono::milliseconds(1);
  // Removing a manual source does not itself register the title.
  config.registry_transport = [&fixture](const std::string& route, const std::string&) -> std::optional<std::string> {
    if (route != "/api/v1/games") { ++fixture.registry_mutations; return R"({"status":0})"; }
    if (!fixture.registered) return R"({"status":0,"games":[]})";
    return R"({"status":0,"games":[{"title_id":"FAKE78001","installed":true,"managed":true,)"
           R"("source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78001"}]})";
  };
  return config;
}

fs::path MakeLeftover(Fixture& fixture) {
  const auto title = fixture.root / "data/homebrew/FAKE78001";
  Write(title / "sce_sys/param.json", R"({"localizedParameters":{"en-US":{"titleName":"Left Behind"}}})", 0644);
  Write(title / "eboot.bin", "title-files", 0755);
  // The title shell's descriptor names the game's bottle (see pw_title_launch.c).
  Write(title / "game-launch.json",
        R"({"schema_version":1,"app_id":220,"title_id":"FAKE78001","install_id":"steam-220",)"
        R"("data_root":"/data/playden/games/steam-220"})", 0644);
  CHECK(chmod(title.c_str(), 0755) == 0);
  return title;
}
}  // namespace

void DeletesLeftoverTitleAndResumesFailures() {
  // Listed, already absent, unavailable build, unavailable remover, refused and silent remover.
  for (int scenario = 0; scenario < 6; ++scenario) {
    Fixture fixture;
    auto config = LeftoverConfig(fixture);
    const auto title = MakeLeftover(fixture);
    fixture.registered = scenario != 1;
    unsigned calls = 0;
    if (scenario != 1 && scenario != 2)
      config.unregister_title = [&fixture, &calls, scenario](const std::string& id, const std::string& nonce) {
        CHECK(id == "FAKE78001" && nonce == kLeftoverOperation);
        ++calls;
        if (scenario == 3) return TitleRemoverResult{false, std::nullopt, "unavailable"};
        if (scenario == 4) return TitleRemoverResult{true, -1, "refused"};
        if (scenario == 5) return TitleRemoverResult{true, std::nullopt, "silent"};
        fixture.registered = false;
        return TitleRemoverResult{true, 0, "removed"};
      };
    const auto bottle = fixture.root / "data/playden/games/steam-220/save.dat";
    const auto files = fixture.root / "data/playden-games/steam-220/game.exe";
    Write(bottle, "saved-game", 0600);
    Write(files, "downloaded-game", 0644);
    {
      RuntimeHost host(config);
      CHECK(host.Start());
      const auto started = WaitFor(host, Started);
      CHECK(started.leftover_titles.size() == 1);
      CHECK(started.leftover_titles[0].title_id == "FAKE78001" && started.leftover_titles[0].name == "Left Behind");
      CHECK(started.leftover_titles[0].bytes > 11 && started.installs.empty());
      CHECK(host.DeleteLeftoverTitle("FAKE78001"));
      const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.leftover_deletes_handled == 1; });
      CHECK(!done.deleting_leftover_title && done.queued_leftover_deletes.empty());
      if (scenario <= 1) {
        CHECK(done.leftover_titles.empty() && done.leftover_delete_failures.empty());
        CHECK(!fs::exists(title));
        CHECK(!fs::exists(config.state_root / "leftover-FAKE78001.json"));
        CHECK(calls == (scenario == 0 ? 1 : 0));
      } else {
        CHECK(done.leftover_delete_failures.count("FAKE78001") == 1);
        CHECK(done.leftover_titles.size() == 1);  // retry remains available after retire
        if (scenario == 2) {
          CHECK(fs::is_directory(title));
          CHECK(!fs::exists(config.state_root / "leftover-FAKE78001.json"));
        } else {
          CHECK(!fs::exists(title));
          const auto journal = config.state_root / "leftover-FAKE78001.json";
          struct stat info{};
          CHECK(lstat(journal.c_str(), &info) == 0 && (info.st_mode & 07777) == 0600);
          CHECK(Read(journal).find("\"step\":\"unregister\"") != std::string::npos);
          CHECK(fs::is_directory(config.state_root / "trash" / (std::string("FAKE78001-") + kLeftoverOperation)));
          if (scenario == 3) CHECK(done.leftover_delete_failures.at("FAKE78001").find("could not start") != std::string::npos);
          if (scenario == 4) CHECK(done.leftover_delete_failures.at("FAKE78001").find("0xffffffff") != std::string::npos);
          if (scenario == 5) CHECK(done.leftover_delete_failures.at("FAKE78001").find("has not confirmed") != std::string::npos);
        }
      }
      if (scenario <= 1) {
        // The title that named the bottle is gone: its saves are kept, as Uninstall keeps them.
        const auto kept = fixture.root / "data/playden/games/steam-220.kept";
        CHECK(!fs::exists(bottle) && Read(kept / "save.dat") == "saved-game");
        CHECK(done.kept_bottles.size() == 1 && done.kept_bottles[0].install_id == "steam-220");
      } else {
        CHECK(Read(bottle) == "saved-game" && done.kept_bottles.empty());
      }
      CHECK(Read(files) == "downloaded-game");
      host.Shutdown();
    }
    if (scenario < 3) continue;
    config.unregister_title = [&fixture, &calls](const std::string&, const std::string&) {
      ++calls;
      fixture.registered = false;
      return TitleRemoverResult{true, 0, "removed"};
    };
    RuntimeHost restarted(config);
    CHECK(restarted.Start());
    const auto done = WaitFor(restarted, Started);
    CHECK(done.leftover_deletes_handled == 1 && done.leftover_titles.empty() && done.leftover_delete_failures.empty());
    CHECK(calls == 2 && !fixture.registered);
    CHECK(!fs::exists(config.state_root / "leftover-FAKE78001.json"));
    CHECK(fs::is_empty(config.state_root / "trash"));
    CHECK(!fs::exists(bottle) && Read(fixture.root / "data/playden/games/steam-220.kept/save.dat") == "saved-game");
    CHECK(done.kept_bottles.size() == 1 && Read(files) == "downloaded-game");
    restarted.Shutdown();
  }
}

void LeftoverRemovalRevalidatesBeforeEffects() {
  for (int scenario = 0; scenario < 4; ++scenario) {
    Fixture fixture;
    auto config = LeftoverConfig(fixture);
    const auto title = MakeLeftover(fixture);
    unsigned calls = 0;
    config.unregister_title = [&calls](const std::string&, const std::string&) { ++calls; return TitleRemoverResult{}; };
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(WaitFor(host, Started).leftover_titles.size() == 1);
    const auto original = title.parent_path() / "original";
    if (scenario == 0) {
      // Preserve the old inode so the new directory cannot accidentally reuse it.
      fs::rename(title, original);
      Write(title / "replacement", "another-title", 0644);
    } else if (scenario == 1) {
      fs::rename(title, original);
      CHECK(symlink(original.c_str(), title.c_str()) == 0);
    } else {
      fixture.activity->store(static_cast<int>(scenario == 2 ? NativeActivity::kActive : NativeActivity::kUnknown));
    }
    CHECK(host.DeleteLeftoverTitle("FAKE78001"));
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.leftover_deletes_handled == 1; });
    CHECK(done.leftover_delete_failures.count("FAKE78001") == 1 && calls == 0 && fixture.registry_mutations == 0);
    CHECK(fs::exists(title));
    CHECK(!fs::exists(config.state_root / "leftover-FAKE78001.json"));
    host.Shutdown();
  }
}

void LeftoverRemovalRecoversRetireWriteGap(int scenario) {
  Fixture fixture;
  auto config = LeftoverConfig(fixture);
  const auto title = MakeLeftover(fixture);
  fixture.registered = true;
  {
    RuntimeHost first(config);
    CHECK(first.Start());
    CHECK(WaitFor(first, Started).leftover_titles.size() == 1);
    first.Shutdown();
  }
  struct stat info{};
  CHECK(lstat(title.c_str(), &info) == 0);
  const auto text = std::string("{\"schema\":1,\"title\":\"FAKE78001\",\"operation_id\":\"") + kLeftoverOperation +
      "\",\"step\":\"retire\",\"device\":" + std::to_string(info.st_dev) + ",\"inode\":" +
      std::to_string(info.st_ino) + ",\"name\":\"Left Behind\",\"bytes\":11}\n";
  playden::Storage storage(config.state_root.string());
  CHECK(storage.WriteDurable("leftover-FAKE78001.json", playden::Bytes(text.begin(), text.end())).ok());
  const auto trash = config.state_root / "trash";
  fs::create_directory(trash);
  CHECK(chmod(trash.c_str(), 0700) == 0);
  const auto retired = trash / (std::string("FAKE78001-") + kLeftoverOperation);
  fs::rename(title, retired);
  if (scenario == 1) {
    fs::rename(retired, config.state_root / "protected-original");
    Write(retired / "replacement", "another-title", 0644);
  } else if (scenario == 2) {
    CHECK(chmod((config.state_root / "leftover-FAKE78001.json").c_str(), 0644) == 0);
  }
  unsigned calls = 0;
  config.unregister_title = [&fixture, &calls](const std::string&, const std::string&) {
    ++calls;
    fixture.registered = false;
    return TitleRemoverResult{true, 0, "removed"};
  };
  RuntimeHost restarted(config);
  CHECK(restarted.Start());
  const auto done = WaitFor(restarted, Started);
  if (scenario == 0) {
    CHECK(done.leftover_deletes_handled == 1 && done.leftover_titles.empty() && calls == 1);
    CHECK(!fs::exists(config.state_root / "leftover-FAKE78001.json") && fs::is_empty(trash));
  } else {
    CHECK(calls == 0 && fixture.registered && fixture.registry_mutations == 0);
    CHECK(fs::exists(config.state_root / "leftover-FAKE78001.json") && fs::is_directory(retired));
    if (scenario == 1) {
      CHECK(done.leftover_deletes_handled == 1);
      CHECK(done.leftover_delete_failures.at("FAKE78001").find("folder changed") != std::string::npos);
      CHECK(Read(retired / "replacement") == "another-title");
      CHECK(Read(config.state_root / "protected-original/eboot.bin") == "title-files");
    } else {
      CHECK(done.leftover_deletes_handled == 0);
      CHECK(Read(retired / "eboot.bin") == "title-files");
    }
  }
  restarted.Shutdown();
}

void LeftoverScanRequiresLoadedRecordsAndRepeatsAfterUninstall() {
  for (bool readable : {false, true}) {
    Fixture fixture;
    auto config = LeftoverConfig(fixture);
    if (!readable) {
      Write(config.state_root / "installs.json", "broken", 0600);
      CHECK(chmod(config.state_root.c_str(), 0700) == 0);
      MakeLeftover(fixture);
    }
    RuntimeHost host(config);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    if (!readable) {
      CHECK(started.status == RuntimeHostStatus::kUnavailable && started.leftover_titles.empty());
      CHECK(started.reason.find("Runtime install records are unavailable") != std::string::npos);
    } else {
      CHECK(started.leftover_titles.empty());
      MakeLeftover(fixture);
      host.Remove(999, false);  // every completed uninstall refreshes the scan, including an absent title
      const auto refreshed = WaitFor(host, [](const RuntimeSnapshot& s) { return s.removals_handled == 1; });
      CHECK(refreshed.leftover_titles.size() == 1 && refreshed.leftover_titles[0].title_id == "FAKE78001");
    }
    host.Shutdown();
  }
}

void LeftoverDeletionSharesThePreparationAndUninstallQueue() {
  for (int scenario = 0; scenario < 3; ++scenario) {
    Fixture fixture;
    auto config = LeftoverConfig(fixture);
    MakeLeftover(fixture);
    std::promise<void> scanned, released;
    auto release = released.get_future().share();
    std::mutex order_mutex;
    std::vector<std::string> order;
    config.log = [&](const std::string& line) {
      if (line == "startup trash emptied") { scanned.set_value(); release.wait(); }
      std::lock_guard<std::mutex> lock(order_mutex);
      if (line.rfind("prepare result app=220", 0) == 0) order.push_back("prepare");
      if (line.rfind("remove result app=999", 0) == 0) order.push_back("uninstall");
      if (line == "leftover delete complete title=FAKE78001") order.push_back("leftover");
    };
    RuntimeHost host(config);
    CHECK(host.Start());
    CHECK(scanned.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(host.Snapshot().leftover_titles.size() == 1);
    const auto prepare = [&] { host.Prepare(Hl2()); };
    const auto remove = [&] { host.Remove(999, false); };
    const auto leftover = [&] { CHECK(host.DeleteLeftoverTitle("FAKE78001")); };
    std::vector<std::string> expected;
    if (scenario == 0) { prepare(); leftover(); remove(); expected = {"prepare", "leftover", "uninstall"}; }
    if (scenario == 1) { leftover(); remove(); prepare(); expected = {"leftover", "uninstall", "prepare"}; }
    if (scenario == 2) { remove(); prepare(); leftover(); expected = {"uninstall", "prepare", "leftover"}; }
    CHECK(host.Snapshot().queued_leftover_deletes == std::vector<std::string>{"FAKE78001"});
    released.set_value();
    const auto done = WaitFor(host, [](const RuntimeSnapshot& s) {
      return s.prepares_handled == 1 && s.removals_handled == 1 && s.leftover_deletes_handled == 1;
    });
    CHECK(done.leftover_delete_failures.empty() && done.leftover_titles.empty());
    host.Shutdown();
    CHECK(order == expected);
  }
}

void LeftoverDeletionRefusesANewInstallUsingTheId() {
  Fixture fixture;
  auto config = LeftoverConfig(fixture);
  const auto title = MakeLeftover(fixture);
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
  unsigned calls = 0;
  config.unregister_title = [&calls](const std::string&, const std::string&) { ++calls; return TitleRemoverResult{}; };
  // Preparation needs the fixture's registration hook; its source-removal mutation must never run.
  config.registry_transport = fixture.Config().registry_transport;
  Write(fixture.root / "data/homebrew/FAKE77031/games/220/hl2.exe", "existing-game", 0644);
  RuntimeHost host(config);
  CHECK(host.Start());
  CHECK(WaitFor(host, Started).leftover_titles.size() == 1);
  fs::rename(title, title.parent_path() / "original");
  host.Prepare(Hl2());
  const auto prepared = WaitFor(host, [](const RuntimeSnapshot& s) { return s.prepares_handled == 1; });
  CHECK(prepared.refusals.empty() && prepared.installs.size() == 1 && prepared.installs[0].title_id == "FAKE78001");
  const auto mutations = fixture.registry_mutations;
  // The scan is deliberately stale: the worker must recheck its authoritative records.
  CHECK(host.DeleteLeftoverTitle("FAKE78001"));
  const auto done = WaitFor(host, [](const RuntimeSnapshot& s) { return s.leftover_deletes_handled == 1; });
  CHECK(done.leftover_delete_failures.at("FAKE78001").find("now belongs to an installed game") != std::string::npos);
  CHECK(done.installs.size() == 1 && fs::is_regular_file(title / "eboot.bin"));
  CHECK(fixture.registry_mutations == mutations && calls == 0);
  CHECK(!fs::exists(config.state_root / "leftover-FAKE78001.json"));
  host.Shutdown();
}

// Bottles with no install record are kept as saves (as Uninstall keeps them) once no unrecorded
// PS5 title names them. Any doubt (unreadable records or descriptor) keeps every bottle as it is.
void KeepsUnrecordedBottlesAsKeptSaves() {
  for (int scenario = 0; scenario < 6; ++scenario) {
    Fixture fixture;
    auto config = LeftoverConfig(fixture);
    config.unix_time = [] { return std::int64_t{1760000000}; };
    const auto games = fixture.root / "data/playden/games";
    Write(games / "steam-221/save.dat", "lost-record", 0600);
    Write(games / "steam-221/title-metadata/sce_sys/param.json",
          R"({"localizedParameters":{"en-US":{"titleName":"Old Save"}}})", 0600);
    Write(games / "steam-220/save.dat", "named-by-title", 0600);
    MakeLeftover(fixture);  // FAKE78001's game-launch.json names steam-220
    Write(games / "steam-225/save.dat", "newer", 0600);
    Write(games / "steam-225/.playden-saves-restored", "", 0600);
    Write(games / "steam-225.kept/save.dat", "older", 0600);
    Write(games / "steam-225.kept/playden-kept.json",
          R"({"schema":1,"install_id":"steam-225","app_id":225,"name":"Older","kept_at":1,"bytes":5})", 0600);
    Write(games / "steam-226.saves/1/users/save.dat", "backup", 0600);
    // A title names its bottle by data_root even when its install_id says otherwise; one with no
    // descriptor names none.
    Write(games / "steam-228/save.dat", "named-by-root", 0600);
    Write(fixture.root / "data/homebrew/FAKE78005/game-launch.json",
          R"({"schema_version":1,"app_id":229,"title_id":"FAKE78005","install_id":"steam-229",)"
          R"("data_root":"/data/playden/games/steam-228"})", 0644);
    Write(fixture.root / "data/homebrew/FAKE78006/eboot.bin", "title-files", 0755);
    Write(fixture.root / "elsewhere/save.dat", "linked", 0600);
    fs::create_symlink(fixture.root / "elsewhere", games / "steam-227");
    if (scenario == 1) {
      Write(fixture.root / "data/homebrew/FAKE78003/game-launch.json", "{broken", 0644);
    } else if (scenario == 2) {
      Write(config.state_root / "installs.json", "broken", 0600);
      CHECK(chmod(config.state_root.c_str(), 0700) == 0);
    } else if (scenario == 3) {
      // An unreadable leftover deletion journal: its retired title may still name a bottle.
      Write(config.state_root / "leftover-FAKE78004.json", "{broken", 0600);
      CHECK(chmod(config.state_root.c_str(), 0700) == 0);
    } else if (scenario == 4) {
      // A journal entry that is not even a file still reserves its title.
      fs::create_directories(config.state_root / "leftover-FAKE78004.json");
      CHECK(chmod(config.state_root.c_str(), 0700) == 0);
    } else if (scenario == 5) {
      // A title folder that cannot be inspected might name any bottle (listable, not searchable).
      CHECK(chmod((fixture.root / "data/homebrew").c_str(), 0644) == 0);
    }
    RuntimeHost host(config);
    CHECK(host.Start());
    const auto started = WaitFor(host, Started);
    CHECK(Read(games / "steam-220/save.dat") == "named-by-title");
    CHECK(Read(games / "steam-228/save.dat") == "named-by-root" && !fs::exists(games / "steam-228.kept"));
    CHECK(Read(games / "steam-226.saves/1/users/save.dat") == "backup");
    CHECK(fs::is_symlink(games / "steam-227") && Read(fixture.root / "elsewhere/save.dat") == "linked");
    if (scenario == 0) {
      CHECK(!fs::exists(games / "steam-221") && Read(games / "steam-221.kept/save.dat") == "lost-record");
      const auto marker = Read(games / "steam-221.kept/playden-kept.json");
      CHECK(marker.find("\"install_id\":\"steam-221\",\"app_id\":221") != std::string::npos);
      CHECK(marker.find("\"name\":\"Old Save\"") != std::string::npos);
      // This bottle already has the older kept copy's saves: it replaces that copy.
      CHECK(!fs::exists(games / "steam-225") && Read(games / "steam-225.kept/save.dat") == "newer");
      std::set<std::string> kept;
      for (const auto& view : started.kept_bottles) kept.insert(view.install_id + "=" + view.name);
      CHECK((kept == std::set<std::string>{"steam-221=Old Save", "steam-225="}));  // steam-225 has no title metadata
    } else {
      CHECK(Read(games / "steam-221/save.dat") == "lost-record" && !fs::exists(games / "steam-221.kept"));
      CHECK(Read(games / "steam-225/save.dat") == "newer" && Read(games / "steam-225.kept/save.dat") == "older");
    }
    host.Shutdown();
    if (scenario == 5) CHECK(chmod((fixture.root / "data/homebrew").c_str(), 0755) == 0);
  }
}

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_runtime_dir = argv[1];
  LeftoverDeletionSharesThePreparationAndUninstallQueue();
  LeftoverDeletionRefusesANewInstallUsingTheId();
  DeletesLeftoverTitleAndResumesFailures();
  LeftoverRemovalRevalidatesBeforeEffects();
  for (int scenario = 0; scenario < 3; ++scenario) LeftoverRemovalRecoversRetireWriteGap(scenario);
  LeftoverScanRequiresLoadedRecordsAndRepeatsAfterUninstall();
  KeepsUnrecordedBottlesAsKeptSaves();
  WorkerMovesGamesUnderPublicationLease();
  NoKitRefusesPreparationWithoutDurableEffects();
  ReloadKitAdmitsAKitInstalledWhileRunning();
  QueuedPreparationIsPublishedUntilTaken();
  PrepareNextReordersWaitingPreparations();
  HeldPreparationsWait();
  HoldsEffectiveUidWhileWorking();
  StartupReportsJournalsWithoutResuming();
  LocalGameGetsRuntimeNumber();
  RuntimeNumberWaitsForTheWorkersHold();
  LocalGameKeepsItsBottle();
  LocalGameSteamworksAppId();
  PublicStateRootIsUnavailable();
  PreparesExistingDownloadToReady();
  PreparesGameOnUnownedDrive();
  PrepareFixesRootOwnedHomebrewFolder();
  PrepareNamesUnfixableFolder();
  TitleRefusalNamesTheSourceThatCannotAnswer();
  AnyTitleKitMakesTheGameItsOwnTitle();
  PreparationEndsAwaitingRegistration();
  PreparesGameWithoutShippedRecipe();
  UpdatedFilesSetUpTheLaunchAgain();
  PreparesProgramChosenInSettings();
  PreparesSteamProgramChosenAfterDownload();
  SteamEntryWithoutFolderStartsInInstallRoot();
  PreparesUdkGameWithEulaAccepted();
  PlayInstallsMfcForGameThatImportsIt();
  ResumesInterruptedCaptureFromSavedRequest();
  ResumesInterruptedCaptureFromSavedRequest(true);
  RefreshesInstalledTitleToRespunKit();
  RetryAfterKitRespinBindsTheNewArtifacts();
  SetupFromAnEarlierKitAsksForRecreate();
  RepairsBottleRegistryModes();
  PlaysReadyTitleThroughHandoff();
  ReplacedKitAsksForRestart();
  MaintainsReadyTitle();
  ModdedGameFolderStillPlays();
  KitWithoutUnownedRootsKeepsTightening();
  for (int route = 0; route <= 5; ++route) OwnedPublicationOpenedUpRecovers(route);
  SettingsSavedDuringPrepareReachReadyTitle();
  KitRespinKeepsWaitingSettingsUpdate();
  StaleTitleWaitsForUpdateBefore120Hz();
  FreshPrepareOn120HzKitPublishesIt();
  UpdateFinishesAFailedRepublish();
  UpdateWithoutSavedSettingsKeepsThePublication();
  StaleTitleKeeps120HzChoicesForUpdate();
  RemovesTitleKeepingSaves();
  PreparesGameInstalledAgainElsewhere();
  SharedProfileKitOpensSaves();
  ExistingBottleOpensWithSharedKit();
  KitWithoutSharedProfileKeepsBottlePrivate();
  ShippedRecipePreparesWithAnotherKitsRecipe();
  ShippedRecipeWritesGameIni(false);
  ShippedRecipeWritesGameIni(true);
  GeneratedRecipeGetsShippedIni();
  RunsSetupProgramOnce();
  ShippedRecipeRequirementsFollowTheKit(false);
  ShippedRecipeRequirementsFollowTheKit(true);
  MigrationRewritesInstallPathsAndRepublishes();
  MovedModdedGameRepublishesUnowned();
  MovedGameStartupKeepsSteamAccount();
  StuckMovedGameOffersRecreateBottle();
  PlayBeforeRepublishRepublishesFirst();
  MigrationSkipsRecordsOutsideOldPrefix();
  RestartDuringRepublishResumes();
  MovedGameMissingAtNewRootLetsTheMoveFinish();
  UnownedUpgradeSkippedWhenFolderMissing();
  for (int scenario = 0; scenario <= 9; ++scenario) UnownedUpgradeAfterCommittedSettingsUpdates(scenario);
  MovedGameWithUnreadableOperationRepublishes();
  CopiedGameKeepsItsTitle();
  RelocationWaitsForQueuedPreparation();
  MovedGameKeepsItsTitle();
  RelocationResumesAfterRestart();
  RelocationFallsBackToANewTitle();
  RelocationKeepsTheTitleWhenTheRepublishWaits();
  RelocationStopsWaitingWhenPlaydenCloses();
  std::puts("runtime host tests passed");
  return 0;
}
