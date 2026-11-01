// SPDX-License-Identifier: GPL-3.0-or-later
// The controller's update check of the installed games: it waits for the library refresh that reads the
// account's licenses, ends as "Couldn't check" when no refresh comes, and a game page answered before then
// leaves the saved list alone.
#include "homebrew/controller.hpp"
#include "steam/backend/cm_client.h"
#include "installs/install_record.h"
#include "stores/registry.h"
#include "stores/steam.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace playden;
using namespace playden::homebrew;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

constexpr std::uint64_t kAccount = 123;
const GameId kGame = GameId::Steam(230);

class Sessions final : public SessionRepository {
 public:
  mutable std::mutex mutex;
  mutable std::optional<StoredSession> session = StoredSession{kAccount, "Synthetic", "refresh", "access", {}};
  Result<std::optional<StoredSession>> Load() const override {
    std::lock_guard lock(mutex);
    return Result<std::optional<StoredSession>>::Success(session);
  }
  Result<void> Save(const StoredSession& value) const override {
    std::lock_guard lock(mutex); session = value; return Result<void>::Success();
  }
  Result<void> Clear() const override {
    std::lock_guard lock(mutex); session.reset(); return Result<void>::Success();
  }
};
class Auth final : public steam::AuthBackend {
 public:
  steam::AuthResult Restore(const StoredSession& session, const std::atomic<bool>&) override {
    return {steam::AuthError::kNone, session};
  }
  steam::AuthResult AuthenticateQr(const std::atomic<bool>&, const steam::ChallengeCallback&) override {
    return {steam::AuthError::kCanceled, {}};
  }
};

// Steam's library: a refresh is accepted (or refused) and answered when the test says.
class ScriptedLibrary final : public stores::LibrarySource {
 public:
  std::mutex mutex;
  bool refuse = false;
  std::uint64_t request = 0, account = 0, generation = 0;
  std::vector<LibraryEvent> events;
  Store store() const override { return Store::kSteam; }
  std::uint64_t Refresh(std::uint64_t account_id, std::uint64_t generation_id) override {
    std::lock_guard lock(mutex);
    if (refuse) return 0;
    account = account_id;
    generation = generation_id;
    return ++request;
  }
  std::uint64_t LoadCached(std::uint64_t account_id, std::uint64_t generation_id) override {
    return Refresh(account_id, generation_id);
  }
  std::uint64_t Clear(std::uint64_t, std::uint64_t) override { return 0; }
  void Invalidate(std::uint64_t) override {}
  void Shutdown() override {}
  std::atomic<int> license_reads{0};
  std::uint64_t EnsureLicenses(std::uint64_t, std::uint64_t) override {
    ++license_reads;
    return 1;
  }
  std::vector<LibraryEvent> Pump(std::size_t) override {
    std::lock_guard lock(mutex);
    return std::exchange(events, {});
  }
  // The refresh answers with the account's games, read from Steam; `licenses`: their licenses were read too.
  void Complete(bool licenses = true) {
    std::lock_guard lock(mutex);
    LibraryEvent event;
    event.kind = LibraryEventKind::kComplete;
    event.request_id = request;
    event.account_id = account;
    event.account_generation = generation;
    event.retrieved_at_unix_seconds = 1760000000;
    event.licenses_read = licenses;
    GameSummary game;
    game.id = kGame;
    game.title = "Stray";
    event.games.push_back(game);
    events.push_back(std::move(event));
  }
  // The license read finished for the library already loaded: the same request, with the dates filled in.
  void LicensesRead() {
    std::lock_guard lock(mutex);
    LibraryEvent event;
    event.kind = LibraryEventKind::kUpdated;
    event.license_attempt = true;
    event.licenses_read = true;
    event.request_id = request;
    event.account_id = account;
    event.account_generation = generation;
    event.retrieved_at_unix_seconds = 1760000000;
    GameSummary game;
    game.id = kGame;
    game.title = "Stray";
    event.games.push_back(game);
    events.push_back(std::move(event));
  }
};

// Steam's details: page answers are queued by the test; a check of the installed games is recorded and
// reports what the test sets.
class ScriptedDetails final : public stores::DetailsSource {
 public:
  std::mutex mutex;
  std::vector<steam::AppDetails> answers;
  std::vector<std::vector<GameId>> checks;
  std::vector<GameId> requests;
  steam::InstalledCheck progress;
  Store store() const override { return Store::kSteam; }
  void Request(const GameId& id, std::uint64_t) override {
    std::lock_guard lock(mutex);
    requests.push_back(id);
  }
  void Invalidate(std::uint64_t) override {}
  std::vector<steam::AppDetails> Pump(std::size_t) override {
    std::lock_guard lock(mutex);
    return std::exchange(answers, {});
  }
  bool CheckInstalled(const std::vector<GameId>& ids, std::uint64_t generation) override {
    std::lock_guard lock(mutex);
    checks.push_back(ids);
    progress = {generation, ids.size(), 0, 0, 0};
    return true;
  }
  steam::InstalledCheck CheckProgress() override {
    std::lock_guard lock(mutex);
    return progress;
  }
  void Shutdown() override {}
  void Answer(std::uint64_t generation, bool update) {
    std::lock_guard lock(mutex);
    steam::AppDetails details;
    details.id = kGame;
    details.account_generation = generation;
    details.available = true;
    details.update.newer = update;
    answers.push_back(details);
  }
  std::size_t Checks() {
    std::lock_guard lock(mutex);
    return checks.size();
  }
};

class NoDownloads final : public steam::InstallRunner {
 public:
  Result Install(const GameId&, const std::string&, const steam::InstallOptions&, const std::atomic<bool>&,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                 const std::function<void(steam::InstallStep)>&) override {
    return {false, false, "No downloads here."};
  }
  std::string Remove(const std::string&) override { return {}; }
};

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

struct Fixture {
  fs::path root;
  Simulation model{{}, {}, ModelMode::Live};
  Sessions sessions;
  Auth auth;
  ScriptedLibrary* library = nullptr;
  ScriptedDetails* details = nullptr;
  stores::Registry registry;
  NoDownloads downloads;
  std::unique_ptr<steam::InstallService> installs;
  std::unique_ptr<Controller> controller;

  explicit Fixture(bool installed = true, bool refuse = false) {
    char name[] = "/tmp/playden-update-check-XXXXXX";
    CHECK(mkdtemp(name));
    root = fs::canonical(name);
    if (installed) {
      Write(root / "230" / installs::kInstallRecordName, "{\"version\":2,\"app_id\":230,\"install_bytes\":4096,\"depots\":[]}\n");
      Write(root / ".playden-games", "230\tinternal\t4096\n");
    }
    registry.Add(std::make_unique<stores::SteamAccount>(auth, sessions));
    auto library_owned = std::make_unique<ScriptedLibrary>();
    library = library_owned.get();
    library->refuse = refuse;
    registry.Add(std::move(library_owned));
    auto details_owned = std::make_unique<ScriptedDetails>();
    details = details_owned.get();
    registry.Add(std::move(details_owned));
    installs = std::make_unique<steam::InstallService>(downloads, root.string());
    controller = std::make_unique<Controller>(model, registry, nullptr, installs.get());
    model.SetWallClock(1760000000);
    Until([&] { return model.state().account().authentication == AuthState::SignedIn; });
    // The account's saved check, a day old, lists an update for the game.
    model.BeginUpdateCheckLoad(kAccount);
    SavedUpdateCheck saved;
    saved.checked_at = 1760000000 - 2 * 24 * 60 * 60;
    if (installed) saved.games[kGame] = {true, false, 1000};
    model.ApplyUpdateCheck(kAccount, saved);
  }
  ~Fixture() {
    controller->Shutdown();
    installs->Shutdown();
    fs::remove_all(root);
  }
  template <typename Done> void Until(Done done) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!done()) {
      controller->Tick();
      CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(1ms);
    }
  }
  void Ticks(int count) {
    for (int i = 0; i < count; ++i) {
      controller->Tick();
      std::this_thread::sleep_for(1ms);
    }
  }
  bool Saved() const { return model.state().update_check.games.count(kGame) != 0; }
};

void WaitsForTheLibraryRefresh() {
  Fixture f;
  CHECK(f.model.action("update-check"));
  f.Ticks(20);
  CHECK(f.model.state().update_check.checking && f.details->Checks() == 0);
  // A page answered before the licenses are read says "no update": the saved list keeps the game.
  f.details->Answer(f.model.state().account_generation, false);
  f.Ticks(5);
  CHECK(f.Saved());
  // The refresh reads them: the check runs over the installed Steam games.
  f.library->Complete();
  f.Until([&] { return f.details->Checks() == 1; });
  CHECK(f.details->checks.front() == std::vector<GameId>{kGame});
  {
    std::lock_guard lock(f.details->mutex);
    f.details->progress.asked = f.details->progress.answered = 1;
  }
  f.details->Answer(f.model.state().account_generation, false);
  f.Until([&] { return !f.model.state().update_check.checking; });
  CHECK(!f.model.state().update_check.failed && !f.Saved());
  CHECK(f.model.state().update_check.checked_at > 1760000000 - 2 * 24 * 60 * 60);  // a new day starts
}

void ARefusedRefreshEndsAsCouldntCheck() {
  Fixture f(true, true);
  CHECK(f.model.action("update-check"));
  f.Until([&] { return !f.model.state().update_check.checking; });
  CHECK(f.model.state().update_check.failed && f.details->Checks() == 0);
  CHECK(f.Saved());  // the saved results stay
}

// The games came back but Steam's license read failed: every game would answer "no update", so the check
// ends as "Couldn't check" and the saved update stays.
void UnreadLicensesEndAsCouldntCheck() {
  Fixture f;
  CHECK(f.model.action("update-check"));
  f.Ticks(5);
  CHECK(f.model.state().update_check.checking);
  f.library->Complete(false);
  f.Until([&] { return !f.model.state().update_check.checking; });
  CHECK(f.model.state().update_check.failed && f.details->Checks() == 0 && f.Saved());
  // A page answered afterwards doesn't change the saved list either.
  f.details->Answer(f.model.state().account_generation, false);
  f.Ticks(5);
  CHECK(f.Saved());
}

void AFreshGrantChecksWithoutReadingLicenses() {
  Fixture f;
  steam::LicensedDepots depots;
  steam::AcquisitionResult grant;
  grant.owned_apps = {230};
  grant.owned_depots = {231};
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  grant.fetched_at = now;
  depots.Set(kAccount, grant);
  f.controller->SetLicensedDepots(&depots);
  CHECK(f.model.action("update-check"));
  f.Until([&] { return f.details->Checks() == 1; });
  CHECK(f.library->license_reads == 0);
  {
    std::lock_guard lock(f.details->mutex);
    f.details->progress.asked = f.details->progress.answered = 1;
  }
  f.details->Answer(f.model.state().account_generation, false);
  f.Until([&] { return !f.model.state().update_check.checking; });

  grant.fetched_at = now - 7200;
  depots.Set(kAccount, grant);
  CHECK(f.model.action("update-check"));
  f.Ticks(8);
  CHECK(f.library->license_reads == 1 && f.details->Checks() == 1);
  grant.fetched_at = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  depots.Set(kAccount, grant);
  f.Until([&] { return f.details->Checks() == 2; });
  f.Ticks(8);
  CHECK(f.library->license_reads == 1);
}

void AStaleCheckSurvivesALibraryRefresh() {
  Fixture f;
  steam::LicensedDepots depots;
  steam::AcquisitionResult grant;
  grant.owned_apps = {230};
  grant.owned_depots = {231};
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  grant.fetched_at = now - 7200;
  depots.Set(kAccount, grant);
  f.controller->SetLicensedDepots(&depots);
  CHECK(f.model.action("update-check"));
  f.Ticks(8);
  CHECK(f.library->license_reads == 1 && f.model.state().update_check.checking && !f.model.state().update_check.failed);
  CHECK(f.model.action("refresh-library"));
  f.Ticks(8);
  CHECK(f.library->license_reads == 2 && f.model.state().update_check.checking && !f.model.state().update_check.failed);
}

void AnUninstalledGameIsMeasuredAgainWhenLicensesArrive() {
  Fixture f(false);
  f.library->Complete(false);
  f.Ticks(5);
  CHECK(f.model.action("details:230"));
  f.Ticks(3);
  CHECK(f.details->requests.size() == 1 && f.details->requests.front() == kGame);
  f.library->LicensesRead();
  f.Ticks(5);
  CHECK(f.details->requests.size() == 2 && f.details->requests.back() == kGame);
}

void OpeningAGameReadsLicensesOncePerVisit() {
  Fixture f;
  f.library->Complete();
  f.Ticks(5);
  steam::LicensedDepots depots;
  steam::AcquisitionResult grant;
  grant.fetched_at = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  depots.Set(kAccount, grant);
  f.controller->SetLicensedDepots(&depots);
  CHECK(f.model.action("details:230"));
  f.Ticks(5);
  CHECK(f.library->license_reads == 0);
  CHECK(f.model.action("circle"));
  f.Ticks(2);
  grant.fetched_at -= 7200;
  depots.Set(kAccount, grant);
  CHECK(f.model.action("details:230"));
  f.Ticks(5);
  CHECK(f.library->license_reads == 1);
  grant.fetched_at = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  depots.Set(kAccount, grant);
  f.Ticks(3);
  CHECK(f.model.action("circle"));
  f.Ticks(2);
  CHECK(f.model.action("details:230"));
  f.Ticks(5);
  CHECK(f.library->license_reads == 1);
}

void NoSteamGamesIsUpToDate() {
  Fixture f(false);
  f.library->Complete();
  f.Ticks(5);
  CHECK(f.model.action("update-check"));
  f.Until([&] { return !f.model.state().update_check.checking; });
  CHECK(!f.model.state().update_check.failed && f.details->Checks() == 0);
}

}  // namespace

int main() {
  WaitsForTheLibraryRefresh();
  ARefusedRefreshEndsAsCouldntCheck();
  UnreadLicensesEndAsCouldntCheck();
  AFreshGrantChecksWithoutReadingLicenses();
  AStaleCheckSurvivesALibraryRefresh();
  OpeningAGameReadsLicensesOncePerVisit();
  AnUninstalledGameIsMeasuredAgainWhenLicensesArrive();
  NoSteamGamesIsUpToDate();
  std::puts("update check controller tests passed");
  return 0;
}
