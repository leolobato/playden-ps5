// simulation.hpp - Renderer-neutral deterministic Playden UI simulation.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "app/preferences.h"
#include "library/organization.h"
#include "library/model.h"
#include "runtime/game_settings.h"
#include "steam/backend/content_language.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace playden::homebrew {

using GameId = playden::GameId;

enum class Route { Startup, SignIn, Home, Library, Downloads, Settings, Details, Session, Saves, SaveDetails };
enum class ModelMode { Simulation, Live };
// UsePassword/UseQr switch the sign-in screen's method; BeginPassword and SubmitCode carry the form's
// secrets, which the controller takes with TakeSignInCredentials and TakeGuardCode.
enum class ServiceAction { BeginQr, CancelQr, SignOut, SwitchAccount, RefreshLibrary, Reauthenticate,
                           UsePassword, UseQr, BeginPassword, SubmitCode, StaySignedIn };
// A service action for one store's account. The sign-in actions (QR, password, Steam Guard code) are Steam's.
struct ServiceRequest {
    ServiceAction action = ServiceAction::BeginQr;
    Store store = Store::kSteam;
};
inline bool operator==(const ServiceRequest& request, ServiceAction action) { return request.action == action; }
inline bool operator!=(const ServiceRequest& request, ServiceAction action) { return request.action != action; }
enum class AuthState { SignedOut, Restoring, Waiting, Expired, Failed, Cancelled, Syncing, SigningOut, SignedIn, Offline };
enum class JobState { Queued, Downloading, Pausing, Paused, Verifying, Downloaded, Failed, Cancelled, Interrupted };
enum class JobStage { Download, Verify, Install, Uninstall };
enum class SessionState { Idle, Preparing, Launching, Running, Stopping, Failed, Crashed };
enum class RuntimeState { Unavailable, Checking, Ready, Failed };
enum class LogState { Loading, Ready, Failed };
enum class InstallRequestKind { Install, Pause, Resume, DownloadNow, MoveUp, MoveDown, Cancel, Uninstall, SetDefaultVolume,
                                DismissCompleted, ClearCompleted };
struct InstallRequest {
    InstallRequestKind kind = InstallRequestKind::Install;
    GameId game_id;
    std::string volume_id;  // SetDefaultVolume; Install: this install's drive (empty: the default)
    bool delete_saves = false;  // Uninstall only: also delete the game's kept saves and settings
};
// Game maintenance the details page offers (simulated in fixture mode).
enum class MaintenanceKind { ApplySettings, ClearShaderCache, Repair, BackupSaves, RecreateBottle, UpdateRuntime };
// Whether a game's PS5 title runs now, as the console last answered.
enum class TitleActivity { Unknown, Stopped, Running };
// Live install state projected from the real install service.
std::string FormatDownloadRate(std::optional<std::uint64_t> bytes_per_second, std::optional<int> eta_seconds);
struct LiveInstallJob {
    GameId game_id;
    JobState state = JobState::Queued;
    std::uint64_t done_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::string failure;
    std::string volume_id;
    std::optional<std::uint64_t> bytes_per_second;
    std::optional<int> eta_seconds;
    std::uint64_t held_bytes = 0;  // installed bytes already on the drive
    bool checking_licenses = false;  // a slow Steam license check before the download starts
};
struct LiveInstalledGame {
    GameId game_id;
    std::uint64_t install_bytes = 0;
    std::string volume_id;
    bool available = true;  // false while the game's drive is absent
};
// A recent finished download, listed in Downloads after the queue.
struct LiveCompletedDownload {
    GameId game_id;
    std::uint64_t install_bytes = 0;
    std::string volume_id;
    std::int64_t finished_at = 0;  // Unix seconds
};
// A drive the real install service can use. Space is absent when the platform cannot report it.
struct LiveVolume {
    std::string id;
    std::string label;
    bool available = true;   // the drive is attached
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::uint64_t> free_bytes;
    bool writable = true;    // Playden can make its games folder there
};
// The runtime kit in use, for Settings > About. Every field is empty when no kit is usable.
struct LiveKitInfo {
    std::string kit_id;
    std::string manifest_sha256;
    std::string expected_manifest_sha256;  // the kit this build was tested with
    std::string prospero_win_revision;     // what the kit's PS5 titles were built from
    bool source_dirty = false;             // built from uncommitted changes
    std::string unavailable_reason;        // why no kit is in use
};
// Live: Playden's own update (update/update_service.h), projected for Settings, Downloads and prompts.
struct LiveSelfUpdate {
    std::string phase = "idle";  // idle, checking, up-to-date, available, downloading, unpacking, staged, failed
    std::string current;         // "0.2.0"
    std::string available;       // the newer version, when known
    std::string notes;
    std::string part;            // "kit" or "app" while downloading or unpacking
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::optional<std::uint64_t> bytes_per_second;
    std::optional<int> eta_seconds;
    std::uint64_t download_bytes = 0;
    bool kit_needed = false;
    bool moves = false;          // the update installs Playden as a new app (its new title)
    bool user_check = false;     // the last check was asked for in Settings
    std::string error;
    std::string previous;        // the version "go back" restores; empty when none is kept
    std::string notes_url;       // the available release's page
    std::string current_notes;   // the running version's release notes, when Playden updated to it
    std::string current_notes_url;
    bool whats_new = false;      // the previous run updated Playden: show its release notes once
    bool quiet = false;          // the available version is one the player went back from: no dialog
    std::string moved_to;        // Playden moved to its new title, which hasn't started yet: its version
    std::string finished_message;  // the previous run's update result, shown once
    bool finished_failed = false;
    // The kit-only install (first run, or a kit that isn't this version's): idle, checking, not-needed,
    // unavailable, available, downloading, unpacking, installed, failed.
    std::string kit_phase = "idle";
    bool kit_replaces = false;   // another kit is installed: this one replaces it
    std::uint64_t kit_download_bytes = 0;
    std::uint64_t kit_done = 0;
    std::uint64_t kit_total = 0;
    std::string kit_error;
};
// Live: what Game settings needs to choose a game's program, projected from the runtime host.
// A program a Steam launch option names, as a path in the game folder.
struct LaunchProgram {
    std::string path;
    bool helper = false;  // an installer or tool (the import rules)
};

struct LiveGameSetup {
    std::vector<playden::runtime::GameProgram> programs;  // the game folder's Windows programs
    bool scanned = false;
    // A game Playden could not prepare: the recipe the sheet starts from, whether a choice in
    // Game settings can get past the refusal, and the refusal itself.
    std::optional<playden::runtime::GameRecipe> draft;
    bool refusal_soft = false;
    std::string refusal;
    // Steam's options are all for other branches and more than one program is in the folder:
    // setting up waits for the player's choice (the Play row and a sheet ask).
    std::vector<LaunchProgram> steam_choices;
};
// Live per-game runtime (private bottle and PS5 title) state, projected from the runtime host.
struct LiveRuntimeGame {
    GameId game_id;
    std::string status;   // short value, e.g. "Not prepared", "Preparing", "Ready"
    std::string detail;   // why, or what happens next
    bool can_prepare = false;
    bool ready = false;
    // Play: whether it can start now, and the current or last launch.
    bool can_play = false;
    std::string play_status;  // e.g. "Starting…", "Playing"; empty when idle
    std::string play_detail;  // the last launch's outcome or refusal
    // Uninstall removes the game's PS5 title first; has_title says there is one.
    bool has_title = false;
    std::string uninstall_status;  // e.g. "Syncing saves…", "Removing PS5 title…"; empty when idle
    // How far the current copy or deletion is (0–1); absent when it cannot be measured.
    std::optional<float> progress;
    // Prepared and waiting for the PS5 to register its title, which it does once Playden
    // quits: the game is ready to play from Home, and Play quits there.
    bool awaiting_registration = false;
    // The title runs the installed kit's runtime (false while a Ready title waits for Update): settings only the
    // newer runtime takes, such as 120 Hz output, can be chosen before but apply only then.
    bool runtime_current = true;
    // Its setup stopped before Ready and left a PS5 title or bottle made with an earlier kit: Try again
    // can't finish it, Recreate bottle does (RuntimeInstallView::setup_other_kit).
    bool setup_other_kit = false;
    // Has a prepared title (Ready, perhaps with an update or repair to make): its Game
    // settings and maintenance apply to it.
    bool prepared = false;
    // The settings revision its PS5 title publishes and the launch entry it was made for.
    std::uint64_t settings_revision = 0;
    std::string launch_entry_id;
    TitleActivity activity = TitleActivity::Unknown;
    // The newest maintenance request and its outcome.
    std::optional<MaintenanceKind> maintenance;
    bool maintenance_running = false;
    bool maintenance_ok = false;
    std::string maintenance_message;
    // Save backups kept for the game and when the newest was made (Unix time).
    std::size_t save_backups = 0;
    std::int64_t latest_save_backup = 0;
    std::string failure_code;  // Recorded preparation failure; detail remains available in diagnostics.
    // Its setup waits for another game's to finish: Playden sets up one game at a time.
    bool queued = false;
    // Another store's game that uses Steamworks: the Steam app ID it was set up with, and whether that
    // came from the game's own steam_appid.txt (which wins over the one in Game settings).
    std::uint32_t steam_app_id = 0;
    bool steam_app_from_game_file = false;
};
// A preparation that ended Ready, listed in Queue after the waiting games.
struct LiveCompletedPreparation {
    GameId game_id;
    std::int64_t finished_at = 0;  // Unix seconds
    bool operator==(const LiveCompletedPreparation&) const = default;
};
// An uninstall that runs or waits its turn, listed in Queue's Preparation section.
struct LiveUninstall {
    GameId game_id;
    std::string step;  // what it does now, e.g. "Removing PS5 title…"; empty while it waits its turn
    bool operator==(const LiveUninstall&) const = default;
};
// An uninstall that ended this session: uninstalled, or stopped with the reason.
struct EndedUninstall {
    GameId game_id;
    std::string title;  // kept: an imported game leaves the library once it is uninstalled
    std::string stopped;  // why it stopped; empty once uninstalled
    std::int64_t finished_at = 0;  // Unix seconds, 0 unknown
    bool operator==(const EndedUninstall&) const = default;
};
// Live: the runtime's preparation queue, for Queue's Preparation section.
struct PreparationQueue {
    std::optional<GameId> active;  // being prepared now
    std::vector<GameId> waiting;   // in the order they start
    std::vector<LiveCompletedPreparation> completed;  // newest first
    // Why the latest preparation of a game that is neither prepared nor waiting was refused.
    std::map<GameId, std::string> refused;
    std::vector<LiveUninstall> uninstalls;  // in the order they were asked for
    bool operator==(const PreparationQueue&) const = default;
};
// Saves and settings kept from an uninstalled game (its private bottle).
struct LiveKeptBottle {
    GameId game_id;
    std::string name;
    std::int64_t kept_at = 0;
    std::uint64_t bytes = 0;
    bool operator==(const LiveKeptBottle&) const = default;
};
// A game that plays in place from a folder Playden doesn't manage (an imported game on a drive):
// never in Installed games, its files never deleted.
struct LiveExternalGame {
    GameId game_id;
    std::string volume_id;
    bool available = true;  // its drive and folder are there and the PS5 can play from them
    std::string reason;     // why a folder that is there can't be played from; empty otherwise
    bool operator==(const LiveExternalGame&) const = default;
};
// A folder in an import folder that could be added to the library (stores/local/library_source.h).
struct LiveImportProgram {
    std::string path;  // inside the game folder, '/' separators
    std::uint64_t bytes = 0;
    bool helper = false;  // an installer or tool, ranked last
    bool operator==(const LiveImportProgram&) const = default;
};
struct LiveImportCandidate {
    std::string volume_id;
    std::string folder;  // relative to the volume's mount
    std::string path;    // console path
    std::string title;
    std::vector<LiveImportProgram> programs;  // ranked: the likely game first
    std::uint64_t total_bytes = 0;  // estimate
    bool ascii_ok = false;  // can play in place
    bool partial = false;   // the scan stopped at a limit or depth
    GameId imported;        // already in the library as this game; invalid when not
    std::uint64_t file_count = 0;  // files in the folder (0 when unknown)
    bool operator==(const LiveImportCandidate&) const = default;
};
// Requests for the Local store: look for games, add a candidate (in place), remove an imported game.
// A game moved into Playden is prepared once it is there, as a finished copy is.
// CopyIntoPlayden: copy an imported game playing in place into Playden's own storage (volume_id:
// an install volume, empty for the default one). MoveIntoPlayden: rename a drop-folder game into
// Playden's games folder on the console storage (instant; its PS5 title, if any, follows).
enum class ImportRequestKind { LookForGames, Add, Remove, CopyIntoPlayden, MoveIntoPlayden };
struct ImportRequest {
    ImportRequestKind kind = ImportRequestKind::LookForGames;
    GameId game_id;          // Remove, CopyIntoPlayden, MoveIntoPlayden
    std::string volume_id;   // Add, CopyIntoPlayden
    std::string folder;      // Add: relative to the volume's mount
    std::string program;     // Add: relative to the folder
    bool prepare = false;    // Add: prepare the game once it is in the library (it plays in place)
};
// Live Steam Cloud save state for one game, projected from the cloud service.
// Waiting: transfers that need no choice wait for the player (an uninstalled game's saves, or
// saves deleted here that Cloud still has).
enum class CloudStatus { None, Checking, Syncing, Synced, UpToDate, Conflict, NeedsAccount, Unsupported, Failed, Waiting };
struct LiveCloudGame {
    GameId game_id;
    CloudStatus status = CloudStatus::None;
    std::string message;
    std::size_t local_files = 0;
    std::size_t remote_files = 0;
    std::uint64_t local_bytes = 0;
    std::uint64_t remote_bytes = 0;
    std::int64_t local_latest = 0;
    std::int64_t remote_latest = 0;
    bool first_sync = false;      // a conflict before any sync on this console: both sides already had saves
    bool first_overwrite = false; // a conflict because this console's first upload would replace Cloud saves
    std::int64_t last_sync = 0;   // Unix time saves were last in step
    bool queued = false;          // waiting in the running check or Sync all
    std::uint64_t moved_bytes = 0, total_bytes = 0;  // the transfer in progress
    std::size_t downloads = 0, uploads = 0;          // what the transfer moves
    bool after_play = false;      // the game may be running: its saves sync once it stops
    bool background = false;      // the result of a check the player did not start: no dialogs
};
// The Saves tab's check or Sync all, projected from the cloud service.
enum class SavesJobKind { None, QuickCheck, SyncAll };
struct LiveSavesJob {
    SavesJobKind kind = SavesJobKind::None;
    std::size_t done = 0, total = 0;
    GameId current;
    bool stopping = false;
};
// RemoveLocal deletes a game's saves on this console (backed up first); RemoveLeftover deletes an
// uninstalled game's kept data with its sync state. QuickCheck, SyncAll and StopJob are the Saves
// tab's (game_id 0).
enum class CloudRequestKind { Sync, UseLocal, UseCloud, ConfirmAccount, RemoveLocal, RemoveLeftover, QuickCheck, SyncAll, StopJob };
struct CloudRequest {
    CloudRequestKind kind = CloudRequestKind::Sync;
    GameId game_id;
};
enum class Compatibility { Untested, Works, Playable, Broken };
enum class RowKind { Heading, Action, Toggle, Choice, Text, Game, Job, Volume, Profile, Status, Separator };

// The sign-in screen: the QR code, or the account name and password form and its Steam Guard steps.
enum class SignInStep { Qr, Password, Submitting, Approval, EmailCode, AuthenticatorCode };
struct SignInForm {
    SignInStep step = SignInStep::Qr;
    std::string account_name;
    std::string password;        // never saved, shown or snapshotted; cleared after submit, cancel or leaving
    std::string code;            // the Steam Guard code being typed
    std::string email_domain;    // where Steam sent the email code or approval
    bool code_allowed = false;   // Approval: a typed code works too
    bool code_is_email = false;  // the code Steam takes is the emailed one
    bool code_entry = false;     // Approval: the player chose to type a code
    bool code_rejected = false;  // the last code was wrong
    bool code_sent = false;      // waiting for Steam to check the code
    std::string error;           // why the last password sign-in failed
};
// A password as the screen shows it: one dot per character.
std::string mask_secret(std::string_view value);
struct SignInCredentials {
    std::string account_name;
    std::string password;
};

struct Account {
    std::string id;
    std::string display_name;
    std::string challenge_url;
    AuthState authentication = AuthState::SignedOut;
    std::int64_t challenge_expiry = 0;
    std::int64_t last_sync = 0;
    bool cache_available = false;
    std::string error;
    bool storage_full = false;  // Failed: the sign-in can't be saved until console storage has room
};

struct AccountUpdate {
    AuthState authentication = AuthState::SignedOut;
    std::string id;
    std::string display_name;
    std::string challenge_url;
    std::string error;
    bool clear_library = false;
    // Waiting and Failed: the sign-in screen's step and its Steam Guard prompt.
    SignInStep sign_in = SignInStep::Qr;
    std::string email_domain;
    bool code_allowed = false;
    bool code_is_email = false;
    bool code_rejected = false;
    // Authenticated by a sign-in (not a restore): whether it used the password, remembered for next time.
    std::optional<bool> signed_in_with_password;
    bool storage_full = false;  // Failed: console storage is full
    // Failed or SignedOut: the saved sign-in could not be restored (Steam out of reach, storage busy or
    // full, the session revoked), not a sign-in the player started. With another store's games, Playden
    // stays on (or goes to) Home and a notification gives the error.
    bool restoring = false;
};

struct LibraryUpdate {
    std::vector<playden::GameSummary> games;
    bool loading = false;
    std::string error;
    bool stale = false;
    bool cache_available = false;
    std::optional<std::int64_t> retrieved_at;
    Store source = Store::kSteam;  // the store whose games these are; other stores' games stay
};

struct PersistenceLoadToken {
    std::uint64_t id = 0;
    std::uint64_t baseline_revision = 0;
};

inline bool operator==(PersistenceLoadToken left, PersistenceLoadToken right) {
    return left.id == right.id && left.baseline_revision == right.baseline_revision;
}

struct Game {
    GameId id;
    std::string title;
    std::string description;
    std::string source;
    std::string artwork_source;
    std::string folded_title; // Derived once for production query/sort; never persisted.
    std::string cover_key;
    std::string hero_key;
    std::string logo_key;
    std::vector<std::string> genres;
    std::string controller_support;
    std::optional<std::uint64_t> download_bytes;
    std::optional<std::uint64_t> install_bytes;
    std::optional<std::int64_t> added_at;
    std::optional<std::int64_t> last_played;
    std::optional<std::uint64_t> playtime_minutes;
    std::optional<bool> windows_installable;  // from Steam app info; absent until fetched
    bool demo = false;  // a Steam demo; Sort and filters › Demos hides it
    bool art_pending = false;
    bool art_failed = false;
};

struct Collection {
    std::string id;
    std::string name;
    bool pinned = false;
    std::set<GameId> members;
};

struct UserLibrary {
    std::set<GameId> favorites;
    std::set<GameId> hidden;
    std::vector<Collection> collections;
    std::map<GameId, Compatibility> ratings;
    std::map<GameId, std::string> notes;
    std::string query;
    std::string scope = "all";
    std::string sort = "name";
    // The order "More" on Continue playing shows (most recently played first) while the Library,
    // or a game opened from it, is on screen. Never saved: `sort` stays the player's own choice.
    std::string view_sort;
    std::set<std::string> filters;
};

struct Installation {
    GameId game_id;
    std::string volume_id;
    std::uint64_t installed_bytes = 0;
    bool available = true;
    bool runtime_ready = true;
    bool saves_present = true;
};

struct Job {
    std::string id;
    GameId game_id;
    std::string kind = "install";
    int queue_order = 0;
    JobStage stage = JobStage::Download;
    JobState state = JobState::Queued;
    std::uint64_t completed_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::optional<std::uint64_t> bytes_per_second;
    std::optional<int> eta_seconds;
    std::string failure;
    JobStage retry_stage = JobStage::Download;
    std::string volume_id;
    int stage_ticks = 0;
    std::uint64_t reserved_bytes = 0;
    bool reservation_held = false;
    bool completion_applied = false;
    std::uint64_t held_bytes = 0;  // live: installed bytes already on the drive
    std::int64_t finished_at = 0;  // live: when a Downloaded job finished (Unix seconds), 0 unknown
    bool checking_licenses = false;  // live: a slow Steam license check before the download starts
    // A job that is not a game's (the Playden update): its title and the words for its stage and state.
    std::string title;
    std::string stage_text;
    std::string state_text;
};

struct Volume {
    std::string id;
    std::string label;
    std::uint64_t capacity_bytes = 0;
    std::uint64_t used_bytes = 0;
    std::uint64_t reserved_bytes = 0;
    bool available = true;
    bool writable = true;
    std::uint64_t base_used_bytes = 0;
    std::uint64_t base_reserved_bytes = 0;
    bool capacity_known = true;    // false when the storage query did not return capacity and free space
    std::uint64_t games_bytes = 0; // live: bytes of finished installs on this volume
};

struct ModalEntry {
    std::string id;
    std::string title;
    std::string opener_focus_key;
    std::vector<std::string> action_keys;
    int focused_action = 0;
    bool destructive = false;
    std::string body;  // explanatory text; empty uses the simulation's default wording
    std::string setting_id;  // Game settings picker: the catalogue setting being chosen
    std::string link;  // a web page the dialog shows as a QR code with its address (release notes)
};

struct Navigation {
    Route route = Route::Startup;
    Route detail_origin = Route::Library;
    std::optional<GameId> detail_id;
    std::vector<ModalEntry> modals;
    std::string focus_key = "startup-connect";
    std::map<std::string, int> scroll_offsets;
    GameId selected_game_id;
    int selected_index = 0;
    int home_shelf = 0;
    std::map<std::string, int> home_shelf_focus;
    bool library_rail_focused = false;
    bool settings_rail_focused = false;
    int library_grid_column = 0;
    // The Home/Library/Downloads/Saves/Settings bar has focus (Up from the top of a page), and
    // which tab is focused there: 0 Home, 1 Library, 2 Downloads, 3 Saves, 4 Settings.
    bool tabs_focused = false;
    int tab_focus = 0;
    // Saves tab: the filter (0 All, 1 Needs you, 2 In sync, 3 This console only, 4 Not installed)
    // and the game its details page shows.
    int saves_filter = 0;
    GameId save_id;
};

struct EditorState {
    bool active = false;
    std::string purpose;
    std::string value;
    std::string original;
    std::string opener_focus_key;
    std::size_t caret = 0;
    bool shifted = false;
    bool symbols = false;
    bool secret = false;    // a password: shown as dots, never snapshotted
    bool revealed = false;  // the keyboard's Show key; reset each time the editor opens
    std::uint64_t serial = 0;  // tells each opened editor apart (the PS5 offers each one to its system keyboard)
    bool system_keyboard = false;  // the PS5's keyboard is (about to be) open for it: Playden's own is not drawn
    int focus_row = 0;
    int focus_key = 0;
};

struct Notice {
    std::string id;
    std::string message;
    std::string action;
    bool persistent = false;
    int remaining_ticks = 0;  // simulation ticks; live mode counts wall-clock seconds while visible
    bool failure = false;     // drawn with the failure tone
    std::string detail;       // second line; when empty the action key is shown, as fixtures expect
    bool alert = true;        // a live failure waits in a dialog for OK; false keeps it a corner notification
};

struct ControllerState {
    float left_x = 0.0F;
    float left_y = 0.0F;
    float right_x = 0.0F;
    float right_y = 0.0F;
    float l2 = 0.0F;
    float r2 = 0.0F;
    bool cross = false;
    bool circle = false;
    bool square = false;
    bool triangle = false;
    bool l1 = false;
    bool r1 = false;
    bool options = false;
};

struct State {
    // Each store's account. Steam's has its own sign-in screens (sign_in, sign_in_password below).
    std::map<Store, Account> accounts;
    Account& account(Store store = Store::kSteam) { return accounts[store]; }
    const Account& account(Store store = Store::kSteam) const {
        static const Account none;
        const auto found = accounts.find(store);
        return found == accounts.end() ? none : found->second;
    }
    std::uint64_t account_generation = 1; // Invalidates account-scoped presentation resources.
    std::vector<Game> games;
    UserLibrary library;
    std::vector<Installation> installations;
    std::vector<Job> jobs;
    std::vector<Volume> volumes;
    std::string default_volume_id;  // live: where new installs go
    std::map<GameId, LiveCloudGame> cloud;  // live: Steam Cloud save state by game
    std::map<GameId, LiveRuntimeGame> runtime_games;  // live: per-game runtime state
    PreparationQueue preparation_queue;                 // live: games being prepared, waiting and done
    std::vector<EndedUninstall> ended_uninstalls;       // live: this session's, newest first; one stop per game
    // Queue: the section the player expanded ("downloads", "preparation"), "none" once they closed it;
    // empty until they choose.
    std::string queue_expanded;
    std::size_t runtime_leftover_requests = 0;          // live: unused settings requests to clean up
    LiveKitInfo kit;                                    // live: the runtime kit in use
    LiveSelfUpdate self_update;                         // live: Playden's own update
    std::vector<LiveKeptBottle> kept_bottles;           // live: saves kept from uninstalled games
    std::vector<LiveExternalGame> external_games;       // live: games playing in place (imported)
    std::vector<LiveImportCandidate> import_candidates; // live: folders on the drives that could be imported
    bool import_scanning = false;                       // live: Playden is looking for games to import
    LiveSavesJob saves_job;                             // live: the Saves tab's running check or Sync all
    std::map<GameId, std::uint64_t> save_sizes;         // live: Windows user folders of games without Steam Cloud
    Navigation navigation;
    EditorState editor;
    // The Steam account's sign-in screen and its preference: the last sign-in used the account name and password.
    SignInForm sign_in;
    bool sign_in_password = false;
    bool stay_signed_in = true;       // preference: keep Steam's sign-in token on the PS5 (else memory only)
    std::vector<Notice> notices;
    // Per-game settings: preset and player overrides (see runtime/game_settings.h).
    std::map<GameId, playden::runtime::GameSettingsRecord> game_settings;
    SessionState session = SessionState::Idle;
    RuntimeState runtime = RuntimeState::Unavailable;
    std::string runtime_problem;  // why games can't be prepared or played, in words a person can act on
    // Live: started without the root grant, so only the "Root access is missing" screen shows.
    bool root_access_missing = false;
    std::string root_access_detail;  // the startup error, shortened
    bool root_access_granted = false;  // the grant succeeded; its data in /data/playden failed
    LogState log_state = LogState::Ready;
    std::vector<std::string> log_lines;
    int log_offset = 0;
    std::string log_context;
    GameId session_game_id;
    std::string session_error;
    bool reduced_motion = false;
    bool title_store_suffix = false;  // PS5 titles named "<game> (Steam)"
    bool cloud_saves_auto = false;    // Cloud saves move before Play, after sessions and at startup (opt-in)
    bool beta_updates = false;
    // The language games are downloaded in, as Steam names its depot languages ("german").
    std::string content_language{playden::steam::kDefaultContentLanguage};
    bool downloads_slow_network = false;  // 8 download pieces at a time instead of 4 (Settings › Storage)
    bool controller_connected = true;
    ControllerState controller;
    bool persistence_available = true;
    bool library_syncing = false;
    bool startup_loading = false;
    bool library_loading = false;
    bool library_offline = false;
    bool screenshot_mode = false;
    bool quit_requested = false;
    bool quit_waiting = false;  // a quit to Home waits for a preparation; no new one starts meanwhile
    bool simulation_mode = true;
    std::int64_t clock_ticks = 0;
};

struct ViewRow {
    std::string key;
    RowKind kind = RowKind::Action;
    std::string title;
    std::string subtitle;
    std::string value;
    std::string badge;
    std::string action;
    GameId game_id;
    float progress = 0.0F;
    bool enabled = true;
    bool selected = false;
    bool dangerous = false;
    bool modified = false;  // Game settings: the player changed this value
    std::string section;  // Settings: the rail section the row belongs to
    std::string tone;     // Saves: the status colour (good, busy, attention, problem, quiet)
    std::string aside;    // Saves: the right-hand note (size, last sync)
    bool rail_only = false;  // Settings: the section's rail entry is the action (Quit Playden), no card
};

struct ViewData {
    std::string surface;
    std::string title;
    std::string subtitle;
    std::string eyebrow;  // a sheet's label above its title; empty: the sheet's place (view.cpp)
    std::vector<ViewRow> rows;
    std::vector<ViewRow> actions;
    struct Shelf {
        std::string key;
        std::string title;
        std::vector<ViewRow> items;
        int focused_item = -1;
    };
    std::vector<Shelf> shelves;
    std::vector<std::string> legends;
    std::string footer;  // a sheet's pinned footer note (Installed games: the selection or the batch)
    int focused_row = 0;
    int focused_action = 0;
    std::size_t total_count = 0;
    std::size_t first_index = 0;
    bool show_simulation_label = true;
    std::string tone;  // Saves details: the status colour
    // Preset chooser: the focused preset's name and hint, each setting it changes as
    // {setting, current value, value with the preset}, and a closing line.
    struct Comparison {
        std::string title;
        std::string hint;
        std::vector<std::array<std::string, 3>> rows;
        std::string note;
    };
    std::optional<Comparison> comparison;
};

// Characters a Details description page holds: live pages have status lines above it.
inline std::size_t DetailsDescriptionPage(bool simulation_mode) { return simulation_mode ? 300 : 200; }

// Player words for a preparation journal step ("copy-template" → "Copying the Windows environment").
std::string PreparationStepWords(const std::string& step);
// Where a preparation stands, for the player: "Step 3 of 6 · Copying the Windows environment".
// No record yet (the PS5 title is being reserved) is step 1.
inline constexpr int kPreparationSteps = 6;
int PreparationStepNumber(const std::string& step);
std::string PreparationProgressWords(bool has_record, const std::string& step);

// A job's state for the player ("Downloading", "Paused", "Complete"…): its own words when it has them
// (the Playden update), and "Copying" while an imported game is copied into Playden.
std::string JobStateText(const Job& job);

class Simulation {
public:
    Simulation(std::filesystem::path fixture_directory, std::filesystem::path profile_path,
               ModelMode mode = ModelMode::Simulation);

    bool scenario(const std::string& name);
    bool action(const std::string& logical_action);
    void controller_sample(ControllerState sample);
    void text(const std::string& utf8);
    // The PS5 system keyboard's accepted text replaces the open editor's value and commits it, as Done does.
    void SubmitEditorText(std::string utf8);
    void SetSystemKeyboardOpen(bool open) { state_.editor.system_keyboard = open && state_.editor.active; }
    // The PS5 offers every field to its system keyboard: fields open with Playden's keyboard hidden
    // until that one is refused (SetSystemKeyboardOpen(false)).
    void SetSystemKeyboardAvailable(bool available) { system_keyboard_available_ = available; }
    void tick(int ticks = 1);
    void SetControllerConnected(bool connected);
    void SetWallClock(std::int64_t unix_seconds);
    void UpdateAccount(AccountUpdate update);
    // Another store has a signed-in account or games: Steam's sign-in screen need not take over.
    [[nodiscard]] bool HasGamesBeyondSteam() const { return beyond_steam(); }
    bool UpdateLibrary(LibraryUpdate update);
    // Live mode: requests for the real install service, and its state for display.
    [[nodiscard]] std::vector<InstallRequest> TakeInstallRequests();
    void UpdateInstalls(const std::vector<LiveInstallJob>& jobs, const std::vector<LiveInstalledGame>& installed,
                        const std::vector<LiveCompletedDownload>& completed = {});
    // Live mode: Steam Cloud save requests for the cloud service, and its state for display.
    [[nodiscard]] std::vector<CloudRequest> TakeCloudRequests();
    void UpdateCloud(const std::vector<LiveCloudGame>& games);
    // Live mode: the Saves tab's running job, and the size of saves of games without Steam Cloud.
    void UpdateSavesJob(LiveSavesJob job);
    void UpdateSaveSizes(std::map<GameId, std::uint64_t> sizes);
    // Live mode: the answer to a Cloud request the player made (its settled result), or its refusal
    // (the game's title may run). Play waiting to quit to Home goes on from these.
    void NotifyCloudAnswered(GameId id, CloudStatus status);
    void NotifyCloudRefused(GameId id);
    // Live mode: games to prepare (bottle + PS5 title), and the runtime state for display.
    [[nodiscard]] std::vector<GameId> TakeRuntimePrepareRequests();
    // Queues Prepare without a button press: after a download, or before Play.
    void RequestRuntimePrepare(GameId id);
    [[nodiscard]] std::vector<GameId> TakeRuntimePlayRequests();
    [[nodiscard]] bool TakeRuntimeCleanupRequest();
    // Live mode: kept game data, deletions the player confirmed, and why an uninstall stopped.
    void UpdateKeptBottles(std::vector<LiveKeptBottle> kept);
    // Live mode: games playing in place, and what the drives offer to import.
    void UpdateExternalGames(std::vector<LiveExternalGame> games);
    void UpdateImportCandidates(std::vector<LiveImportCandidate> candidates, bool scanning);
    // Live mode: requests for the Local store (the Import sheet, the dev request file).
    void RequestImport(ImportRequest request);
    [[nodiscard]] std::vector<ImportRequest> TakeImportRequests();
    // Live mode: a request from TakeImportRequests that the Local store refused (an add, copy, move
    // or removal), with its sentence for the player: shown as a notice, and what waited on it dropped.
    void NotifyImportFailed(const ImportRequest& request, const std::string& message);
    // Writes the program an imported game starts with into its Game settings when the game has no
    // saved settings yet (just added); never over anything the player changed, a reset Program
    // included. Saved at once so the next preparation reads it. True when it was written.
    bool SeedGameProgram(GameId id, const std::string& program);
    [[nodiscard]] std::vector<GameId> TakeKeptDeleteRequests();
    // Live mode: maintenance the player confirmed (and settings to apply when the sheet
    // closes), and games whose title activity the sheet needs to know.
    [[nodiscard]] std::vector<std::pair<GameId, MaintenanceKind>> TakeMaintenanceRequests();
    [[nodiscard]] std::vector<GameId> TakeActivityRequests();
    void NotifyUninstallStopped(GameId id, const std::string& reason);
    // The uninstall is over: the game's PS5 title and files are gone. Queue lists it as done.
    void NotifyUninstallFinished(GameId id);
    // Play prepared the game first and that ended without a playable title.
    void NotifyPlayStopped(GameId id, const std::string& reason);
    // A preparation the player started left the game waiting for the PS5: ask, when
    // nothing else is open, whether to quit to the PS5 Home screen to play it.
    void NotifyReadyToPlay(GameId id);
    void SetRuntimeLeftovers(std::size_t count);
    void SetKitInfo(LiveKitInfo kit);
    void UpdateRuntime(RuntimeState runtime, const std::vector<LiveRuntimeGame>& games);
    // Live mode: the preparation queue for Queue, and what the player asked of it there: games to
    // prepare next, finished preparations dismissed, and Clear completed.
    void UpdatePreparationQueue(PreparationQueue queue);
    [[nodiscard]] std::vector<GameId> TakePrepareNextRequests();
    [[nodiscard]] std::vector<LiveCompletedPreparation> TakePreparedDismissals();
    [[nodiscard]] bool TakePreparedClear();
    // The game runtime could not start (raw: its log reason; empty: it runs). Shown on every screen
    // until it is fixed, with the reason under Settings › Runtime status (#64).
    void UpdateRuntimeProblem(const std::string& raw);
    // Live mode: a one-line note from startup's data layout move (a step held back this start).
    void ShowLayoutNotice(const std::string& text);
    // Live mode: startup got no root grant (raw: NativeAccessResult::error). Shows only the
    // "Root access is missing" screen; its one action quits to Home (quit_requested).
    // granted: the grant succeeded but Playden can't use /data/playden (other words, same screen).
    void ShowRootAccessMissing(const std::string& raw, bool granted = false);
    // Live mode: Playden's own update, and what the player asked of it: "check" (from Settings),
    // "install", "cancel", "restart" (apply at quit; quit follows), "rollback" (put the previous
    // version back at quit; quit follows) and "acknowledge" (a finished update's message was shown).
    void UpdateSelfUpdate(const LiveSelfUpdate& update);
    [[nodiscard]] std::vector<std::string> TakeSelfUpdateRequests();
    // Live mode: the folder holding install.log, auth.log and launch.log for the log viewer.
    void SetLogDirectory(std::filesystem::path directory);
    // Live mode: the drives installs can use and the one new installs go to.
    // default_volume_saved false: the drive choice couldn't be written and lasts until Playden closes.
    void UpdateVolumes(const std::vector<LiveVolume>& volumes, const std::string& default_volume_id,
                       bool default_volume_saved = true);
    // False on a console that cannot read free space at all: installs are then allowed without it.
    void SetFreeSpaceSupported(bool supported) noexcept { free_space_supported_ = supported; }
    // Live mode: Steam app-info estimates for one game (download/install size, installability).
    bool UpdateAppDetails(GameId id, std::optional<std::uint64_t> download_bytes,
                          std::optional<std::uint64_t> install_bytes, std::optional<bool> windows_installable);
    // Live mode: the Steam store's description, genres and controller support for one game;
    // kept across library refreshes.
    void UpdateStoreDetails(GameId id, std::string description, std::vector<std::string> genres,
                            std::string controller_support);
    PersistenceLoadToken BeginOrganizationLoad(std::uint64_t account);
    bool ApplyOrganization(std::uint64_t account, PersistenceLoadToken token,
                           std::optional<playden::OrganizationSnapshot> loaded);
    bool FailOrganizationLoad(std::uint64_t account, PersistenceLoadToken token,
                              playden::StorageError error);
    void OrganizationCleared(std::uint64_t account);
    PersistenceLoadToken BeginPreferencesLoad();
    bool ApplyPreferences(PersistenceLoadToken token,
                          const playden::PreferencesLoadResult& loaded);
    [[nodiscard]] playden::OrganizationSnapshot ExportOrganization() const;
    [[nodiscard]] playden::Preferences ExportPreferences() const;
    [[nodiscard]] std::uint64_t OrganizationAccount() const noexcept { return organization_account_; }
    [[nodiscard]] std::uint64_t OrganizationRevision() const noexcept { return organization_revision_; }
    [[nodiscard]] std::uint64_t PreferencesRevision() const noexcept { return preferences_revision_; }
    [[nodiscard]] bool OrganizationDirty() const noexcept;
    [[nodiscard]] bool PreferencesDirty() const noexcept;
    [[nodiscard]] bool OrganizationReadyForSave() const noexcept;
    void OrganizationSaveResult(std::uint64_t account, std::uint64_t revision,
                                playden::StorageError error);
    void PreferencesSaveResult(std::uint64_t revision, playden::StorageError error);
    bool TakePersistenceRetryRequest();
    [[nodiscard]] std::vector<ServiceRequest> TakeServiceActions();
    // The password sign-in's secrets, handed over once; the form keeps no copy of the password.
    [[nodiscard]] std::optional<SignInCredentials> TakeSignInCredentials();
    [[nodiscard]] std::optional<std::string> TakeGuardCode();
    // Presets and recipes for the Game settings sheet (runtime/presets.json, runtime/recipes/).
    void SetSettingsData(std::vector<playden::runtime::SettingsPreset> presets,
                         std::vector<playden::runtime::GameRecipe> recipes);
    void SetKitCapabilities(playden::runtime::KitCapabilities kit);
    // Live mode: where per-game settings are kept (loaded now, saved on every change).
    void SetGameSettingsFile(std::filesystem::path path);
    // The recipe and effective settings the sheet shows for a game.
    // Recipes the runtime generated from Steam launch options; a shipped recipe wins.
    void SetGeneratedRecipes(const std::vector<playden::runtime::GameRecipe>& recipes);
    // Live: each game's programs and, for a refused game, its draft recipe (RecipeFor uses it).
    void SetGameSetups(std::map<GameId, LiveGameSetup> setups);
    // Live: games whose folder the Program picker needs scanned.
    [[nodiscard]] std::vector<GameId> TakeProgramScanRequests();
    // Where "Export as recipe" writes recipe-<app id>.json, and the console's firmware for its metadata.
    void SetRecipeExportTarget(std::filesystem::path directory, std::string firmware);
    [[nodiscard]] const playden::runtime::GameRecipe& RecipeFor(GameId id) const;
    [[nodiscard]] bool EffectiveSettingsFor(GameId id, playden::runtime::EffectiveSettings* settings) const;
    // Whether Game settings lists "Known problems" (Try anyway) for a game.
    [[nodiscard]] bool known_problem(GameId id) const;
    void follow_refresh_rate(GameId game);
    [[nodiscard]] bool game_settings_row_shown(const playden::runtime::SettingDefinition& definition, GameId game) const;
    // The overrides a preset change or a reset keeps: the program choices (simulation.cpp).
    [[nodiscard]] std::map<std::string, std::string> kept_start_choices(GameId id) const;
    // Opens the picker, editor or "Change anyway" for one Game settings row.
    void open_setting(const std::string& id);
    // The actions of a row whose value the recipe starts (edit, reset, library override entries).
    [[nodiscard]] std::vector<std::string> setting_value_actions(const std::string& id) const;
    // "Export as recipe": the game's effective settings as recipe-<app id>.json for a pull request.
    void export_recipe(GameId id);

    [[nodiscard]] const State& state() const noexcept { return state_; }
    [[nodiscard]] const ViewData& view() const noexcept { return view_; }
    [[nodiscard]] const ViewData& route_view() const noexcept { return route_view_; }
    [[nodiscard]] std::vector<const Game*> visible_games() const;
    [[nodiscard]] std::string snapshot() const;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    std::filesystem::path fixture_directory_;
    std::filesystem::path profile_path_;
    ModelMode mode_ = ModelMode::Simulation;
    State state_;
    ViewData view_;
    ViewData route_view_;
    std::string last_error_;
    std::string pending_destructive_action_;
    std::vector<std::string> self_update_requests_;
    std::string self_update_prompted_;   // "available 0.3.0" or "staged 0.3.0": each prompt shows once
    std::string pending_update_prompt_;  // a prompt waiting for the open dialog to close
    bool kit_prompted_ = false;          // the kit was offered since its last check (a cancel doesn't offer it again)
    void show_update_prompt();
    void sync_self_update_job();
    void request_self_update_restart(const std::string& request = "restart");
    void close_game_modals();
    void open_release_notes(bool after_update);
    std::string selected_volume_id_;
    bool default_volume_saved_ = true;  // last install-service report, so a failed save is said once
    // Live: the drive chosen in the install sheet for this one install (empty: the default drive),
    // and whether the open storage picker chooses it rather than the default.
    std::string install_volume_id_;
    bool picking_install_volume_ = false;
    bool free_space_supported_ = true;
    std::vector<playden::runtime::SettingsPreset> settings_presets_;
    // Shipped and generated recipes by game (runtime::RecipeGame).
    std::map<GameId, playden::runtime::GameRecipe> recipes_;
    std::map<GameId, playden::runtime::GameRecipe> generated_recipes_;
    mutable std::map<GameId, playden::runtime::GameRecipe> simulated_recipes_;
    playden::runtime::KitCapabilities kit_;
    std::set<std::string> expanded_settings_sections_;
    std::string editing_setting_id_;
    bool editing_append_ = false;  // the editor adds one library override to the list
    std::map<GameId, LiveGameSetup> game_setups_;
    std::vector<GameId> pending_program_scans_;
    std::set<GameId> requested_program_scans_;
    std::filesystem::path recipe_export_directory_;
    std::string console_firmware_;
    std::filesystem::path game_settings_file_;
    std::map<GameId, playden::runtime::GameSettingsRecord> saved_game_settings_;
    bool settings_save_failed_ = false;
    bool settings_load_failed_ = false;
    std::optional<playden::runtime::GameSettingsRecord> settings_opened_record_;  // for the close toast
    std::string editing_target_id_;
    std::uint64_t challenge_generation_ = 0;
    std::uint64_t cancelled_challenge_generation_ = 0;
    std::uint64_t next_collection_serial_ = 1;
    std::uint64_t next_job_serial_ = 1;
    bool controller_seen_ = false;  // a connected controller has been reported this run
    std::vector<ServiceRequest> pending_service_actions_;
    std::optional<SignInCredentials> pending_credentials_;
    std::optional<std::string> pending_guard_code_;
    std::uint64_t editor_serial_ = 0;
    bool system_keyboard_available_ = false;
    std::vector<InstallRequest> pending_install_requests_;
    std::vector<ImportRequest> pending_import_requests_;
    std::vector<GameId> pending_runtime_prepare_requests_;
    std::vector<GameId> pending_prepare_next_requests_;
    std::vector<LiveCompletedPreparation> pending_prepared_dismissals_;
    bool pending_prepared_clear_ = false;
    // Asked of the runtime and not carried out yet: finished preparations dismissed, a game moved first.
    std::vector<LiveCompletedPreparation> prepared_dismissed_;
    std::optional<GameId> prepare_next_asked_;
    std::vector<GameId> pending_runtime_play_requests_;
    bool pending_runtime_cleanup_ = false;
    bool library_catalog_received_ = false;
    std::set<Store> catalog_sources_;  // stores whose library arrived at least once
    // The sign-in screen shows because Steam's saved sign-in failed to restore, not for a sign-in the player started.
    bool restore_failed_sign_in_ = false;
    std::set<Store> loading_sources_;  // stores whose library is loading now
    // Live install events become notifications; the first snapshot is restored history, not an event.
    bool live_installs_seen_ = false;
    std::map<GameId, JobState> live_job_states_;
    std::set<GameId> live_installed_ids_;
    std::set<GameId> pending_uninstalls_;
    std::vector<GameId> pending_kept_delete_requests_;
    std::vector<std::pair<GameId, MaintenanceKind>> pending_maintenance_requests_;
    std::vector<GameId> pending_activity_requests_;
    bool uninstall_delete_saves_ = false;  // the uninstall dialog's choice
    std::set<GameId> kept_selected_;  // the kept game data manager's checked games
    // Installed games (Settings > Storage): the checked games and the list's order.
    std::set<GameId> installed_selected_;
    bool installed_by_played_ = false;  // false: largest first; true: never played, then the oldest
    // Games uninstalled together from the Installed games list: Queue follows them, so they post
    // no notice of their own when they finish, and a stop is a corner notice, not a dialog.
    std::set<GameId> batch_uninstalls_;
    std::map<GameId, std::string> uninstall_titles_;  // names of games being uninstalled, for once they are gone
    // Steam games still being uninstalled when their account signed out: how they end is not listed.
    std::set<GameId> previous_account_uninstalls_;
    std::string installed_list_focus_;  // the game row the focus left for the footer
    std::string installed_message_;  // the list's note when a confirmed uninstall could not start
    bool rebuilding_overlay_ = false;  // rebuild_view is building the screen under an open sheet
    struct StoreFacts { std::string description; std::vector<std::string> genres; std::string controller_support; };
    std::map<GameId, StoreFacts> store_facts_;  // live: from the Steam store, applied over library refreshes
    std::vector<GameId> ready_prompts_;  // games to announce as ready to play, oldest first
    // settings_game: a game whose refusal Game settings can get past; the dialog offers it.
    struct AlertPrompt { std::string title; std::string body; GameId settings_game; };
    GameId alert_settings_game_;  // the game the open failure dialog's "Game settings" opens
    // Games refused at Prepare whose program or "Try anyway" the player changed since: their page
    // no longer shows the old refusal.
    std::set<GameId> rechosen_after_refusal_;
    // Launches that stopped before their store's library arrived (a start from Home reported when Playden
    // starts): told once the game's name is known.
    std::map<GameId, std::string> pending_play_stops_;
    void tell_play_stopped(GameId id, const std::string& reason);
    // Game settings asked for on a game not set up yet: it opens once preparing makes a draft or a title.
    std::set<GameId> settings_after_prepare_;
    void open_settings_after_prepare();
    std::vector<AlertPrompt> alert_prompts_;  // failures waiting for an OK, oldest first
    std::filesystem::path log_directory_;
    std::vector<CloudRequest> pending_cloud_requests_;
    std::map<GameId, CloudStatus> settled_cloud_;  // each game's last settled Cloud state, for notices
    GameId ready_prompt_game_;  // the game the "ready to play" dialog is about
    GameId quit_sync_game_;     // Play quits to Home once this game's saves are in step
    std::string quit_wait_title_;   // the dialog a quit to Home waits in while a game is prepared
    std::string live_log_topic_;
    GameId live_log_game_;
    bool details_primary_pending_ = false;  // live Details: move focus to the first action once known
    // Settings: the last focused row, so focus stays nearby when that row goes away.
    int settings_focus_index_ = 0;
    int queue_focus_index_ = 0;  // Queue: the focused row's place, for when that row goes away
    bool queue_shown_ = false;   // Queue was on screen at the last rebuild
    std::string settings_focus_section_;

    struct OrganizationJournalEntry {
        playden::OrganizationSnapshot before;
        playden::OrganizationSnapshot after;
        bool reset = false;
        bool selected = false;
        std::size_t bytes = 0;
    };
    std::uint64_t next_persistence_load_token_ = 0;
    std::uint64_t organization_account_ = 0;
    std::uint64_t organization_revision_ = 0;
    std::uint64_t organization_saved_revision_ = 0;
    std::uint64_t preferences_revision_ = 0;
    std::uint64_t preferences_saved_revision_ = 0;
    PersistenceLoadToken organization_load_token_;
    PersistenceLoadToken preferences_load_token_;
    bool organization_load_pending_ = false;
    bool organization_load_failed_ = false;
    bool organization_save_failed_ = false;
    bool organization_journal_full_ = false;
    bool organization_save_suppressed_ = false;
    bool preferences_load_pending_ = false;
    bool preferences_load_may_apply_ = false;
    bool preferences_load_failed_ = false;
    bool preferences_save_failed_ = false;
    bool persistence_retry_requested_ = false;
    bool force_organization_reset_ = false;
    bool force_organization_selection_ = false;
    bool force_preferences_reset_ = false;
    std::size_t organization_journal_bytes_ = 0;
    std::vector<OrganizationJournalEntry> organization_journal_;
    playden::OrganizationSnapshot captured_organization_;
    playden::OrganizationSnapshot persisted_organization_;
    playden::Preferences captured_preferences_;

    void load_fixtures();
    void load_profile();
    bool save_profile();
    void reset_default_state();
    void rebuild_view();
    void activate();
    void cancel();
    void move(int delta_x, int delta_y);
    void move_content(int delta_x, int delta_y);
    [[nodiscard]] bool tab_bar_shown() const;
    bool move_settings(int delta_x, int delta_y);
    bool move_saves(int delta_x, int delta_y);
    // The Saves tab's games, in name order, after the filter when `filtered`.
    [[nodiscard]] std::vector<GameId> save_games(bool filtered) const;
    [[nodiscard]] std::vector<GameId> saves_awaiting_account() const;
    void ask_upload_all();
    void open_saves();
    void settle_quit_sync(GameId id, std::optional<CloudStatus> status);
    void quit_to_home();
    void change_route(int delta);
    void open_modal(ModalEntry modal);
    void prepare_next(GameId id);
    void close_modal();
    void start_editor(std::string purpose, std::string value, bool secret = false);
    // Sign-in screen: opened in the remembered method, its actions, and the form's secrets wiped.
    void open_sign_in(bool reauthenticate = false);
    bool sign_in_action(const std::string& action_name);
    void submit_fixture_sign_in();
    void clear_sign_in_secrets();
    void set_route_focus(std::string key);
    // Steam Guard codes: five characters, uppercase.
    void normalize_editor();
    void commit_editor();
    void advance_jobs();
    [[nodiscard]] std::vector<std::string> game_settings_actions() const;
    [[nodiscard]] std::vector<std::string> kept_manager_actions() const;
    // The Import sheet: Look for games, then the candidates grouped by drive (headings never take
    // the focus), then Close. Opened from Startup, Home's empty state and Settings › Library.
    [[nodiscard]] std::vector<std::string> import_sheet_actions() const;
    [[nodiscard]] const LiveImportCandidate* import_candidate(const std::string& key) const;
    void open_import_sheet();
    void refresh_import_sheet();
    void move_import_sheet(int delta);
    void look_for_games();
    std::string import_programs_key_;  // the candidate whose program list is open
    // The import wizard. Step 1 (surface import-choice), for the program picked in the open program
    // list: play the game from its folder, or Import into Playden. Step 2 (surface import-storage):
    // the drive the game goes to, for that program's folder or for a game already added
    // (import_choice_game_ valid: its page's Import into Playden opens step 2 directly).
    void open_import_choice(std::size_t program);
    void open_import_storage(GameId game = {});
    void choose_import(const std::string& action);
    void choose_import_volume(const std::string& volume_id);
    // Sends the add of the open program list's folder with the transfer to start once it has its
    // game, and returns to the Import sheet.
    void add_import_candidate(const std::string& key, std::optional<ImportRequestKind> then, const std::string& volume_id);
    // What the wizard imports: the candidate (step 1 from the program list) or the game added
    // already, its size and the volume its files are on now.
    struct ImportSubject {
        const LiveImportCandidate* candidate = nullptr;
        std::string title;
        std::string volume;
        std::uint64_t bytes = 0;
        bool ascii_ok = true;
    };
    [[nodiscard]] ImportSubject import_subject() const;
    // Why a drive can't take the game in step 2 (empty: it can), and its words there.
    [[nodiscard]] std::string import_volume_blocker(const ImportSubject& subject, const Volume& volume) const;
    // An add sent to the Local store waits for its game: then the copy or move chosen with it is sent
    // and its page opens.
    struct PendingImport {
        std::optional<ImportRequestKind> then;  // CopyIntoPlayden or MoveIntoPlayden
        std::string volume_id;                  // the copy's install volume
        Route from = Route::Startup;            // where the Import sheet was open
    };
    void settle_pending_imports();
    void start_import_transfer(GameId id, ImportRequestKind kind, const std::string& volume_id);
    void open_game_page(GameId id);
    // Imported games playing in place from their folder (State::external_games), and a copy running.
    [[nodiscard]] const LiveExternalGame* external_game(GameId id) const;
    [[nodiscard]] const Job* import_copy_job(GameId id) const;
    // A game whose setup waits for its program (LiveGameSetup::steam_choices): the sheet that asks,
    // installers and tools last, and the choice, which sets the game up again.
    void open_steam_programs(GameId id);
    void choose_steam_launch(GameId id, const std::string& program);
    std::vector<LaunchProgram> steam_programs_shown_;
    std::set<GameId> steam_programs_asked_;  // games whose waiting setup asked once already
    GameId steam_programs_game_;
    std::size_t import_program_index_ = 0;
    bool import_program_chosen_ = false;  // the program was clear (ClearGameProgram): its list was skipped
    GameId import_choice_game_;
    std::map<std::string, PendingImport> pending_imports_;  // by candidate key
    // Live: a copy or move asked for whose job (or finished move) isn't listed yet; its page shows it starting.
    std::map<GameId, ImportRequestKind> import_transfers_starting_;
    // Why the latest add of a folder was refused, by candidate key: its row on the Import sheet says it
    // (a dialog waits until the sheet closes).
    std::map<std::string, std::string> import_failures_;
    // Installed games list: its games in the chosen order (those a running batch uninstalled
    // included), why a game cannot be picked now (empty when it can), and the list's refresh.
    [[nodiscard]] std::vector<GameId> installed_list() const;
    [[nodiscard]] std::string installed_blocker(GameId id) const;
    [[nodiscard]] std::vector<std::string> installed_manager_actions() const;
    void refresh_installed_manager();
    // Asks the controller to uninstall a game; batch: from the Installed games list.
    void request_uninstall(GameId id, bool delete_saves, bool batch);
    void end_uninstall(GameId id, const std::string& stopped);
    [[nodiscard]] const EndedUninstall* uninstall_stop(GameId id) const;
    void move_installed_manager(int delta_x, int delta_y);
    [[nodiscard]] std::vector<std::string> collection_sheet_actions(const std::string& sheet) const;
    void refresh_collection_sheets(const std::string& focus = {});
    void refresh_game_settings();
    bool can_change_game_settings();
    void set_game_setting(const std::string& id, const std::string& value);
    void announce_settings_saved();
    // The game's Game settings and maintenance can change it: in live mode, its title is prepared.
    [[nodiscard]] bool game_prepared(GameId id) const;
    // Whether a setting or action that needs the game closed can run: false while the game
    // runs or the console has not said; reason says why.
    [[nodiscard]] bool game_closed(GameId id, std::string* reason = nullptr) const;
    [[nodiscard]] std::vector<std::string> maintenance_actions() const;
    // The game page's More menu: Game settings and Maintenance first for a prepared game.
    [[nodiscard]] std::vector<std::string> details_context_actions() const;
    void request_maintenance(GameId id, MaintenanceKind kind);
    // Rows the preset chooser compares: Current → This preset for the focused preset.
    void build_preset_comparison(GameId id, const std::string& preset_id);
    [[nodiscard]] const playden::runtime::SettingsPreset* preset_for(GameId id) const;
    // The kit's capabilities as the game's PS5 title can use them: a title still on an earlier kit's runtime (Update
    // pending) gets no 120 Hz output until it is updated (runtime::KitForTitleRuntime); the rows wait for it.
    [[nodiscard]] playden::runtime::KitCapabilities kit_for(GameId id) const;
    void start_logs();
    void finish_logs();
    void queue_service_action(ServiceAction action, Store store = Store::kSteam);
    // Another store has a signed-in account or games: then Playden opens on Home without Steam.
    [[nodiscard]] bool beyond_steam() const;
    // Steam's sign-in ended without an account: Home when another store has games, else Startup.
    void leave_steam_sign_in();
    // A notification for Steam's saved sign-in that failed while another store's games show.
    void steam_restore_notice();
    void clear_account_specific_state();
    void apply_organization_snapshot(const playden::OrganizationSnapshot& snapshot);
    void reconcile_applied_organization_selection(GameId prior_selected_id);
    bool record_live_persistence();
    void update_persistence_notice();
    void apply_live_volume_usage();
    void push_notice(Notice notice);
    void show_ready_prompt();
    void show_alert(std::string title, std::string body, GameId settings_game = {});
    void open_game_settings();
    void show_alert_prompt();
    void open_live_log(const std::string& topic, GameId game_id);
    void load_live_log();
    [[nodiscard]] bool live_notices_visible() const noexcept;
    [[nodiscard]] bool live() const noexcept { return mode_ == ModelMode::Live; }
    [[nodiscard]] const std::string& install_target() const noexcept {
        return install_volume_id_.empty() ? selected_volume_id_ : install_volume_id_;
    }
    [[nodiscard]] std::string next_job_id();
};

} // namespace playden::homebrew
