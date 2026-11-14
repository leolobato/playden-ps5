// SPDX-License-Identifier: GPL-3.0-or-later
// The controller's update check of the installed games: it waits for the library refresh that reads the
// account's licenses, ends as "Couldn't check" when no refresh comes, and a game page answered before then
// leaves the saved list alone. GOG's runs beside it on its own sign-in, with real saved-check files: GOG only,
// both stores, Steam's sign-out, a restart, GOG's sign-out, a new GOG sign-in mid-check, sizes through GOG's
// details source, and a finished GOG update asking GOG again. Synthetic accounts only.
#include "app/persistence_service.h"
#include "app/preferences.h"
#include "fake_library_source.h"
#include "homebrew/controller.hpp"
#include "installs/install_record.h"
#include "library/organization.h"
#include "library/update_check.h"
#include "platform/storage.h"
#include "stores/registry.h"
#include "stores/steam.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
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
};

// Steam's details: page answers are queued by the test; a check of the installed games is recorded and
// reports what the test sets.
class ScriptedDetails final : public stores::DetailsSource {
 public:
  std::mutex mutex;
  std::vector<steam::AppDetails> answers;
  std::vector<std::vector<GameId>> checks;
  steam::InstalledCheck progress;
  Store store() const override { return Store::kSteam; }
  void Request(const GameId&, std::uint64_t) override {}
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

void NoSteamGamesIsUpToDate() {
  Fixture f(false);
  f.library->Complete();
  f.Ticks(5);
  CHECK(f.model.action("update-check"));
  f.Until([&] { return !f.model.state().update_check.checking; });
  CHECK(!f.model.state().update_check.failed && f.details->Checks() == 0);
}

// ---- GOG beside Steam ------------------------------------------------------------------------------------------

constexpr std::uint64_t kGogKey = 0xabc;
const GameId kGogGame = GameId::Gog(1900000001);

// GOG's account: its restore answers as the test says; later sign-ins and sign-outs come when the test raises them.
class ScriptedGogAccount final : public stores::StoreAccount {
 public:
  explicit ScriptedGogAccount(stores::AccountEventKind restore) : restore_(restore) {}
  Store store() const override { return Store::kGog; }
  std::uint64_t RestoreSession() override {
    std::lock_guard lock(mutex_);
    Queue(restore_, ++next_);
    return next_;
  }
  void Cancel(std::uint64_t) override {}
  void SignOut() override {
    std::lock_guard lock(mutex_);
    Queue(stores::AccountEventKind::kSignedOut, 0);
  }
  std::vector<stores::AccountEvent> PumpEvents(std::size_t) override {
    std::lock_guard lock(mutex_);
    return std::exchange(events_, {});
  }
  void Raise(stores::AccountEventKind kind) {
    std::lock_guard lock(mutex_);
    Queue(kind, 0);
  }

 private:
  void Queue(stores::AccountEventKind kind, std::uint64_t request) {
    stores::AccountEvent event;
    event.store = Store::kGog;
    event.kind = kind;
    event.request_id = request;
    event.generation = ++generation_;
    if (kind == stores::AccountEventKind::kSignedIn || kind == stores::AccountEventKind::kOffline) {
      event.account_key = kGogKey;
      event.display_name = "Synthetic GOG player";
    }
    events_.push_back(std::move(event));
  }
  stores::AccountEventKind restore_;
  std::mutex mutex_;
  std::vector<stores::AccountEvent> events_;
  std::uint64_t next_ = 0;
  std::uint64_t generation_ = 0;
};

// GOG's details: answers and sizes queued by the test; checks, measures and refreshes recorded.
class ScriptedGogDetails final : public stores::DetailsSource {
 public:
  std::mutex mutex;
  std::vector<stores::GameDetails> answers;
  std::vector<std::vector<GameId>> checks;
  std::vector<std::uint64_t> check_generations;
  steam::InstalledCheck progress;
  std::vector<steam::SizeRequest> measures;
  std::vector<steam::MeasuredSize> sizes;
  std::vector<GameId> refreshed;
  Store store() const override { return Store::kGog; }
  void Request(const GameId&, std::uint64_t) override {}
  void Refresh(const GameId& id, std::uint64_t, bool) override {
    std::lock_guard lock(mutex);
    refreshed.push_back(id);
  }
  void Invalidate(std::uint64_t generation) override {
    std::lock_guard lock(mutex);
    if (progress.account_generation < generation) progress = {};
  }
  std::vector<stores::GameDetails> Pump(std::size_t) override {
    std::lock_guard lock(mutex);
    return std::exchange(answers, {});
  }
  bool CheckInstalled(const std::vector<GameId>& ids, std::uint64_t generation) override {
    std::lock_guard lock(mutex);
    checks.push_back(ids);
    check_generations.push_back(generation);
    progress = {generation, ids.size(), ids.size(), 0, 0};
    return true;
  }
  steam::InstalledCheck CheckProgress() override {
    std::lock_guard lock(mutex);
    return progress;
  }
  bool Measure(const steam::SizeRequest& request, std::uint64_t) override {
    std::lock_guard lock(mutex);
    measures.push_back(request);
    return true;
  }
  std::vector<steam::MeasuredSize> PumpMeasured() override {
    std::lock_guard lock(mutex);
    return std::exchange(sizes, {});
  }
  void Shutdown() override {}
  // The check's (or a page's) answer: a newer build, named by `target`, or none.
  void Answer(std::uint64_t generation, bool newer, std::uint64_t target = 7) {
    std::lock_guard lock(mutex);
    stores::GameDetails details;
    details.id = kGogGame;
    details.account_generation = generation;
    details.available = true;
    details.windows_installable = true;
    details.update.newer = newer;
    details.update.target = newer ? target : 0;
    answers.push_back(details);
    if (progress.total != 0 && progress.answered < progress.total) ++progress.answered;
  }
  void Size(std::uint64_t generation, std::uint64_t target, std::uint64_t download) {
    std::lock_guard lock(mutex);
    steam::MeasuredSize size;
    size.request = {kGogGame, {}, {}, true};
    size.account_generation = generation;
    size.measured = true;
    size.target = target;
    size.download_bytes = download;
    size.staged_bytes = download;
    sizes.push_back(size);
  }
  std::size_t Checks() {
    std::lock_guard lock(mutex);
    return checks.size();
  }
  std::size_t Measures() {
    std::lock_guard lock(mutex);
    return measures.size();
  }
  std::size_t Refreshes(const GameId& id) {
    std::lock_guard lock(mutex);
    return static_cast<std::size_t>(std::count(refreshed.begin(), refreshed.end(), id));
  }
};

// GOG's install runner: an update waits until the test lets it finish (or it is canceled), then succeeds.
class GogUpdates final : public installs::InstallRunner {
 public:
  Result Install(const GameId&, const std::string&, const installs::InstallOptions&, const std::atomic<bool>&,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                 const std::function<void(installs::InstallStep)>&) override {
    return {false, false, "No downloads here."};
  }
  std::string Remove(const std::string&) override { return {}; }
  Result Update(const GameId&, const std::string&, const installs::InstallOptions&, const std::atomic<bool>& cancel,
                const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                const std::function<void(installs::InstallStep)>&, const std::function<bool()>&) override {
    std::unique_lock lock(mutex_);
    ++started_;
    while (!release_ && !cancel) wake_.wait_for(lock, 2ms);
    if (cancel) return {false, true, {}};
    Result result;
    result.ok = true;
    result.install_bytes = 4096;
    return result;
  }
  int Started() {
    std::lock_guard lock(mutex_);
    return started_;
  }
  void Release() {
    std::lock_guard lock(mutex_);
    release_ = true;
    wake_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool release_ = false;
  int started_ = 0;
};

// What the console keeps between runs: the installed Steam and GOG games, and the saved files.
struct Disk {
  fs::path root;
  fs::path games;
  std::unique_ptr<Storage> storage;
  std::unique_ptr<PreferencesStore> preferences;
  std::unique_ptr<OrganizationStore> organization;
  std::unique_ptr<UpdateCheckStore> steam_updates;
  std::unique_ptr<UpdateCheckStore> gog_updates;
  Disk() {
    char name[] = "/tmp/playden-update-check-stores-XXXXXX";
    CHECK(mkdtemp(name));
    root = fs::canonical(name);
    games = root / "games";
    Write(games / "230" / installs::kInstallRecordName, "{\"version\":2,\"app_id\":230,\"install_bytes\":4096,\"depots\":[]}\n");
    installs::GogRecord record;
    record.build_id = "1000000000000000101";
    record.products = {1900000001};
    record.language = "en-US";
    record.meta_id = std::string(32, 'a');
    Write(games / "gog-1900000001" / installs::kInstallRecordName,
          installs::FormatGogInstallRecord(kGogGame, 4096, 1, "english", record, {{"Game.exe", "", "", "Synthetic"}}));
    Write(games / ".playden-games", "230\tinternal\t4096\ngog-1900000001\tinternal\t4096\n");
    storage = std::make_unique<Storage>((root / "user").string());
    preferences = std::make_unique<PreferencesStore>(*storage);
    organization = std::make_unique<OrganizationStore>(*storage);
    steam_updates = std::make_unique<UpdateCheckStore>(*storage);
    gog_updates = std::make_unique<UpdateCheckStore>(*storage, "updates-gog-");
  }
  ~Disk() { fs::remove_all(root); }
  bool Has(const std::string& name) const { return fs::exists(root / "user" / name); }
};
const std::string kSteamFile = "updates-123.v2";
const std::string kGogFile = "updates-gog-" + std::to_string(kGogKey) + ".v2";

// One run of the app: Steam (signed in or out) and GOG (restored as asked) on the disk's games and saved files.
struct Run {
  Disk& disk;
  Simulation model{{}, {}, ModelMode::Live};
  Sessions sessions;
  Auth auth;
  test::FakeLibrarySource* steam_library = nullptr;
  ScriptedDetails* steam_details = nullptr;
  ScriptedGogAccount* gog = nullptr;
  ScriptedGogDetails* gog_details = nullptr;
  stores::Registry registry;
  NoDownloads steam_downloads;
  GogUpdates gog_downloads;
  std::unique_ptr<app::PersistenceService> persistence;
  std::unique_ptr<installs::InstallService> installs;
  std::unique_ptr<Controller> controller;

  // steam_hold: Steam's library refresh (which reads the licenses) waits for ReleaseSteam.
  Run(Disk& on, bool steam, stores::AccountEventKind gog_restore, bool steam_hold = false) : disk(on) {
    if (!steam) sessions.session.reset();
    model.SetGogStoreEnabled(true);
    model.SetStoreInstalls(Store::kGog, true);
    registry.Add(std::make_unique<stores::SteamAccount>(auth, sessions));
    auto steam_library_owned = std::make_unique<test::FakeLibrarySource>(Store::kSteam);
    steam_library = steam_library_owned.get();
    steam_library->hold = steam_hold;
    GameSummary stray;
    stray.id = kGame;
    stray.title = "Stray";
    steam_library->games = {stray};
    registry.Add(std::move(steam_library_owned));
    auto steam_details_owned = std::make_unique<ScriptedDetails>();
    steam_details = steam_details_owned.get();
    registry.Add(std::move(steam_details_owned));
    auto gog_owned = std::make_unique<ScriptedGogAccount>(gog_restore);
    gog = gog_owned.get();
    registry.Add(std::unique_ptr<stores::StoreAccount>(std::move(gog_owned)));
    auto gog_library = std::make_unique<test::FakeLibrarySource>(Store::kGog);
    GameSummary synthetic;
    synthetic.id = kGogGame;
    synthetic.title = "Synthetic GOG game";
    gog_library->games = {synthetic};
    registry.Add(std::move(gog_library));
    auto gog_details_owned = std::make_unique<ScriptedGogDetails>();
    gog_details = gog_details_owned.get();
    registry.Add(std::unique_ptr<stores::DetailsSource>(std::move(gog_details_owned)));
    persistence = std::make_unique<app::PersistenceService>(*disk.preferences, *disk.organization, disk.steam_updates.get());
    persistence->SetUpdateChecks(Store::kGog, disk.gog_updates.get());
    installs = std::make_unique<installs::InstallService>(
        installs::InstallService::Runners{{Store::kSteam, &steam_downloads}, {Store::kGog, &gog_downloads}},
        std::vector<installs::InstallVolume>{{"internal", "Console storage", disk.games.string(), true}});
    controller = std::make_unique<Controller>(model, registry, persistence.get(), installs.get());
    // Both stores' saved checks are read (each store's that signed in), and both games are installed.
    Until([&] {
      const auto& check = model.state().update_check;
      const bool gog_in = gog_restore == stores::AccountEventKind::kSignedIn || gog_restore == stores::AccountEventKind::kOffline;
      return (!steam || model.state().account().authentication == AuthState::SignedIn) &&
             (!gog_in || check.stores.count(Store::kGog)) && (!steam || check.stores.count(Store::kSteam)) &&
             check.loaded && model.state().installations.size() == 2;
    });
  }
  ~Run() {
    gog_downloads.Release();
    controller->Shutdown();
    installs->Shutdown();
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
  std::uint64_t GogGeneration() const { return controller->StoreGeneration(Store::kGog); }
  const UpdateCheckState& Check() const { return model.state().update_check; }
  bool Listed(const GameId& id) const { return Check().games.count(id) != 0; }
  bool Checking(Store store) const {
    const auto found = Check().stores.find(store);
    return found != Check().stores.end() && found->second.pending;
  }
  // Steam's check of its games answers: Stray has an update.
  void SteamAnswers() {
    {
      std::lock_guard lock(steam_details->mutex);
      steam_details->progress.asked = steam_details->progress.answered = 1;
    }
    steam_details->Answer(model.state().account_generation, true);
  }
};

// Steam signed out: Check for updates runs on GOG only; the GOG game with a newer build is listed, its size arrives
// through GOG's details source, and GOG's own file keeps it.
void GogOnlyChecksGog() {
  Disk disk;
  Run run(disk, false, stores::AccountEventKind::kSignedIn);
  CHECK(!run.Check().stores.count(Store::kSteam));
  CHECK(run.model.action("update-check"));
  run.Until([&] { return run.gog_details->Checks() == 1; });
  CHECK(run.gog_details->checks.front() == std::vector<GameId>{kGogGame});
  CHECK(run.gog_details->check_generations.front() == run.GogGeneration());
  CHECK(run.Check().checking && run.steam_details->Checks() == 0);
  run.gog_details->Answer(run.GogGeneration(), true);
  run.Until([&] { return !run.Check().checking; });
  CHECK(!run.Check().failed && run.Listed(kGogGame) && run.Check().games.at(kGogGame).target == 7);
  // Its size: measured by GOG's details source, then shown with the update.
  run.Until([&] { return run.gog_details->Measures() == 1; });
  CHECK(run.gog_details->measures.front().id == kGogGame && run.gog_details->measures.front().update);
  run.gog_details->Size(run.GogGeneration(), 7, 400'000'000);
  run.Until([&] { return run.Check().games.at(kGogGame).sized; });
  CHECK(run.Check().games.at(kGogGame).download_bytes == 400'000'000);
  CHECK(run.steam_details->Checks() == 0);
  run.Until([&] { return disk.Has(kGogFile); });
  CHECK(!disk.Has(kSteamFile));
}

// Steam and GOG side by side: Steam's check waits for its license read, GOG's doesn't; both games are listed, each in
// its own file. Steam's sign-out keeps the GOG game listed. A restart lists both again from the files, without asking
// either store. GOG's sign-out takes its game off the list and keeps its file.
void BothStoresSideBySide() {
  Disk disk;
  {
    Run run(disk, true, stores::AccountEventKind::kSignedIn, true);
    CHECK(run.model.action("update-check"));
    run.Until([&] { return run.gog_details->Checks() == 1; });
    run.Ticks(10);
    CHECK(run.Checking(Store::kSteam) && run.steam_details->Checks() == 0);
    run.gog_details->Answer(run.GogGeneration(), true);
    run.Until([&] { return !run.Checking(Store::kGog); });
    CHECK(run.Listed(kGogGame) && run.Checking(Store::kSteam));
    // The licenses are read: Steam's check runs over Steam's games only.
    run.steam_library->Release();
    run.Until([&] { return run.steam_details->Checks() == 1; });
    CHECK(run.steam_details->checks.front() == std::vector<GameId>{kGame});
    run.SteamAnswers();
    run.Until([&] { return !run.Check().checking; });
    CHECK(!run.Check().failed && run.Listed(kGame) && run.Listed(kGogGame));
    CHECK(run.gog_details->Checks() == 1);
    run.Until([&] { return disk.Has(kSteamFile) && disk.Has(kGogFile); });
    run.controller->Shutdown();
  }
  {
    // A restart offline on GOG: both come back from their files, and neither store is asked.
    Run run(disk, true, stores::AccountEventKind::kOffline);
    run.Ticks(20);
    CHECK(run.Listed(kGame) && run.Listed(kGogGame));
    CHECK(run.steam_details->Checks() == 0 && run.gog_details->Checks() == 0 && !run.Check().checking);
    // Steam signs out: GOG's update stays.
    run.model.RequestServiceAction({ServiceAction::SignOut, Store::kSteam});
    run.Until([&] { return run.model.state().account().authentication == AuthState::SignedOut; });
    run.Ticks(10);
    CHECK(!run.Listed(kGame) && run.Listed(kGogGame));
    // GOG signs out: its update leaves the list, its file stays.
    run.gog->Raise(stores::AccountEventKind::kSignedOut);
    run.Until([&] { return !run.Listed(kGogGame); });
    CHECK(!run.Check().stores.count(Store::kGog));
    run.controller->Shutdown();
    CHECK(disk.Has(kGogFile));
  }
}

// A new GOG sign-in while its check runs ends that check as "Couldn't check", and Steam's is untouched.
void GogSignInMidCheckFails() {
  Disk disk;
  Run run(disk, true, stores::AccountEventKind::kSignedIn, true);
  CHECK(run.model.action("update-check"));
  run.Until([&] { return run.gog_details->Checks() == 1; });
  const auto before = run.GogGeneration();
  run.gog->Raise(stores::AccountEventKind::kSignedIn);
  run.Until([&] { return !run.Checking(Store::kGog); });
  CHECK(run.GogGeneration() > before && run.Check().stores.at(Store::kGog).failed);
  CHECK(run.Checking(Store::kSteam) && !run.Check().stores.at(Store::kSteam).failed);
  // A late answer of the old sign-in changes nothing.
  run.gog_details->Answer(before, true);
  run.Ticks(10);
  CHECK(!run.Listed(kGogGame));
}

// A GOG update that finished asks GOG's details source again for the game, with Steam signed in or out.
void FinishedGogUpdateAsksGogAgain() {
  for (const bool steam : {true, false}) {
    Disk disk;
    Run run(disk, steam, stores::AccountEventKind::kSignedIn);
    CHECK(run.installs->Update(kGogGame, {}).empty());
    run.Until([&] {
      return run.gog_downloads.Started() == 1 &&
             std::any_of(run.model.state().jobs.begin(), run.model.state().jobs.end(),
                         [](const Job& job) { return job.game_id == kGogGame && job.update; });
    });
    run.Ticks(5);
    CHECK(run.gog_details->Refreshes(kGogGame) == 0);
    run.gog_downloads.Release();
    run.Until([&] { return run.gog_details->Refreshes(kGogGame) == 1; });
    CHECK(run.steam_details->Checks() == 0);
  }
}

}  // namespace

int main() {
  WaitsForTheLibraryRefresh();
  ARefusedRefreshEndsAsCouldntCheck();
  UnreadLicensesEndAsCouldntCheck();
  NoSteamGamesIsUpToDate();
  GogOnlyChecksGog();
  BothStoresSideBySide();
  GogSignInMidCheckFails();
  FinishedGogUpdateAsksGogAgain();
  std::puts("update check controller tests passed");
  return 0;
}
