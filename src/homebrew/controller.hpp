#pragma once
#include <array>
#include "homebrew/simulation.hpp"
#include "homebrew/uninstall_flow.hpp"
#include "steam/service.h"
#include "app/library_controller.h"
#include "app/persistence_controller.h"
#include "steam/app_details_service.h"
#include "steam/store_details_service.h"
#include "steam/install_service.h"
#include "installs/installed_location.h"
#include "steam/cloud_service.h"
#include "homebrew/runtime_host.hpp"
#include "stores/registry.h"
#include "stores/steam.h"
#include "stores/local/library_source.h"
#include "steam/download_rate.h"
#include "steam/download_server_service.h"
#include "update/update_service.h"
#include "web/service.h"
#include <chrono>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>

namespace playden::homebrew {
// What the "Store name in PS5 titles" setting adds to a game's title name: " (Steam)"; imported games
// have no store to name.
std::string TitleStoreSuffix(Store store);
// Main-thread bridge. The backend/repository outlive this object. IO stays on service
// workers; the presentation receives no credential tokens.
class Controller {
 public:
  // The stores' accounts, libraries and details come from the registry, which must hold Steam's account.
  // Every other store's library is merged with Steam's.
  Controller(Simulation& model, stores::Registry& stores, app::PersistenceService* persistence_service = nullptr,
             steam::InstallService* installs = nullptr, steam::CloudService* cloud = nullptr,
             RuntimeHost* runtime = nullptr, std::function<void(const std::string&)> auth_log = {});
  // Steam only: a registry of Steam's account over the backend, and of the given services.
  Controller(Simulation& model, steam::AuthBackend& backend, SessionRepository& repository,
             LibraryService* library_service = nullptr, app::PersistenceService* persistence_service = nullptr,
             steam::AppDetailsService* app_details = nullptr, steam::InstallService* installs = nullptr,
             steam::CloudService* cloud = nullptr, RuntimeHost* runtime = nullptr,
             std::function<void(const std::string&)> auth_log = {});
  ~Controller();
  void Tick();
  // Microseconds each part of the last Tick took, for slow-frame diagnostics.
  std::string LastTickProfile() const;
  void Shutdown();
  // Steam store descriptions, genres and controller support for the games the player opens.
  void SetStoreDetails(steam::StoreDetailsService* store_details) { store_details_ = store_details; }
  // The server list and measurements run on their own worker; only their public snapshot enters the UI.
  void SetDownloadServers(steam::DownloadServerService* servers) { download_servers_ = servers; }
  // Playden's own update: the controller passes the player's requests on and shows its state.
  void SetSelfUpdate(update::UpdateService* self_update) { self_update_ = self_update; }
  // Settings › Web access turns it on and off (once the saved preferences are read); the signed-in account's
  // name and SteamID go to it for redaction. Shutdown stops it.
  void SetWebAccess(web::Service* web) { web_ = web; }
  // Receives one line per shutdown step with how long it took, so a quit that hangs names the step.
  void SetShutdownLog(std::function<void(const std::string&)> log) { shutdown_log_ = std::move(log); }
  // Preferences and organization load/save failures (app::PersistenceController::SetLog). The first loads
  // report on a later Tick, so setting it right after construction still sees them.
  void SetPersistenceLog(std::function<void(const std::string&)> log);
  // The player turned "Stay signed in" on or off: the host moves the Steam session to disk or memory.
  void SetStaySignedIn(std::function<void(bool)> apply) { stay_signed_in_ = std::move(apply); }
  // Counts changes to the account's Steam licenses (steam::CmLicenseChanges): the library refreshes
  // once they settle for `delay`, so a game added on another device shows up.
  void SetLicenseChanges(std::function<std::uint64_t()> changes,
                         std::chrono::steady_clock::duration delay = std::chrono::seconds(5)) {
    license_changes_ = std::move(changes);
    license_refresh_delay_ = delay;
  }
  // Games a source plays in place (external locations), asked for on every tick next to the install
  // service's own games, so they are prepared, played and re-checked after a restart like those. They
  // never reach a managed uninstall or the Installed games list. Keep it cheap: it runs on the main thread.
  void SetExternalLocations(std::function<std::vector<installs::InstalledLocation>()> locations) {
    external_locations_ = std::move(locations);
  }
  // The Local store (imported games), registered in the registry as Store::kLocal's library. Its
  // games playing in place become external locations next to SetExternalLocations' ones; the
  // model's import requests (look for games, add, remove) go to it, and removing an imported game
  // removes its PS5 title first, keeping its bottle as kept data. Set before the first Tick.
  void SetLocalLibrary(stores::local::LocalLibrarySource* local) { local_ = local; }
  // Tests: how long a Play or preparation waits for a drop-folder game's files to be opened.
  void SetDropFolderOpenTimeout(std::chrono::milliseconds timeout) { drop_folder_open_timeout_ = timeout; }
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;
 private:
  enum class Phase { Idle, Restore, SignIn, ClearLibrary, ClearOrganization, SignOut };
  void Start(app::PersistenceService* persistence_service);
  // The store's details source, if it has one.
  stores::DetailsSource* Details(Store store) const { return stores_.details(store); }
  // A new sign-in in the given method; without one, in the method the last sign-in used.
  void Begin();
  void Begin(bool password);
  // The sign-in screen's method changed: a QR sign-in starts, or the password form waits for input.
  void UseMethod(bool password);
  void BeginPassword();
  void SignOut(bool switch_account);
  void Reauthenticate();
  // quiet: an automatic retry keeps the failure on screen instead of showing Restoring.
  void Restore(bool quiet = false);
  void AdvanceClear();
  void BeginOrganizationClear();
  void AdvanceOrganizationClear();
  void Apply(const steam::SteamEvent& event);
  void Update(AuthState state, const std::string& error = {}, bool clear = false,
              const std::string& challenge = {}, SignInStep step = SignInStep::Qr,
              const steam::SteamEvent* prompt = nullptr, bool storage_full = false, bool restoring = false);
  void TickAppDetails();
  void TickStoreDetails();
  steam::StoreDetailsService* store_details_ = nullptr;
  std::optional<GameId> requested_store_detail_;
  void TickInstalls();
  void TickDownloadServers();
  bool DownloadServerTestAllowed() const;
  steam::DownloadServerService* download_servers_ = nullptr;
  web::Service* web_ = nullptr;
  void TickWebAccess();
  std::string download_servers_account_;
  std::optional<LiveDownloadServers> shown_download_servers_;
  void TickSelfUpdate();
  void TickLicenseChanges();
  std::function<void(const std::string&)> shutdown_log_;
  std::function<void(const std::string&)> persistence_log_;
  std::function<void(bool)> stay_signed_in_;
  std::function<std::uint64_t()> license_changes_;
  std::chrono::steady_clock::duration license_refresh_delay_{};
  std::optional<std::uint64_t> seen_license_changes_;
  std::optional<std::chrono::steady_clock::time_point> license_refresh_at_;
  update::UpdateService* self_update_ = nullptr;
  bool self_update_check_pending_ = false;
  std::uint64_t self_update_revision_ = ~0ull;
  steam::DownloadRate self_update_rate_;
  std::string self_update_rate_part_;
  bool kit_check_asked_ = false;  // the runtime started without a kit: CheckKit asked once
  bool kit_reload_asked_ = false;  // the kit-only install finished: the runtime asked to admit it
  void TickCloud();
  void TickRuntime();
  Simulation& model_;
  std::unique_ptr<stores::Registry> owned_stores_;
  stores::Registry& stores_;
  stores::DetailsSource* app_details_ = nullptr;  // Steam's
  steam::InstallService* installs_ = nullptr;
  std::uint64_t install_revision_ = 0;
  std::uint64_t install_generation_ = 0;
  steam::CloudService* cloud_ = nullptr;
  std::uint64_t cloud_revision_ = 0;
  // Every installed game's location, managed and external; the folders of those available now, and
  // their volume ids, are what Prepare, Play and Cloud use.
  std::function<std::vector<installs::InstalledLocation>()> external_locations_;
  std::vector<installs::InstalledLocation> locations_;
  std::uint64_t locations_revision_ = 0;
  std::uint64_t runtime_locations_revision_ = 0;
  void ProjectLocations(const std::vector<steam::InstalledGame>& managed);
  // The Local store: model requests in, candidates and library changes out.
  void TickImports();
  stores::local::LocalLibrarySource* local_ = nullptr;
  std::uint64_t local_revision_ = 0;
  std::uint64_t local_games_revision_ = 0;
  std::chrono::steady_clock::time_point local_volumes_checked_at_{};
  std::vector<LiveExternalGame> external_projected_;
  // Removals asked for before the Local store's first snapshot reached the locations: until then a
  // game's folder (and so its PS5 title's removal first) isn't known.
  std::vector<GameId> held_import_removals_;
  // Copies and moves into Playden whose game's first runtime number waits for the runtime worker
  // (starting up, or in a batch): asked again on the next tick, in order.
  std::vector<ImportRequest> held_import_transfers_;
  // Games added to play in place, or moved into Playden, that join the preparation queue once their
  // folder is listed: true for a move, which waits for Playden's own folder.
  std::map<GameId, bool> prepare_after_import_;
  // Runtime calls whose game's first number waited for the runtime worker: made again on the next
  // tick, in order. The main thread never waits for that worker.
  std::vector<std::function<void()>> deferred_runtime_;
  void WithApp(const GameId& id, std::function<void(std::uint32_t)> use);
  // The player's add, copy, move and removal requests sent to the Local store, by request number,
  // until their outcome: a refusal reaches the model as a notice.
  std::map<std::uint64_t, ImportRequest> import_requests_;
  // Copies into Playden were reconciled with the install service after the first imports.json read.
  bool copies_reconciled_ = false;
  // Play and every preparation of a game playing from the drop folder (the player's Prepare,
  // Recreate bottle, the startup check of a title waiting for the PS5) wait for its files to be
  // opened (LocalLibrarySource::OpenForPlay); a failure stops them with the walk's sentence, and so
  // does an opening with no outcome within the timeout (its outcome may have left the store's list).
  enum class DropFolderNext { kPlay, kPrepare, kRecreate, kRegistrationCheck };
  struct DropFolderOpen {
    std::uint64_t request = 0;
    bool play = false, prepare = false, recreate = false, registration_check = false;
    std::chrono::steady_clock::time_point started;
  };
  std::map<GameId, DropFolderOpen> drop_folder_opens_;
  std::chrono::milliseconds drop_folder_open_timeout_ = std::chrono::seconds(60);
  // True when the request waits (or was stopped) for the drop folder.
  bool HoldForDropFolder(GameId id, DropFolderNext next);
  // Finished openings: their requests go on (into plays, prepares and direct submissions, with
  // their announce flag) or stop.
  void ReleaseDropFolderOpens(std::vector<GameId>& plays, std::vector<GameId>& prepares,
                              std::vector<std::pair<GameId, bool>>& submits);
  const installs::InstalledLocation* LocationOf(GameId id) const;
  // Whether Cloud saves apply to the game: a Cloud service exists and the game's store has them.
  bool CloudCapable(GameId id) const { return cloud_ && Traits(id.store).cloud_saves; }
  std::map<GameId, std::string> game_roots_;  // installed games' folders, for Prepare, Play and Cloud
  std::map<GameId, std::string> game_volumes_;  // installed games' volume ids
  // Steam's Windows launch options per game, from app details. A game Playden ships
  // no recipe for is prepared from them; Prepare waits while they are fetched.
  std::map<GameId, std::vector<steam::LaunchOption>> launch_options_;
  // Games whose Steam options are all for other branches: preparing may ask for the program.
  std::set<GameId> launches_any_branch_;
  std::map<GameId, std::string> launch_options_reason_;
  std::set<GameId> prepare_after_details_;
  // announce: tell the player when this preparation leaves the game ready to play
  // (waiting for the PS5 to register it); the startup check does not.
  // Queues a game's preparation, after asking Steam for its launch options when they are missing.
  void RequestPrepare(GameId id);
  void SubmitPrepare(GameId id, bool announce = true);
  RuntimeHost* runtime_ = nullptr;
  std::uint64_t runtime_revision_ = 0;
  std::uint64_t runtime_install_revision_ = 0;
  std::uint64_t runtime_cloud_revision_ = 0;
  std::string saves_root_;                    // per-game save state folders, on the primary drive
  // Save lifecycle around Play: games whose session just ended sync once their
  // folder is known; games syncing now cannot start.
  std::set<GameId> sync_after_session_;
  std::set<GameId> cloud_busy_;
  // Play requested but not yet visible in the runtime's launch state.
  std::set<GameId> play_queued_;
  std::uint64_t plays_requested_ = 0;
  // Play checks Cloud saves first: games waiting for that check (true once the
  // sync was issued), games whose check passed and can start, games the player
  // may start without syncing after a failed check, and what Play shows meanwhile.
  std::map<GameId, bool> sync_before_play_;
  std::vector<GameId> plays_ready_;
  std::set<GameId> play_without_sync_;
  // Games are prepared without a button press: once their download finishes (games
  // downloading now, then games waiting to be prepared), and when Play finds one not
  // prepared yet. Play then starts it once it is Ready; the refusal seen when Prepare
  // was queued tells a new refusal from an old one.
  std::set<GameId> downloading_;
  std::set<GameId> updating_;  // the install jobs that are updates of installed games
  // Installed games whose update finished: their Steam details (launch options, update state) are asked again.
  std::set<GameId> refresh_details_;
  bool library_loading_ = false;  // the library refresh last seen running
  std::vector<GameId> auto_prepare_;  // in the order their downloads (copies, imports) finished
  std::set<GameId> refresh_asked_;  // updated games whose setup for the new files was asked for (TickRuntime)
  std::optional<GameId> prepare_next_pending_;  // Prepare next on a game not in the runtime's queue yet
  struct PlayAfterPrepare {
    bool queued = false;
    bool seen_preparing = false;
    std::string refusal;
    std::uint64_t prepares_handled = 0;  // the worker's count when this prepare was queued
    std::chrono::steady_clock::time_point queued_at{};
    std::string words = "Preparing…";  // what Play says meanwhile: preparing, updating or repairing the runtime
  };
  std::map<GameId, PlayAfterPrepare> play_after_prepare_;
  // Preparations the player started (Play, Prepare, a finished download): when one
  // ends waiting for the PS5, the model asks whether to quit to Home to play it.
  struct ReadyWatch {
    bool seen_preparing = false;
    std::chrono::steady_clock::time_point submitted_at{};
  };
  std::map<GameId, ReadyWatch> ready_watch_;
  std::vector<GameId> ready_notices_;  // ready to play, announced after the next projection
  // Recreate bottle: the PS5 title is removed with its saves kept, then the game is
  // prepared again (which brings the saves back). Results stay for the maintenance sheet.
  struct Recreate {
    std::uint64_t removals_before = 0;
    bool prepare_submitted = false;
    bool seen_preparing = false;
    std::string refusal;  // the game's Prepare refusal when it was submitted
    std::chrono::steady_clock::time_point submitted_at{};
  };
  std::map<GameId, Recreate> recreate_;
  std::map<GameId, std::pair<bool, std::string>> recreate_results_;
  // Maintenance › Update runtime: the preparation Play runs first for a title on an older kit's runtime (RunPrepare's
  // update, with its settings republish), without starting the game. Results stay for the maintenance sheet.
  struct RuntimeUpdate {
    std::uint64_t prepares_handled = 0;  // the worker's count when it was asked for
    bool seen_preparing = false;
    std::chrono::steady_clock::time_point submitted_at{};
  };
  std::map<GameId, RuntimeUpdate> runtime_updates_;
  std::map<GameId, std::pair<bool, std::string>> runtime_update_results_;
  // The verified kit's capabilities, which gate Game settings rows.
  std::optional<std::vector<std::string>> kit_capabilities_;
  // Titles waiting for the PS5 when Playden started are checked once (query only).
  bool startup_awaiting_known_ = false;
  std::set<GameId> startup_awaiting_;
  // Uninstall order: Cloud saves, then the PS5 title, then the download.
  UninstallFlow uninstalls_;
  void BeginUninstall(GameId id, bool delete_saves);
  void StopUninstall(GameId id, const std::string& reason);
  void TickUninstalls();
  // What a game's uninstall does now ("Waiting to uninstall" until its turn); empty without one.
  std::string UninstallStatus(GameId id) const;
  // Games whose files the install service is deleting (it lists them as installed no more), in the
  // order it deletes them, and the drives it has now.
  std::vector<GameId> files_deleting_;
  std::set<std::string> drives_present_;
  // The uninstalls with their status, in order, as last projected.
  std::vector<std::pair<GameId, std::string>> uninstall_statuses_;
  std::uint64_t kept_revision_ = 0;
  std::vector<LiveKeptBottle> kept_projected_;
  std::vector<LiveLeftoverTitle> leftovers_projected_;
  std::vector<LiveLeftoverDelete> leftover_deletions_projected_;
  std::map<std::string, std::string> leftover_names_;
  std::map<GameId, std::string> play_notes_;
  std::uint64_t play_notes_revision_ = 0;
  std::uint64_t runtime_notes_revision_ = 0;
  std::map<GameId, CloudStatus> cloud_status_;
  // Cloud requests wait for the console to confirm the game's title is stopped:
  // asked once the activity check is queued, answered by a newer serial.
  struct HeldCloudRequest {
    CloudRequest request;
    bool asked = false;
    std::uint64_t after = 0;
  };
  std::vector<HeldCloudRequest> cloud_held_;
  // The Saves tab: its last result restored once; the quick check once per run and when the tab
  // opens (at most every two minutes); jobs wait for the console to say which titles run.
  std::optional<steam::CloudLocation> CloudLocationFor(GameId id, bool for_job) const;
  void QueueSavesJob(steam::CloudJobKind kind);
  struct PendingJob {
    steam::CloudJobKind kind = steam::CloudJobKind::kNone;
    std::vector<GameId> ids;
    bool asked = false;
    std::uint64_t after = 0;
    std::chrono::steady_clock::time_point deadline{};
  };
  std::optional<PendingJob> pending_job_;
  bool cloud_restored_ = false;
  bool startup_quick_check_ = false;
  std::optional<std::chrono::steady_clock::time_point> last_quick_check_;
  // Games whose title may have been running when a job started: they sync once it stops.
  std::set<GameId> after_play_;
  std::set<GameId> after_play_projected_;
  std::optional<std::chrono::steady_clock::time_point> after_play_checked_;
  // Games whose next Cloud result answers the player (a dialog may open); others are background.
  // The serial after which the game's next settled report answers the player's request.
  static constexpr std::uint64_t kNotDispatched = ~std::uint64_t{0};
  // How long a Cloud job waits for the console to say which games are stopped.
  static constexpr std::chrono::seconds kJobActivityWait{45};
  std::map<GameId, std::uint64_t> cloud_foreground_;
  void TickGate();
  // Games the gates refuse (sessions Playden knows of), read on the Cloud and install workers.
  std::mutex gate_mutex_;
  std::set<GameId> gate_blocked_;
  LiveSavesJob saves_job_;
  std::map<GameId, std::uint64_t> save_sizes_;
  std::optional<std::pair<GameId, runtime::LaunchState>> last_launch_;
  // A game whose title may be running: Cloud must not touch its saves.
  bool SessionActive(GameId id) const;
  std::chrono::steady_clock::time_point volume_refreshed_at_{};
  std::optional<std::chrono::steady_clock::time_point> licenses_notice_at_;  // when a license check starts showing
  std::optional<GameId> requested_detail_;
  std::uint64_t requested_generation_ = 0;
  // The download language app details were last asked for: sizes follow it, so a change asks again.
  std::string requested_language_;
  bool reissue_after_language_ = false;
  stores::SteamAccount* service_ = nullptr;  // Steam's account, in the registry; null once shut down
  std::function<void(const std::string&)> auth_log_;  // sign-in steps and results (auth.log)
  std::unique_ptr<app::LibraryController> library_;
  std::unique_ptr<app::PersistenceController> persistence_;
  AuthState established_auth_ = AuthState::SignedOut;
  bool retry_saved_session_ = false;
  std::chrono::steady_clock::time_point retry_saved_session_at_{};
  steam::RequestId request_ = 0;
  std::uint64_t generation_ = 0;
  Phase phase_ = Phase::Idle;
  bool shut_down_ = false;  // Shutdown ran
  bool begin_after_clear_ = false;
  // A QR sign-in for an account that is already signed in: its library and organization stay, and
  // a cancel or failure goes back to the saved sign-in.
  bool reauthenticating_ = false;
  // The current sign-in uses the account name and password (the form, or its request).
  bool password_sign_in_ = false;
  bool begin_password_after_clear_ = false;
  std::array<std::int64_t, 9> tick_us_{};
};
}
