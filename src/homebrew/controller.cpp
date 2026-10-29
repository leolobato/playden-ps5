#include "homebrew/controller.hpp"
#include "diagnostics/redaction.h"
#include "steam/install_volumes.h"
#include "steam/secret.h"
#include <algorithm>
#include <chrono>
#include <utility>

namespace playden::homebrew {
namespace {
// RuntimeHost names games by runtime number (a Steam game's app ID); 0 stands for no game, as it
// did. For a call: the game's number, given out on first use. Nothing when the runtime has none for
// it (RuntimeHost logs why to the runtime log); the call is then not made. A first number that
// would wait for the runtime worker is not given out: *busy says to ask again later. This is the
// one for the main thread, which never waits for the runtime worker.
std::optional<std::uint32_t> RuntimeApp(RuntimeHost& runtime, const GameId& id, bool* busy) {
  *busy = false;
  if (id == GameId{}) return 0u;
  return runtime.RuntimeNumberFor(id, busy);
}
// For a store's own worker thread, which may wait for the runtime worker (never the main thread).
std::optional<std::uint32_t> RuntimeAppWaiting(RuntimeHost& runtime, const GameId& id) {
  if (id == GameId{}) return 0u;
  return runtime.RuntimeNumberFor(id);
}
// For lookups in a runtime snapshot's number-keyed fields: a game with no number finds nothing there.
std::uint32_t NumberIn(const RuntimeSnapshot& snapshot, const GameId& id) {
  if (const auto app = id.steam_app()) return *app;
  for (const auto& [number, game] : snapshot.runtime_games)
    if (game == id) return number;
  return 0;
}
// The game a number in a runtime snapshot stands for.
GameId GameIn(const RuntimeSnapshot& snapshot, std::uint32_t number) {
  const auto found = snapshot.runtime_games.find(number);
  return found != snapshot.runtime_games.end() ? found->second : GameId::Steam(number);
}
bool IsNumber(const RuntimeSnapshot& snapshot, const std::optional<std::uint32_t>& number, const GameId& id) {
  return number && GameIn(snapshot, *number) == id;
}
std::unique_ptr<stores::Registry> SteamRegistry(steam::AuthBackend& backend, SessionRepository& repository,
                                                LibraryService* library_service, steam::AppDetailsService* app_details,
                                                std::function<void(const std::string&)> auth_log) {
  auto registry = std::make_unique<stores::Registry>();
  registry->Add(std::make_unique<stores::SteamAccount>(backend, repository, std::move(auth_log)));
  if (library_service) registry->Add(std::make_unique<stores::SteamLibrarySource>(*library_service));
  if (app_details) registry->Add(std::make_unique<stores::SteamDetailsSource>(*app_details));
  return registry;
}
}  // namespace
std::string TitleStoreSuffix(Store store) {
  if (store == Store::kLocal) return {};
  return " (" + std::string(Traits(store).name) + ")";
}
Controller::Controller(Simulation& model, stores::Registry& stores, app::PersistenceService* persistence_service,
                       steam::InstallService* installs, steam::CloudService* cloud, RuntimeHost* runtime,
                       std::function<void(const std::string&)> auth_log)
    : model_(model), stores_(stores), installs_(installs), cloud_(cloud), runtime_(runtime), auth_log_(std::move(auth_log)) {
  Start(persistence_service);
}
Controller::Controller(Simulation& model, steam::AuthBackend& backend, SessionRepository& repository,
                       LibraryService* library_service, app::PersistenceService* persistence_service,
                       steam::AppDetailsService* app_details, steam::InstallService* installs,
                       steam::CloudService* cloud, RuntimeHost* runtime,
                       std::function<void(const std::string&)> auth_log)
    : model_(model), owned_stores_(SteamRegistry(backend, repository, library_service, app_details, auth_log)),
      stores_(*owned_stores_), installs_(installs), cloud_(cloud), runtime_(runtime), auth_log_(std::move(auth_log)) {
  Start(persistence_service);
}
void Controller::SetPersistenceLog(std::function<void(const std::string&)> log) {
  persistence_log_ = std::move(log);
  if (persistence_) persistence_->SetLog(persistence_log_);
}

void Controller::Start(app::PersistenceService* persistence_service) {
  if (model_.state().simulation_mode) return;
  app_details_ = stores_.details(Store::kSteam);
  if (auto* steam = stores_.library(Store::kSteam)) {
    library_ = std::make_unique<app::LibraryController>(model_, *steam);
  }
  service_ = stores_.steam_account();
  phase_ = Phase::Restore;
  Update(AuthState::Restoring,{},true);
  if(persistence_service) {
    persistence_=std::make_unique<app::PersistenceController>(model_,*persistence_service);
    persistence_->SetLog(persistence_log_);
  }
  request_ = service_->RestoreSession();
  // Every other store's library loads now, next to Steam's.
  if (library_)
    for (auto* source : stores_.libraries())
      if (source->store() != Store::kSteam) library_->AddSource(*source);
  if (cloud_)
    // On the Cloud worker, just before a job checks a game: never while its title may run.
    cloud_->SetGate([this](GameId id) {
      {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        if (gate_blocked_.count(id)) return false;
      }
      // Only Steam's games have Cloud saves: another store's game needs no runtime number here.
      if (!CloudCapable(id) || !runtime_) return true;
      bool busy = false;
      const auto app = RuntimeApp(*runtime_, id, &busy);
      return app && runtime_->ActivityNow(*app, std::chrono::seconds(5)) == runtime::NativeActivity::kStopped;
    });
  if (installs_)
    // On the install worker, just before an update puts a game's files in place: never while its title runs
    // or starts, or when the console doesn't answer (kUnknown). The service has locked the game already, and
    // Play checks that lock after marking the game as starting (TickRuntime), so neither slips past the other.
    installs_->SetApplyGate([this](GameId id) {
      using Answer = installs::ApplyAnswer;
      {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        if (gate_blocked_.count(id)) return Answer::kGameRunning;
      }
      if (!runtime_) return Answer::kApply;
      bool busy = false;
      const auto app = RuntimeApp(*runtime_, id, &busy);
      if (!app) return Answer::kUnknown;
      // A console that doesn't answer may take the whole wait each time: asked again only after a longer one.
      switch (runtime_->ActivityNow(*app, std::chrono::seconds(5))) {
        case runtime::NativeActivity::kStopped: return Answer::kApply;
        case runtime::NativeActivity::kUnknown: return Answer::kUnknown;
        default: return Answer::kGameRunning;
      }
    });
}
Controller::~Controller() { Shutdown(); }
void Controller::Shutdown() {
  if (shut_down_) return;
  shut_down_ = true;
  // Preserve a confirmed sign-out queued by the last input frame before exit.
  if (service_)
    for (const auto action : model_.TakeServiceActions()) {
      if (action.store == Store::kSteam && (action == ServiceAction::SignOut || action == ServiceAction::SwitchAccount) &&
          phase_!=Phase::ClearLibrary && phase_!=Phase::ClearOrganization && phase_!=Phase::SignOut) SignOut(false);
      // Turned off just before quitting: the saved token must still go.
      if (action.store == Store::kSteam && action == ServiceAction::StaySignedIn && stay_signed_in_)
        stay_signed_in_(model_.state().stay_signed_in);
    }
  begin_after_clear_=false;
  // Each step is logged before it starts and with its time after, so a hang shows where it stopped.
  const auto step = [this](const char* name, const auto& work) {
    if (shutdown_log_) shutdown_log_(std::string("shutdown ") + name + " start");
    const auto started = std::chrono::steady_clock::now();
    work();
    if (shutdown_log_)
      shutdown_log_(std::string("shutdown ") + name + " ms=" +
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count()));
  };
  // The server source may inspect the install snapshot for its fallback payload.
  if(download_servers_) step("download-servers", [&] { download_servers_->Shutdown(); });
  if(web_) step("web", [&] { web_->Shutdown(); });
  if(library_) step("library", [&] { library_->Shutdown(); library_->Tick(); AdvanceClear(); });
  if(persistence_) step("persistence", [&] { persistence_->FlushAndShutdown(); AdvanceOrganizationClear(); });
  if(app_details_) step("details", [&] { app_details_->Shutdown(); });
  // A copy or move waiting for the runtime (RuntimeHost::Relocate) stops waiting first, so the
  // install service and the Local store's worker it blocks are joined at once; it resumes at the
  // next start.
  if(runtime_) runtime_->BeginClosing();
  // The install service stops whether or not a Steam account is registered: its Local copy runner
  // calls into the Local store, which the caller destroys after this (host.cpp, ps5.cpp).
  if(installs_) step("installs", [&] { installs_->Shutdown(); });
  if(cloud_) step("cloud", [&] { cloud_->Shutdown(); });
  if(runtime_) step("runtime", [&] { runtime_->Shutdown(); });
  persistence_.reset();
  library_.reset();
  if (!service_) return;
  service_->Shutdown();
  service_ = nullptr;
  phase_ = Phase::Idle;
  Update(AuthState::SignedOut,{},true);
}
void Controller::Update(AuthState state,const std::string& error,bool clear,const std::string& challenge,
                        SignInStep step,const steam::SteamEvent* prompt,bool storage_full,bool restoring) {
  AccountUpdate update;
  update.authentication = state;
  update.id = clear ? std::string{} : model_.state().account().id;
  update.display_name = clear ? std::string{} : model_.state().account().display_name;
  update.error = error; update.challenge_url = challenge; update.clear_library = clear;
  update.sign_in = step;
  update.storage_full = storage_full;
  update.restoring = restoring;
  if (prompt) {
    update.email_domain = prompt->email_domain;
    update.code_allowed = prompt->code_allowed;
    update.code_is_email = prompt->code_is_email;
    update.code_rejected = prompt->kind == steam::SteamEventKind::kCodeRejected;
  }
  model_.UpdateAccount(std::move(update));
}
void Controller::Begin() { Begin(model_.state().sign_in_password); }
void Controller::Begin(bool password) {
  if (retry_saved_session_) {
    if (phase_ == Phase::Idle) Restore();
    return;
  }
  if (phase_ == Phase::SignOut || phase_==Phase::ClearLibrary || phase_==Phase::ClearOrganization) {
    begin_after_clear_ = true; begin_password_after_clear_ = password; return;
  }
  if(!model_.state().account().id.empty() && established_auth_==AuthState::Offline) { Restore();return; }
  if(!model_.state().account().id.empty() && established_auth_==AuthState::SignedIn) {
    SignOut(true); begin_password_after_clear_ = password; return;
  }
  if(library_) library_->Reset();
  if(persistence_) persistence_->Reset();
  phase_ = Phase::SignIn;
  password_sign_in_ = password;
  if (password) {
    // The form waits for the player; nothing runs until Sign in.
    if (request_) service_->Cancel(request_);
    request_ = 0;
    Update(AuthState::Waiting,{},true,{},SignInStep::Password);
    return;
  }
  request_ = service_->BeginQrSignIn();
  Update(AuthState::Waiting,{},true);
}
void Controller::UseMethod(bool password) {
  if (phase_ != Phase::SignIn) { Begin(password); return; }
  password_sign_in_ = password;
  if (password) {
    if (request_) service_->Cancel(request_);
    request_ = 0;
    Update(AuthState::Waiting,{},!reauthenticating_,{},SignInStep::Password);
  } else {
    request_ = service_->BeginQrSignIn();
    Update(AuthState::Waiting,{},!reauthenticating_);
  }
}
void Controller::BeginPassword() {
  auto credentials = model_.TakeSignInCredentials();
  if (!credentials) return;
  const bool ready = phase_ == Phase::SignIn || (phase_ == Phase::Idle && model_.state().account().id.empty());
  if (!ready || credentials->account_name.empty() || credentials->password.empty()) {
    steam::ZeroizeSecret(credentials->password);
    Update(model_.state().account().authentication == AuthState::Failed ? AuthState::Failed : AuthState::Waiting,
           {},false,{},SignInStep::Password);
    return;
  }
  if (phase_ == Phase::Idle) {
    if(library_) library_->Reset();
    if(persistence_) persistence_->Reset();
  }
  phase_ = Phase::SignIn;
  password_sign_in_ = true;
  request_ = service_->BeginCredentialSignIn(std::move(credentials->account_name), std::move(credentials->password));
  steam::ZeroizeSecret(credentials->password);
  Update(AuthState::Waiting,{},!reauthenticating_,{},SignInStep::Submitting);
}
void Controller::Reauthenticate() {
  if (phase_ != Phase::Idle || model_.state().account().id.empty()) return;
  reauthenticating_ = true;
  phase_ = Phase::SignIn;
  password_sign_in_ = model_.state().sign_in_password;
  if (password_sign_in_) {
    request_ = 0;
    Update(AuthState::Waiting,{},false,{},SignInStep::Password);
    return;
  }
  request_ = service_->BeginQrSignIn();
  Update(AuthState::Waiting);
}
void Controller::Restore(bool quiet) {
  if(library_) library_->Reset();
  phase_=Phase::Restore;request_=service_->RestoreSession();
  // A retry every few seconds that flipped to Restoring and back would move the screen and close
  // the focus of a list opened from it (Installed games on a full console storage).
  if (!quiet) Update(AuthState::Restoring);
}
void Controller::SignOut(bool switch_account) {
  if(phase_==Phase::ClearLibrary || phase_==Phase::ClearOrganization || phase_==Phase::SignOut) return;
  if (download_servers_) download_servers_->Invalidate();
  retry_saved_session_ = false;
  begin_after_clear_ = switch_account;
  begin_password_after_clear_ = model_.state().sign_in_password;
  request_ = 0;
  if(library_) { phase_=Phase::ClearLibrary;library_->Clear(); }
  else BeginOrganizationClear();
  Update(AuthState::SigningOut);
}
void Controller::AdvanceClear() {
  if(phase_!=Phase::ClearLibrary || !library_) return;
  const auto result=library_->TakeClearResult();
  if(!result) return;
  if(*result==steam::LibraryError::kNone) {
    BeginOrganizationClear();
  } else {
    phase_=Phase::Idle;begin_after_clear_=false;
    Update(model_.state().account().id.empty()?AuthState::Failed:established_auth_,
           "Sign-out failed. Your saved library remains. Retry sign-out in Settings.");
  }
}
void Controller::BeginOrganizationClear() {
  if(persistence_) {phase_=Phase::ClearOrganization;persistence_->Clear();}
  else {phase_=Phase::SignOut;service_->SignOut();}
}
void Controller::AdvanceOrganizationClear() {
  if(phase_!=Phase::ClearOrganization || !persistence_) return;
  const auto result=persistence_->TakeClearResult();
  if(!result) return;
  if(*result==StorageError::kNone) {phase_=Phase::SignOut;service_->SignOut();}
  else {
    phase_=Phase::Idle;begin_after_clear_=false;
    Update(model_.state().account().id.empty()?AuthState::Failed:established_auth_,
           "Sign-out failed. Your saved organization remains. Retry sign-out in Settings.");
  }
}
void Controller::Tick() {
  if (!service_) return;
  model_.SetWallClock(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  for (const auto request : model_.TakeServiceActions()) {
    // Only Steam has an account to act on.
    if (request.store != Store::kSteam) continue;
    switch (request.action) {
      case ServiceAction::BeginQr: Begin(false); break;
      case ServiceAction::Reauthenticate: Reauthenticate(); break;
      case ServiceAction::UsePassword: UseMethod(true); break;
      case ServiceAction::UseQr: UseMethod(false); break;
      case ServiceAction::BeginPassword: BeginPassword(); break;
      case ServiceAction::SubmitCode:
        if (auto code = model_.TakeGuardCode()) {
          if (phase_ == Phase::SignIn && password_sign_in_ && request_) service_->SubmitGuardCode(std::move(*code));
          steam::ZeroizeSecret(*code);
        }
        break;
      case ServiceAction::CancelQr:
        // Stop automatic retries while preserving the need to restore the
        // saved account when the user chooses Connect again.
        retry_saved_session_at_ = std::chrono::steady_clock::time_point::max();
        if (phase_ == Phase::SignIn && reauthenticating_) {
          // Back to the saved sign-in; nothing was cleared.
          service_->Cancel(request_);
          reauthenticating_ = false;
          Restore();
          break;
        }
        if (phase_ == Phase::SignIn && !request_) {
          // The password form, with nothing sent to Steam yet.
          phase_ = Phase::Idle;
          if(library_) library_->Reset();
          if(persistence_) persistence_->Reset();
          Update(AuthState::Cancelled,{},!retry_saved_session_);
          break;
        }
        if (phase_ == Phase::SignIn || phase_ == Phase::Restore) {
          service_->Cancel(request_);
          if(library_) library_->Reset();
          if(persistence_) persistence_->Reset();
          Update(AuthState::Cancelled,{},!retry_saved_session_);
        }
        break;
      case ServiceAction::SignOut: SignOut(false); break;
      case ServiceAction::SwitchAccount: SignOut(true); break;
      case ServiceAction::StaySignedIn:
        if (stay_signed_in_) stay_signed_in_(model_.state().stay_signed_in);
        break;
      case ServiceAction::RefreshLibrary:
        if (phase_ == Phase::Idle && !model_.state().account().id.empty()) {
          if(library_) {
            if(established_auth_==AuthState::Offline || library_->NeedsRenewal()) Restore();
            else library_->Refresh();
          } else model_.UpdateLibrary({{},false,"Library sync is unavailable. Try again later.",
                                        model_.state().library_offline,model_.state().account().cache_available,{}});
        }
        break;
    }
  }
  if (retry_saved_session_ && phase_ == Phase::Idle &&
      std::chrono::steady_clock::now() >= retry_saved_session_at_) Restore(true);
  auto mark = std::chrono::steady_clock::now();
  const auto lap = [&](std::size_t index) {
    const auto now = std::chrono::steady_clock::now();
    tick_us_[index] = std::chrono::duration_cast<std::chrono::microseconds>(now - mark).count();
    mark = now;
  };
  for (const auto& event : service_->Pump(16)) Apply(event);
  lap(0);
  if(library_) { library_->Tick();AdvanceClear(); }
  TickLicenseChanges();
  lap(1);
  if(persistence_) { persistence_->Tick();AdvanceOrganizationClear(); }
  lap(2);
  TickAppDetails();
  lap(3);
  TickStoreDetails();
  lap(4);
  TickImports();
  TickInstalls();
  TickDownloadServers();
  TickWebAccess();
  TickSelfUpdate();
  lap(5);
  TickGate();
  TickCloud();
  lap(6);
  TickRuntime();
  lap(7);
  TickUninstalls();
  lap(8);
}
std::string Controller::LastTickProfile() const {
  static const char* const kNames[] = {"auth", "library", "persistence", "details", "store", "installs", "cloud", "runtime",
                                       "uninstalls"};
  std::string out;
  for (std::size_t index = 0; index < tick_us_.size(); ++index)
    if (tick_us_[index] >= 1000) out += std::string(out.empty() ? "" : " ") + kNames[index] + "=" + std::to_string(tick_us_[index] / 1000);
  return out;
}
bool Controller::SessionActive(GameId id) const {
  if (!runtime_) return false;
  const auto snapshot_shared = runtime_->Shared();
  const auto& snapshot = *snapshot_shared;
  // Until the runtime has started, a session pending from before a restart is
  // not known yet: no game's saves move.
  if (snapshot.status == RuntimeHostStatus::kStarting) return true;
  if (play_queued_.count(id)) return true;
  if (!snapshot.launch || snapshot.launch->game != id) return false;
  using runtime::LaunchState;
  const auto state = snapshot.launch->state;
  return state == LaunchState::kReadyToHandoff || state == LaunchState::kLaunching || state == LaunchState::kRunning;
}
std::optional<steam::CloudLocation> Controller::CloudLocationFor(GameId id, bool for_job) const {
  // A store without Cloud saves has nothing to sync: every request for its games is answered as such.
  if (saves_root_.empty() || !Traits(id.store).cloud_saves) return std::nullopt;
  steam::CloudLocation location{id, {}, saves_root_ + "/" + ToKey(id)};
  if (const auto root = game_roots_.find(id); root != game_roots_.end()) {
    location.game_root = root->second;
    // A prepared game keeps its Windows-folder saves in its private bottle, also while its
    // PS5 title waits for registration: the bottle is complete and the game runs from Home.
    if (runtime_) {
      const auto installs_shared = runtime_->Shared();
      // A title made for another folder (its game moved drives) is about to be replaced: only check.
      for (const auto& view : installs_shared->installs)
        if (view.game == id && (view.state == runtime::PreparationState::kReady || view.awaiting_registration) &&
            (view.install_path.empty() || view.install_path == root->second))
          location.prefix_root = view.prefix_root;
    }
    // Not set up yet (or still being prepared): there is no bottle the game reads, so Cloud only
    // checks. Saves written anywhere else were never seen by the game (Mullet Mad Jack, #72).
    if (location.prefix_root.empty()) location.check_only = true;
    return location;
  }
  // An uninstalled game's kept bottle: no game folder; a job only checks it.
  if (runtime_) {
    const auto kept_bottles_shared = runtime_->Shared();
    for (const auto& kept : kept_bottles_shared->kept_bottles)
      if (kept.game == id && !kept.data_root.empty()) {
        location.prefix_root = kept.data_root + "/library/prefixes/game";
        location.check_only = for_job;
        return location;
      }
  }
  return std::nullopt;
}

void Controller::QueueSavesJob(steam::CloudJobKind kind) {
  if (!cloud_ || saves_root_.empty()) return;
  if (cloud_->Snapshot().job.kind != steam::CloudJobKind::kNone) return;
  if (pending_job_ && (pending_job_->kind == kind || kind == steam::CloudJobKind::kQuickCheck)) return;
  PendingJob job;
  job.kind = kind;
  for (const auto& [id, root] : game_roots_)
    if (CloudCapable(id)) job.ids.push_back(id);
  if (runtime_) {
    const auto kept_bottles_shared = runtime_->Shared();
    for (const auto& kept : kept_bottles_shared->kept_bottles)
      if (!game_roots_.count(kept.game) && CloudCapable(kept.game)) job.ids.push_back(kept.game);
  }
  job.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  pending_job_ = std::move(job);
  if (kind == steam::CloudJobKind::kQuickCheck) last_quick_check_ = std::chrono::steady_clock::now();
  if (runtime_) runtime_->MeasureSaves();
}

void Controller::TickGate() {
  // Games whose sessions Playden knows about, for the gates on the Cloud and install workers.
  std::set<GameId> blocked;
  for (const auto& [id, root] : game_roots_)
    if (SessionActive(id)) blocked.insert(id);
  std::lock_guard<std::mutex> lock(gate_mutex_);
  gate_blocked_ = std::move(blocked);
}
void Controller::TickCloud() {
  if (!cloud_) return;
  if (!cloud_restored_ && !saves_root_.empty()) {
    // The last result of every game, so the Saves tab shows them at once.
    cloud_->Restore(saves_root_);
    cloud_restored_ = true;
  }
  for (const auto id : cloud_->TakeDeferred()) after_play_.insert(id);
  const auto now = std::chrono::steady_clock::now();
  const bool signed_in = model_.state().account().authentication == AuthState::SignedIn;
  auto requests = model_.TakeCloudRequests();
  std::vector<CloudRequest> transfers;
  for (const auto& request : requests) {
    switch (request.kind) {
      case CloudRequestKind::QuickCheck:
        // Opening Saves checks what may have changed, at most every two minutes.
        if (signed_in && (!last_quick_check_ || now - *last_quick_check_ >= std::chrono::minutes(2)))
          QueueSavesJob(steam::CloudJobKind::kQuickCheck);
        else if (runtime_)
          runtime_->MeasureSaves();
        break;
      case CloudRequestKind::SyncAll:
        if (signed_in) QueueSavesJob(steam::CloudJobKind::kSyncAll);
        break;
      case CloudRequestKind::StopJob:
        pending_job_.reset();
        cloud_->StopJob();
        break;
      case CloudRequestKind::RemoveLeftover:
        // The kept bottle and the game's sync state; no PS5 title is involved.
        if (runtime_)
          WithApp(request.game_id, [this](std::uint32_t app) { runtime_->DeleteKeptBottle(app); });
        if (!saves_root_.empty() && CloudCapable(request.game_id))
          cloud_->Forget({request.game_id, {}, saves_root_ + "/" + ToKey(request.game_id)});
        after_play_.erase(request.game_id);
        break;
      default:
        cloud_foreground_[request.game_id] = kNotDispatched;
        transfers.push_back(request);
        break;
    }
  }
  // Once per run, after sign-in and once the runtime knows the games: catch up with
  // sessions started from the PS5 Home screen since Playden last ran.
  const bool auto_sync = model_.state().cloud_saves_auto;
  if (!startup_quick_check_ && auto_sync && signed_in && !saves_root_.empty() &&
      (!runtime_ || runtime_->Shared()->status != RuntimeHostStatus::kStarting)) {
    startup_quick_check_ = true;
    QueueSavesJob(steam::CloudJobKind::kQuickCheck);
  }
  // A session that just ended syncs its saves, as Steam does after play.
  for (auto& [id, issued] : sync_before_play_)
    if (!issued) {
      transfers.push_back({CloudRequestKind::Sync, id});
      cloud_foreground_[id] = kNotDispatched;
      issued = true;
    }
  for (const auto id : uninstalls_.TakeSyncRequests()) {
    transfers.push_back({CloudRequestKind::Sync, id});
    cloud_foreground_[id] = kNotDispatched;
  }
  for (auto it = sync_after_session_.begin(); it != sync_after_session_.end();) {
    if (!auto_sync || !CloudCapable(*it)) {
      it = sync_after_session_.erase(it);  // automatic sync is off: the player syncs from Saves
    } else if (game_roots_.count(*it) && !saves_root_.empty()) {
      transfers.push_back({CloudRequestKind::Sync, *it});
      it = sync_after_session_.erase(it);
    } else {
      ++it;
    }
  }
  // Games that may have been running when a job started sync once they stop; asked again
  // every half minute.
  if (!auto_sync) after_play_.clear();
  std::erase_if(after_play_, [this](const GameId& id) { return !CloudCapable(id); });
  if (!after_play_.empty() && (!after_play_checked_ || now - *after_play_checked_ >= std::chrono::seconds(30))) {
    after_play_checked_ = now;
    for (const auto id : after_play_)
      if (std::none_of(cloud_held_.begin(), cloud_held_.end(), [id](const HeldCloudRequest& held) { return held.request.game_id == id; }))
        transfers.push_back({CloudRequestKind::Sync, id});
  }
  // A game whose store has no Cloud saves never waits for the console: its request is answered at
  // once, as one with nothing to sync.
  std::vector<CloudRequest> confirmed;
  for (const auto& request : transfers) {
    if (CloudCapable(request.game_id)) cloud_held_.push_back({request, false, 0});
    else confirmed.push_back(request);
  }
  // Saves move only once the console confirms the game's own title is stopped,
  // whoever started it (Playden or the PS5 Home screen). Unknown counts as running.
  if (!runtime_) {
    for (const auto& held : cloud_held_) confirmed.push_back(held.request);
    cloud_held_.clear();
  } else {
    const auto runtime_shared = runtime_->Shared();
    const auto& runtime = *runtime_shared;
    std::vector<HeldCloudRequest> waiting;
    for (auto& held : cloud_held_) {
      const auto id = held.request.game_id;
      if (runtime.status == RuntimeHostStatus::kStarting) { waiting.push_back(held); continue; }
      if (!held.asked) {
        bool busy = false;
        const auto app = RuntimeApp(*runtime_, id, &busy);
        if (busy) { waiting.push_back(held); continue; }  // asked again next tick
        held.asked = true;
        held.after = runtime.activity_serial;
        if (app) runtime_->CheckActivity(*app);
        waiting.push_back(held);
        continue;
      }
      const auto answer = runtime.activity.find(NumberIn(runtime, id));
      if (answer == runtime.activity.end() || answer->second.second <= held.after) { waiting.push_back(held); continue; }
      if (answer->second.first == runtime::NativeActivity::kStopped && !SessionActive(id)) {
        confirmed.push_back(held.request);
        continue;
      }
      // Refused: the title runs, or its state is unknown.
      if (cloud_foreground_.erase(id)) model_.NotifyCloudRefused(id);
      if (const auto stopped = uninstalls_.CloudRefused(id); stopped.kind == UninstallAction::Kind::kStop)
        StopUninstall(id, stopped.reason);
      if (sync_before_play_.erase(id)) {
        play_notes_[id] = "The game's PS5 title may still be running. Close it, then press Play.";
        ++play_notes_revision_;
      }
    }
    cloud_held_ = std::move(waiting);
  }
  for (const auto& request : confirmed) {
    after_play_.erase(request.game_id);
    const auto location = CloudLocationFor(request.game_id, false);
    if (!location) {
      // Nothing is there to sync: the request is answered as such.
      if (cloud_foreground_.erase(request.game_id)) model_.NotifyCloudAnswered(request.game_id, CloudStatus::Unsupported);
      // A pre-uninstall check that cannot run goes straight on: nothing is there to sync.
      uninstalls_.CloudSkipped(request.game_id);
      // A pre-play check that cannot run must not leave Play waiting.
      if (sync_before_play_.erase(request.game_id)) {
        play_notes_.erase(request.game_id);
        ++play_notes_revision_;
        plays_ready_.push_back(request.game_id);
      }
      continue;
    }
    // Busy from now on, before Cloud reports it, so Play cannot slip in.
    const bool busy_before = cloud_busy_.count(request.game_id) != 0;
    cloud_busy_.insert(request.game_id);
    if (request.kind == CloudRequestKind::Sync) uninstalls_.SyncDispatched(request.game_id);
    std::uint64_t serial = 0;
    switch (request.kind) {
      case CloudRequestKind::Sync: serial = cloud_->Sync(*location); break;
      case CloudRequestKind::UseLocal: serial = cloud_->Resolve(*location, steam::CloudResolution::kUseLocal, true); break;
      case CloudRequestKind::UseCloud: serial = cloud_->Resolve(*location, steam::CloudResolution::kUseCloud, true); break;
      case CloudRequestKind::ConfirmAccount: serial = cloud_->Resolve(*location, steam::CloudResolution::kAutomatic, true); break;
      case CloudRequestKind::RemoveLocal: serial = cloud_->RemoveLocal(*location); break;
      default: break;
    }
    if (serial == 0) {
      // Cloud took nothing: no transfer runs for the game, and an uninstall waiting on it stops.
      if (!busy_before) cloud_busy_.erase(request.game_id);
      if (request.kind == CloudRequestKind::Sync)
        if (const auto stopped = uninstalls_.SyncRefused(request.game_id); stopped.kind == UninstallAction::Kind::kStop)
          StopUninstall(request.game_id, stopped.reason);
    }
    // The answer is the game's first settled report after this serial.
    if (const auto foreground = cloud_foreground_.find(request.game_id); foreground != cloud_foreground_.end()) {
      if (serial == 0) {
        cloud_foreground_.erase(foreground);
        model_.NotifyCloudRefused(request.game_id);
      } else {
        foreground->second = serial;
      }
    }
  }
  // A job asks the console about every game first; games whose title may run wait.
  if (pending_job_) {
    auto& job = *pending_job_;
    bool answered = true;
    if (runtime_) {
      const auto runtime_shared = runtime_->Shared();
      const auto& runtime = *runtime_shared;
      if (runtime.status == RuntimeHostStatus::kStarting) {
        answered = false;
      } else if (!job.asked) {
        job.asked = true;
        job.after = runtime.activity_serial;
        // The console answers for every game in turn, and right after start-up that takes longer
        // than 8 s. A game left unanswered is synced on its own afterwards, two Steam logons each,
        // so wait long enough for the answers instead (#51).
        job.deadline = now + kJobActivityWait;
        for (const auto& id : job.ids)
          WithApp(id, [this](std::uint32_t app) { runtime_->CheckActivity(app); });
        answered = false;
      } else {
        for (const auto id : job.ids) {
          const auto answer = runtime.activity.find(NumberIn(runtime, id));
          if (answer == runtime.activity.end() || answer->second.second <= job.after) answered = false;
        }
      }
      if (!answered && now >= job.deadline && job.asked) answered = true;  // unanswered ones count as running
      if (answered) {
        std::vector<GameId> stopped;
        for (const auto id : job.ids) {
          const auto answer = runtime.activity.find(NumberIn(runtime, id));
          const bool known = answer != runtime.activity.end() && answer->second.second > job.after;
          if (known && answer->second.first == runtime::NativeActivity::kStopped && !SessionActive(id)) stopped.push_back(id);
          else after_play_.insert(id);
        }
        job.ids = std::move(stopped);
      }
    }
    if (answered) {
      std::vector<steam::CloudLocation> locations;
      // With automatic sync off, the check when Saves opens only looks: saves move when the player
      // chooses Sync or Sync all.
      const bool look_only = job.kind == steam::CloudJobKind::kQuickCheck && !model_.state().cloud_saves_auto;
      for (const auto id : job.ids)
        if (auto location = CloudLocationFor(id, true)) {
          if (look_only) location->check_only = true;
          locations.push_back(*location);
        }
      cloud_->StartJob(job.kind, std::move(locations));
      pending_job_.reset();
    }
  }
  const auto snapshot = cloud_->Snapshot();
  LiveSavesJob job{snapshot.job.kind == steam::CloudJobKind::kSyncAll     ? SavesJobKind::SyncAll
                   : snapshot.job.kind == steam::CloudJobKind::kQuickCheck ? SavesJobKind::QuickCheck
                                                                           : SavesJobKind::None,
                   snapshot.job.done, snapshot.job.total, snapshot.job.current, snapshot.job.stopping};
  if (job.kind != saves_job_.kind || job.done != saves_job_.done || job.total != saves_job_.total ||
      job.current != saves_job_.current || job.stopping != saves_job_.stopping) {
    saves_job_ = job;
    model_.UpdateSavesJob(job);
  }
  if (runtime_) {
    const auto sizes_shared = runtime_->Shared();
    std::map<GameId, std::uint64_t> projected;
    for (const auto& [app, bytes] : sizes_shared->save_bytes) projected.emplace(GameIn(*sizes_shared, app), bytes);
    if (projected != save_sizes_) {
      save_sizes_ = projected;
      model_.UpdateSaveSizes(std::move(projected));
    }
  }
  if (snapshot.revision == cloud_revision_ && after_play_ == after_play_projected_) return;
  cloud_revision_ = snapshot.revision;
  after_play_projected_ = after_play_;
  // Settle Play requests whose Cloud check has finished. Sync marks a game as
  // checking at once, so a finished state seen here is the check's own result.
  const auto settle_uninstall = [this](GameId id, CloudStatus status) {
    const bool waiting = uninstalls_.WaitingForCloud(id);
    const auto action = uninstalls_.CloudSettled(id, status);
    if (action.kind == UninstallAction::Kind::kStop) StopUninstall(id, action.reason);
    else if (waiting && !uninstalls_.WaitingForCloud(id)) ++play_notes_revision_;
  };
  const auto settle = [this, &settle_uninstall](GameId id, CloudStatus status) {
    settle_uninstall(id, status);
    // A choice made after Play stopped for it: the note and the second-Play escape no longer apply.
    if ((status == CloudStatus::Synced || status == CloudStatus::UpToDate) && play_without_sync_.erase(id)) {
      play_notes_.erase(id);
      ++play_notes_revision_;
    }
    const auto pending = sync_before_play_.find(id);
    if (pending == sync_before_play_.end() || !pending->second) return;
    if (status == CloudStatus::Checking || status == CloudStatus::Syncing) return;
    sync_before_play_.erase(pending);
    ++play_notes_revision_;
    switch (status) {
      case CloudStatus::Synced:
      case CloudStatus::UpToDate:
      case CloudStatus::Unsupported:
        play_notes_.erase(id);
        plays_ready_.push_back(id);
        break;
      case CloudStatus::Conflict:
      case CloudStatus::NeedsAccount:
        // Like Steam's "Play anyway": a second Play starts on this console's saves and changes nothing in the Cloud.
        play_notes_[id] = "Choose which Cloud saves to keep, or press Play again to start without syncing.";
        play_without_sync_.insert(id);
        break;
      default:
        play_notes_[id] = "Cloud saves could not be checked. Press Play again to start without syncing.";
        play_without_sync_.insert(id);
        break;
    }
  };
  std::vector<LiveCloudGame> games;
  std::vector<std::pair<GameId, CloudStatus>> answered;
  for (const auto& game : snapshot.games) {
    CloudStatus status = CloudStatus::None;
    switch (game.report.state) {
      case steam::CloudState::kIdle: status = CloudStatus::None; break;
      case steam::CloudState::kChecking: status = CloudStatus::Checking; break;
      case steam::CloudState::kSyncing: status = CloudStatus::Syncing; break;
      case steam::CloudState::kSynced: status = CloudStatus::Synced; break;
      case steam::CloudState::kUpToDate: status = CloudStatus::UpToDate; break;
      case steam::CloudState::kConflict: status = CloudStatus::Conflict; break;
      case steam::CloudState::kNeedsAccount: status = CloudStatus::NeedsAccount; break;
      case steam::CloudState::kUnsupported: status = CloudStatus::Unsupported; break;
      case steam::CloudState::kFailed: status = CloudStatus::Failed; break;
      case steam::CloudState::kWaiting: status = CloudStatus::Waiting; break;
    }
    const auto& report = game.report;
    if (status == CloudStatus::Checking || status == CloudStatus::Syncing) cloud_busy_.insert(game.id);
    else cloud_busy_.erase(game.id);
    cloud_status_[game.id] = status;
    settle(game.id, status);
    LiveCloudGame live{game.id, status, report.message, report.local_files, report.remote_files, report.local_bytes,
                       report.remote_bytes, static_cast<std::int64_t>(report.local_latest),
                       static_cast<std::int64_t>(report.remote_latest)};
    live.first_sync = report.first_sync;
    live.first_overwrite = report.first_overwrite;
    live.last_sync = report.last_sync;
    live.queued = report.queued;
    live.moved_bytes = report.moved_bytes;
    live.total_bytes = report.total_bytes;
    live.downloads = report.downloads;
    live.uploads = report.uploads;
    live.after_play = after_play_.count(game.id) > 0;
    // A result the player asked for may open a dialog; one from a background check never does.
    const auto foreground = cloud_foreground_.find(game.id);
    live.background = foreground == cloud_foreground_.end();
    if (foreground != cloud_foreground_.end() && foreground->second != kNotDispatched && report.serial > foreground->second &&
        status != CloudStatus::Checking && status != CloudStatus::Syncing && status != CloudStatus::None) {
      cloud_foreground_.erase(foreground);
      answered.emplace_back(game.id, status);
    }
    games.push_back(std::move(live));
  }
  model_.UpdateCloud(games);
  for (const auto& [id, status] : answered) model_.NotifyCloudAnswered(id, status);
}
// Per-game runtime: queue Prepare with the Steam install's location, queue Play,
// and project the runtime host's state for installed games. The worker checks a
// Play request; this thread, the application thread, then starts the title.
// Runs `use` with the game's runtime number. The main thread never waits for the runtime worker: a
// first number that would is asked for again next tick (a game the Local store added or reconciled
// already has one). Nothing runs for a game the runtime has no number for.
void Controller::WithApp(const GameId& id, std::function<void(std::uint32_t)> use) {
  if (!runtime_) return;
  bool busy = false;
  const auto app = RuntimeApp(*runtime_, id, &busy);
  if (busy) { deferred_runtime_.push_back([this, id, use = std::move(use)] { WithApp(id, use); }); return; }
  if (app) use(*app);
}

void Controller::TickRuntime() {
  if (!runtime_) return;
  // Calls that waited for the runtime worker (a game's first number) are asked again, in order.
  for (auto& again : std::exchange(deferred_runtime_, {})) again();
  {
    // The name each installed game's PS5 title should carry.
    // Keyed by runtime number; a game with none yet has no title to name.
    std::map<std::uint32_t, std::string> names;
    const auto numbers_shared = runtime_->Shared();
    for (const auto& [id, root] : game_roots_)
      for (const auto& game : model_.state().games)
        if (const auto number = NumberIn(*numbers_shared, id); game.id == id && !game.title.empty() && number)
          names[number] = model_.state().title_store_suffix ? game.title + TitleStoreSuffix(id.store) : game.title;
    runtime_->SetTitleNames(std::move(names));
  }
  // A quit to Home waits for the running preparation; the queued ones would be cut off.
  runtime_->HoldPreparations(model_.state().quit_waiting);
  // A drop-folder game's files are opened before each Play and Prepare.
  std::vector<GameId> opened_plays, opened_prepares;
  std::vector<std::pair<GameId, bool>> opened_submits;
  ReleaseDropFolderOpens(opened_plays, opened_prepares, opened_submits);
  for (const auto id : model_.TakeRuntimePrepareRequests())
    if (!HoldForDropFolder(id, DropFolderNext::kPrepare)) RequestPrepare(id);
  for (const auto id : opened_prepares) RequestPrepare(id);
  for (const auto& [id, announce] : opened_submits) {
    // Recreate bottle's wait for its preparation to start counts from now.
    if (const auto pending = recreate_.find(id); pending != recreate_.end())
      pending->second.submitted_at = std::chrono::steady_clock::now();
    SubmitPrepare(id, announce);
  }
  // The latest choice wins: a game Playden still holds back moves first once it is asked for.
  for (const auto id : model_.TakePrepareNextRequests()) {
    bool busy = false;
    const auto app = RuntimeApp(*runtime_, id, &busy);
    if (app && runtime_->PrepareNext(*app)) prepare_next_pending_.reset();
    else prepare_next_pending_ = id;
  }
  for (const auto& done : model_.TakePreparedDismissals())
    WithApp(done.game_id, [this, done](std::uint32_t app) { runtime_->DismissPrepared(app, done.finished_at); });
  if (model_.TakePreparedClear()) runtime_->ClearPrepared();
  if (model_.TakeRuntimeCleanupRequest()) runtime_->CleanUpRequests();
  for (const auto& id : model_.TakeKeptDeleteRequests())
    WithApp(id, [this](std::uint32_t app) { runtime_->DeleteKeptBottle(app); });
  for (const auto& id : model_.TakeLeftoverDeleteRequests()) {
    std::string error;
    if (!runtime_->DeleteLeftoverTitle(id, &error)) {
      auto deletions = leftover_deletions_projected_;
      std::erase_if(deletions, [&](const LiveLeftoverDelete& deletion) { return deletion.title_id == id; });
      deletions.push_back({id, leftover_names_[id], "", error.empty() ? "The title could not be queued for deletion." : error});
      model_.UpdateLeftoverDeletions(std::move(deletions));
    }
  }
  for (const auto& id : model_.TakeActivityRequests())
    WithApp(id, [this](std::uint32_t app) { runtime_->CheckActivity(app); });
  for (const auto id : model_.TakeProgramScanRequests())
    if (const auto root = game_roots_.find(id); root != game_roots_.end())
      WithApp(id, [this, path = root->second](std::uint32_t app) { runtime_->ScanPrograms(app, path); });
  for (const auto& [id, kind] : model_.TakeMaintenanceRequests())
    WithApp(id, [this, id = id, kind = kind](std::uint32_t app) {
      if (kind == MaintenanceKind::UpdateRuntime) {
        if (runtime_updates_.count(id) || recreate_.count(id)) return;
        runtime_updates_[id] = {runtime_->Shared()->prepares_handled, false, std::chrono::steady_clock::now()};
        runtime_update_results_.erase(id);
        recreate_results_.erase(id);
        ++play_notes_revision_;
        // As Play does for such a title: a drop-folder game's files are opened first.
        if (!HoldForDropFolder(id, DropFolderNext::kPrepare)) RequestPrepare(id);
        return;
      }
      if (kind == MaintenanceKind::RecreateBottle) {
        if (recreate_.count(id)) return;
        recreate_[id] = {runtime_->Shared()->removals_handled, false, false, {}, {}};
        recreate_results_.erase(id);
        runtime_update_results_.erase(id);
        runtime_->Remove(app, false);
        ++play_notes_revision_;
        return;
      }
      recreate_results_.erase(id);  // the sheet shows the newest request
      runtime_update_results_.erase(id);
      ++play_notes_revision_;
      runtime_->Maintain(app, kind == MaintenanceKind::ClearShaderCache ? RuntimeMaintenance::kClearShaderCache
                           : kind == MaintenanceKind::Repair ? RuntimeMaintenance::kRepair
                           : kind == MaintenanceKind::BackupSaves ? RuntimeMaintenance::kBackupSaves
                                                                  : RuntimeMaintenance::kApplySettings);
    });
  {
    const auto current_shared = runtime_->Shared();
    const auto& current = *current_shared;
    if (current.revision != kept_revision_) {
      kept_revision_ = current.revision;
      std::vector<LiveKeptBottle> kept;
      for (const auto& bottle : current.kept_bottles) kept.push_back({bottle.game, bottle.name, bottle.kept_at, bottle.bytes});
      if (kept != kept_projected_) { kept_projected_ = kept; model_.UpdateKeptBottles(std::move(kept)); }
      std::vector<LiveLeftoverTitle> leftovers;
      for (const auto& title : current.leftover_titles) {
        leftovers.push_back({title.title_id, title.name, title.bytes});
        leftover_names_[title.title_id] = title.name;
      }
      if (leftovers != leftovers_projected_) {
        leftovers_projected_ = leftovers;
        model_.UpdateLeftoverTitles(std::move(leftovers));
      }
      std::vector<LiveLeftoverDelete> deletions;
      if (current.deleting_leftover_title) {
        const auto& id = *current.deleting_leftover_title;
        deletions.push_back({id, leftover_names_[id], current.deleting_leftover_step, ""});
      }
      for (const auto& id : current.queued_leftover_deletes)
        if (current.deleting_leftover_title != id) deletions.push_back({id, leftover_names_[id], "", ""});
      for (const auto& [id, error] : current.leftover_delete_failures)
        deletions.push_back({id, leftover_names_[id], "", error});
      if (deletions != leftover_deletions_projected_) {
        leftover_deletions_projected_ = deletions;
        model_.UpdateLeftoverDeletions(std::move(deletions));
      }
    }
  }
  auto plays = model_.TakeRuntimePlayRequests();
  // A game whose update is putting its files in place doesn't start (the details page says so).
  if (installs_) std::erase_if(plays, [this](GameId id) { return installs_->Locked(id); });
  std::erase_if(plays, [this](GameId id) { return HoldForDropFolder(id, DropFolderNext::kPlay); });
  plays.insert(plays.end(), opened_plays.begin(), opened_plays.end());
  {
    const auto prepared_shared = runtime_->Shared();
    const auto& prepared = *prepared_shared;
    const bool usable = prepared.status == RuntimeHostStatus::kReady && prepared.kit_available;
    const auto record_of = [&prepared](GameId id) -> const RuntimeInstallView* {
      for (const auto& view : prepared.installs) if (view.game == id) return &view;
      return nullptr;
    };
    const auto ready = [](const RuntimeInstallView* view) {
      return view && view->state == runtime::PreparationState::kReady && !view->recovery_pending &&
             view->title_current && view->admission == "ok" && !view->files_changed;
    };
    const auto refusal_of = [&prepared](GameId id) {
      const auto found = prepared.refusals.find(NumberIn(prepared, id));
      return found == prepared.refusals.end() ? std::string{} : found->second;
    };
    // Play on a game that was never prepared prepares it first; on one whose title needs an
    // Update or a Repair, it updates or repairs it first. Both then start the game.
    const auto needs_work = [](const RuntimeInstallView* view) {
      return view && view->state == runtime::PreparationState::kReady && !view->recovery_pending &&
             (!view->title_current || view->admission != "ok" || view->files_changed);
    };
    // A record left before Ready by an earlier session, with no journal to resume, is what the
    // game page calls "Not prepared": Play prepares it too, instead of asking the launcher.
    const auto unprepared = [](const RuntimeInstallView* view) {
      using runtime::PreparationState;
      return !view || ((view->state == PreparationState::kDownloaded || view->state == PreparationState::kPreparing ||
                        view->state == PreparationState::kRegistering) &&
                       !view->recovery_pending && !view->awaiting_registration);
    };
    // A game installed again on another drive still has the title made for its old folder; Prepare
    // makes it again for the new one.
    const auto moved = [this](const RuntimeInstallView* view) {
      if (view && view->moving) return true;  // a move that stopped part way: Prepare finishes it
      if (!view || view->install_path.empty() || view->recovery_pending) return false;
      const auto root = game_roots_.find(view->game);
      return root != game_roots_.end() && root->second != view->install_path;
    };
    std::vector<GameId> direct;
    for (const auto id : plays) {
      if (usable && game_roots_.count(id) &&
          (unprepared(record_of(id)) || needs_work(record_of(id)) || moved(record_of(id))) &&
          !play_after_prepare_.count(id)) {
        auto& pending = play_after_prepare_[id];
        if (const auto* view = record_of(id); moved(view))
          pending.words = "Preparing the game on its new drive…";
        else if (needs_work(view))
          pending.words = !view->title_current ? "Updating runtime…"
                          : view->admission != "ok" ? "Repairing runtime…"
                                                    : "Setting up the updated game…";
        std::erase(auto_prepare_, id);
        ++play_notes_revision_;
      } else if (!play_after_prepare_.count(id)) {
        direct.push_back(id);
      }
    }
    plays = std::move(direct);
    // Finished downloads join the runtime's preparation queue in the order they finished, never over a
    // game that already has a record. The runtime prepares them one at a time.
    if (prepared.status != RuntimeHostStatus::kStarting)
      for (auto it = auto_prepare_.begin(); it != auto_prepare_.end();) {
        if (!usable || !game_roots_.count(*it) || (record_of(*it) && !moved(record_of(*it)))) {
          it = auto_prepare_.erase(it);
          continue;
        }
        if (cloud_busy_.count(*it)) { ++it; continue; }  // a move waits for saves in transit
        play_after_prepare_.erase(*it);
        if (!HoldForDropFolder(*it, DropFolderNext::kPrepare)) RequestPrepare(*it);  // a drop-folder game's files open first
        it = auto_prepare_.erase(it);
      }
    // An updated game is set up for its new files as soon as nothing uses it, so it also starts the new version from
    // Home; Play does it only when this couldn't. Asked once per update and per Playden start (a refusal waits for
    // Play), and signed in, after Steam's details for the new content (TickAppDetails asks for them again).
    if (usable)
      for (const auto& view : prepared.installs) {
        const auto id = view.game;
        if (!view.files_changed || view.recovery_pending || view.state != runtime::PreparationState::kReady ||
            !game_roots_.count(id) || refresh_asked_.count(id) || play_after_prepare_.count(id) ||
            prepare_after_details_.count(id) || PrepareQueued(prepared, NumberIn(prepared, id)) ||
            IsNumber(prepared, prepared.preparing_app, id) || cloud_busy_.count(id) || SessionActive(id) ||
            (installs_ && installs_->Locked(id)) ||
            (refresh_details_.count(id) && model_.state().account().authentication == AuthState::SignedIn))
          continue;
        refresh_asked_.insert(id);
        RequestPrepare(id);
      }
    // A title still waiting for the PS5 when Playden started is checked once: the
    // player left Playden since, so ShadowMountPlus has usually registered it. The
    // check only asks the console.
    if (usable && !startup_awaiting_known_) {
      startup_awaiting_known_ = true;
      for (const auto& view : prepared.installs)
        if (view.awaiting_registration) startup_awaiting_.insert(view.game);
    }
    for (auto it = startup_awaiting_.begin(); it != startup_awaiting_.end();) {
      if (prepared.preparing_app) break;
      if (!game_roots_.count(*it)) { ++it; continue; }
      const auto* record = record_of(*it);
      if (record && record->awaiting_registration && !HoldForDropFolder(*it, DropFolderNext::kRegistrationCheck))
        SubmitPrepare(*it, false);
      it = startup_awaiting_.erase(it);
      break;  // one at a time, after the worker takes it
    }
    // A preparation the player started that ends waiting for the PS5: the game is
    // ready to play once Playden quits to Home, so the model offers that.
    for (auto it = ready_watch_.begin(); it != ready_watch_.end();) {
      const auto id = it->first;
      if (IsNumber(prepared, prepared.preparing_app, id)) { it->second.seen_preparing = true; ++it; continue; }
      // A preparation still queued in the runtime is not lost, however long it waits.
      const bool unseen_too_long = !it->second.seen_preparing && !PrepareQueued(prepared, NumberIn(prepared, id)) &&
          !prepare_after_details_.count(id) &&
          std::chrono::steady_clock::now() - it->second.submitted_at > std::chrono::seconds(15);
      if (!it->second.seen_preparing && !unseen_too_long) { ++it; continue; }
      const auto* record = record_of(id);
      // Announced once the model has the game's new state (after the projection below).
      if (it->second.seen_preparing && record && record->awaiting_registration) {
        ready_notices_.push_back(id);
        // The bottle is complete now and the game will first start from Home, maybe with Playden
        // closed: bring its Cloud saves in while Playden is still open (#50). With automatic sync
        // off, or for a store without Cloud saves, this does nothing (TickCloud drops it).
        if (CloudCapable(id)) sync_after_session_.insert(id);
      }
      it = ready_watch_.erase(it);
    }
    // Recreate bottle: once the title is removed, prepare the game again; it ends when that
    // preparation does (Ready, or waiting for the PS5 to register the new title).
    for (auto it = recreate_.begin(); it != recreate_.end();) {
      const auto id = it->first;
      auto& pending = it->second;
      const auto finish = [&](bool ok, std::string message) {
        recreate_results_[id] = {ok, std::move(message)};
        ++play_notes_revision_;
        it = recreate_.erase(it);
      };
      if (!pending.prepare_submitted) {
        if (prepared.removals_handled <= pending.removals_before || IsNumber(prepared, prepared.removing_app, id)) { ++it; continue; }
        if (const auto failure = prepared.removal_failures.find(NumberIn(prepared, id)); failure != prepared.removal_failures.end()) {
          finish(false, failure->second);
          continue;
        }
        if (record_of(id)) { finish(false, "The PS5 title could not be removed."); continue; }
        pending.prepare_submitted = true;
        pending.refusal = refusal_of(id);
        pending.submitted_at = std::chrono::steady_clock::now();
        if (!HoldForDropFolder(id, DropFolderNext::kRecreate)) {
          SubmitPrepare(id);
        } else if (!drop_folder_opens_.count(id)) {
          finish(false, "Playden is busy. Try again in a moment.");
          continue;
        }
        ++it;
        continue;
      }
      // Its drop folder's files are still being opened (that has a timeout of its own).
      if (drop_folder_opens_.count(id)) { ++it; continue; }
      if (IsNumber(prepared, prepared.preparing_app, id)) { pending.seen_preparing = true; ++it; continue; }
      const bool unseen_too_long = !pending.seen_preparing && !PrepareQueued(prepared, NumberIn(prepared, id)) &&
          std::chrono::steady_clock::now() - pending.submitted_at > std::chrono::seconds(15);
      if (!pending.seen_preparing && !unseen_too_long) { ++it; continue; }
      const auto* record = record_of(id);
      if (record && (ready(record) || record->awaiting_registration))
        finish(true, record->awaiting_registration ? "Bottle recreated. The PS5 adds the game to Home when Playden quits."
                                                   : "Bottle recreated with the saves and settings brought back.");
      else
        finish(false, refusal_of(id).empty() ? "The game could not be prepared again; press Prepare on the game page."
                                              : refusal_of(id));
    }
    // Update runtime: done when its preparation ends, with the title on the kit's runtime or with the refusal.
    for (auto it = runtime_updates_.begin(); it != runtime_updates_.end();) {
      const auto id = it->first;
      auto& pending = it->second;
      if (drop_folder_opens_.count(id)) { ++it; continue; }  // its files are still being opened
      if (IsNumber(prepared, prepared.preparing_app, id)) { pending.seen_preparing = true; ++it; continue; }
      const bool queued = PrepareQueued(prepared, NumberIn(prepared, id)) || prepare_after_details_.count(id);
      const bool handled = !queued && prepared.prepares_handled > pending.prepares_handled;
      const bool unseen_too_long = !pending.seen_preparing && !queued && !prepared.preparing_app &&
          std::chrono::steady_clock::now() - pending.submitted_at > std::chrono::seconds(15);
      if (!pending.seen_preparing && !handled && !unseen_too_long) { ++it; continue; }
      const auto* record = record_of(id);
      const auto refusal = refusal_of(id);
      runtime_update_results_[id] = ready(record) ? std::pair<bool, std::string>{true, "Runtime updated."}
          : std::pair<bool, std::string>{false, refusal.empty() ? "The runtime could not be updated." : refusal};
      ++play_notes_revision_;
      it = runtime_updates_.erase(it);
    }
    for (auto it = play_after_prepare_.begin(); it != play_after_prepare_.end();) {
      const auto id = it->first;
      auto& pending = it->second;
      if (!pending.queued) {
        // A move removes the old title and bottle: saves still in transit finish first.
        if (moved(record_of(id)) && cloud_busy_.count(id)) { ++it; continue; }
        pending.queued = true;
        pending.refusal = refusal_of(id);
        pending.prepares_handled = prepared.prepares_handled;
        pending.queued_at = std::chrono::steady_clock::now();
        RequestPrepare(id);
        ++it;
        continue;
      }
      if (IsNumber(prepared, prepared.preparing_app, id)) { pending.seen_preparing = true; ++it; continue; }
      // A title still made for the game's old folder waits for the preparation that moves it.
      if (ready(record_of(id)) && !moved(record_of(id))) {
        plays.push_back(id);
        it = play_after_prepare_.erase(it);
        ++play_notes_revision_;
        continue;
      }
      // Prepare ended without a Ready title, or was refused before it was seen starting:
      // Play says why, so a press never ends with nothing on screen.
      // Another game's preparation finishing says nothing about this one while it is still queued.
      const bool queued = PrepareQueued(prepared, NumberIn(prepared, id)) || prepare_after_details_.count(id);
      const bool handled = !queued && prepared.prepares_handled > pending.prepares_handled && !IsNumber(prepared, prepared.preparing_app, id);
      const bool unseen_too_long = !pending.seen_preparing && !queued && !prepared.preparing_app &&
          std::chrono::steady_clock::now() - pending.queued_at > std::chrono::seconds(15);
      if (pending.seen_preparing || handled || refusal_of(id) != pending.refusal || !game_roots_.count(id) ||
          unseen_too_long) {
        if (game_roots_.count(id) && !record_of(id)) {
          const auto refusal = refusal_of(id);
          model_.NotifyPlayStopped(id, refusal.empty() ? "The game could not be prepared." : refusal);
        } else if (const auto* record = record_of(id); record && !record->awaiting_registration) {
          model_.NotifyPlayStopped(id, !refusal_of(id).empty() ? refusal_of(id)
                                       : !record->failure.empty() ? record->failure
                                                                  : "The game could not be prepared.");
        }
        it = play_after_prepare_.erase(it);
        ++play_notes_revision_;
        continue;
      }
      ++it;
    }
  }
  for (const auto id : plays) {
    if (sync_before_play_.count(id)) continue;
    // Play checks Cloud saves first, as Steam does before a game starts; the
    // game starts once its saves are in step.
    const bool cloud_known = CloudCapable(id) && game_roots_.count(id) && !saves_root_.empty() && model_.state().cloud_saves_auto;
    if (cloud_known && !play_without_sync_.erase(id)) {
      sync_before_play_[id] = false;
      play_notes_[id] = "Checking Cloud saves before starting…";
      ++play_notes_revision_;
      continue;
    }
    play_notes_.erase(id);
    ++play_notes_revision_;
    plays_ready_.push_back(id);
  }
  std::vector<GameId> later;
  for (const auto id : plays_ready_) {
    // Never start a game while its saves are moving; it starts once they stop.
    if (cloud_busy_.count(id)) { later.push_back(id); continue; }
    bool busy = false;
    const auto app = RuntimeApp(*runtime_, id, &busy);
    if (busy) { later.push_back(id); continue; }  // the game's first number waits for the runtime worker
    if (!app) continue;
    // Marked as starting for the install worker's gate first, then the update lock checked: an update that
    // began putting its files in place since Play was pressed stops the game here, and one that asks the
    // gate after this sees the game starting and waits.
    {
      std::lock_guard<std::mutex> lock(gate_mutex_);
      gate_blocked_.insert(id);
    }
    if (installs_ && installs_->Locked(id)) {
      model_.NotifyPlayStopped(id, "The game's update is putting its files in place. Play once it finishes.");
      continue;
    }
    play_queued_.insert(id);
    ++plays_requested_;
    runtime_->Play(*app);
  }
  plays_ready_ = std::move(later);
  {
    const auto current_shared = runtime_->Shared();
    const auto& current = *current_shared;
    if (current.handoff_ready && current.launch && !cloud_busy_.count(current.launch->game))
      runtime_->Handoff();
  }
  const auto snapshot_shared = runtime_->Shared();
  const auto& snapshot = *snapshot_shared;
  // Queued Plays are settled once the worker has handled every request; their
  // sessions or refusals are then visible.
  if (snapshot.plays_handled >= plays_requested_) play_queued_.clear();
  if (snapshot.launch) {
    const std::pair<GameId, runtime::LaunchState> current{snapshot.launch->game, snapshot.launch->state};
    if (current.second == runtime::LaunchState::kExited && last_launch_ != current && CloudCapable(current.first))
      sync_after_session_.insert(current.first);
    // An update waiting for the game to close tries again now rather than after its wait.
    if (current.second == runtime::LaunchState::kExited && last_launch_ != current && installs_)
      installs_->RetryWaiting(current.first.store);
    last_launch_ = current;
  }
  const auto announce_ready = [this] {
    for (const auto id : ready_notices_) model_.NotifyReadyToPlay(id);
    ready_notices_.clear();
  };
  {
    // Queue's Preparation section: the runtime's queue in order, then games Playden holds back before
    // asking for them (Steam's launch options, saves in transit, a Play waiting to ask).
    PreparationQueue queue;
    if (snapshot.preparing_app) queue.active = GameIn(snapshot, *snapshot.preparing_app);
    for (const auto app : snapshot.queued_prepares) queue.waiting.push_back(GameIn(snapshot, app));
    const auto add = [&queue](GameId id) {
      if (queue.active != id && std::find(queue.waiting.begin(), queue.waiting.end(), id) == queue.waiting.end())
        queue.waiting.push_back(id);
    };
    for (const auto id : prepare_after_details_) add(id);
    for (const auto id : auto_prepare_) if (game_roots_.count(id)) add(id);
    for (const auto& [id, pending] : play_after_prepare_) if (!pending.queued) add(id);
    if (prepare_next_pending_)
      if (const auto found = std::find(queue.waiting.begin(), queue.waiting.end(), *prepare_next_pending_);
          found != queue.waiting.end())
        std::rotate(queue.waiting.begin(), found, found + 1);
    for (const auto& done : snapshot.prepared)
      if (game_roots_.count(GameIn(snapshot, done.runtime_number))) queue.completed.push_back({GameIn(snapshot, done.runtime_number), done.finished_at});
    for (const auto& [app, refusal] : snapshot.refusals)
      if (const auto id = GameIn(snapshot, app); game_roots_.count(id) && queue.active != id &&
          std::find(queue.waiting.begin(), queue.waiting.end(), id) == queue.waiting.end())
        queue.refused[id] = refusal;
    // Uninstalls in the order they run; only those at work say their step.
    for (const auto id : uninstalls_.Active())
      queue.uninstalls.push_back({id, uninstalls_.Started(id, files_deleting_) ? UninstallStatus(id) : std::string()});
    model_.UpdatePreparationQueue(std::move(queue));
  }
  if (snapshot.revision == runtime_revision_ && install_revision_ == runtime_install_revision_ &&
      locations_revision_ == runtime_locations_revision_ &&
      cloud_revision_ == runtime_cloud_revision_ && play_notes_revision_ == runtime_notes_revision_) {
    announce_ready();
    return;
  }
  runtime_notes_revision_ = play_notes_revision_;
  runtime_revision_ = snapshot.revision;
  runtime_install_revision_ = install_revision_;
  runtime_locations_revision_ = locations_revision_;
  runtime_cloud_revision_ = cloud_revision_;
  const bool usable = snapshot.status == RuntimeHostStatus::kReady && snapshot.kit_available;
  const auto state = snapshot.status == RuntimeHostStatus::kStarting ? RuntimeState::Checking
                   : snapshot.status == RuntimeHostStatus::kUnavailable ? RuntimeState::Failed
                   : usable ? RuntimeState::Ready : RuntimeState::Unavailable;
  model_.UpdateRuntimeProblem(snapshot.status == RuntimeHostStatus::kUnavailable
                                  ? (snapshot.reason.empty() ? std::string("unknown reason") : snapshot.reason)
                                  : std::string{});
  std::vector<LiveRuntimeGame> games;
  for (const auto& [id, root] : game_roots_) {
    LiveRuntimeGame game; game.game_id = id;
    // The Steam app ID its Steamworks emulator was given, for Game settings' "Steam app ID".
    if (const auto steam_app = snapshot.steamworks_apps.find(NumberIn(snapshot, id)); steam_app != snapshot.steamworks_apps.end()) {
      game.steam_app_id = steam_app->second.app_id;
      game.steam_app_from_game_file = steam_app->second.source == SteamworksAppView::Source::kGameFile;
    }
    const RuntimeInstallView* record = nullptr;
    for (const auto& view : snapshot.installs) if (view.game == id) record = &view;
    const auto refusal = snapshot.refusals.find(NumberIn(snapshot, id));
    game.has_title = record != nullptr;
    if (record) game.failure_code = record->failure_code;
    if (snapshot.status == RuntimeHostStatus::kStarting) {
      game.status = "Checking";
    } else if (snapshot.status != RuntimeHostStatus::kReady) {
      game.status = "Unavailable"; game.detail = snapshot.reason;
    } else if (IsNumber(snapshot, snapshot.removing_app, id)) {
      game.status = "Removing"; game.detail = "Removing the game's PS5 title";
    } else if (IsNumber(snapshot, snapshot.preparing_app, id) && record && record->moving) {
      // Prepare removes the title made for the game's old folder (installed again on another drive).
      game.status = "Preparing"; game.detail = "Removing the PS5 title made for the game's old drive";
    } else if (record && record->moving) {
      // That removal stopped part way: Play or Prepare finishes the move, never an uninstall of the new download.
      game.status = "Needs repair"; game.can_prepare = usable;
      game.detail = "Play finishes moving the PS5 title to the game's new drive.";
    } else if (record && record->state == runtime::PreparationState::kRemoving) {
      game.status = "Removal incomplete"; game.can_prepare = true;
      game.detail = record->failure.empty() ? "Retry finishes removing the PS5 title, then uninstalls the game."
                                            : record->failure;
    } else if (IsNumber(snapshot, snapshot.preparing_app, id)) {
      game.status = "Preparing"; game.detail = PreparationProgressWords(record != nullptr, record ? record->step : std::string{});
    } else if (record && record->state == runtime::PreparationState::kReady && !record->recovery_pending &&
               !record->title_current) {
      game.status = "Kit update available"; game.can_prepare = usable;
      game.detail = "Play updates the runtime kit first; the game and its saves stay as they are";
    } else if (record && record->state == runtime::PreparationState::kReady && !record->recovery_pending &&
               record->admission != "ok") {
      game.status = "Needs repair"; game.can_prepare = usable;
      game.detail = "Play repairs the runtime first (" + record->admission + ")";
    } else if (record && record->state == runtime::PreparationState::kReady && !record->recovery_pending) {
      game.status = "Ready"; game.detail = record->title_id; game.ready = true;
    } else if (record && record->awaiting_registration) {
      // Staged and submitted: ShadowMountPlus registers it as soon as Playden quits,
      // so the game is ready to play from Home. Checking only asks the console.
      game.status = "Waiting for the PS5"; game.awaiting_registration = true;
      game.detail = "Waiting for the PS5, which adds the game to Home when Playden quits";
      game.can_prepare = usable;
    } else if (record && record->recovery_pending &&
               (record->step == "register" || record->step == "await-native" || record->step == "query-native")) {
      // The registration was submitted but not confirmed. Checking only asks the
      // console; an uncertain registration is never submitted again.
      game.status = "Registration pending";
      game.detail = "The console has not confirmed " + record->title_id +
                    " yet. Check again only asks the console; Playden never registers a title twice.";
      game.can_prepare = usable;
    } else if (record && record->recovery_pending) {
      game.status = "Needs recovery";
      game.detail = record->failure.empty() ? "Resume continues the saved preparation." : record->failure;
      game.can_prepare = usable;
    } else if (record && (record->state == runtime::PreparationState::kFailed ||
                          record->state == runtime::PreparationState::kUnavailable)) {
      game.status = "Failed"; game.detail = record->failure; game.can_prepare = usable;
    } else {
      game.status = usable ? "Not prepared" : "No kit";
      game.detail = usable ? "Play prepares it: a private bottle and the game's own PS5 title" : snapshot.kit_reason;
      game.can_prepare = usable;
    }
    // Preparation and its recovery checks require the kit. Keep actions that
    // launch or remove an existing title, and route blocked setup to Settings.
    if (snapshot.status == RuntimeHostStatus::kReady && !usable &&
        (game.status == "Kit update available" || game.status == "Needs repair" ||
         game.status == "Needs recovery" || game.status == "Failed" || game.status == "Registration pending"))
      game.status = "No kit";
    if (refusal != snapshot.refusals.end() && !game.ready) game.detail = refusal->second;
    // A setup left over from an earlier kit: what to do, not the check that failed (or the refusal saying so).
    if (record && record->setup_other_kit && usable &&
        (game.status == "Needs recovery" || game.status == "Failed" || game.status == "Not prepared")) {
      game.setup_other_kit = true;
      game.detail = "Started with an earlier kit · Recreate the bottle to finish (saves are kept)";
    }
    if (const auto refused = snapshot.play_refusals.find(NumberIn(snapshot, id)); refused != snapshot.play_refusals.end())
      game.play_detail = "Could not start: " + refused->second;
    const bool launching = snapshot.launch && snapshot.launch->game == id;
    if (launching) {
      using runtime::LaunchState;
      switch (snapshot.launch->state) {
        case LaunchState::kWaitingForCloud: game.play_status = "Waiting for Cloud"; break;
        case LaunchState::kReadyToHandoff:
        case LaunchState::kLaunching: game.play_status = "Starting…"; break;
        case LaunchState::kRunning: game.play_status = "Playing"; break;
        case LaunchState::kExited: game.play_detail = "The last session ended."; break;
        case LaunchState::kFailed:
          game.play_detail = snapshot.launch->message.empty() ? "The game could not start."
                                                              : "Could not start: " + snapshot.launch->message;
          break;
        case LaunchState::kIdle: break;
      }
    }
    // What the latest Play went ahead without (Microsoft's MFC), unless something newer is said.
    if (const auto notice = snapshot.play_notices.find(NumberIn(snapshot, id));
        notice != snapshot.play_notices.end() && game.play_detail.empty())
      game.play_detail = notice->second;
    // One launch at a time: Play waits while any session is undecided.
    const bool busy = snapshot.launch && (snapshot.launch->state == runtime::LaunchState::kWaitingForCloud ||
                                          snapshot.launch->state == runtime::LaunchState::kReadyToHandoff ||
                                          snapshot.launch->state == runtime::LaunchState::kLaunching ||
                                          snapshot.launch->state == runtime::LaunchState::kRunning);
    // Updating or repairing waits while this game is being started or played, and a queued
    // preparation is not asked for twice.
    if ((launching && busy) || PrepareQueued(snapshot, NumberIn(snapshot, id))) game.can_prepare = false;
    // Queued behind another game's setup, from the runtime's queue or a Play that waits to ask it.
    const auto waiting_play = play_after_prepare_.find(id);
    game.queued = !IsNumber(snapshot, snapshot.preparing_app, id) &&
                  (PrepareQueued(snapshot, NumberIn(snapshot, id)) ||
                   (waiting_play != play_after_prepare_.end() && !waiting_play->second.queued));
    game.can_play = game.ready && !busy && !IsNumber(snapshot, snapshot.preparing_app, id) && !cloud_busy_.count(id) &&
                    play_queued_.empty() && !sync_before_play_.count(id);
    if (const auto note = play_notes_.find(id); note != play_notes_.end() && !launching) game.play_detail = note->second;
    if (sync_before_play_.count(id)) game.play_status = "Checking saves…";
    // Play's own work before the launch (a game moved in 0.6 is republished first).
    if (const auto progress = snapshot.play_progress.find(NumberIn(snapshot, id)); progress != snapshot.play_progress.end()) {
      game.play_status = progress->second; game.can_play = false; game.can_prepare = false;
    }
    if (const auto pending = play_after_prepare_.find(id); pending != play_after_prepare_.end()) {
      game.play_status = pending->second.words; game.can_prepare = false;
    }
    if (auto status = UninstallStatus(id); !status.empty()) {
      game.uninstall_status = std::move(status);
      game.can_play = false; game.can_prepare = false;
    }
    // A retry is in flight: do not republish its previous refusal as a new outcome.
    if (play_queued_.count(id) || sync_before_play_.count(id)) game.play_detail.clear();
    if (game.ready && cloud_busy_.count(id) && game.play_detail.empty()) game.play_detail = "Waiting for Cloud saves to finish.";
    if (snapshot.work_progress && (IsNumber(snapshot, snapshot.preparing_app, id) || IsNumber(snapshot, snapshot.removing_app, id)))
      game.progress = snapshot.work_progress;
    // Game settings and maintenance: the prepared title's state, the console's answer on
    // whether it runs, and the newest maintenance outcome.
    if (record) {
      game.prepared = record->state == runtime::PreparationState::kReady && !record->recovery_pending &&
                      snapshot.status == RuntimeHostStatus::kReady;
      game.settings_revision = record->settings_revision;
      game.launch_entry_id = record->launch_entry_id;
      game.runtime_current = record->runtime_current;
      game.save_backups = record->save_backups;
      game.latest_save_backup = record->latest_save_backup;
    }
    if (const auto answer = snapshot.activity.find(NumberIn(snapshot, id)); answer != snapshot.activity.end())
      game.activity = answer->second.first == runtime::NativeActivity::kStopped ? TitleActivity::Stopped
                    : answer->second.first == runtime::NativeActivity::kActive ? TitleActivity::Running
                                                                              : TitleActivity::Unknown;
    if (launching && busy) game.activity = TitleActivity::Running;
    if (const auto found = snapshot.maintenance.find(NumberIn(snapshot, id)); found != snapshot.maintenance.end()) {
      const auto action = found->second.action;
      game.maintenance = action == RuntimeMaintenance::kClearShaderCache ? MaintenanceKind::ClearShaderCache
                       : action == RuntimeMaintenance::kRepair ? MaintenanceKind::Repair
                       : action == RuntimeMaintenance::kBackupSaves ? MaintenanceKind::BackupSaves
                                                                    : MaintenanceKind::ApplySettings;
      game.maintenance_running = found->second.running;
      game.maintenance_ok = found->second.ok;
      game.maintenance_message = found->second.message;
    }
    if (recreate_.count(id)) {
      game.maintenance = MaintenanceKind::RecreateBottle;
      game.maintenance_running = true;
      game.maintenance_message.clear();
    } else if (const auto result = recreate_results_.find(id); result != recreate_results_.end() &&
               (!game.maintenance || !game.maintenance_running)) {
      game.maintenance = MaintenanceKind::RecreateBottle;
      game.maintenance_running = false;
      game.maintenance_ok = result->second.first;
      game.maintenance_message = result->second.second;
    }
    if (runtime_updates_.count(id)) {
      game.maintenance = MaintenanceKind::UpdateRuntime;
      game.maintenance_running = true;
      game.maintenance_message.clear();
    } else if (const auto result = runtime_update_results_.find(id); result != runtime_update_results_.end() &&
               (!game.maintenance || !game.maintenance_running)) {
      game.maintenance = MaintenanceKind::UpdateRuntime;
      game.maintenance_running = false;
      game.maintenance_ok = result->second.first;
      game.maintenance_message = result->second.second;
    }
    games.push_back(std::move(game));
  }
  model_.SetGeneratedRecipes(snapshot.generated_recipes);
  {
    std::map<GameId, LiveGameSetup> setups;
    for (const auto& [app, setup] : snapshot.game_setups) {
      auto& live = setups[GameIn(snapshot, app)];
      live.programs = setup.programs;
      live.scanned = setup.scanned;
      live.draft = setup.draft;
      live.refusal_soft = setup.refusal_soft;
      for (const auto& choice : setup.steam_choices) live.steam_choices.push_back({choice.path, choice.helper});
      if (const auto refusal = snapshot.refusals.find(app); refusal != snapshot.refusals.end()) live.refusal = refusal->second;
    }
    model_.SetGameSetups(std::move(setups));
  }
  // Game settings rows follow what the verified kit can apply; no kit applies nothing gated.
  if (snapshot.status != RuntimeHostStatus::kStarting) {
    auto capabilities = snapshot.kit ? snapshot.kit->capabilities : std::vector<std::string>{};
    if (kit_capabilities_ != capabilities) {
      model_.SetKitCapabilities(runtime::KitCapabilitiesFromManifest(capabilities, true));
      kit_capabilities_ = std::move(capabilities);
    }
  }
  model_.UpdateRuntime(state, games);
  model_.SetRuntimeLeftovers(snapshot.leftover_requests);
  LiveKitInfo kit;
  if (snapshot.kit) {
    kit = {snapshot.kit->view.kit_id, snapshot.kit->manifest_sha256, snapshot.kit->expected_manifest_sha256,
           snapshot.kit->prospero_win_revision, snapshot.kit->source_dirty, {}, {}};
    // The artifact is "<wine revision>-kit-<version>"; kits before 0.10 name no version.
    const auto& artifact = snapshot.kit->view.runtime.artifact_revision;
    if (update::KitVersionFromArtifact(artifact)) kit.version = artifact.substr(artifact.find("-kit-") + 5);
  }
  else
    kit.unavailable_reason = snapshot.status == RuntimeHostStatus::kReady ? snapshot.kit_reason : std::string{};
  model_.SetKitInfo(std::move(kit));
  announce_ready();
}
void Controller::BeginUninstall(GameId id, bool delete_saves) {
  // Only Playden's own installs are uninstalled: a game played in place is never deleted. An imported
  // one leaves the library instead, once its PS5 title is gone (its files stay, its bottle is kept).
  const auto* location = LocationOf(id);
  const bool external = location && !location->managed();
  if (external && !(id.store == Store::kLocal && local_)) return;
  if (external) delete_saves = false;
  if (!runtime_ && external) { local_->Remove(id.value); return; }
  // Without the runtime there is no PS5 title to remove: the download goes as before.
  if (!runtime_) { if (installs_) installs_->Uninstall(id); return; }
  // A preparation still waiting would make a title for files about to be deleted: it leaves the queue.
  // One already running finishes first (TickUninstalls waits), so its title is removed with the rest.
  // A game with no number yet has nothing queued to withdraw (queuing gives it one).
  {
    bool busy = false;
    if (const auto app = RuntimeApp(*runtime_, id, &busy)) runtime_->WithdrawPrepare(*app);
  }
  std::erase(auto_prepare_, id);
  prepare_after_import_.erase(id);
  prepare_after_details_.erase(id);
  play_after_prepare_.erase(id);
  runtime_updates_.erase(id);
  if (prepare_next_pending_ == id) prepare_next_pending_.reset();
  // With automatic sync off, nothing moves to or from Steam Cloud unless the player syncs from Saves.
  if (uninstalls_.Begin(id, delete_saves, CloudCapable(id) && game_roots_.count(id) && !saves_root_.empty() &&
                                              model_.state().cloud_saves_auto))
    ++play_notes_revision_;
}
void Controller::StopUninstall(GameId id, const std::string& reason) {
  ++play_notes_revision_;
  model_.NotifyUninstallStopped(id, reason);
}
std::string Controller::UninstallStatus(GameId id) const {
  const auto step = uninstalls_.Step(id);
  if (!step) return {};
  // Only the uninstalls at work say what they do; the others wait their turn.
  if (!uninstalls_.Started(id, files_deleting_)) return "Waiting to uninstall";
  switch (*step) {
    case UninstallStep::kSync: return "Syncing saves…";
    case UninstallStep::kRemoveTitle: return "Removing PS5 title…";
    case UninstallStep::kRemoveDownload: break;
  }
  // An imported game played from its drive leaves the library; its files stay.
  const auto* location = LocationOf(id);
  return location && !location->managed() ? "Removing from Playden…" : "Deleting files…";
}
void Controller::TickUninstalls() {
  if (!runtime_) return;
  // A status that changed (a sync sent, a title removed) is projected again.
  const auto project = [this] {
    std::vector<std::pair<GameId, std::string>> statuses;
    for (const auto id : uninstalls_.Active()) statuses.emplace_back(id, UninstallStatus(id));
    if (statuses != uninstall_statuses_) {
      uninstall_statuses_ = std::move(statuses);
      ++play_notes_revision_;
    }
  };
  const auto ids = uninstalls_.Active();
  if (ids.empty()) return project();
  const auto snapshot_shared = runtime_->Shared();
  const auto& snapshot = *snapshot_shared;
  if (snapshot.status == RuntimeHostStatus::kStarting) return;
  // In the order they were asked for: the first that can go takes the one title removal at a time.
  for (const auto id : ids) {
    if (IsNumber(snapshot, snapshot.preparing_app, id)) continue;  // its title is removed once the preparation ends
    UninstallFacts facts;
    // Unknown records count as a title: the removal reads them again, and the game's files stay
    // while they can't be read.
    facts.has_title = !snapshot.installs_known ||
                      std::any_of(snapshot.installs.begin(), snapshot.installs.end(),
                                  [id](const RuntimeInstallView& view) { return view.game == id; });
    facts.removing = IsNumber(snapshot, snapshot.removing_app, id);
    facts.removal_started = std::any_of(snapshot.installs.begin(), snapshot.installs.end(), [id](const RuntimeInstallView& view) {
      return view.game == id && view.state == runtime::PreparationState::kRemoving;
    });
    facts.removals_handled = snapshot.removals_handled;
    if (const auto failure = snapshot.removal_failures.find(NumberIn(snapshot, id)); failure != snapshot.removal_failures.end())
      facts.removal_failure = failure->second;
    facts.saves_moving = cloud_busy_.count(id) != 0;
    facts.session_active = SessionActive(id);
    // The install service lists a game as installed no more once it starts deleting it: the
    // uninstall is over once the deletion is too. Without the service there is nothing to wait for.
    const auto* location = LocationOf(id);
    facts.download_gone = (!location && std::find(files_deleting_.begin(), files_deleting_.end(), id) == files_deleting_.end()) ||
                          (location && location->managed() && !installs_);
    // As the install service sees it: a game missing from a drive that is gone can't be deleted.
    facts.drive_absent = location && location->managed() && !location->available && !drives_present_.count(location->volume_id);
    const auto action = uninstalls_.Advance(id, facts);
    switch (action.kind) {
      case UninstallAction::Kind::kNone: break;
      case UninstallAction::Kind::kRemoveTitle:
        WithApp(id, [this, delete_saves = action.delete_saves](std::uint32_t app) { runtime_->Remove(app, delete_saves); });
        ++play_notes_revision_;
        break;
      // The title is gone, so no title points at the files about to be deleted.
      case UninstallAction::Kind::kRemoveDownload:
        if (location && !location->managed()) {
          // Its outcome is watched: a refusal stops the uninstall (TickImports).
          if (id.store == Store::kLocal && local_) {
            if (const auto number = local_->Remove(id.value)) {
              import_requests_[number] = {ImportRequestKind::Remove, id};
            } else {
              uninstalls_.Abandon(id);
              StopUninstall(id, "Playden could not take the game out of the library now. Uninstall again to try again.");
            }
          }
        } else if (installs_) {
          installs_->Uninstall(id);
        }
        ++play_notes_revision_;
        break;
      case UninstallAction::Kind::kStop: StopUninstall(id, action.reason); break;
      case UninstallAction::Kind::kFinished:
        model_.NotifyUninstallFinished(id);
        ++play_notes_revision_;
        break;
    }
  }
  project();
}
void Controller::ProjectLocations(const std::vector<steam::InstalledGame>& managed) {
  auto external = external_locations_ ? external_locations_() : std::vector<installs::InstalledLocation>{};
  if (local_)
    for (auto& location : local_->Locations()) external.push_back(std::move(location));
  auto locations = installs::ProjectInstalledLocations(managed, external);
  if (locations != locations_) {
    locations_ = std::move(locations);
    ++locations_revision_;
    // The model's details page offers Play for games played in place too.
    std::vector<LiveExternalGame> games;
    for (const auto& location : locations_)
      if (!location.managed()) games.push_back({location.game, location.volume_id, location.available, location.reason});
    if (games != external_projected_) {
      external_projected_ = games;
      model_.UpdateExternalGames(std::move(games));
    }
  }
  game_roots_.clear();
  game_volumes_.clear();
  for (const auto& location : locations_)
    if (location.available && location.reason.empty()) {
      game_roots_[location.game] = location.root;
      game_volumes_[location.game] = location.volume_id;
    }
}
void Controller::TickImports() {
  if (!local_) return;
  // The locations reflect a Local snapshot once one was seen on an earlier tick (TickInstalls projects them).
  const bool locations_known = local_revision_ != 0;
  const auto track = [this](std::uint64_t number, const ImportRequest& request) {
    if (number == 0) return;
    import_requests_[number] = request;
    // Outcomes are kept for the latest requests only: one never answered is dropped with the oldest.
    while (import_requests_.size() > stores::local::LocalLibrarySource::kMaxOutcomes * 2) import_requests_.erase(import_requests_.begin());
  };
  // A copy or move's install record carries the game's runtime number (given out now if it has
  // none); a game set up from its drive folder keeps its PS5 title, which follows the files. A
  // first number is written to the runtime records, which wait while the runtime worker starts up
  // or works through a batch: the request waits here instead, a tick at a time, and the frame
  // goes on.
  const auto transfer_number = [this](const ImportRequest& request, std::uint32_t* number) {
    *number = 0;
    if (!runtime_) return true;
    bool busy = false;
    *number = RuntimeApp(*runtime_, request.game_id, &busy).value_or(0u);
    if (busy) held_import_transfers_.push_back(request);
    return !busy;
  };
  auto requests = std::exchange(held_import_transfers_, {});
  for (auto& request : model_.TakeImportRequests()) requests.push_back(std::move(request));
  for (const auto& request : requests) {
    std::uint32_t number = 0;
    switch (request.kind) {
      case ImportRequestKind::LookForGames: local_->LookForGames(); break;
      case ImportRequestKind::Add: track(local_->Add(request.volume_id, request.folder, request.program), request); break;
      case ImportRequestKind::Remove:
        if (request.game_id.store == Store::kLocal) held_import_removals_.push_back(request.game_id);
        break;
      case ImportRequestKind::CopyIntoPlayden:
        if (request.game_id.store == Store::kLocal && transfer_number(request, &number))
          track(local_->CopyIntoPlayden(request.game_id.value, request.volume_id, number), request);
        break;
      case ImportRequestKind::MoveIntoPlayden:
        if (request.game_id.store == Store::kLocal && transfer_number(request, &number))
          track(local_->MoveIntoPlayden(request.game_id.value, number), request);
        break;
    }
  }
  if (locations_known)
    for (const auto& id : std::exchange(held_import_removals_, {})) {
      // A game with a folder to play from goes like an uninstall: its PS5 title first.
      if (LocationOf(id)) BeginUninstall(id, false);
      else {
        prepare_after_import_.erase(id);
        track(local_->Remove(id.value), {ImportRequestKind::Remove, id});
      }
    }
  // A drive plugged in or out: its games become (un)available and its folders are looked at.
  const auto now = std::chrono::steady_clock::now();
  if (now - local_volumes_checked_at_ >= std::chrono::seconds(5)) {
    local_->CheckVolumes();
    local_volumes_checked_at_ = now;
  }
  const auto snapshot = local_->Snapshot();
  // Requests the store answered: a refusal is said to the player, not only logged.
  for (auto it = import_requests_.begin(); it != import_requests_.end();) {
    const auto outcome = std::find_if(snapshot->outcomes.begin(), snapshot->outcomes.end(),
                                      [&](const stores::local::ImportOutcome& o) { return o.request == it->first; });
    if (outcome == snapshot->outcomes.end()) {
      // An uninstall's removal answered so long ago its outcome scrolled off the list: the game still
      // in the library means it was refused; one gone ends as usual once its location is gone.
      const bool lost = snapshot->outcomes.size() >= stores::local::LocalLibrarySource::kMaxOutcomes &&
                        it->first < snapshot->outcomes.front().request;
      const auto id = it->second.game_id;
      if (lost && it->second.kind == ImportRequestKind::Remove && uninstalls_.Step(id)) {
        if (std::any_of(snapshot->games.begin(), snapshot->games.end(),
                        [&id](const stores::local::ImportEntry& entry) { return GameId::Local(entry.id) == id; })) {
          uninstalls_.Abandon(id);
          StopUninstall(id, "Playden could not take the game out of the library. Uninstall again to try again.");
        }
        it = import_requests_.erase(it);
        continue;
      }
      ++it;
      continue;
    }
    if (!outcome->ok && it->second.kind == ImportRequestKind::Remove && uninstalls_.Step(it->second.game_id)) {
      // An uninstall's last step was refused: the game stays, and the uninstall says why.
      uninstalls_.Abandon(it->second.game_id);
      StopUninstall(it->second.game_id, outcome->message);
    } else if (!outcome->ok) {
      model_.NotifyImportFailed(it->second, outcome->message);
    } else if (it->second.kind == ImportRequestKind::Add && !outcome->id.empty()) {
      // A game just added: the program chosen with it goes into its Game settings.
      model_.SeedGameProgram(GameId::Local(outcome->id), it->second.program);
      if (it->second.prepare) prepare_after_import_[GameId::Local(outcome->id)] = false;
    } else if (it->second.kind == ImportRequestKind::MoveIntoPlayden) {
      prepare_after_import_[it->second.game_id] = true;
    }
    it = import_requests_.erase(it);
  }
  // Their folder listed, they join the preparation queue as a finished download does (which skips a
  // game already prepared there). One listed that can't play (its drive gone, a name the PS5 can't
  // open) is left for the player.
  for (auto it = prepare_after_import_.begin(); it != prepare_after_import_.end();) {
    const auto* location = LocationOf(it->first);
    if (!location || location->managed() != it->second) { ++it; continue; }
    if (game_roots_.count(it->first) && std::find(auto_prepare_.begin(), auto_prepare_.end(), it->first) == auto_prepare_.end())
      auto_prepare_.push_back(it->first);
    it = prepare_after_import_.erase(it);
  }
  // Once imports.json is read: copies that finished or stopped while Playden was closed are settled.
  // A move a restart interrupted is finished then too, once the runtime has started (its PS5 title
  // follows the folder).
  if (!copies_reconciled_ && snapshot->loaded && installs_ &&
      (!runtime_ || runtime_->Shared()->status != RuntimeHostStatus::kStarting)) {
    copies_reconciled_ = true;
    auto* installs = installs_;  // outlives the Local store's worker
    auto* runtime = runtime_;    // so does the runtime host
    local_->Reconcile([installs] { return installs->Snapshot(); }, [installs](const GameId& id) { installs->Cancel(id); },
                      [runtime](const GameId& id) { return runtime ? RuntimeAppWaiting(*runtime, id).value_or(0u) : 0u; });
    local_->SetRuntimeNumber([runtime](const GameId& id) { return runtime ? RuntimeAppWaiting(*runtime, id).value_or(0u) : 0u; });
    // A copy restored at start waited for imports.json: it carries on now.
    installs_->RetryWaiting(Store::kLocal);
  }
  if (snapshot->revision == local_revision_) return;
  local_revision_ = snapshot->revision;
  std::vector<LiveImportCandidate> candidates;
  for (const auto& candidate : snapshot->candidates) {
    LiveImportCandidate live{candidate.volume, candidate.folder, candidate.path, candidate.title, {},
                             candidate.total_bytes, candidate.ascii_ok, candidate.partial, {}, candidate.file_count};
    for (const auto& program : candidate.programs) live.programs.push_back({program.path, program.bytes, program.helper});
    if (!candidate.imported_id.empty()) live.imported = GameId::Local(candidate.imported_id);
    candidates.push_back(std::move(live));
  }
  model_.UpdateImportCandidates(std::move(candidates), snapshot->scanning);
  // Added or removed games: the Local library loads again (Steam's is left alone).
  if (snapshot->games_revision != local_games_revision_) {
    local_games_revision_ = snapshot->games_revision;
    if (library_) library_->Refresh(Store::kLocal);
  }
}
bool Controller::HoldForDropFolder(GameId id, DropFolderNext next) {
  if (id.store != Store::kLocal || !local_) return false;
  const auto* location = LocationOf(id);
  if (!location || location->managed() || !location->available ||
      location->volume_id != stores::local::ImportStore::kDropFolderVolume)
    return false;
  // The player asked: a stop is said, and the game page says what it waits for.
  const bool asked = next == DropFolderNext::kPlay || next == DropFolderNext::kPrepare;
  auto& open = drop_folder_opens_[id];
  if (!open.request) {
    open.request = local_->OpenForPlay(id.value);
    open.started = std::chrono::steady_clock::now();
  }
  if (!open.request) {
    // The Local store is busy: the caller stops (a Recreate bottle loop finishes its own entry).
    drop_folder_opens_.erase(id);
    if (asked) model_.NotifyPlayStopped(id, "Playden is busy. Press Play again in a moment.");
    return true;
  }
  switch (next) {
    case DropFolderNext::kPlay: open.play = true; break;
    case DropFolderNext::kPrepare: open.prepare = true; break;
    case DropFolderNext::kRecreate: open.recreate = true; break;
    case DropFolderNext::kRegistrationCheck: open.registration_check = true; break;
  }
  if (asked) {
    play_notes_[id] = "Opening the game's files…";
    ++play_notes_revision_;
  }
  return true;
}
void Controller::ReleaseDropFolderOpens(std::vector<GameId>& plays, std::vector<GameId>& prepares,
                                        std::vector<std::pair<GameId, bool>>& submits) {
  if (drop_folder_opens_.empty() || !local_) return;
  const auto snapshot = local_->Snapshot();
  const auto now = std::chrono::steady_clock::now();
  for (auto it = drop_folder_opens_.begin(); it != drop_folder_opens_.end();) {
    const auto outcome = std::find_if(snapshot->outcomes.begin(), snapshot->outcomes.end(),
                                      [&](const stores::local::ImportOutcome& o) { return o.request == it->second.request; });
    const bool timed_out = outcome == snapshot->outcomes.end() && now - it->second.started >= drop_folder_open_timeout_;
    if (outcome == snapshot->outcomes.end() && !timed_out) { ++it; continue; }
    const auto id = it->first;
    const auto& open = it->second;
    if (play_notes_.erase(id)) ++play_notes_revision_;
    if (timed_out || !outcome->ok) {
      const auto message = timed_out ? std::string("Playden couldn't open the game's folder in time.") : outcome->message;
      if (open.play || open.prepare) model_.NotifyPlayStopped(id, message);
      if (open.recreate) {
        recreate_results_[id] = {false, message};
        recreate_.erase(id);
        ++play_notes_revision_;
      }
      if (open.prepare && runtime_updates_.erase(id)) {
        runtime_update_results_[id] = {false, message};
        ++play_notes_revision_;
      }
    } else if (open.play) {
      plays.push_back(id);  // Play prepares a game that needs it on its own
      if (open.recreate) submits.emplace_back(id, true);
    } else if (open.prepare) {
      prepares.push_back(id);
      if (open.recreate) submits.emplace_back(id, true);
    } else if (open.recreate || open.registration_check) {
      submits.emplace_back(id, open.recreate);
    }
    it = drop_folder_opens_.erase(it);
  }
}
const installs::InstalledLocation* Controller::LocationOf(GameId id) const {
  for (const auto& location : locations_)
    if (location.game == id) return &location;
  return nullptr;
}
bool Controller::DownloadServerTestAllowed() const {
  if (model_.state().account(Store::kSteam).authentication != AuthState::SignedIn) return false;
  if (std::any_of(model_.state().jobs.begin(), model_.state().jobs.end(), [](const Job& job) {
        return job.state == JobState::Downloading || job.state == JobState::Pausing || job.state == JobState::Verifying;
      })) return false;
  // The install worker may have started since the presentation's last snapshot.
  if (installs_) {
    const auto snapshot = installs_->Snapshot();
    if (std::any_of(snapshot.jobs.begin(), snapshot.jobs.end(), [](const steam::InstallJobView& job) {
          return job.state == steam::InstallJobState::kRunning;
        })) return false;
  }
  return true;
}
void Controller::TickWebAccess() {
  if (!web_) return;
  // Not before the saved choice is known: a player who turned it off must not get an open port at each start.
  web_->SetEnabled(model_.state().web_access && (model_.PreferencesLoaded() || !persistence_));
  const auto snapshot = web_->Snapshot();
  LiveWebAccess live;
  switch (snapshot.state) {
    case web::ServiceSnapshot::State::kOff: live.state = "off"; break;
    case web::ServiceSnapshot::State::kStarting: live.state = "starting"; break;
    case web::ServiceSnapshot::State::kOn: live.state = "on"; break;
    case web::ServiceSnapshot::State::kFailed: live.state = "failed"; break;
  }
  live.url = snapshot.url;
  live.port = snapshot.port;
  live.error = snapshot.error;
  model_.ApplyLiveWebAccess(live);
  // The page's Quit Playden; it waits for a game being prepared.
  if (web_->TakeQuitRequest()) {
    if (shutdown_log_) shutdown_log_("quit requested from web access");
    model_.RequestQuit();
  }
}
void Controller::TickDownloadServers() {
  const auto requests = model_.TakeDownloadServerRequests();
  if (!download_servers_) return;
  const auto& account = model_.state().account(Store::kSteam);
  if (download_servers_account_ != account.id) {
    download_servers_->Invalidate();
    download_servers_account_ = account.id;
  }
  const bool allowed = DownloadServerTestAllowed();
  if (!allowed) download_servers_->CancelTest();
  for (const auto request : requests) {
    switch (request) {
      case DownloadServerRequest::List: download_servers_->List(); break;
      case DownloadServerRequest::Test:
        if (allowed) download_servers_->Test();
        break;
      case DownloadServerRequest::CancelTest: download_servers_->CancelTest(); break;
    }
  }
  const auto snapshot = download_servers_->Snapshot();
  LiveDownloadServers live;
  switch (snapshot.phase) {
    case steam::DownloadServerSnapshot::Phase::kIdle: live.phase = "idle"; break;
    case steam::DownloadServerSnapshot::Phase::kListing: live.phase = "listing"; break;
    case steam::DownloadServerSnapshot::Phase::kListed: live.phase = "listed"; break;
    case steam::DownloadServerSnapshot::Phase::kListFailed: live.phase = "list-failed"; break;
    case steam::DownloadServerSnapshot::Phase::kTesting: live.phase = "testing"; break;
    case steam::DownloadServerSnapshot::Phase::kTested: live.phase = "tested"; break;
    case steam::DownloadServerSnapshot::Phase::kTestFailed: live.phase = "test-failed"; break;
  }
  for (const auto& server : snapshot.servers)
    live.servers.push_back({server.id, server.name, server.plain_http});
  for (const auto& rate : snapshot.rates)
    live.rates.push_back({rate.id, rate.mb_per_s, rate.failed});
  live.testing_index = snapshot.testing_index;
  live.needs_sign_in = snapshot.needs_sign_in;
  live.needs_installed_game = snapshot.needs_installed_game;
  live.preferred_name = download_servers_->DisplayName(model_.state().download_server);
  if (snapshot.needs_sign_in) live.message = "Sign in to Steam to test servers.";
  else if (snapshot.needs_installed_game) live.message = "Install a game to test servers.";
  if (shown_download_servers_ && requests.empty()) {
    const auto& shown = *shown_download_servers_;
    if (shown.phase == live.phase && shown.testing_index == live.testing_index &&
        shown.needs_sign_in == live.needs_sign_in && shown.needs_installed_game == live.needs_installed_game &&
        shown.preferred_name == live.preferred_name && shown.message == live.message &&
        std::equal(shown.servers.begin(), shown.servers.end(), live.servers.begin(), live.servers.end(),
                   [](const DownloadServerOption& a, const DownloadServerOption& b) {
                     return a.id == b.id && a.name == b.name && a.plain_http == b.plain_http;
                   }) &&
        std::equal(shown.rates.begin(), shown.rates.end(), live.rates.begin(), live.rates.end(),
                   [](const DownloadServerResult& a, const DownloadServerResult& b) {
                     return a.id == b.id && a.mb_per_s == b.mb_per_s && a.failed == b.failed;
                   })) return;
  }
  model_.ApplyLiveDownloadServers(live);
  shown_download_servers_ = std::move(live);
}
void Controller::TickInstalls() {
  if (!installs_) {
    ProjectLocations({});
    return;
  }
  for (const auto& request : model_.TakeInstallRequests()) {
    if (download_servers_ && (request.kind == InstallRequestKind::Install ||
        request.kind == InstallRequestKind::Resume || request.kind == InstallRequestKind::DownloadNow))
      download_servers_->CancelTest();
    switch (request.kind) {
      case InstallRequestKind::Install: {
        const auto& games = model_.state().games;
        const auto game = std::find_if(games.begin(), games.end(), [&](const Game& value) {
          return value.id == request.game_id;
        });
        // The install sheet's choices; empty ones use the Settings language and the public branch. The chosen
        // version's estimate is reserved, not the public one.
        const auto estimate = request.install_bytes ? request.install_bytes
                              : game == games.end() ? std::nullopt : game->install_bytes;
        installs_->Install(request.game_id, estimate, request.volume_id, {request.language, request.branch});
        break;
      }
      case InstallRequestKind::Update: {
        // The record's branch and language select. Nothing is reserved up front: the details' estimate counts
        // whole depots, and the update checks the room its changed files need before it downloads them.
        installs_->Update(request.game_id, {});
        break;
      }
      case InstallRequestKind::Pause: installs_->Pause(request.game_id); break;
      case InstallRequestKind::Resume: installs_->Resume(request.game_id); break;
      case InstallRequestKind::DownloadNow: installs_->DownloadNow(request.game_id); break;
      case InstallRequestKind::MoveUp: installs_->MoveQueued(request.game_id, true); break;
      case InstallRequestKind::MoveDown: installs_->MoveQueued(request.game_id, false); break;
      case InstallRequestKind::Cancel: installs_->Cancel(request.game_id); break;
      case InstallRequestKind::Uninstall: BeginUninstall(request.game_id, request.delete_saves); break;
      case InstallRequestKind::SetDefaultVolume: installs_->SetDefaultVolume(request.volume_id); break;
      case InstallRequestKind::DismissCompleted: installs_->DismissCompleted(request.game_id); break;
      case InstallRequestKind::ClearCompleted: installs_->ClearCompleted(); break;
    }
  }
  // An uninstall whose files could not be deleted lists the game again and says why.
  for (const auto& failure : installs_->TakeRemovalFailures()) {
    uninstalls_.Abandon(failure.id);
    StopUninstall(failure.id, failure.reason + " Uninstall the game again to try again.");
  }
  const auto snapshot = installs_->Snapshot();
  files_deleting_ = snapshot.uninstalling;
  drives_present_.clear();
  for (const auto& volume : snapshot.volumes)
    if (volume.available) drives_present_.insert(volume.id);
  ProjectLocations(snapshot.installed);
  // A download that finished in this session is prepared next, so the game is ready to play.
  std::set<GameId> downloading, updating;
  for (const auto& job : snapshot.jobs) {
    downloading.insert(job.id);
    if (job.kind == steam::InstallJobKind::kUpdate) updating.insert(job.id);
  }
  for (const auto id : downloading_) {
    if (downloading.count(id) || !game_roots_.count(id)) continue;
    // An update that ended (finished or canceled) leaves the game prepared as it was: Play publishes its
    // settings again and puts Playden's Steam files back over the new ones; one that changed the files first
    // has its launch made again from them. Steam's details for it (the launch options that uses, whether more
    // is to download) are asked for again.
    if (updating_.count(id)) {
      refresh_details_.insert(id);
      refresh_asked_.erase(id);
      // A finished update left its marker: the game is set up again for it next (TickRuntime), or by Play first.
      if (runtime_) runtime_->RecheckInstalls();
      continue;
    }
    // A version other than public starts as that branch does: the launch options Steam's details gave before the
    // install were the public branch's. They are asked for again for the installed record, and the preparation
    // waits for them (RequestPrepare).
    const auto installed = std::find_if(snapshot.installed.begin(), snapshot.installed.end(),
                                        [id](const steam::InstalledGame& game) { return game.id == id; });
    if (installed != snapshot.installed.end() && installed->record && !installed->record->branch.empty() &&
        installed->record->branch != "public") {
      launch_options_.erase(id);
      launch_options_reason_.erase(id);
      launches_any_branch_.erase(id);
      refresh_details_.insert(id);
    }
    if (std::find(auto_prepare_.begin(), auto_prepare_.end(), id) == auto_prepare_.end()) auto_prepare_.push_back(id);
  }
  downloading_ = std::move(downloading);
  updating_ = std::move(updating);
  if (!snapshot.volumes.empty()) saves_root_ = snapshot.volumes.front().games_root + "/.playden-saves";
  // Free space changes outside Playden too, so volumes refresh every few seconds as well.
  const auto now = std::chrono::steady_clock::now();
  const bool account_changed = model_.state().account_generation != install_generation_;
  const bool space_due = now - volume_refreshed_at_ >= std::chrono::seconds(10);
  const bool licenses_due = licenses_notice_at_ && now >= *licenses_notice_at_;
  if (snapshot.revision == install_revision_ && !account_changed && !space_due && !licenses_due) return;
  {
    std::vector<LiveVolume> volumes;
    // Only attached drives are listed; one Playden can't write to is still attached, and its
    // space is still readable (the query falls back from a missing games folder to the drive).
    for (const auto& volume : snapshot.volumes) {
      LiveVolume live{volume.id, volume.label, true, std::nullopt, std::nullopt, volume.available};
      const auto space = steam::QueryVolumeSpace(volume.games_root);
      if (space) { live.total_bytes = space->total_bytes; live.free_bytes = space->free_bytes; }
      volumes.push_back(std::move(live));
    }
    model_.SetFreeSpaceSupported(steam::VolumeSpaceSupported());
    model_.UpdateVolumes(volumes, snapshot.default_volume_id, snapshot.default_volume_saved);
    volume_refreshed_at_ = now;
  }
  if (snapshot.revision == install_revision_ && !account_changed && !licenses_due) return;
  install_revision_ = snapshot.revision;
  install_generation_ = model_.state().account_generation;
  std::vector<LiveInstallJob> jobs;
  // A license check that takes a while shows as its own step (issue #47); a quick one never shows.
  constexpr auto kLicensesNoticeDelay = std::chrono::seconds(3);
  licenses_notice_at_.reset();
  for (const auto& job : snapshot.jobs) {
    bool checking_licenses = false;
    if (job.state == steam::InstallJobState::kRunning && job.step == steam::InstallStep::kCheckingLicenses) {
      const auto notice_at = job.step_since + kLicensesNoticeDelay;
      checking_licenses = now >= notice_at;
      if (!checking_licenses) licenses_notice_at_ = notice_at;
    }
    JobState state = JobState::Queued;
    switch (job.state) {
      case steam::InstallJobState::kQueued: state = JobState::Queued; break;
      case steam::InstallJobState::kRunning: state = JobState::Downloading; break;
      case steam::InstallJobState::kPaused: state = JobState::Paused; break;
      case steam::InstallJobState::kFailed: state = JobState::Failed; break;
    }
    const bool update = job.kind == steam::InstallJobKind::kUpdate;
    const bool running = job.state == steam::InstallJobState::kRunning;
    jobs.push_back({job.id, state, job.done_bytes, job.total_bytes, job.failure, job.volume_id,
                    job.bytes_per_second, job.eta_seconds, job.held_bytes, checking_licenses,
                    state == JobState::Downloading ? job.server_name : std::string(), update, job.locked,
                    update && running && job.step == steam::InstallStep::kCheckingFiles,
                    update && running && (job.step == steam::InstallStep::kApplying || job.locked)});
  }
  std::vector<LiveInstalledGame> installed;
  for (const auto& game : snapshot.installed) installed.push_back({game.id, game.install_bytes, game.volume_id, game.available});
  std::vector<LiveCompletedDownload> completed;
  for (const auto& finished : snapshot.completed)
    completed.push_back({finished.id, finished.install_bytes, finished.volume_id, finished.finished_at, finished.update_bytes});
  model_.UpdateInstalls(jobs, installed, completed);
}
void Controller::TickStoreDetails() {
  if (!store_details_) return;
  // The store is public: a game's description is asked for when its details open.
  const auto& detail = model_.state().navigation.detail_id;
  if (detail && detail != requested_store_detail_) {
    requested_store_detail_ = detail;
    store_details_->Request(*detail);
  }
  for (auto& details : store_details_->Pump())
    model_.UpdateStoreDetails(details.id, std::move(details.description), std::move(details.genres),
                              std::move(details.controller_support));
}
void Controller::TickAppDetails() {
  if (!app_details_) {
    // Nothing measures sizes: Steam's figures stand.
    for (const auto& check : model_.TakeSizeChecks()) model_.UpdateMeasuredSize(check, false, false, {});
    return;
  }
  const auto& state = model_.state();
  const auto generation = state.account_generation;
  if (generation != requested_generation_) {
    app_details_->Invalidate(generation);
    model_.ResetSizeChecks();
    requested_generation_ = generation;
    requested_detail_.reset();
    launch_options_.clear();
    launch_options_reason_.clear();
    launches_any_branch_.clear();
    // An updated game's setup that waited for these details was never asked of the runtime: it is asked again.
    for (const auto& id : prepare_after_details_) refresh_asked_.erase(id);
    prepare_after_details_.clear();
    requested_language_ = state.content_language;
    reissue_after_language_ = false;
  } else if (state.content_language != requested_language_) {
    // Sizes count the depots of the download language. The details worker reads the language when
    // a fetch starts and the host passes the new one on after this tick, so ask again next tick.
    requested_language_ = state.content_language;
    app_details_->Invalidate(generation);
    model_.ResetSizeChecks();
    requested_detail_.reset();
    reissue_after_language_ = true;
    return;
  } else if (reissue_after_language_) {
    reissue_after_language_ = false;
    for (const auto& id : prepare_after_details_) app_details_->Request(id, generation);
  }
  if (state.account().authentication == AuthState::SignedIn) {
    // A finished update: the game's details are asked again (they were answered for the old content).
    for (const auto& id : std::exchange(refresh_details_, {})) {
      launch_options_.erase(id);
      launch_options_reason_.erase(id);
      launches_any_branch_.erase(id);
      if (auto* details = Details(id.store); details && details == app_details_) details->Refresh(id, generation);
    }
    // Updates are checked for the game whose page is open, never the whole library. A library refresh reads the
    // licenses (DLC bought since): that page is checked again.
    const bool loading = state.library_loading;
    if (library_loading_ && !loading && state.navigation.detail_id && game_roots_.count(*state.navigation.detail_id))
      if (auto* details = Details(state.navigation.detail_id->store); details && details == app_details_)
        details->Refresh(*state.navigation.detail_id, generation, true);
    library_loading_ = loading;
  }
  // Fetch Steam app info when the player opens a game's details while signed in, ahead of anything queued. An
  // installed game is asked again at each visit: Steam may have a newer version since.
  // Away from a game's page (or its session), the next visit asks again.
  if (state.navigation.route != Route::Details && state.navigation.route != Route::Session) requested_detail_.reset();
  if (state.account().authentication == AuthState::SignedIn && state.navigation.detail_id &&
      state.navigation.detail_id != requested_detail_) {
    requested_detail_ = state.navigation.detail_id;
    if (auto* details = Details(requested_detail_->store); details && details == app_details_) {
      if (game_roots_.count(*requested_detail_)) details->Refresh(*requested_detail_, generation, true);
      else details->RequestFirst(*requested_detail_, generation);
    }
  }
  for (const auto& details : app_details_->Pump(8)) {
    if (details.account_generation != generation) continue;
    if (details.available) {
      std::vector<GameVersion> versions;
      for (const auto& version : details.versions) {
        GameVersion game_version{version.name, version.description, version.time_updated, version.build_id,
                                 version.download_bytes ? std::optional<std::uint64_t>(version.download_bytes) : std::nullopt,
                                 version.install_bytes ? std::optional<std::uint64_t>(version.install_bytes) : std::nullopt, {}};
        for (const auto& size : version.languages)
          game_version.languages.push_back({size.language, size.download_bytes, size.install_bytes});
        versions.push_back(std::move(game_version));
      }
      model_.UpdateAppDetails(details.id, details.download_bytes, details.install_bytes, details.windows_installable,
                              details.languages, details.dlc_count, std::move(versions),
                              {details.update.newer, details.update.new_dlc, details.update.download_bytes});
      if (details.launches_any_branch) launches_any_branch_.insert(details.id);
      else launches_any_branch_.erase(details.id);
      launch_options_[details.id] = details.launches;
      launch_options_reason_.erase(details.id);
    } else if (!launch_options_.count(details.id)) {
      launch_options_reason_[details.id] = details.reason.empty() ? "Steam details are unavailable right now." : details.reason;
    }
    if (prepare_after_details_.erase(details.id) && runtime_) SubmitPrepare(details.id);
  }
  // The install sheet's exact sizes: the chosen version and language, from their manifests.
  const bool signed_in = model_.state().account().authentication == AuthState::SignedIn;
  for (const auto& check : model_.TakeSizeChecks()) {
    auto* details = Details(check.id.store);
    if (signed_in && details == app_details_) details->Measure({check.id, check.branch, check.language}, generation);
    else model_.UpdateMeasuredSize(check, false, !signed_in, {});
  }
  for (const auto& size : app_details_->PumpMeasured()) {
    if (size.account_generation != generation) continue;
    model_.UpdateMeasuredSize({size.request.id, size.request.branch, size.request.language}, size.measured, size.failed,
                              {size.install_bytes, size.download_bytes});
  }
}
void Controller::RequestPrepare(GameId id) {
  if (!game_roots_.count(id)) return;
  // Without Steam's launch options yet, ask Steam first; TickAppDetails submits it.
  if (app_details_ && Details(id.store) == app_details_ && !launch_options_.count(id) && !launch_options_reason_.count(id) &&
      model_.state().account().authentication == AuthState::SignedIn) {
    prepare_after_details_.insert(id);
    app_details_->Request(id, model_.state().account_generation);
    return;
  }
  SubmitPrepare(id);
}
void Controller::SubmitPrepare(GameId id, bool announce) {
  const auto root = game_roots_.find(id);
  if (!runtime_ || root == game_roots_.end()) return;
  bool busy = false;
  const auto app = RuntimeApp(*runtime_, id, &busy);
  if (busy) {  // the game's first number waits for the runtime worker: again next tick
    deferred_runtime_.push_back([this, id, announce] { SubmitPrepare(id, announce); });
    return;
  }
  if (!app) return;
  if (announce) ready_watch_[id] = {false, std::chrono::steady_clock::now()};
  else ready_watch_.erase(id);
  // An imported game starts the program chosen when it was added: written into Game settings when
  // it was added, or here before its first preparation if that couldn't be; never over the player's
  // own choice or reset (SeedGameProgram).
  if (id.store == Store::kLocal && local_) {
    const auto snapshot = local_->Snapshot();
    for (const auto& game : snapshot->games)
      if (game.id == id.value) model_.SeedGameProgram(id, game.program);
  }
  RuntimePrepareRequest request{*app, root->second, game_volumes_[id], {}};
  request.game = id;
  for (const auto& game : model_.state().games) if (game.id == id) request.title_name = game.title;
  if (model_.state().title_store_suffix && !request.title_name.empty()) request.title_name += TitleStoreSuffix(id.store);
  if (const auto found = launch_options_.find(id); found != launch_options_.end())
    for (const auto& option : found->second)
      request.launches.push_back({"", option.description, option.executable, option.working_directory, option.arguments});
  request.launches_any_branch = launches_any_branch_.count(id) != 0;
  // Steam's launch options and app details are for Steam games only: another store's game starts the
  // program the player picks in Game settings.
  const bool steam = id.store == Store::kSteam;
  if (!steam)
    request.launch_reason = "Choose the program that starts this game in Game settings.";
  else if (const auto reason = launch_options_reason_.find(id); reason != launch_options_reason_.end())
    request.launch_reason = "Playden could not read how to start this game from Steam: " + reason->second;
  else if (!launch_options_.count(id))
    request.launch_reason = "Sign in to Steam so Playden can read how to start this game.";
  // After a failed answer, ask Steam again so a later Prepare can use the options.
  if (auto* details = Details(id.store); steam && details && !launch_options_.count(id))
    details->Request(id, model_.state().account_generation);
  runtime_->Prepare(std::move(request));
  if (prepare_next_pending_ == id) {
    runtime_->PrepareNext(*app);
    prepare_next_pending_.reset();
  }
}
void Controller::Apply(const steam::SteamEvent& event) {
  const auto ignore = [&](const char* reason) {
    if (auth_log_)
      auth_log_("request=" + std::to_string(event.request_id) + " event=" + steam::EventName(event.kind) +
                " ignored=" + reason + " phase=" + std::to_string(static_cast<int>(phase_)) +
                " current=" + std::to_string(request_));
  };
  if (event.generation < generation_) return ignore("old-generation");
  if (phase_ == Phase::SignOut) {
    if (event.kind != steam::SteamEventKind::kSignedOut && event.kind != steam::SteamEventKind::kFailure)
      return ignore("signing-out");
  } else if (phase_ == Phase::Idle || phase_==Phase::ClearLibrary || phase_==Phase::ClearOrganization) {
    return ignore("phase");
  } else if (event.request_id != request_) {
    return ignore("other-request");
  }
  generation_ = event.generation;
  switch (event.kind) {
    case steam::SteamEventKind::kChallenge:
      if (phase_ == Phase::SignIn) Update(AuthState::Waiting,{},false,event.challenge_url);
      break;
    case steam::SteamEventKind::kAwaitingApproval:
      // QR: the preceding challenge remains on screen; do not erase it.
      if (phase_ == Phase::SignIn && password_sign_in_) Update(AuthState::Waiting,{},false,{},SignInStep::Approval,&event);
      break;
    case steam::SteamEventKind::kNeedsEmailCode:
    case steam::SteamEventKind::kNeedsAuthenticatorCode:
    case steam::SteamEventKind::kCodeRejected:
      if (phase_ == Phase::SignIn && password_sign_in_)
        Update(AuthState::Waiting,{},false,{},
               event.kind == steam::SteamEventKind::kNeedsEmailCode ||
                       (event.kind == steam::SteamEventKind::kCodeRejected && event.code_is_email)
                   ? SignInStep::EmailCode : SignInStep::AuthenticatorCode,
               &event);
      break;
    case steam::SteamEventKind::kOfflineSession:
    case steam::SteamEventKind::kAuthenticated: {
      const bool signed_in = phase_ == Phase::SignIn && event.kind == steam::SteamEventKind::kAuthenticated;
      phase_ = Phase::Idle;
      retry_saved_session_ = false;
      reauthenticating_ = false;
      const bool offline=event.kind==steam::SteamEventKind::kOfflineSession;
      established_auth_=offline?AuthState::Offline:AuthState::SignedIn;
      AccountUpdate update;
      update.authentication = established_auth_;
      update.id = std::to_string(event.steam_id); update.display_name = event.account_name;
      update.clear_library = model_.state().account().id!=update.id;
      if(!library_) update.error = "Library sync is unavailable. Try again later.";
      if (signed_in) update.signed_in_with_password = password_sign_in_;
      model_.UpdateAccount(std::move(update));
      if(persistence_) persistence_->Activate(event.steam_id);
      if(runtime_) runtime_->SetSteamAccount(event.account_name, event.steam_id);
      if(web_) web_->SetAccount({event.account_name, event.steam_id});
      if(library_) library_->Activate(event.steam_id,offline);
      // Steam downloads that waited for a sign-in start now, not after their wait (up to a minute).
      if(installs_ && !offline) installs_->RetryWaiting(Store::kSteam);
      break;
    }
    case steam::SteamEventKind::kSignedOut: {
      phase_ = Phase::Idle;
      established_auth_=AuthState::SignedOut;
      if(library_) library_->Reset();
      if(persistence_) persistence_->Reset();
      if(runtime_) runtime_->SetSteamAccount({}, 0);
      if(web_) web_->SetAccount({});
      const bool begin = begin_after_clear_; begin_after_clear_ = false;
      Update(AuthState::SignedOut,{},true);
      if (begin) Begin(begin_password_after_clear_);
      break;
    }
    case steam::SteamEventKind::kCanceled:
      phase_ = Phase::Idle;
      if (reauthenticating_) { reauthenticating_ = false; Restore(); break; }
      Update(AuthState::Cancelled,{},!retry_saved_session_); break;
    case steam::SteamEventKind::kFailure: {
      if (reauthenticating_ && phase_ == Phase::SignIn && password_sign_in_ && event.error != steam::AuthError::kCanceled) {
        // A refused password (or code) is worth showing: the form stays, and Cancel returns to the saved sign-in.
        request_ = 0;
        Update(AuthState::Failed,diagnostics::AuthMessage(event.error),false,{},SignInStep::Password);
        break;
      }
      if (reauthenticating_ && phase_ == Phase::SignIn) {
        // The new QR sign-in failed or expired: keep using the saved one.
        reauthenticating_ = false;
        phase_ = Phase::Idle;
        Restore();
        break;
      }
      const bool restoring = phase_ == Phase::Restore;
      const bool clearing = phase_ == Phase::SignOut;
      const bool password = phase_ == Phase::SignIn && password_sign_in_;
      phase_ = Phase::Idle; begin_after_clear_ = false;
      if (restoring && event.error == steam::AuthError::kPersistence) {
        // A busy/private storage probe is not a revoked Steam credential. Keep
        // account state, retry the saved session, and never start QR sign-in.
        retry_saved_session_ = true;
        retry_saved_session_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        // A full console storage refuses the refreshed sign-in until games are uninstalled; the query
        // holds back the space the PS5 keeps, so none left means writes fail.
        std::optional<steam::VolumeSpace> space;
        if (installs_) {
          const auto volumes = installs_->Snapshot().volumes;
          if (!volumes.empty()) space = steam::QueryVolumeSpace(volumes.front().games_root, steam::kConsoleSystemReserve);
        }
        if (space && space->free_bytes == 0)
          Update(AuthState::Failed, "Playden can't save your sign-in. Uninstall games to free up space; it signs in again on its own.",
                 false, {}, SignInStep::Qr, nullptr, true, true);
        else
          Update(AuthState::Failed, "Account storage is unavailable. Retrying your saved sign-in…", false, {}, SignInStep::Qr,
                 nullptr, false, true);
        break;
      }
      retry_saved_session_ = false;
      if(!clearing) { established_auth_=AuthState::SignedOut;if(library_)library_->Reset();if(persistence_)persistence_->Reset(); }
      if (clearing) Update(model_.state().account().id.empty() ? AuthState::Failed : established_auth_,
                           "Sign-out failed. Your saved account remains. Retry sign-out in Settings.");
      else if (restoring && event.error == steam::AuthError::kNoSession) Update(AuthState::SignedOut,{},true);
      else if (restoring && (event.error == steam::AuthError::kRejected || event.error == steam::AuthError::kCredentialsRevoked)) {
        // Steam no longer accepts the saved session: show a new QR code right away
        // instead of a failure the player can only retry. With another store's games to play,
        // Playden stays on Home and says so; Sign in is in Settings.
        if (model_.HasGamesBeyondSteam()) {
          Update(AuthState::SignedOut,"Steam ended your saved sign-in. Sign in again from Settings.",true,{},
                 SignInStep::Qr,nullptr,false,true);
        } else {
          Update(AuthState::SignedOut,{},true);
          Begin();
        }
      }
      else if (event.error == steam::AuthError::kCanceled) Update(AuthState::Cancelled,{},true);
      else if (event.error == steam::AuthError::kExpired) Update(AuthState::Expired,diagnostics::AuthMessage(event.error),true);
      else Update(AuthState::Failed,diagnostics::AuthMessage(event.error),true,{},
                  password ? SignInStep::Password : SignInStep::Qr,nullptr,false,restoring);
      break;
    }
  }
}
void Controller::TickLicenseChanges() {
  if (!license_changes_ || !library_) return;
  const auto changes = license_changes_();
  const auto now = std::chrono::steady_clock::now();
  // Each change restarts the wait, so a burst of license lists makes one refresh.
  if (seen_license_changes_ && changes != *seen_license_changes_) license_refresh_at_ = now + license_refresh_delay_;
  seen_license_changes_ = changes;
  if (!license_refresh_at_) return;
  if (established_auth_ != AuthState::SignedIn || model_.state().account().id.empty()) { license_refresh_at_.reset(); return; }
  // A refresh, sign-in or sign-out in progress: wait for it, then refresh with the new licenses.
  if (now < *license_refresh_at_ || phase_ != Phase::Idle || library_->Busy() || library_->NeedsRenewal()) return;
  license_refresh_at_.reset();
  library_->Refresh();
}
void Controller::TickSelfUpdate() {
  if (!self_update_) return;
  // The track starts from the saved preference (ps5.cpp); only the Settings toggle changes it, so a
  // preference that hasn't loaded yet can't switch tracks at startup.
  for (const auto& request : model_.TakeSelfUpdateRequests()) {
    if (request == "check") self_update_check_pending_ = true;
    else if (request == "betas-on" || request == "betas-off") {
      self_update_->SetIncludeBetas(request == "betas-on");
      self_update_check_pending_ = true;
    }
    else if (request == "install") self_update_->Install();
    else if (request == "cancel") self_update_->Cancel();
    else if (request == "restart") self_update_->RequestRestart();
    else if (request == "rollback") self_update_->RequestRollback();
    else if (request == "acknowledge") self_update_->AcknowledgeFinished();
    else if (request == "kit-check") self_update_->CheckKit();
    else if (request == "kit-install") self_update_->InstallKit();
    else if (request == "kit-cancel") self_update_->Cancel();
  }
  // Retain checks across busy downloads and occupied worker queues, including opt-out then cancel.
  if (self_update_check_pending_ && !self_update_->Busy()) {
    self_update_check_pending_ = false;
    self_update_->Check(true);
  }
  // The runtime started without a usable kit (a first run), or with another kit than the one this
  // version was made for: the running version's kit is looked for once, after the startup update
  // check, before sign-in.
  if (!kit_check_asked_ && runtime_ && !self_update_->Busy()) {
    const auto runtime = runtime_->Shared();
    const bool other_kit = runtime->kit && runtime->kit->manifest_sha256 != runtime->kit->expected_manifest_sha256;
    if (runtime->status == RuntimeHostStatus::kReady && (!runtime->kit_available || other_kit)) {
      kit_check_asked_ = true;
      self_update_->CheckKit();
    }
  }
  const auto snapshot = self_update_->Snapshot();
  if (snapshot.kit.phase == update::KitInstallPhase::kInstalled && !kit_reload_asked_ && runtime_) {
    kit_reload_asked_ = true;
    runtime_->ReloadKit();
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const bool kit_downloading = snapshot.kit.phase == update::KitInstallPhase::kDownloading;
  const bool downloading = snapshot.phase == update::UpdatePhase::kDownloading || kit_downloading;
  const std::string rate_part = kit_downloading ? std::string("kit-only") : snapshot.part;
  const std::uint64_t rate_done = kit_downloading ? snapshot.kit.done : snapshot.done;
  const std::uint64_t rate_total = kit_downloading ? snapshot.kit.total : snapshot.total;
  if (downloading) {
    if (self_update_rate_part_ != rate_part) {
      self_update_rate_.Reset();
      self_update_rate_part_ = rate_part;
    }
    self_update_rate_.Record(rate_done, now);
  } else if (!self_update_rate_part_.empty()) {
    self_update_rate_.Reset();
    self_update_rate_part_.clear();
  }
  if (snapshot.revision == self_update_revision_ && !downloading) {
    model_.UpdateSelfUpdate(model_.state().self_update);  // lets a waiting prompt show
    return;
  }
  self_update_revision_ = snapshot.revision;
  LiveSelfUpdate live;
  live.phase = update::UpdatePhaseName(snapshot.phase);
  live.current = snapshot.current.ToString();
  live.commit = update::BuildCommit();
  live.available = snapshot.available.ToString();
  live.notes = snapshot.notes;
  live.part = snapshot.part;
  live.done = snapshot.done;
  live.total = snapshot.total;
  if (downloading) {
    const auto estimate = self_update_rate_.Estimate(now, rate_done, rate_total);
    live.bytes_per_second = estimate.bytes_per_second;
    live.eta_seconds = estimate.eta_seconds;
  }
  live.download_bytes = snapshot.download_bytes;
  live.kit_needed = snapshot.kit_needed;
  live.user_check = snapshot.user_check;
  live.error = snapshot.error;
  live.previous = snapshot.previous;
  live.notes_url = snapshot.notes_url;
  live.current_notes = snapshot.current_notes;
  live.current_notes_url = snapshot.current_notes_url;
  live.whats_new = snapshot.whats_new;
  live.quiet = snapshot.quiet;
  live.finished_message = snapshot.finished_message;
  live.finished_failed = snapshot.finished_failed;
  live.kit_phase = update::KitInstallPhaseName(snapshot.kit.phase);
  live.kit_replaces = snapshot.kit.replaces;
  live.kit_download_bytes = snapshot.kit.download_bytes;
  live.kit_done = snapshot.kit.done;
  live.kit_total = snapshot.kit.total;
  live.kit_error = snapshot.kit.error;
  model_.UpdateSelfUpdate(live);
}

}
