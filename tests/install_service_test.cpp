#include "steam/install_service.h"
#include "steam/install_volumes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <unistd.h>
#include "steam_id.h"

using namespace playden;
using namespace playden::steam;
using namespace std::chrono_literals;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

class Sessions final : public SessionRepository {
 public:
  std::atomic<bool> present{true};
  Result<std::optional<StoredSession>> Load() const override {
    if (!present) return Result<std::optional<StoredSession>>::Success(std::nullopt);
    return Result<std::optional<StoredSession>>::Success(StoredSession{123, "synthetic", "refresh", "access", {}});
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};

class Runner final : public SessionInstallRunner {
 public:
  explicit Runner(SessionRepository& sessions) : SessionInstallRunner(sessions) {}
  std::atomic<bool> block{false};
  std::atomic<bool> checking_licenses{false};  // holds the run in the license check
  bool telemetry = false;
  std::uint64_t held = 0;  // bytes the fake install reports on disk
  std::atomic<int> installs{0};
  std::atomic<int> canceled{0};
  std::atomic<int> unreachable{0};  // this many runs find Steam out of reach
  std::mutex mutex;
  std::vector<std::string> removed;
  std::vector<std::string> install_roots;
  Result InstallApp(const StoredSession&, std::uint32_t, const std::string& root, const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                 const std::function<void(InstallStep)>& step) override {
    ++installs;
    {
      std::lock_guard<std::mutex> lock(mutex);
      install_roots.push_back(root);
    }
    if (unreachable > 0) { --unreachable; return {false, false, "Could not reach Steam.", 0, true}; }
    if (checking_licenses) {
      step(InstallStep::kCheckingLicenses);
      while (checking_licenses) {
        if (cancel) return {false, true, {}, 0};
        std::this_thread::sleep_for(1ms);
      }
      step(InstallStep::kDownloading);
    }
    if (telemetry) { progress(9000000000ull, 10000000000ull, held); progress(9100000000ull, 10000000000ull, held); }
    else progress(10, 100, held);
    while (block) {
      if (cancel) {
        if (telemetry) progress(9200000000ull, 10000000000ull, held); // a late callback after Pause
        ++canceled; return {false, true, {}, 0};
      }
      std::this_thread::sleep_for(1ms);
    }
    std::filesystem::create_directories(root);
    std::ofstream(std::filesystem::path(root) / ".playden-install.json") << "{\"install_bytes\":4096}";
    progress(100, 100, held);
    return {true, false, {}, 4096};
  }
  std::vector<std::uint32_t> updates;  // what FindAppUpdates answers
  std::vector<std::string> discarded;
  std::optional<std::vector<std::uint32_t>> FindAppUpdates(const StoredSession&, const std::map<std::uint32_t, std::string>& apps,
                                                           const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::uint32_t> found;
    for (const auto app : updates) if (apps.count(app)) found.push_back(app);
    return found;
  }
  void DiscardUpdate(const std::string& root) override {
    std::lock_guard<std::mutex> lock(mutex);
    discarded.push_back(root);
  }
  std::string remove_failure;  // the next removals fail with this, leaving the folder
  std::string Remove(const std::string& root) override {
    std::lock_guard<std::mutex> lock(mutex);
    removed.push_back(root);
    if (!remove_failure.empty()) return remove_failure;
    std::filesystem::remove_all(root);
    return {};
  }
};

template <typename Ready>
void AwaitAt(int line, InstallService& service, Ready ready) {
  const auto deadline = std::chrono::steady_clock::now() + 6s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready(service.Snapshot())) return;
    std::this_thread::sleep_for(1ms);
  }
  std::fprintf(stderr, "deadline waiting at line %d\n", line);
  std::exit(1);
}

const InstallJobView* JobOf(const InstallSnapshot& snapshot, GameId id) {
  for (const auto& job : snapshot.jobs) if (job.id == id) return &job;
  return nullptr;
}

bool Installed(const InstallSnapshot& snapshot, GameId id) {
  for (const auto& game : snapshot.installed) if (game.id == id) return true;
  return false;
}

void SpaceAdmission(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  Sessions sessions;
  Runner runner(sessions); runner.block = true;
  InstallService service(runner, root.string());
  // No wraparound can turn an impossible estimate into an accepted install.
  service.Install(SteamId(901), std::numeric_limits<std::uint64_t>::max());
  auto snapshot = service.Snapshot();
  CHECK(JobOf(snapshot, SteamId(901)) && JobOf(snapshot, SteamId(901))->state == InstallJobState::kFailed);
  CHECK(runner.installs == 0);
  CHECK(JobOf(snapshot, SteamId(901))->failure.find("space") != std::string::npos);

  const auto space = QueryVolumeSpace(root.string());
  CHECK(space && space->free_bytes > 256ull * 1024 * 1024);
  const auto estimate = space->free_bytes / 4 * 3;
  service.Install(SteamId(902), estimate);
  AwaitAt(__LINE__, service, [&](const InstallSnapshot&) { return runner.installs == 1; });
  service.Install(SteamId(903), estimate);
  snapshot = service.Snapshot();
  CHECK(JobOf(snapshot, SteamId(903)) && JobOf(snapshot, SteamId(903))->state == InstallJobState::kFailed);
  CHECK(runner.installs == 1); // the running job's reservation prevents overcommit
  service.Cancel(SteamId(902));
  AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) { return !JobOf(s, SteamId(902)); });
  service.Install(SteamId(904), estimate);
  AwaitAt(__LINE__, service, [&](const InstallSnapshot&) { return runner.installs == 2; });
  service.Cancel(SteamId(904));
  AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) { return !JobOf(s, SteamId(904)); });
  // A download that already holds its estimate on the drive reserves nothing more: free space
  // already counts those bytes.
  runner.held = estimate;
  service.Install(SteamId(906), estimate);
  AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) { return runner.installs == 3 && JobOf(s, SteamId(906)) && JobOf(s, SteamId(906))->held_bytes == estimate; });
  service.Install(SteamId(907), estimate);
  snapshot = service.Snapshot();
  CHECK(JobOf(snapshot, SteamId(907)) && JobOf(snapshot, SteamId(907))->state != InstallJobState::kFailed);
  // An install can name its own drive; one the service does not know is refused, not redirected.
  service.Install(SteamId(908), std::nullopt, "missing-drive");
  snapshot = service.Snapshot();
  CHECK(JobOf(snapshot, SteamId(908)) && JobOf(snapshot, SteamId(908))->state == InstallJobState::kFailed &&
        JobOf(snapshot, SteamId(908))->volume_id == "missing-drive");
  CHECK(snapshot.default_volume_id != "missing-drive");
  service.Shutdown();
  std::filesystem::remove_all(root);

  // An unreadable destination is not equivalent to infinite (or zero) capacity.
  std::ofstream(root) << "not a directory";
  Runner unavailable_runner(sessions);
  InstallService unavailable(unavailable_runner, (root / "games").string());
  unavailable.Install(SteamId(905));
  snapshot = unavailable.Snapshot();
  CHECK(JobOf(snapshot, SteamId(905)) && JobOf(snapshot, SteamId(905))->state == InstallJobState::kFailed);
  CHECK(JobOf(snapshot, SteamId(905))->failure.find("unavailable") != std::string::npos);
  CHECK(unavailable_runner.installs == 0);
  unavailable.Shutdown();
  std::filesystem::remove(root);
}

void LiveTelemetryDecaysWithoutCallbacks(const std::filesystem::path& root) {
  Sessions sessions;
  Runner runner(sessions); runner.block = true; runner.telemetry = true;
  InstallService service(runner, root.string());
  service.Install(SteamId(900));
  AwaitAt(__LINE__, service, [](const InstallSnapshot& snapshot) {
    const auto* job = JobOf(snapshot, SteamId(900));
    return job && job->bytes_per_second && *job->bytes_per_second > 0;
  });
  const auto moving = service.Snapshot();
  const auto* job = JobOf(moving, SteamId(900));
  CHECK(job && *job->bytes_per_second <= 100000000 && job->eta_seconds);
  CHECK(job->done_bytes == 9100000000ull); // only the fresh 100 MB belongs in the rate
  const auto deadline = std::chrono::steady_clock::now() + 9s;
  bool stalled = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = service.Snapshot();
    const auto* current = JobOf(snapshot, SteamId(900));
    CHECK(current && current->done_bytes == job->done_bytes);
    if (current->bytes_per_second == 0) {
      CHECK(!current->eta_seconds && snapshot.revision > moving.revision);
      stalled = true; break;
    }
    std::this_thread::sleep_for(10ms);
  }
  CHECK(stalled);
  service.Pause(SteamId(900));
  const auto paused = service.Snapshot();
  CHECK(!JobOf(paused, SteamId(900))->bytes_per_second && !JobOf(paused, SteamId(900))->eta_seconds);
  AwaitAt(__LINE__, service, [&](const InstallSnapshot&) { return runner.canceled == 1; });
  CHECK(!JobOf(service.Snapshot(), SteamId(900))->bytes_per_second); // late callback cannot restore it
  service.Resume(SteamId(900));
  AwaitAt(__LINE__, service, [&](const InstallSnapshot& snapshot) {
    const auto* current = JobOf(snapshot, SteamId(900));
    return runner.installs == 2 && current && current->state == InstallJobState::kRunning;
  });
  const auto resumed = service.Snapshot();
  CHECK(!JobOf(resumed, SteamId(900))->bytes_per_second && !JobOf(resumed, SteamId(900))->eta_seconds);
  service.Shutdown();
  std::filesystem::remove_all(root);
}

}  // namespace

// Finished downloads stay listed after the queue for a week, newest first, until dismissed.
void CompletedDownloadsList(const std::filesystem::path& root) {
  constexpr std::int64_t kDay = 24 * 60 * 60;
  const std::int64_t now = 1'800'000'000;
  std::vector<InstalledGame> installed = {
      {SteamId(1), 10, "/g/1", "internal", true, now - 60},
      {SteamId(2), 20, "/g/2", "m2", true, now - 3 * kDay},
      {SteamId(3), 30, "/g/3", "internal", true, now - 8 * kDay},  // older than a week
      {SteamId(4), 40, "/g/4", "internal", true, 0},      // unknown finish time
      {SteamId(5), 50, "/g/5", "internal", true, now - 10},
  };
  auto completed = CompletedDownloads(installed, {}, now);
  CHECK(completed.size() == 3 && completed[0].id == SteamId(5) && completed[1].id == SteamId(1) && completed[2].id == SteamId(2));
  CHECK(completed[2].volume_id == "m2" && completed[2].install_bytes == 20 && completed[2].finished_at == now - 3 * kDay);
  // A dismissal names the finish time: the same game installed again is listed again.
  completed = CompletedDownloads(installed, {{SteamId(5), now - 10}, {SteamId(1), now - 999}}, now);
  CHECK(completed.size() == 2 && completed[0].id == SteamId(1) && completed[1].id == SteamId(2));
  std::vector<InstalledGame> many;
  for (std::uint32_t id = 1; id <= kMaxCompleted + 5; ++id) many.push_back({SteamId(id), 1, "/g", "internal", true, now - id});
  completed = CompletedDownloads(many, {}, now);
  CHECK(completed.size() == kMaxCompleted && completed.front().id == SteamId(1) && completed.back().id == SteamId(kMaxCompleted));

  // The service lists an install as it finishes, keeps dismissals across a restart, and Clear
  // dismisses everything listed.
  std::filesystem::remove_all(root);
  Sessions sessions;
  {
    Runner runner(sessions);
    InstallService service(runner, root.string());
    service.Install(SteamId(70));
    service.Install(SteamId(71));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(70)) && Installed(s, SteamId(71)); });
    const auto snapshot = service.Snapshot();
    CHECK(snapshot.completed.size() == 2 && snapshot.completed[0].install_bytes == 4096);
    const auto revision = snapshot.revision;
    service.DismissCompleted(SteamId(70));
    const auto dismissed = service.Snapshot();
    CHECK(dismissed.revision > revision);
    CHECK(dismissed.completed.size() == 1 && dismissed.completed[0].id == SteamId(71));
    CHECK(Installed(dismissed, SteamId(70)));
  }
  {
    Runner runner(sessions);
    InstallService restarted(runner, root.string());
    const auto snapshot = restarted.Snapshot();
    CHECK(snapshot.completed.size() == 1 && snapshot.completed[0].id == SteamId(71) && snapshot.completed[0].finished_at > 0);
    restarted.ClearCompleted();
    CHECK(restarted.Snapshot().completed.empty());
    // Uninstalling drops the game's dismissal with it.
    restarted.Uninstall(SteamId(70));
    restarted.DismissCompleted(SteamId(71));
  }
  {
    std::ifstream dismissed(root / ".playden-downloads-dismissed");
    const std::string text((std::istreambuf_iterator<char>(dismissed)), std::istreambuf_iterator<char>());
    CHECK(text.find("71\t") == 0 && text.find("70\t") == std::string::npos);
  }
  std::filesystem::remove_all(root);

  // Clear dismisses every finished download, not only the `kMaxCompleted` the list shows.
  {
    Runner runner(sessions);
    InstallService service(runner, root.string());
    const std::uint32_t count = kMaxCompleted + 3;
    for (std::uint32_t id = 100; id < 100 + count; ++id) service.Install(SteamId(id));
    AwaitAt(__LINE__, service, [count](const InstallSnapshot& s) {
      for (std::uint32_t id = 100; id < 100 + count; ++id)
        if (!Installed(s, SteamId(id))) return false;
      return true;
    });
    CHECK(service.Snapshot().completed.size() == kMaxCompleted);
    service.ClearCompleted();
    CHECK(service.Snapshot().completed.empty());
  }
  std::filesystem::remove_all(root);
}

// A download that cannot reach Steam keeps its place and waits instead of failing; the job behind
// it runs meanwhile.
void UnreachableSteamWaits(const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  Sessions sessions;
  Runner runner(sessions);
  runner.unreachable = 1;
  InstallService service(runner, root.string());
  service.SetRetryDelays(10s, 60s);
  service.Install(SteamId(80));
  service.Install(SteamId(81));
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
    const auto* job = JobOf(s, SteamId(80));
    return job && job->state == InstallJobState::kQueued && job->failure.find("Waiting to reach Steam") == 0 &&
           Installed(s, SteamId(81));
  });
  CHECK(runner.installs == 2);
  service.Resume(SteamId(80));  // the player's request ends the wait at once
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(80)) && s.jobs.empty(); });
  std::filesystem::remove_all(root);
}

int AttemptsAt(Runner& runner, const std::filesystem::path& root) {
  std::lock_guard<std::mutex> lock(runner.mutex);
  return static_cast<int>(std::count(runner.install_roots.begin(), runner.install_roots.end(), root.string()));
}

// Installs another store's games; holds them while `block` is set.
class LocalRunner final : public InstallRunner {
 public:
  std::atomic<bool> block{false};
  std::atomic<bool> ignore_cancel{false};  // the run finishes whatever a cancel says (it came too late)
  std::mutex mutex;
  std::vector<GameId> installed;
  std::vector<std::string> removed;
  std::vector<GameId> forgotten;
  Result Install(const GameId& id, const std::string& root, const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                 const std::function<void(InstallStep)>&) override {
    while (block) {
      if (cancel && !ignore_cancel) return {false, true, {}, 0};
      std::this_thread::sleep_for(1ms);
    }
    std::filesystem::create_directories(root);
    std::ofstream(std::filesystem::path(root) / ".playden-install.json") << "{\"install_bytes\":7}";
    progress(7, 7, 0);
    std::lock_guard<std::mutex> lock(mutex);
    installed.push_back(id);
    return {true, false, {}, 7};
  }
  std::string Remove(const std::string& root) override {
    std::lock_guard<std::mutex> lock(mutex);
    removed.push_back(root);
    std::filesystem::remove_all(root);
    return {};
  }
  void Forget(const GameId& id) override {
    std::lock_guard<std::mutex> lock(mutex);
    forgotten.push_back(id);
  }
};

// A cancel that arrives while the run finishes comes too late: the run's install stays, and
// nothing is removed (a copy may already have deleted its source). An uninstall of a game whose
// folder is missing deletes nothing but tells its runner to forget it.
void LateCancelKeepsTheInstall(const std::filesystem::path& root) {
  const auto local = GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
  std::filesystem::remove_all(root);
  {
    Sessions sessions;
    Runner steam(sessions);
    LocalRunner imports;
    imports.block = true;
    imports.ignore_cancel = true;
    InstallService service({{Store::kSteam, &steam}, {Store::kLocal, &imports}},
                           {{InstallService::kPrimaryVolumeId, "Console storage", root.string(), true}});
    service.Install(local, 7);
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      const auto* job = JobOf(s, local);
      return job && job->state == InstallJobState::kRunning;
    });
    service.Cancel(local);
    imports.block = false;
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) { return Installed(s, local) && s.jobs.empty(); });
    {
      std::lock_guard<std::mutex> lock(imports.mutex);
      CHECK(imports.removed.empty() && imports.installed.size() == 1);
    }
    CHECK(std::filesystem::exists(root / ToKey(local) / ".playden-install.json"));
  }
  // Its folder goes missing while Playden is closed: listed unavailable, then uninstalled.
  std::filesystem::remove_all(root / ToKey(local));
  {
    Sessions sessions;
    Runner steam(sessions);
    LocalRunner imports;
    InstallService service({{Store::kSteam, &steam}, {Store::kLocal, &imports}},
                           {{InstallService::kPrimaryVolumeId, "Console storage", root.string(), true}});
    const auto listed = service.Snapshot();
    CHECK(listed.installed.size() == 1 && listed.installed[0].id == local && !listed.installed[0].available);
    service.Uninstall(local);
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      std::lock_guard<std::mutex> lock(imports.mutex);
      return !Installed(s, local) && imports.forgotten.size() == 1;
    });
    std::lock_guard<std::mutex> lock(imports.mutex);
    CHECK(imports.forgotten[0] == local && imports.removed.empty());
  }
  std::filesystem::remove_all(root);
}

// Jobs waiting to try again never hold up the queue: another store's job behind a Steam job that
// waits for the sign-in runs, two waiting Steam jobs both keep trying, and a waiting job can be
// canceled, paused and resumed.
void WaitingJobsDontBlockTheQueue(const std::filesystem::path& root) {
  const auto local = GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
  {
    std::filesystem::remove_all(root);
    Sessions sessions;
    sessions.present = false;
    Runner steam(sessions);
    LocalRunner imports;
    InstallService service({{Store::kSteam, &steam}, {Store::kLocal, &imports}},
                           {{InstallService::kPrimaryVolumeId, "Console storage", root.string(), true}});
    service.Install(SteamId(80));
    service.Install(local);
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      const auto* job = JobOf(s, SteamId(80));
      return Installed(s, local) && job && job->state == InstallJobState::kQueued &&
             job->failure == "Waiting for the Steam sign-in.";
    });
    CHECK(steam.installs == 0 && std::filesystem::exists(root / ToKey(local) / ".playden-install.json"));
    // The Local game's removal goes to its own runner.
    service.Uninstall(local);
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) { return !Installed(s, local); });
    for (auto deadline = std::chrono::steady_clock::now() + 6s;; std::this_thread::sleep_for(1ms)) {
      std::lock_guard<std::mutex> lock(imports.mutex);
      if (!imports.removed.empty()) { CHECK(imports.removed[0] == (root / ToKey(local)).string()); break; }
      CHECK(std::chrono::steady_clock::now() < deadline);
    }
    {
      std::lock_guard<std::mutex> lock(steam.mutex);
      CHECK(steam.removed.empty());
    }
    // Signed in: the waiting job carries on by itself after its wait.
    service.SetRetryDelays(5ms, 20ms);
    sessions.present = true;
    service.Resume(SteamId(80));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(80)); });
  }
  {
    // Two Steam jobs out of reach both keep trying, each after its own wait, and finish by themselves.
    std::filesystem::remove_all(root);
    Sessions sessions;
    Runner runner(sessions);
    runner.unreachable = 1000;
    InstallService service(runner, root.string());
    service.SetRetryDelays(5ms, 40ms);
    service.Install(SteamId(80));
    service.Install(SteamId(81));
    for (auto deadline = std::chrono::steady_clock::now() + 6s;
         AttemptsAt(runner, root / "80") < 3 || AttemptsAt(runner, root / "81") < 3; std::this_thread::sleep_for(1ms))
      CHECK(std::chrono::steady_clock::now() < deadline);
    runner.unreachable = 0;
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(80)) && Installed(s, SteamId(81)); });
  }
  {
    // Other requests don't cut a wait short; Pause keeps a waiting job off the worker, Resume tries it
    // at once, and Cancel takes it out of the queue.
    std::filesystem::remove_all(root);
    Sessions sessions;
    Runner runner(sessions);
    runner.unreachable = 1000;
    InstallService service(runner, root.string());
    service.SetRetryDelays(30s, 60s);
    service.Install(SteamId(80));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
      const auto* job = JobOf(s, SteamId(80));
      return job && job->state == InstallJobState::kQueued && !job->failure.empty();
    });
    service.Install(SteamId(81));
    service.Install(SteamId(82));
    AwaitAt(__LINE__, service, [&](const InstallSnapshot&) { return AttemptsAt(runner, root / "82") == 1; });
    CHECK(AttemptsAt(runner, root / "80") == 1 && AttemptsAt(runner, root / "81") == 1);
    service.Pause(SteamId(80));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
      const auto* job = JobOf(s, SteamId(80));
      return job && job->state == InstallJobState::kPaused && job->failure.empty();
    });
    service.Cancel(SteamId(81));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return !JobOf(s, SteamId(81)); });
    runner.unreachable = 0;
    std::this_thread::sleep_for(50ms);
    CHECK(AttemptsAt(runner, root / "80") == 1 && JobOf(service.Snapshot(), SteamId(80))->state == InstallJobState::kPaused);
    service.Resume(SteamId(80));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(80)); });
    CHECK(AttemptsAt(runner, root / "81") == 1 && JobOf(service.Snapshot(), SteamId(82)));
    service.Cancel(SteamId(82));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return s.jobs.empty(); });
  }
  std::filesystem::remove_all(root);
}

// A Steam sign-in ends the wait of Steam jobs waiting for one (their next try may be a minute away);
// another store's waiting jobs keep theirs.
void SignInEndsSteamWait(const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  Sessions sessions;
  sessions.present = false;
  Runner steam(sessions);
  InstallService service({{Store::kSteam, &steam}},
                         {{InstallService::kPrimaryVolumeId, "Console storage", root.string(), true}});
  service.SetRetryDelays(60s, 60s);
  service.Install(SteamId(80));
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
    const auto* job = JobOf(s, SteamId(80));
    return job && job->state == InstallJobState::kQueued && job->failure == "Waiting for the Steam sign-in.";
  });
  sessions.present = true;
  service.RetryWaiting(Store::kLocal);  // not Steam's: the wait goes on
  std::this_thread::sleep_for(50ms);
  CHECK(steam.installs == 0 && !Installed(service.Snapshot(), SteamId(80)));
  const auto started = std::chrono::steady_clock::now();
  service.RetryWaiting(Store::kSteam);
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(80)) && s.jobs.empty(); });
  CHECK(std::chrono::steady_clock::now() - started < 10s && steam.installs == 1);
  std::filesystem::remove_all(root);
}

// A running job reports its step, so a slow license check can show before the download starts (issue #47).
void RunningJobReportsItsStep(const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  Sessions sessions;
  Runner runner(sessions);
  runner.checking_licenses = true;
  InstallService service(runner, root.string());
  const auto before = std::chrono::steady_clock::now();
  service.Install(SteamId(70));
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
    const auto* job = JobOf(s, SteamId(70));
    return job && job->step == InstallStep::kCheckingLicenses;
  });
  const auto checking = service.Snapshot();
  CHECK(JobOf(checking, SteamId(70))->state == InstallJobState::kRunning && JobOf(checking, SteamId(70))->step_since >= before);
  runner.checking_licenses = false;
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(70)); });
  std::filesystem::remove_all(root);
}

// A folder check that names a problem fails the download before the runner starts; an empty answer proceeds.
void FolderCheckGuardsDownloadStart(const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  Sessions sessions;
  Runner runner(sessions);
  InstallService service(runner, root.string());
  std::mutex mutex;
  std::string answer = "The folder /games is not writable.";
  std::string checked;
  service.SetFolderCheck([&](const std::string& games_root) {
    std::lock_guard<std::mutex> lock(mutex);
    checked = games_root;
    return answer;
  });
  service.Install(SteamId(90));
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
    const auto* job = JobOf(s, SteamId(90));
    return job && job->state == InstallJobState::kFailed;
  });
  CHECK(JobOf(service.Snapshot(), SteamId(90))->failure == "The folder /games is not writable.");
  CHECK(runner.installs == 0);
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(checked == root.string());
    answer.clear();
  }
  service.Install(SteamId(91));
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(91)); });
  CHECK(runner.installs == 1);
  std::filesystem::remove_all(root);
}

bool Uninstalling(const InstallSnapshot& snapshot, GameId id) {
  return std::find(snapshot.uninstalling.begin(), snapshot.uninstalling.end(), id) != snapshot.uninstalling.end();
}

void InstalledGameFolder(const std::filesystem::path& root) {
  std::filesystem::create_directories(root / "bin");
  std::ofstream(root / ".playden-install.json") << "{\"install_bytes\":4096}";
  std::ofstream(root / "bin/game.exe") << "game";
}

// Uninstalling deletes the game's folder on any drive, prepared or not, even while a long
// download runs; an uninstall Playden stopped carries on at the next start, and one whose files
// could not be deleted lists the game again and says why.
void UninstallDeletesFilesWhileDownloading(const std::filesystem::path& base) {
  std::filesystem::remove_all(base);
  const auto primary = base / "data";
  const auto m2 = base / "ext1";
  for (const auto id : {11, 12, 13}) InstalledGameFolder(primary / std::to_string(id));
  InstalledGameFolder(m2 / "21");
  std::filesystem::create_directories(primary);
  std::ofstream(primary / ".playden-games") << "11\tinternal\t4096\n12\tinternal\t4096\n13\tinternal\t4096\n21\tm2\t4096\n";
  const std::vector<InstallVolume> volumes{{"internal", "Console storage", primary.string(), true},
                                           {"m2", "M.2 SSD", m2.string(), true}};
  Sessions sessions;
  {
    Runner runner(sessions);
    runner.block = true;
    InstallService service(runner, volumes);
    service.Install(SteamId(30));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
      const auto* job = JobOf(s, SteamId(30));
      return job && job->state == InstallJobState::kRunning;
    });
    // The download is still running: the folders go now, on console storage and on the M.2 drive.
    service.Uninstall(SteamId(11));
    service.Uninstall(SteamId(21));
    CHECK(!Installed(service.Snapshot(), SteamId(11)) && !Installed(service.Snapshot(), SteamId(21)));
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      return !Uninstalling(s, SteamId(11)) && !Uninstalling(s, SteamId(21)) && !std::filesystem::exists(primary / "11") &&
             !std::filesystem::exists(m2 / "21");
    });
    CHECK(runner.block);
    // The download stepped back into the queue and carried on.
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      const auto* job = JobOf(s, SteamId(30));
      return runner.installs >= 2 && job && job->state == InstallJobState::kRunning;
    });

    // Files that cannot be deleted: the game is listed again and the reason is reported once.
    {
      std::lock_guard<std::mutex> lock(runner.mutex);
      runner.remove_failure = "Some of the game's files could not be deleted (Permission denied).";
    }
    service.Uninstall(SteamId(12));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(12)) && !Uninstalling(s, SteamId(12)); });
    auto failures = service.TakeRemovalFailures();
    CHECK(failures.size() == 1 && failures[0].id == SteamId(12) && failures[0].reason.find("Permission denied") != std::string::npos);
    CHECK(service.TakeRemovalFailures().empty());
    CHECK(std::filesystem::exists(primary / "12/.playden-install.json"));
    // Uninstalling it again tries again.
    {
      std::lock_guard<std::mutex> lock(runner.mutex);
      runner.remove_failure.clear();
    }
    service.Uninstall(SteamId(12));
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      return !Installed(s, SteamId(12)) && !Uninstalling(s, SteamId(12)) && !std::filesystem::exists(primary / "12");
    });
    CHECK(service.TakeRemovalFailures().empty());
    runner.block = false;
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(30)); });
  }
  {
    // Playden stopped before the folder was gone: the registry still names the uninstall, so the
    // next start deletes the folder instead of forgetting it.
    std::ofstream(primary / ".playden-games") << "13\tinternal\t4096\tremoving\n30\tinternal\t4096\n22\tusb0\t4096\tremoving\n"
                                              << "local-3bb51844-7741-4ff2-a345-cdcc1e62a872\tusb0\t4096\tremoving\n";
    Runner runner(sessions);
    InstallService service(runner, volumes);
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      return !Uninstalling(s, SteamId(13)) && !std::filesystem::exists(primary / "13");
    });
    const auto snapshot = service.Snapshot();
    CHECK(!Installed(snapshot, SteamId(13)) && Installed(snapshot, SteamId(30)));
    // One on a drive that is not connected waits for the drive, and the game cannot be installed meanwhile.
    CHECK(Uninstalling(snapshot, SteamId(22)) && !Installed(snapshot, SteamId(22)));
    service.Install(SteamId(22));
    CHECK(!JobOf(service.Snapshot(), SteamId(22)));
  }
  {
    std::ifstream registry(primary / ".playden-games");
    const std::string text((std::istreambuf_iterator<char>(registry)), std::istreambuf_iterator<char>());
    CHECK(text.find("13\t") == std::string::npos && text.find("22\tusb0\t4096\tremoving") != std::string::npos);
    // An imported game's key is kept as it was.
    CHECK(text.find("local-3bb51844-7741-4ff2-a345-cdcc1e62a872\tusb0\t4096\tremoving") != std::string::npos);
  }
  std::filesystem::remove_all(base);
}

const InstalledGame* InstalledOf(const InstallSnapshot& snapshot, GameId id) {
  for (const auto& game : snapshot.installed) if (game.id == id) return &game;
  return nullptr;
}

void UpdatesInstalledGames(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  Sessions sessions;
  Runner runner(sessions);
  {
    InstallService service(runner, root.string());
    service.Install(SteamId(301));
    service.Install(SteamId(302));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(301)) && Installed(s, SteamId(302)); });
    // Nothing to update until a check finds it.
    service.Update(SteamId(301));
    CHECK(!JobOf(service.Snapshot(), SteamId(301)));
    {
      std::lock_guard<std::mutex> lock(runner.mutex);
      runner.updates = {301};
    }
    service.CheckUpdates();
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
      const auto* game = InstalledOf(s, SteamId(301));
      return game && game->update_available;
    });
    CHECK(!InstalledOf(service.Snapshot(), SteamId(302))->update_available);
    service.Update(SteamId(302));
    CHECK(!JobOf(service.Snapshot(), SteamId(302)));

    // The update runs as a job; the game stays installed meanwhile, and Cancel keeps it.
    runner.block = true;
    const int before = runner.installs;
    service.Update(SteamId(301));
    AwaitAt(__LINE__, service, [&](const InstallSnapshot&) { return runner.installs == before + 1; });
    auto snapshot = service.Snapshot();
    CHECK(JobOf(snapshot, SteamId(301)) && JobOf(snapshot, SteamId(301))->update);
    CHECK(Installed(snapshot, SteamId(301)));
    service.Cancel(SteamId(301));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return !JobOf(s, SteamId(301)); });
    AwaitAt(__LINE__, service, [&](const InstallSnapshot&) {
      std::lock_guard<std::mutex> lock(runner.mutex);
      return runner.discarded.size() == 1;
    });
    CHECK(Installed(service.Snapshot(), SteamId(301)));
    CHECK(std::filesystem::exists(root / "301" / ".playden-install.json"));
    {
      std::lock_guard<std::mutex> lock(runner.mutex);
      CHECK(runner.removed.empty());
    }

    // A queued update is kept across a restart.
    service.Update(SteamId(301));
    AwaitAt(__LINE__, service, [&](const InstallSnapshot&) { return runner.installs == before + 2; });
  }
  runner.block = false;
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.updates.clear();
  }
  InstallService service(runner, root.string());
  // It carries on by itself and replaces the game's entry.
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return !JobOf(s, SteamId(301)) && Installed(s, SteamId(301)); });
  const auto snapshot = service.Snapshot();
  CHECK(std::count_if(snapshot.installed.begin(), snapshot.installed.end(),
                      [](const InstalledGame& game) { return game.id == SteamId(301); }) == 1);
  CHECK(!InstalledOf(snapshot, SteamId(301))->update_available);
  CHECK(runner.installs == 5);
  std::filesystem::remove_all(root);
}

// Uninstalling a game stops its update and deletes the folder.
void UninstallStopsItsUpdate(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  Sessions sessions;
  Runner runner(sessions);
  InstallService service(runner, root.string());
  service.Install(SteamId(311));
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(311)); });
  {
    std::lock_guard<std::mutex> lock(runner.mutex);
    runner.updates = {311};
  }
  service.CheckUpdates();
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
    const auto* game = InstalledOf(s, SteamId(311));
    return game && game->update_available;
  });
  runner.block = true;
  service.Update(SteamId(311));
  AwaitAt(__LINE__, service, [&](const InstallSnapshot&) { return runner.installs == 2; });
  service.Uninstall(SteamId(311));
  AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
    return !JobOf(s, SteamId(311)) && !Installed(s, SteamId(311)) && s.uninstalling.empty();
  });
  CHECK(!std::filesystem::exists(root / "311"));
  std::filesystem::remove_all(root);
}

int main() {
  FolderCheckGuardsDownloadStart(std::filesystem::temp_directory_path() / ("playden-foldercheck-service-" + std::to_string(::getpid())));
  UninstallDeletesFilesWhileDownloading(std::filesystem::temp_directory_path() /
                                        ("playden-uninstall-service-" + std::to_string(::getpid())));
  RunningJobReportsItsStep(std::filesystem::temp_directory_path() / ("playden-step-service-" + std::to_string(::getpid())));
  SpaceAdmission(std::filesystem::temp_directory_path() / ("playden-space-service-" + std::to_string(::getpid())));
  LiveTelemetryDecaysWithoutCallbacks(std::filesystem::temp_directory_path() / ("playden-rate-service-" + std::to_string(::getpid())));
  UnreachableSteamWaits(std::filesystem::temp_directory_path() / ("playden-unreachable-service-" + std::to_string(::getpid())));
  WaitingJobsDontBlockTheQueue(std::filesystem::temp_directory_path() / ("playden-waiting-service-" + std::to_string(::getpid())));
  LateCancelKeepsTheInstall(std::filesystem::temp_directory_path() / ("playden-late-cancel-service-" + std::to_string(::getpid())));
  SignInEndsSteamWait(std::filesystem::temp_directory_path() / ("playden-signin-wait-service-" + std::to_string(::getpid())));
  CompletedDownloadsList(std::filesystem::temp_directory_path() / ("playden-completed-service-" + std::to_string(::getpid())));
  UpdatesInstalledGames(std::filesystem::temp_directory_path() / ("playden-update-service-" + std::to_string(::getpid())));
  UninstallStopsItsUpdate(std::filesystem::temp_directory_path() / ("playden-update-uninstall-" + std::to_string(::getpid())));
  const auto requests = ParseInstallRequests("uninstall 439490\n439490\ncancel 108600\n108600\n");
  CHECK(requests.size() == 4);
  CHECK(requests[0].action == InstallRequest::Action::kUninstall && requests[0].app == 439490);
  CHECK(requests[1].action == InstallRequest::Action::kInstall && requests[1].app == 439490);
  CHECK(requests[2].action == InstallRequest::Action::kCancel && requests[2].app == 108600);
  CHECK(requests[3].action == InstallRequest::Action::kInstall && requests[3].app == 108600);
  CHECK(ParseInstallRequests("0\n-1\n4294967296\n12x\nuninstall 12 extra\nunknown 12\n").empty());
  CHECK(ParseInstallRequests("  cancel 42  \r\n")[0].app == 42);
  CHECK(requests[0].game == playden::GameId::Steam(439490));
  {
    // Imported games: by their key, and `import` adds one in place (quoted words may hold spaces).
    const std::string uuid = "0f8fad5b-d9cb-469f-a165-70867728950e";
    const auto local = ParseInstallRequests("local-" + uuid + "\nuninstall local-" + uuid + "\n"
                                            "import usb0 Delores Delores.exe\n"
                                            "import\tusb0 \"Old School Rally\" \"bin/Old School Rally.exe\"\r\n"
                                            "import internal playden-import/Night-Sky NightSky.exe\n");
    CHECK(local.size() == 5);
    CHECK(local[0].action == InstallRequest::Action::kInstall && local[0].game == playden::GameId::Local(uuid) && local[0].app == 0);
    CHECK(local[1].action == InstallRequest::Action::kUninstall && local[1].game == playden::GameId::Local(uuid));
    CHECK(local[2].action == InstallRequest::Action::kImport && local[2].volume == "usb0" && local[2].folder == "Delores" &&
          local[2].program == "Delores.exe" && !local[2].game.valid());
    CHECK(local[3].folder == "Old School Rally" && local[3].program == "bin/Old School Rally.exe");
    CHECK(local[4].volume == "internal" && local[4].folder == "playden-import/Night-Sky");
    // `copy` copies an imported game into Playden, to the default volume or the one named.
    const auto copies = ParseInstallRequests("copy local-" + uuid + "\ncopy local-" + uuid + " usb0\ncopy 220\n"
                                             "copy local-" + uuid + " usb0 extra\ncopy\n");
    CHECK(copies.size() == 2);
    CHECK(copies[0].action == InstallRequest::Action::kCopy && copies[0].game == playden::GameId::Local(uuid) &&
          copies[0].volume.empty());
    CHECK(copies[1].action == InstallRequest::Action::kCopy && copies[1].volume == "usb0");
    // Refused: a cancel (imported games have no download), a malformed key, missing or extra words,
    // unclosed or empty quotes, a quote glued to the next word.
    CHECK(ParseInstallRequests("cancel local-" + uuid + "\nlocal-XYZ\nlocal-" + uuid + " extra\n"
                               "import usb0 Delores\nimport usb0 Delores a.exe b.exe\nimport usb0 \"Delores a.exe\n"
                               "import usb0 \"\" a.exe\nimport usb0 \"Del\"ores a.exe\nimport\n").empty());
  }
  std::string many_requests;
  for (int app = 1; app <= 70; ++app) many_requests += std::to_string(app) + "\n";
  CHECK(ParseInstallRequests(many_requests).size() == 64);
  {
    // Installed records must survive removal of the title's /app0 view.
    const auto stable = std::filesystem::temp_directory_path() /
        ("playden-stable-title-" + std::to_string(::getpid()));
    std::filesystem::remove_all(stable);
    std::filesystem::create_directories(stable / "games/220");
    std::ofstream(stable / "games/.playden-games") << "220\n";
    std::ofstream(stable / "games/220/.playden-install.json") << "{\"install_bytes\":8192}";
    auto candidates = ConsoleVolumeCandidates(stable.string());
    candidates.resize(1);
    Sessions sessions;
    Runner runner(sessions);
    InstallService service(runner, DiscoverInstallVolumes(candidates));
    const auto installed = service.Snapshot().installed;
    CHECK(installed.size() == 1 && installed[0].id == SteamId(220) && installed[0].available);
    CHECK(installed[0].root == (stable / "games/220").string() && installed[0].install_bytes == 8192);
    std::filesystem::remove_all(stable);
  }

  const auto root = std::filesystem::temp_directory_path() / ("playden-install-service-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "100");
  std::ofstream(root / "100" / ".playden-install.json") << "{\"version\":1,\"app_id\":100,\"install_bytes\":777}";
  std::filesystem::create_directories(root / "200" / ".playden-download");
  std::filesystem::create_directories(root / "not-a-game");
  std::ofstream(root / ".playden-games") << "100\n200\nnot-a-number\n0\n";

  Sessions sessions;
  Runner runner(sessions);
  {
    // Queue order survives progress snapshots and restart without interrupting the active download.
    const auto reorder_root = root / "reorder";
    Runner reorder_runner(sessions);
    reorder_runner.block = true;
    {
      InstallService service(reorder_runner, reorder_root.string());
      service.Install(SteamId(10));
      AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
        return JobOf(s, SteamId(10)) && JobOf(s, SteamId(10))->state == InstallJobState::kRunning;
      });
      service.Install(SteamId(20));
      service.Install(SteamId(30));
      service.Install(SteamId(40));
      service.Pause(SteamId(30));
      service.MoveQueued(SteamId(20), false);
      auto snapshot = service.Snapshot();
      CHECK(snapshot.jobs[0].id == SteamId(10) && snapshot.jobs[1].id == SteamId(40) && snapshot.jobs[3].id == SteamId(20));
      CHECK(JobOf(snapshot, SteamId(10))->state == InstallJobState::kRunning && reorder_runner.canceled == 0);
      service.MoveQueued(SteamId(20), true);
      CHECK(service.Snapshot().jobs[1].id == SteamId(20));
      service.MoveQueued(SteamId(20), true);  // first queued: cannot displace the running download
      service.MoveQueued(SteamId(10), false);  // running downloads cannot be moved
      service.MoveQueued(SteamId(30), false);  // paused downloads cannot be moved
      service.MoveQueued(SteamId(999), false);
      CHECK(service.Snapshot().jobs[0].id == SteamId(10) && service.Snapshot().jobs[1].id == SteamId(20));
      service.MoveQueued(SteamId(20), false);
      service.MoveQueued(SteamId(20), false);  // last queued: stays put
      CHECK(service.Snapshot().jobs[3].id == SteamId(20));
    }
    {
      InstallService restored(reorder_runner, reorder_root.string());
      const auto snapshot = restored.Snapshot();
      CHECK(snapshot.jobs.size() == 4);
      CHECK(snapshot.jobs[0].id == SteamId(10) && snapshot.jobs[1].id == SteamId(40) &&
            snapshot.jobs[2].id == SteamId(30) && snapshot.jobs[3].id == SteamId(20));
      CHECK(JobOf(snapshot, SteamId(30))->state == InstallJobState::kPaused);
    }
    std::filesystem::remove_all(reorder_root);
  }
  {
    InstallService service(runner, root.string());
    const auto start = service.Snapshot();
    CHECK(start.installed.size() == 1 && start.installed[0].id == SteamId(100) && start.installed[0].install_bytes == 777);
    CHECK(start.jobs.size() == 1 && start.jobs[0].id == SteamId(200) && start.jobs[0].state == InstallJobState::kPaused);

    // Install, pause while running, resume to completion.
    runner.block = true;
    service.Install(SteamId(300));
    service.Install(SteamId(300));  // duplicates are ignored
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { auto* job = JobOf(s, SteamId(300)); return job && job->state == InstallJobState::kRunning && job->total_bytes == 100; });
    service.Pause(SteamId(300));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { auto* job = JobOf(s, SteamId(300)); return job && job->state == InstallJobState::kPaused; });
    AwaitAt(__LINE__, service, [&](const InstallSnapshot&) { return runner.canceled.load() == 1; });
    runner.block = false;
    service.Resume(SteamId(300));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(300)) && !JobOf(s, SteamId(300)); });
    CHECK(runner.installs == 2);
    service.Install(SteamId(300));  // already installed
    CHECK(!JobOf(service.Snapshot(), SteamId(300)));

    // Download now: the chosen download goes first and starts; the running one waits right behind it.
    runner.block = true;
    service.Install(SteamId(610));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { auto* job = JobOf(s, SteamId(610)); return job && job->state == InstallJobState::kRunning; });
    service.Install(SteamId(620));
    service.Install(SteamId(630));
    service.DownloadNow(SteamId(630));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
      auto* now = JobOf(s, SteamId(630)); auto* before = JobOf(s, SteamId(610));
      return now && now->state == InstallJobState::kRunning && before && before->state == InstallJobState::kQueued;
    });
    {
      const auto snapshot = service.Snapshot();
      const auto at = [&](GameId id) {
        return std::find_if(snapshot.jobs.begin(), snapshot.jobs.end(), [id](const InstallJobView& job) { return job.id == id; }) -
               snapshot.jobs.begin();
      };
      CHECK(at(SteamId(630)) == 0 && at(SteamId(610)) == 1 && at(SteamId(620)) > 1);
      CHECK(JobOf(snapshot, SteamId(620))->state == InstallJobState::kQueued);
    }
    // When it finishes, the one it interrupted carries on by itself.
    service.Cancel(SteamId(630));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
      auto* resumed = JobOf(s, SteamId(610));
      return !JobOf(s, SteamId(630)) && resumed && resumed->state == InstallJobState::kRunning;
    });
    service.DownloadNow(SteamId(610));  // already running: nothing changes
    CHECK(JobOf(service.Snapshot(), SteamId(610))->state == InstallJobState::kRunning);
    service.Cancel(SteamId(620));
    service.Cancel(SteamId(610));
    // The run stops for the cancel first (one that finished meanwhile would stay installed).
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return !JobOf(s, SteamId(610)); });
    runner.block = false;
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      std::lock_guard<std::mutex> lock(runner.mutex);
      return !JobOf(s, SteamId(610)) && !JobOf(s, SteamId(620)) && !JobOf(s, SteamId(630)) && runner.removed.size() >= 3;
    });
    {
      std::lock_guard<std::mutex> lock(runner.mutex);
      runner.removed.clear();
    }

    // Cancel a running install deletes its files; uninstall removes a finished one.
    runner.block = true;
    service.Install(SteamId(400));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { auto* job = JobOf(s, SteamId(400)); return job && job->state == InstallJobState::kRunning; });
    service.Cancel(SteamId(400));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return !JobOf(s, SteamId(400)); });
    runner.block = false;
    service.Uninstall(SteamId(100));
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      std::lock_guard<std::mutex> lock(runner.mutex);
      return !Installed(s, SteamId(100)) && runner.removed.size() >= 2;
    });
    {
      std::lock_guard<std::mutex> lock(runner.mutex);
      CHECK(std::find(runner.removed.begin(), runner.removed.end(), (root / "400").string()) != runner.removed.end());
      CHECK(std::find(runner.removed.begin(), runner.removed.end(), (root / "100").string()) != runner.removed.end());
    }

    // Without a session (Playden may still be restoring it) the job waits, queued, and says why.
    sessions.present = false;
    service.Install(SteamId(510));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) {
      auto* job = JobOf(s, SteamId(510));
      return job && job->state == InstallJobState::kQueued && job->failure == "Waiting for the Steam sign-in.";
    });
    // Once the sign-in is there it carries on by itself.
    sessions.present = true;
    service.Resume(SteamId(510));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(510)) && !JobOf(s, SteamId(510)); });
    // A waiting job the player pauses comes back paused after a restart.
    sessions.present = false;
    service.Install(SteamId(500));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { auto* job = JobOf(s, SteamId(500)); return job && !job->failure.empty(); });
    service.Pause(SteamId(500));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { auto* job = JobOf(s, SteamId(500)); return job && job->state == InstallJobState::kPaused; });
  }
  {
    // The registry survives a restart: the finished 300 and failed 500 are found again.
    std::ifstream registry(root / ".playden-games");
    const std::string text((std::istreambuf_iterator<char>(registry)), std::istreambuf_iterator<char>());
    CHECK(text.find("300") != std::string::npos);
    InstallService again(runner, root.string());
    CHECK(Installed(again.Snapshot(), SteamId(300)));
    // 500 was paused before it wrote anything; it comes back paused instead of disappearing.
    const auto restored = again.Snapshot();
    auto* failed = JobOf(restored, SteamId(500));
    CHECK(failed && failed->state == InstallJobState::kPaused && !failed->failure.empty());
  }
  std::filesystem::remove_all(root);

  {
    // Downloads that were running or queued when Playden stopped carry on after the restart (a
    // game started, or Playden was restarted); one the player paused waits for Resume.
    const auto resume_root = std::filesystem::temp_directory_path() / ("playden-install-resume-" + std::to_string(::getpid()));
    std::filesystem::remove_all(resume_root);
    Sessions resume_sessions;
    Runner resume_runner(resume_sessions);
    {
      InstallService service(resume_runner, resume_root.string());
      resume_runner.block = true;
      service.Install(SteamId(600));
      AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { auto* job = JobOf(s, SteamId(600)); return job && job->state == InstallJobState::kRunning; });
      service.Install(SteamId(700));
      service.Install(SteamId(800));
      service.Pause(SteamId(800));
    }
    std::ifstream registry(resume_root / ".playden-games");
    const std::string text((std::istreambuf_iterator<char>(registry)), std::istreambuf_iterator<char>());
    auto line_of = [&](const std::string& id) {
      const auto at = text.find(id + "\t");
      return at == std::string::npos ? std::string() : text.substr(at, text.find('\n', at) - at);
    };
    CHECK(!line_of("600").empty() && line_of("600").find("paused") == std::string::npos);
    CHECK(!line_of("700").empty() && line_of("700").find("paused") == std::string::npos);
    CHECK(line_of("800").find("\tpaused") != std::string::npos);
    resume_runner.block = false;
    InstallService again(resume_runner, resume_root.string());
    AwaitAt(__LINE__, again, [](const InstallSnapshot& s) { return Installed(s, SteamId(600)) && Installed(s, SteamId(700)); });
    const auto restored = again.Snapshot();
    auto* paused = JobOf(restored, SteamId(800));
    CHECK(paused && paused->state == InstallJobState::kPaused);
    std::filesystem::remove_all(resume_root);
  }

  // Several volumes: the primary keeps the registry; new installs follow the default volume.
  const auto primary = std::filesystem::temp_directory_path() / ("playden-install-primary-" + std::to_string(::getpid()));
  const auto second = std::filesystem::temp_directory_path() / ("playden-install-second-" + std::to_string(::getpid()));
  std::filesystem::remove_all(primary);
  std::filesystem::remove_all(second);
  std::filesystem::create_directories(primary / "100");
  std::ofstream(primary / "100" / ".playden-install.json") << "{\"install_bytes\":10}";
  std::filesystem::create_directories(second / "600");
  std::ofstream(second / "600" / ".playden-install.json") << "{\"install_bytes\":60}";
  // Legacy line (primary), a game on a drive that is absent now, a game on the second drive, and junk.
  std::ofstream(primary / ".playden-games") << "100\n500\tusb0\t1234\n600\tm2\t0\n700\tBAD VOLUME\t1\n";
  const std::vector<InstallVolume> volumes = {
      {"internal", "Console storage", primary.string(), true},
      {"m2", "M.2 SSD", second.string(), true},
      {"usb0", "USB drive", (second / "absent").string(), false},
  };
  sessions.present = true;
  {
    Runner multi(sessions);
    InstallService service(multi, volumes);
    auto start = service.Snapshot();
    CHECK(start.default_volume_id == "internal");
    CHECK(start.volumes.size() == 3);
    CHECK(start.installed.size() == 3);
    const auto on = [&](GameId id) {
      for (const auto& game : start.installed) if (game.id == id) return &game;
      return static_cast<const InstalledGame*>(nullptr);
    };
    CHECK(on(SteamId(100)) && on(SteamId(100))->volume_id == "internal" && on(SteamId(100))->available && on(SteamId(100))->install_bytes == 10);
    CHECK(on(SteamId(500)) && on(SteamId(500))->volume_id == "usb0" && !on(SteamId(500))->available && on(SteamId(500))->install_bytes == 1234);
    CHECK(on(SteamId(600)) && on(SteamId(600))->volume_id == "m2" && on(SteamId(600))->available && on(SteamId(600))->install_bytes == 60);
    CHECK(!on(SteamId(700)));

    service.SetDefaultVolume("usb0");  // unavailable: ignored
    CHECK(service.Snapshot().default_volume_id == "internal");
    // A refused write (a full disk) keeps the choice for the session and says it wasn't saved.
    std::filesystem::permissions(primary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    service.SetDefaultVolume("m2");
    CHECK(service.Snapshot().default_volume_id == "m2" && !service.Snapshot().default_volume_saved);
    std::filesystem::permissions(primary, std::filesystem::perms::owner_all);
    CHECK(service.RootFor(SteamId(800)) == (second / "800").string());
    CHECK(service.RootFor(SteamId(100)) == (primary / "100").string());

    service.Install(SteamId(800));
    AwaitAt(__LINE__, service, [](const InstallSnapshot& s) { return Installed(s, SteamId(800)); });
    {
      std::lock_guard<std::mutex> lock(multi.mutex);
      CHECK(multi.install_roots.size() == 1 && multi.install_roots[0] == (second / "800").string());
    }
    // With room again, the next registry write saves the choice too, without choosing it again.
    CHECK(service.Snapshot().default_volume_saved);
    for (const auto& game : service.Snapshot().installed) if (game.id == SteamId(800)) CHECK(game.volume_id == "m2");

    service.Uninstall(SteamId(500));  // its drive is absent: nothing to delete, the game stays listed
    CHECK(Installed(service.Snapshot(), SteamId(500)));
    service.Uninstall(SteamId(600));
    AwaitAt(__LINE__, service, [&](const InstallSnapshot& s) {
      std::lock_guard<std::mutex> lock(multi.mutex);
      return !Installed(s, SteamId(600)) && !multi.removed.empty();
    });
    {
      std::lock_guard<std::mutex> lock(multi.mutex);
      CHECK(multi.removed.back() == (second / "600").string());
    }
  }
  {
    // The default volume and every game's volume survive a restart.
    Runner multi(sessions);
    InstallService again(multi, volumes);
    const auto snapshot = again.Snapshot();
    CHECK(snapshot.default_volume_id == "m2");
    CHECK(Installed(snapshot, SteamId(800)) && Installed(snapshot, SteamId(500)) && Installed(snapshot, SteamId(100)) && !Installed(snapshot, SteamId(600)));
    for (const auto& game : snapshot.installed) {
      if (game.id == SteamId(800)) CHECK(game.volume_id == "m2" && game.available);
      if (game.id == SteamId(500)) CHECK(game.volume_id == "usb0" && !game.available && game.install_bytes == 1234);
    }
  }
  {
    // A game whose folder is missing from a present drive stays listed, unavailable, and is
    // saved again; it is available once its folder is back, and Uninstall can forget it.
    std::filesystem::rename(second / "800", second / "800.aside");
    {
      Runner multi(sessions);
      InstallService missing(multi, volumes);
      bool listed = false;
      for (const auto& game : missing.Snapshot().installed)
        if (game.id == SteamId(800)) listed = !game.available && game.volume_id == "m2" && game.install_bytes > 0;
      CHECK(listed);
      missing.SetDefaultVolume("internal");  // any save writes the registry again
      missing.SetDefaultVolume("m2");
      missing.Install(SteamId(900));
      AwaitAt(__LINE__, missing, [](const InstallSnapshot& s) { return Installed(s, SteamId(900)); });
    }
    std::filesystem::rename(second / "800.aside", second / "800");
    {
      Runner multi(sessions);
      InstallService back(multi, volumes);
      bool available = false;
      for (const auto& game : back.Snapshot().installed)
        if (game.id == SteamId(800)) available = game.available;
      CHECK(available);
    }
    std::filesystem::rename(second / "800", second / "800.aside");
    {
      Runner multi(sessions);
      InstallService forget(multi, volumes);
      forget.Uninstall(SteamId(800));
      AwaitAt(__LINE__, forget, [](const InstallSnapshot& s) { return !Installed(s, SteamId(800)); });
      // Its path names no install now, so nothing is removed.
      std::lock_guard<std::mutex> lock(multi.mutex);
      CHECK(multi.removed.empty());
    }
    {
      Runner multi(sessions);
      InstallService after(multi, volumes);
      CHECK(!Installed(after.Snapshot(), SteamId(800)));
    }
  }
  {
    // A remembered default whose drive is now absent falls back to the primary volume.
    auto without_m2 = volumes;
    without_m2[1].available = false;
    Runner multi(sessions);
    InstallService fallback(multi, without_m2);
    CHECK(fallback.Snapshot().default_volume_id == "internal");
    for (const auto& game : fallback.Snapshot().installed)
      if (game.id == SteamId(800)) CHECK(!game.available);
  }
  std::filesystem::remove_all(primary);
  std::filesystem::remove_all(second);
  std::puts("install service tests passed");
}
