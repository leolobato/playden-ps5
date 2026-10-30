#pragma once
// runtime_host.hpp - Worker-owned composition of the per-game runtime services.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "platform/ps5/native_coordinator.h"
#include "platform/ps5/title_registry.h"
#include "platform/ps5/title_runtime.h"
#include "runtime/game_install.h"
#include "runtime/game_settings.h"
#include "runtime/install_service.h"
#include "runtime/launch_service.h"
#include "runtime/leftover_titles.h"
#include "runtime/package_installer.h"
#include "runtime/redist_install.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include "runtime/layout_migration.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace playden::homebrew {

// An admitted kit: the verified view and the capabilities its manifest declares.
struct RuntimeKit {
  runtime::VerifiedKitView view;
  std::vector<std::string> capabilities;
  std::string manifest_sha256;
  std::string expected_manifest_sha256;  // the kit this build was tested with
  std::string prospero_win_revision;     // what its PS5 titles were built from
  bool source_dirty = false;
};

// A game's private bottle kept after its PS5 title was removed: its saves and
// settings, until the game is prepared again or the player deletes it.
struct KeptBottleView {
  std::string install_id;
  std::uint32_t runtime_number = 0;
  GameId game;               // from the marker; one without "store"/"game" is Steam(runtime_number)
  std::string name;          // the game's name when it was kept
  std::int64_t kept_at = 0;  // Unix time
  std::uint64_t bytes = 0;   // size when it was kept
  std::string data_root;     // the kept bottle's folder (its library/prefixes/game holds the saves)
};

// What the one-shot title remover reported: whether it was sent, its result code
// when it answered, and a diagnostic.
struct TitleRemoverResult {
  bool sent = false;
  std::optional<int> code;
  std::string detail;
};

struct RuntimeHostConfig {
  // Existing trusted parent of state_root (for example /data).
  std::filesystem::path boundary;
  // Private 0700 OWNER state: installs.json and requests/ (settings requests).
  std::filesystem::path state_root;
  std::filesystem::path homebrew_root = "/data/homebrew";
  // Host-test namespace prefix for console paths; empty on the console.
  std::filesystem::path host_namespace_prefix;
  // Empty members use the native SQLite inventory and process activity readers.
  ps5::NativeCoordinatorOperations native;
  ps5::TitleRegistry::Transport registry_transport;
  // Verifies the installed kit on the worker, or returns nothing with a reason.
  std::function<std::optional<RuntimeKit>(std::string* reason)> admit_kit;
  // Package layer paths and base operations (hashing, sync, space, bottle
  // validation). The host adds cancellation.
  runtime::PackageInstallPaths package_paths;
  runtime::PackageInstallerOperations package_operations;
  // Bundled recipes/presets/input presets (<title>/runtime).
  std::string runtime_dir;
  // gbe_fork's steam_api DLLs; empty: <runtime_dir>/steamworks.
  std::string steamworks_dir;
  // The player's saved settings for a game; nothing means Playden defaults.
  std::function<std::optional<runtime::GameSettingsRecord>(const GameId& game)> settings_record;
  // Canonical lower-case UUID for a publication transaction, or nothing when no
  // secure random source is available. Empty uses /dev/urandom.
  std::function<std::optional<std::string>()> uuid;
  std::function<std::int64_t()> unix_time;
  // Diagnostic lines (no credentials): startup, each preparation step and refusals.
  std::function<void(const std::string&)> log;
  // Held around startup and each batch of work on the worker: while the token lives, nothing
  // else may raise root, which is process-wide and would fail the worker's ownership checks
  // (ps5::HoldEffectiveUid). Empty holds nothing.
  std::function<std::shared_ptr<void>()> hold_effective_uid;
  // hold_effective_uid without waiting, for RuntimeNumberFor's callers that would rather come back
  // later than wait for a batch: sets *busy and returns nothing when another thread holds it
  // (ps5::TryHoldEffectiveUid). Empty: RuntimeNumberFor waits, with hold_effective_uid.
  std::function<std::shared_ptr<void>(bool* busy)> try_hold_effective_uid;
  // Runs work with root raised (ps5::WithRoot), as UpdateConfig::as_root; used on the worker,
  // whose batch hold keeps other threads from raising root meanwhile. Empty: folders can't be fixed.
  std::function<bool(const std::function<bool()>& work)> as_root;
  // Console folders Prepare, Repair and Play check (and fix with as_root) before writing, with the
  // game's root and working folder (runtime/writable_folders.h). Missing ones are skipped.
  std::vector<std::string> writable_folders;
  // Receives each prepared title's newest wrapper session log (bounded tail) at
  // startup. The wrapper writes it inside the private bottle tree, which only
  // the app user can read.
  std::function<void(const std::string& title_id, const std::string& text)> title_log;
  // The game's PS5 title icon: a 512x512 PNG made from its store artwork, or
  // nothing (the kit's icon stays). Runs on the worker; may use the network.
  // Named by the game, so a runtime number never reaches a store's artwork.
  std::function<std::optional<std::string>(const GameId& game)> title_icon;
  // The DLC of a Steam game this account owns, from the Library's license read and app lookups
  // (EntitledLibraryBackend::DlcOf). Each prepare adds them to what the download recorded, so DLC past
  // the 64 that Steam's listofdlc names, or bought after the download, reach the emulator. Empty: none.
  std::function<std::vector<std::uint32_t>(std::uint64_t steam_id, std::uint32_t app_id)> owned_dlc;
  // One ranged HTTPS GET for what Playden downloads for a game's bottle: Microsoft's Visual C++ redistributable,
  // for a game whose files import MFC (runtime/redist_install.h). Runs on the worker, only for work the player
  // started (Play, Prepare, maintenance), never at start-up; it should give up soon after cancel turns true
  // (Shutdown). Empty: nothing is downloaded, and such games start without MFC.
  std::function<std::optional<std::string>(const std::string& url, std::uint64_t offset, std::uint64_t length,
                                           const std::atomic<bool>& cancel, std::string* error)>
      fetch_range;
  // The redistributables MFC comes from, by architecture; kept under <console data>/playden/redist.
  std::vector<runtime::VcRedist> vc_redists = runtime::MicrosoftVcRedists();
  // How long a failed download is not tried again (a preparation runs its steps more than once).
  std::chrono::seconds redist_retry_wait{60};
  // Native start of a prepared title with its arguments ("session=<UUID>").
  // Called only on the application thread, from Handoff. Empty disables Play.
  std::function<ps5::TitleResult(const std::string& title_id, const std::vector<std::string>& arguments)> launch_title;
  // Unregisters a game's PS5 title through the one-shot title remover (worker
  // only; bounded). The nonce ties its answer to this request. Empty disables
  // removal of PS5 titles.
  std::function<TitleRemoverResult(const std::string& title_id, const std::string& nonce)> unregister_title;
  // Replaces the root-owned allow-list (runtime::kTitleRuleAllowList) that a kit's any-title
  // helper reads with these bytes: one title ID per line (worker only). Empty: a kit with one
  // shell for every game can't prepare games.
  std::function<bool(const std::string& contents, std::string* error)> write_title_allow_list;
  // How long removal waits for the console to drop an unregistered title.
  int removal_confirm_polls = 30;
  std::chrono::milliseconds removal_poll_interval{1000};
  // Canonical /data as this process sees it, for the wrapper's session records.
  std::filesystem::path session_data_root = "/data";
  std::function<std::int64_t()> now_ms;
  // How long a started title may take to report its session before Play gives up.
  std::int64_t launch_evidence_wait_ms = 60000;
  // The 0.6 layout's games move (runtime/layout_migration.h), finished on the worker.
  // moved_games_root: the old and new games root (console paths) while the journal is at
  // "games-moved"; nothing otherwise. finish_games_move: called once every moved Ready game is
  // republished; false keeps the move pending for the next start. Empty: LoadLayoutJournal and
  // MarkLayoutDone on state_root.
  std::optional<runtime::LayoutPaths> layout_paths;
  runtime::LayoutOps layout_operations;
  std::function<std::optional<std::pair<std::string, std::string>>()> moved_games_root;
  std::function<bool(std::string* error)> finish_games_move;
  // SQLite and the native readers run on this worker. PS5 std::thread stacks are
  // too small for them, so the worker uses an explicit stack size.
  std::size_t stack_bytes = 1024 * 1024;
};

struct RuntimeInstallView {
  std::string install_id;
  std::uint32_t runtime_number = 0;
  GameId game;
  std::string title_id;
  // The game folder the title starts (console path). When the game is installed again on another
  // drive it no longer matches, and Prepare makes the title again.
  std::string install_path;
  // Its title is being removed to follow the game to another drive; Prepare finishes that.
  bool moving = false;
  // The game's private bottle (console path); Cloud saves under Windows
  // folders live here.
  std::string prefix_root;
  runtime::PreparationState state = runtime::PreparationState::kDownloaded;
  std::string step;     // current journal step, if any
  std::string failure;
  // A durable journal is present while no preparation runs. Recovery is
  // explicit; startup never resumes it.
  bool recovery_pending = false;
  // Staged and its registration submitted: ShadowMountPlus registers it once no
  // title runs, so the game is ready to play from the PS5 Home screen.
  bool awaiting_registration = false;
  // False when a Ready title was made from an earlier kit; Prepare updates it.
  bool title_current = true;
  // A Ready game whose files an update changed since its launch was set up (the update's marker in its
  // folder): Play refuses it until Prepare has made its launch again from the new files and published it.
  bool files_changed = false;
  // Whether the title runs the installed kit's runtime (its files and artifacts are the kit's). Unlike title_current,
  // a new game name or icon does not count. Settings only the newer runtime takes (120 Hz output) wait for Update.
  bool runtime_current = true;
  // A setup that stopped before Ready (Failed, Unavailable or Downloaded) left a title or bottle made with
  // another kit: Try again can't finish over it, and Prepare refuses. Recreate bottle (remove, then prepare)
  // does, keeping saves and settings.
  bool setup_other_kit = false;
  // Publication admission result for Ready titles ("ok" or the refusal); a
  // refusal means the next start would be refused, and Prepare repairs it.
  std::string admission = "ok";
  // The settings revision its published profile was made from, and the launch entry the
  // title was prepared with (a different Launch option needs the bottle made again).
  std::uint64_t settings_revision = 0;
  std::string launch_entry_id;
  std::string launch_path;               // its program, relative to the install root
  std::string launch_working_directory;  // its folder; empty: the program's own, or the install root:
  bool launch_in_install_root = false;    // a Steam entry that names no folder starts there
  // Save backups kept for the game, and when the newest was made (Unix time).
  std::size_t save_backups = 0;
  std::int64_t latest_save_backup = 0;
  std::string failure_code;  // Recorded preparation failure; the message remains available in diagnostics.
};

// Work on a prepared game's files, run while its PS5 title is stopped.
enum class RuntimeMaintenance {
  kApplySettings,     // publish the saved settings when they are newer than the title's
  kClearShaderCache,  // delete DXVK's pipeline cache for the launch program
  kRepair,            // fix the bottle's start files, then apply the settings
  kBackupSaves,       // copy the bottle's Windows user folders aside
};

// The newest maintenance request for one game.
struct RuntimeMaintenanceView {
  RuntimeMaintenance action = RuntimeMaintenance::kApplySettings;
  bool running = false;
  bool ok = false;
  std::string message;  // the outcome, for the player
};

// Play: the launch in progress, or how the last one ended.
struct RuntimeLaunchView {
  std::string install_id;
  std::uint32_t runtime_number = 0;
  GameId game;
  std::string title_id;
  runtime::LaunchState state = runtime::LaunchState::kIdle;
  std::string code;
  std::string message;
  std::optional<int> exit_code;
};

enum class RuntimeHostStatus { kStarting, kUnavailable, kReady, kStopped };

// A program one of Steam's launch options names, found in the game's folder.
struct SteamLaunchChoice {
  std::string path;     // as Steam names it, '/' separated
  bool helper = false;  // an installer or tool (the import rules)
};

// What Game settings needs to pick a game's program: the Windows programs in its folder and,
// for a game Playden could not prepare, a draft recipe to start the sheet from.
struct RuntimeGameSetup {
  std::vector<runtime::GameProgram> programs;
  bool scanned = false;  // programs is the folder's latest scan
  // Set when preparing was refused: the recipe the player's choices start from (it may list
  // no launch entry yet) and whether a choice (a program, "Try anyway") can get past the refusal.
  std::optional<runtime::GameRecipe> draft;
  bool refusal_soft = false;
  // Steam's options are all for other branches and more than one of their programs is in the game's
  // folder: preparing waits for the player to choose one (Game settings' steam_launch).
  std::vector<SteamLaunchChoice> steam_choices;
};


// The Steam app ID a game's Steamworks emulator is given (RuntimeSnapshot::steamworks_apps).
struct SteamworksAppView {
  enum class Source {
    kNone,      // none: the game keeps its own Steam files ("Steam features" acts as "game")
    kStore,     // a Steam game's own app ID
    kGameFile,  // the game's steam_appid.txt
    kSettings,  // the "Steam app ID" in Game settings
  };
  std::uint32_t app_id = 0;  // 0 with kNone
  Source source = Source::kNone;
  // Game settings names another app ID, which the game's own steam_appid.txt overrides.
  bool overridden = false;
  bool operator==(const SteamworksAppView&) const = default;
};

// A preparation that ended Ready. Queue lists it after the waiting games until the player dismisses it;
// a dismissal names the finish time, so preparing the game again lists it again.
struct PreparedGame {
  std::uint32_t runtime_number = 0;
  std::int64_t finished_at = 0;  // Unix seconds
  bool operator==(const PreparedGame&) const = default;
};
inline constexpr std::int64_t kPreparedDays = 7;
inline constexpr std::size_t kMaxPrepared = 20;

struct RuntimeSnapshot {
  std::uint64_t revision = 0;
  RuntimeHostStatus status = RuntimeHostStatus::kStarting;
  std::string reason;  // why the runtime services are unavailable
  bool kit_available = false;
  std::string kit_reason;  // why preparation is unavailable
  std::optional<RuntimeKit> kit;  // the admitted kit, for display
  std::vector<RuntimeInstallView> installs;
  // False while the PS5 title records could not be read (after a failed write): `installs` is then
  // empty, which does not mean that no game has a PS5 title.
  bool installs_known = true;
  // Below, games are named by runtime number (RuntimeHost::RuntimeNumberFor): a Steam game's app ID.
  // The games of the other numbers, for the fields that carry no GameId.
  std::map<std::uint32_t, GameId> runtime_games;
  // Latest refused or failed request per runtime number, for presentation, and how many
  // preparation requests the worker has finished.
  std::map<std::uint32_t, std::string> refusals;
  std::uint64_t prepares_handled = 0;
  std::optional<std::uint32_t> preparing_app;  // the game being prepared now
  // Preparations that ended Ready at most kPreparedDays ago and were not dismissed, newest first, at most
  // kMaxPrepared; only games whose title is still Ready.
  std::vector<PreparedGame> prepared;
  // Games whose preparation is asked for but not started yet, in the order they will start: each
  // waits behind another game's preparation, or for the runtime to finish starting.
  std::vector<std::uint32_t> queued_prepares;
  // How far the current copy or deletion (including a leftover title) is, when
  // that is measurable; absent for steps whose progress is unknown.
  std::optional<float> work_progress;
  std::optional<RuntimeLaunchView> launch;
  // Settings requests no install record's journal names: left by an interrupted
  // preparation, a refused request or a finished publication. They grant nothing.
  std::size_t leftover_requests = 0;
  // Answers to CheckActivity: the title's activity per runtime number, with the
  // check's serial so a caller can tell a fresh answer from an older one.
  std::map<std::uint32_t, std::pair<runtime::NativeActivity, std::uint64_t>> activity;
  std::uint64_t activity_serial = 0;
  // Play requests the worker has finished handling (accepted or refused).
  std::uint64_t plays_handled = 0;
  // Latest refused Play per runtime number, for presentation.
  std::map<std::uint32_t, std::string> play_refusals;
  // A checked session waits for Handoff on the application thread.
  bool handoff_ready = false;
  // Recipes Playden generated (from Steam's launch options or the game's files), for the settings sheet.
  std::vector<runtime::GameRecipe> generated_recipes;
  // Program choices per runtime number, from ScanPrograms and from preparation.
  std::map<std::uint32_t, RuntimeGameSetup> game_setups;
  // Removal: the game whose PS5 title is being removed now, the latest refusal or
  // failure per runtime number, and how many removal requests the worker has finished.
  std::optional<std::uint32_t> removing_app;
  std::map<std::uint32_t, std::string> removal_failures;
  std::uint64_t removals_handled = 0;
  // Game data kept when a game was uninstalled (bottles with no install record).
  std::vector<KeptBottleView> kept_bottles;
  // Unrecorded PS5 titles, and their queued/running deletion or latest failure (keyed by title ID).
  std::vector<runtime::LeftoverTitle> leftover_titles;
  std::vector<std::string> queued_leftover_deletes;
  std::optional<std::string> deleting_leftover_title;
  std::string deleting_leftover_step;
  std::map<std::string, std::string> leftover_delete_failures;
  std::uint64_t leftover_deletes_handled = 0;
  // Maintenance per runtime number, and how many requests the worker has finished.
  std::map<std::uint32_t, RuntimeMaintenanceView> maintenance;
  std::uint64_t maintenance_handled = 0;
  // Bytes in each game's Windows user folders (installed and kept), from the latest MeasureSaves.
  std::map<std::uint32_t, std::uint64_t> save_bytes;
  // What Play does before the launch is requested, per runtime number, while it runs: a game whose
  // folder moved in Playden 0.6 is republished first (kPlayUpdatingForLayout).
  std::map<std::uint32_t, std::string> play_progress;
  // What the latest Play from Playden went ahead without, per runtime number (Microsoft's MFC that could not be
  // downloaded): Play still starts the game, and the player sees why it may stop.
  std::map<std::uint32_t, std::string> play_notices;
  // The Steam app ID each game from another store was last prepared with, by runtime number. Steam
  // games are not listed: theirs is their own.
  std::map<std::uint32_t, SteamworksAppView> steamworks_apps;
};

// Whether the game's preparation waits in the runtime's queue.
inline bool PrepareQueued(const RuntimeSnapshot& snapshot, std::uint32_t runtime_number) {
  return std::find(snapshot.queued_prepares.begin(), snapshot.queued_prepares.end(), runtime_number) !=
         snapshot.queued_prepares.end();
}

inline constexpr const char* kPlayUpdatingForLayout = "Updating this game for Playden 0.6…";
inline constexpr const char* kPlayDownloadingMfc = "Downloading Microsoft's MFC for this game…";
inline constexpr const char* kPlayMfcNotDownloaded = "Microsoft's MFC could not be downloaded. Play again to retry.";
inline constexpr const char* kPlayMfcNotInstalled = "Microsoft's MFC could not be installed for this game.";

// A downloaded game to prepare. The game files stay at install_path.
struct RuntimePrepareRequest {
  // From RuntimeHost::RuntimeNumberFor(game).
  std::uint32_t runtime_number = 0;
  std::string install_path;  // console path of the Steam install
  std::string volume_id;
  std::string title_name;    // the game's name, shown for its PS5 title
  // Steam's Windows launch options (label = Steam's description), used when Playden
  // ships no recipe for the game. launch_reason says why they are missing.
  std::vector<runtime::LaunchEntry> launches;
  std::string launch_reason;
  // No option is for the public branch: `launches` are every branch's, and Steam doesn't say
  // which program starts the game.
  bool launches_any_branch = false;
  // The game; a default one stands for the Steam game runtime_number names.
  GameId game;
};

// An imported game's folder becoming its folder in Playden ("Copy into Playden" once the copy is
// verified, "Move into Playden" for a drop-folder game): RuntimeHost::Relocate.
struct RuntimeRelocation {
  std::uint32_t runtime_number = 0;  // 0: the game has no number yet, so no PS5 title
  std::string to;                    // console path of the game's folder in Playden
  std::string volume_id;             // the install volume of `to`
  // The move itself, run on the worker under the game's publication lease once its title is
  // stopped (and at once for a game without a title): empty, or why not. It must be safe to run
  // again after it succeeded (a restart in between). For a copy it copies again the files
  // changed since they were copied. Empty: the files are at `to` already.
  std::function<std::string()> move_files;
};

// What RuntimeHost::Relocate answers once Playden is closing (BeginClosing).
inline constexpr const char* kRelocationClosing = "Playden is closing.";

struct RuntimeRelocationResult {
  enum class Outcome {
    kDone,    // the game plays from `to` (its record names it; a marked republish may still wait)
    kBusy,    // nothing changed: its title runs or is unknown, or the runtime is busy; try again
    kFailed,  // nothing changed, or the files moved but the record could not follow (try again)
  };
  Outcome outcome = Outcome::kFailed;
  std::string message;  // for the player
  // The moved publication could not be verified: the old PS5 title was removed keeping its saves,
  // and the next Play prepares the game again where it is now.
  bool prepare_again = false;
};

// The worker constructs and owns every runtime collaborator, so durable work and
// native inventory never run on the main thread. The main thread reads snapshots
// and queues requests. Play is split the same way: the worker checks the game
// and prepares the launch session, and only Handoff, on the application thread
// that constructed this object, starts the title.
class RuntimeHost {
 public:
  explicit RuntimeHost(RuntimeHostConfig config);
  ~RuntimeHost();
  RuntimeHost(const RuntimeHost&) = delete;
  RuntimeHost& operator=(const RuntimeHost&) = delete;

  bool Start();
  void Shutdown();
  RuntimeSnapshot Snapshot() const;
  // Before any download service receives its games root.
  bool WaitForStartup();
  // The same state without a copy per call: shared and read-only, made again only after a change.
  // The UI reads it many times a frame; Snapshot() copies every field each time.
  std::shared_ptr<const RuntimeSnapshot> Shared() const;
  // Queues preparation of a downloaded game, or a retry of a failed one. Games are prepared one
  // at a time, in the order asked; asking again for a queued game keeps its place.
  void Prepare(RuntimePrepareRequest request);
  // Moves a queued preparation to the front of the queue. The one running now finishes first.
  // False when the game is not queued (it started or was never asked for): nothing changes.
  bool PrepareNext(std::uint32_t runtime_number);
  // Takes a queued preparation out of the queue (its game is being uninstalled). False when it is not
  // queued: it started already, or was never asked for.
  bool WithdrawPrepare(std::uint32_t runtime_number);
  // While held, no queued preparation starts (a quit to Home waits for the running one only).
  void HoldPreparations(bool hold);
  // Takes a finished preparation off RuntimeSnapshot::prepared; ClearPrepared takes every one off,
  // including those past the kMaxPrepared the snapshot lists.
  void DismissPrepared(std::uint32_t runtime_number, std::int64_t finished_at);
  void ClearPrepared();
  // Asks the console whether a game's PS5 title runs (from any origin, Home
  // included). A game with no prepared title reports stopped.
  void CheckActivity(std::uint32_t runtime_number);
  // CheckActivity, waiting up to `timeout` for the answer (kUnknown without one). For other
  // threads, such as the Cloud worker checking a game just before moving its saves.
  runtime::NativeActivity ActivityNow(std::uint32_t runtime_number, std::chrono::milliseconds timeout);
  // Queues removal of the leftover settings requests (rechecked on the worker).
  void CleanUpRequests();
  // The names each game's PS5 title should carry. A Ready title whose installed
  // name differs reports an update, and Prepare applies the new name.
  void SetTitleNames(std::map<std::uint32_t, std::string> names);
  // Queues Play of a Ready game. With setup_program (a program in the game folder, relative to it), the title starts
  // that program once instead, with the Mouse controller preset, and the game's saved settings come back with the
  // next publication: the next Play, or the next start of Playden (RuntimeInstall::setup_program).
  void Play(std::uint32_t runtime_number, std::string setup_program = {});
  // A game's files changed outside the runtime (an update finished): the installs are checked again, so
  // RuntimeInstallView::files_changed follows.
  void RecheckInstalls();
  // The signed-in Steam account, for the Steamworks emulator's settings (empty when signed out).
  void SetSteamAccount(std::string account_name, std::uint64_t steam_id);
  // Queues removal of a game's PS5 title. Its bottle (saves and settings) is kept
  // unless delete_saves; an interrupted removal continues with its first choice.
  void Remove(std::uint32_t runtime_number, bool delete_saves);
  // Queues deletion of a game's kept bottle.
  void DeleteKeptBottle(std::uint32_t runtime_number);
  // Deletes only a scanned leftover's PS5 title and registration; its game files and saves stay.
  bool DeleteLeftoverTitle(const std::string& title_id, std::string* error = nullptr);
  // Queues a scan of a game folder for the Program picker (RuntimeSnapshot::game_setups).
  void ScanPrograms(std::uint32_t runtime_number, std::string install_path);
  // Measures each game's Windows user folders (where saves of games without Steam Cloud live).
  void MeasureSaves();
  // A kit was put in place while Playden runs (the first-run kit download): the worker stops its
  // services and starts them again to admit it, publishing kStarting in between, once no launch is
  // being handed off or followed. Queued requests wait for it.
  void ReloadKit();
  // Queues maintenance of a prepared game. Each action is refused while the game's
  // title runs or its state is unknown.
  void Maintain(std::uint32_t runtime_number, RuntimeMaintenance action);
  // A game's runtime number, which every call above takes: a Steam game's app ID; for a game from
  // another store, the number of its install record, or a new one, saved before it is returned.
  // Any thread. A new number is written to installs.json under the effective-UID hold, so it waits
  // while the worker starts up or works through a batch; with `busy`, it returns nothing at once
  // and sets *busy instead (ask again later). Nothing otherwise when the game is invalid, the
  // runtime records are not loaded (yet) or the write fails; the runtime log says which.
  // RuntimeSnapshot::runtime_games lists the number before it returns.
  std::optional<std::uint32_t> RuntimeNumberFor(const GameId& game, bool* busy = nullptr);
  // Makes `request.to` the game's folder, keeping its PS5 title, runtime number and bottle (the
  // 0.6 games move's republish, for one game): with the game's publication lease held and its title
  // stopped, move_files runs, then one record write names the new folder (moved_from the old one)
  // and the game is published again at its revision. A publication that can't be verified falls
  // back to removing the old title, keeping its saves (prepare_again). A record already naming `to`
  // (a restart after its write) only finishes the republish. A preparation queued for the game
  // answers kBusy (it names the old folder). Blocks the calling thread (never the worker's) until
  // the worker answers; a request the worker hasn't started within `timeout` is dropped and answers
  // kBusy. Once Playden is closing (BeginClosing), a request not started yet
  // answers kBusy with kRelocationClosing at once: the copy or move resumes at the next start.
  RuntimeRelocationResult Relocate(RuntimeRelocation request, std::chrono::milliseconds timeout);
  // Playden is quitting: callers waiting in Relocate for a relocation the worker hasn't started stop
  // waiting now, and new ones are refused, so the install and Local workers they block can be
  // joined before this host shuts down. Doesn't stop the worker (Shutdown does); safe to call twice.
  void BeginClosing();
  // Application thread only: starts the title of a prepared launch session.
  // Returns false when no session is waiting.
  bool Handoff();

 private:
  struct Composition;
  static void* Entry(void* self);
  void Run();
  // Starts the services and runs requests until Shutdown (false) or ReloadKit (true).
  bool RunServices();
  void Publish(const std::function<void(RuntimeSnapshot&)>& change);
  // With `only`, that install's title is checked again and the others keep their last
  // check (about two seconds per title on the console).
  void PublishInstalls(const Composition& composition, const std::string& only = {});
  // Empty when the title is Ready or, with *awaiting set, staged and waiting for the
  // console to register it; otherwise why not.
  std::string RunPrepare(Composition& composition, const RuntimePrepareRequest& request, bool* awaiting = nullptr);
  const runtime::GameRecipe* RecipeForPrepare(Composition& composition, const RuntimePrepareRequest& request,
                                              std::string* reason);
  void LoadGeneratedRecipes(Composition& composition);
  void PublishRecipes(const Composition& composition);
  runtime::PackageActionResult DrivePublication(Composition& composition, const runtime::RuntimeInstall& record);
  // Puts the saved settings into a Ready title when they are newer than its publication:
  // Steam files, then a journaled settings update (profile, dxvk.conf, registry). Call only
  // while the title is stopped. Empty on success or when nothing changed; *published says
  // whether an update ran. The title keeps the launch entry it was prepared with.
  // A moved game, or an owned publication with a kit that admits unowned folders
  // (NeedsUnownedPublication), is published again at its revision.
  // steam_account_known false (at startup): the Steam emulator's files wait for Play.
  // *prepare_again: refused for a reason only a new preparation gets past (recipe or settings).
  // The unowned upgrade alone (not moved, no newer settings) never refuses: its failure is in
  // *upgrade_deferred and the owned publication stays, as it played before.
  // `code`: the publication's failure code when the publication itself refused (empty otherwise).
  // With force, the settings are published again even when the title already has their revision:
  // after Update moved the title to another runtime, which may take different profile keys (120 Hz
  // output). Nothing is published when they write the same profile and dxvk.conf as before.
  // With setup_program, the saved settings are published with that program and the Mouse controller preset in
  // their place, for one run (Play). A title that publishes such a run gets its saved settings back from the next
  // call without it, whatever their revision.
  std::string RepublishSettings(Composition& composition, const runtime::RuntimeInstall& record,
                                bool* published = nullptr, bool steam_account_known = true,
                                bool* prepare_again = nullptr, std::string* upgrade_deferred = nullptr,
                                std::string* code = nullptr, bool force = false, const std::string& setup_program = {});
  // RepublishSettings without the upgrade's fallback.
  // `defaults_published`: with force, a game with no saved settings whose title has revision 0 was
  // published from Playden's defaults, so they are published again.
  std::string PublishSavedSettings(Composition& composition, const runtime::RuntimeInstall& record,
                                   bool* published, bool steam_account_known, bool* prepare_again,
                                   std::string* code = nullptr, bool force = false, bool defaults_published = false,
                                   const std::string& setup_program = {});
  // Records the program the title's profile starts once (RuntimeInstall::setup_program); empty: none.
  std::string MarkSetupProgram(Composition& composition, const std::string& install_id, const std::string& program);
  // Whether an update left its marker in the Ready game's folder (installs::kFilesChangedName).
  bool FilesChanged(const runtime::RuntimeInstall& record) const;
  // The Ready-path part of Prepare for a game whose files changed: its generated recipe is made again
  // from the request's launch options and the files (as a fresh preparation makes it), the saved
  // settings are published with it, and the marker goes. Empty, or why not (the marker stays).
  std::string RefreshChangedFiles(Composition& composition, const RuntimePrepareRequest& request,
                                  const runtime::RuntimeInstall& record);
  // A Ready game published owned (schema 2, before 0.6 or by a kit before 0.10) while the kit
  // admits unowned game folders: published again as unowned, so folders the player opened pass.
  bool NeedsUnownedPublication(const Composition& composition, const runtime::RuntimeInstall& record);
  // The request of an unowned upgrade alone (same revision, the game not moved) that failed before
  // its publication began: the record still carries the request arm, so the owned publication the
  // game played under is intact. Dropped (true), the game plays as before; Play, Update, Repair and
  // the next start try the upgrade again. Moved games and newer settings keep their request.
  bool DropUpgradeRequest(Composition& composition, const std::string& install_id, const std::string& why);
  // The 0.6 games move: rewrites the records and Cloud indexes under the old games root at startup,
  // and marks the move done once no moved Ready game is left to republish.
  bool MoveGames(Composition& composition, std::string* error);
  bool MoveGameRecords(Composition& composition);
  void FinishGamesMove(Composition& composition);
  // Relocate on the worker.
  RuntimeRelocationResult RunRelocate(Composition& composition, const RuntimeRelocation& request);
  struct PendingRelocation {
    RuntimeRelocation request;
    bool started = false;
    bool abandoned = false;  // the caller stopped waiting before it started
    std::optional<RuntimeRelocationResult> result;
  };
  // Empty when the game's Ready title is stopped with no launch pending, otherwise why not.
  std::string MaintenanceRefusal(Composition& composition, const runtime::RuntimeInstall& record);
  // Runs one maintenance action; empty message on refusal means none; *ok tells success.
  std::string RunMaintenance(Composition& composition, std::uint32_t runtime_number, RuntimeMaintenance action, bool* ok);
  std::filesystem::path SaveBackupRoot(const std::string& install_id) const;
  // A game's files and settings as the preparation steps see them.
  struct GameFiles;
  // Puts the game's files and bottle in the state its recipe and settings ask for, before it starts:
  // generic steps for every store, and Steam-file steps that act on the Steam files a game has,
  // whatever its store (the steam_api DLLs follow its "Steam features" setting). Call only while the
  // game is stopped; empty on success, otherwise why not (for the player).
  // steam_account_known false: the emulator's files (which name the account) are left as they are.
  std::string ApplyPreparationSteps(Composition& composition, const runtime::RuntimeInstall& record,
                                    bool steam_account_known = true);
  // Generic steps.
  void MediaStep(Composition& composition, const runtime::RuntimeInstall& record);
  std::string Ue3EulaStep(const GameFiles& files);
  std::string Direct3D12Step(Composition& composition, const GameFiles& files);
  void KitMonoStep(Composition& composition, const runtime::RuntimeInstall& record, const char* when);  // never stops the start
  void VcRuntimeStep(const GameFiles& files);  // never stops the start
  std::string BottleEnvironmentStep(const GameFiles& files);
  std::string RecipeRegistryStep(const GameFiles& files);
  std::string RecipeIniStep(const GameFiles& files);
  std::string DotnetHeapStep(const GameFiles& files);
  void MfcStep(const GameFiles& files);  // an upgrade: never stops the start
  // Steam-file steps.
  std::string SteamStubStep(const GameFiles& files);
  std::string SteamworksStep(const GameFiles& files, bool steam_account_known);  // and the legacy steam.dll
  std::string InstallScriptsStep(const GameFiles& files);                        // and redistributables
  // The Steam app ID the game's emulator uses.
  SteamworksAppView SteamworksAppFor(const GameFiles& files) const;
  // Whether the install's title runs the installed kit's runtime: its owner markers name this kit
  // (PackageInstaller::TitleCurrent) and its bound artifacts are the kit's. A title made from an earlier kit runs
  // that kit's runtime until Update.
  bool TitleRuntimeCurrent(const Composition& composition, const runtime::RuntimeInstall& record) const;
  // The installed kit's capabilities as this install's title can use them (runtime::KitForTitleRuntime): profile
  // keys only a newer runtime takes are left out until Update. Use it wherever a profile is built for the title.
  runtime::KitCapabilities TitleKit(const Composition& composition, const runtime::RuntimeInstall& record) const;
  // Empty while the kit on disk is the one admitted at startup; otherwise what to tell the player.
  std::string KitReplaced(const Composition& composition) const;
  // Makes an owned, world-writable folder from root down to working 0755 (dxvk.conf goes there).
  std::string TightenWorkingFolder(const std::filesystem::path& root, const std::string& working);
  // Checks (and fixes, as root) config.writable_folders, the game's root and its working folder
  // (relative to the root; empty: none) before anything is written; empty, or which can't be written.
  // Call before any ManagedDirectory of the operation is opened.
  std::string EnsureWritable(const std::string& install_path, const std::string& working);
  // Once the game is published unowned (kit with game-root-unowned): opens what older Playdens
  // tightened, the folders from the game root down to its working folder that the app user owns
  // and that are 0755 become 0777, so FTP can write into them (mods). Folders of other users are
  // left alone. Call after the publication, while none of its ManagedDirectory objects is open.
  void OpenGameFolders(const Composition& composition, const runtime::RuntimeInstall& record);
  // A dxvk.conf with more than one link fails the title's start check; it becomes a copy of its own.
  std::string UnlinkDxvkConfig(const runtime::RuntimeInstall& record);
  // The bottle's way to its user profile takes the kit's mode, and with bottle-profile-shared its
  // profile is opened to FTP again (files Wine made since are 0600). Only for a stopped title,
  // while no ManagedDirectory of the bottle is open. Failures are logged; the start check decides.
  void SettleBottle(Composition& composition, const runtime::RuntimeInstall& record, const char* when);
  runtime::LaunchPreflight Preflight(Composition& composition, const runtime::RuntimeInstall& record);
  // Worker only. Tightens the bottle at data_root (console path) when it looks opened up and its title is stopped.
  void HealBottle(Composition& composition, const std::string& data_root, const std::string& title_id);
  runtime::LaunchEvidence Evidence(Composition& composition, const runtime::RuntimeInstall& record,
                                   const std::string& session_id);
  void RunPlay(Composition& composition, std::uint32_t runtime_number, const std::string& setup_program = {});
  // Removes a game's PS5 title; empty when it has none left, otherwise why not.
  // moving: the title goes because its game was installed again elsewhere (journal "remove-move"),
  // so Prepare, not Uninstall, finishes a removal that stopped part way.
  std::string RunRemove(Composition& composition, std::uint32_t runtime_number, bool delete_saves, bool moving = false);
  struct RemovalFailure { std::string code, message; };
  std::optional<RemovalFailure> RetireTitle(Composition& composition, const std::string& title_id,
                                           const std::string& operation_id,
                                           const runtime::LeftoverTitle* expected = nullptr);
  std::optional<RemovalFailure> UnregisterTitle(Composition& composition, const std::string& title_id,
                                               const std::string& operation_id);
  std::string DeleteRetiredTitle(const std::string& title_id, const std::string& operation_id);
  // Marks a bottle as kept saves (<install id>.kept), as Uninstall without "delete saves" does.
  std::optional<RemovalFailure> KeepBottle(const std::filesystem::path& bottle, const std::string& install_id,
                                           const GameId& game, std::uint32_t runtime_number, const std::string& name,
                                           const std::string& operation_id);
  // Keeps bottles no install record and no unrecorded PS5 title names (their title was deleted
  // as a leftover, or its record was lost) as kept saves, so Storage lists them.
  void KeepUnrecordedBottles(Composition& composition);
  struct LeftoverDelete {
    runtime::LeftoverTitle title;
    std::string operation_id;
    std::string step = "retire";
  };
  std::string RunDeleteLeftover(Composition& composition, LeftoverDelete request);
  std::string SaveLeftoverDelete(Composition& composition, const LeftoverDelete& request);
  void ResumeLeftoverDeletes(Composition& composition);
  void PublishLeftoverTitles(Composition& composition);
  // Whether the console still lists the title (app.db or /user/app); unknown counts as listed.
  bool TitleListed(Composition& composition, const std::string& title_id);
  // Moves the saves of a kept bottle into the game's new bottle and drops the kept copy.
  std::string RestoreKeptSaves(Composition& composition, std::uint32_t runtime_number);
  std::string DeleteKept(Composition& composition, std::uint32_t runtime_number);
  // Lists the kept bottles; a non-Steam game's number is remembered (RememberNumber) for it.
  void PublishKeptBottles(Composition& composition);
  // Finished preparations, in prepared_history_ and the state root's prepared-history file (worker thread).
  void LoadPreparedHistory();
  void SavePreparedHistory();
  std::vector<PreparedGame> ListPrepared(const Composition* composition) const;
  void PublishPrepared(const Composition* composition);
  // With an any-title kit, lists every recorded game title in the helper's allow-list so their
  // titles may start; empty on success (or with no such kit), otherwise why not.
  std::string SyncTitleAllowList(Composition& composition);
  std::filesystem::path HostPath(const std::string& console_path) const;
  std::optional<std::string> InstalledTitleName(const runtime::RuntimeInstall& record) const;
  bool TitleNameCurrent(const runtime::RuntimeInstall& record) const;
  // Writes the game's own title name (and icon) for its PS5 title.
  runtime::PackageActionResult WriteTitleMetadata(Composition& composition, const runtime::RuntimeInstall& record,
                                                  const std::string& title_name);
  // Polls a pending launch session and ends it once its outcome is final.
  // Returns true while a session is still undecided.
  bool PollLaunch(Composition& composition);
  void PublishLaunch(Composition& composition, const runtime::LaunchSnapshot& launch);
  // Settings request files that no record's journal names; with remove, deletes them.
  std::size_t LeftoverRequests(Composition& composition, bool remove);
  bool Stopping() const;
  void Log(const std::string& line) const;
  void LogSteamStubPrograms(std::uint32_t app, const std::vector<runtime::GameProgram>& programs) const;
  // Worker thread: publishes the work fraction when its whole percent changes; ClearWork removes it.
  void ReportWork(std::uint64_t done, std::uint64_t total);
  void ClearWork();
  // Worker thread: makes the loaded repository the one RuntimeNumberFor asks (nullptr: none) and
  // learns its records' numbers.
  void ShareNumbers(runtime::RuntimeInstallRepository* repository);
  // Remembers a non-Steam game's number for RuntimeNumberFor and RuntimeSnapshot::runtime_games.
  void LearnNumber(const GameId& game, std::uint32_t runtime_number);

  RuntimeHostConfig config_;
  // When each redistributable's download last failed (worker only): a preparation runs its steps more than once,
  // and Play shouldn't wait for a network that just failed.
  std::map<std::string, std::chrono::steady_clock::time_point> redist_download_failed_;
  // What the work in progress lets MfcStep do (worker only; RedistWorkScope sets it): download only for work the
  // player started, never at start-up or for other background republishing. play_app: the Play that waits, for
  // its progress line; notice: what that Play went ahead without.
  struct RedistWork {
    bool downloads = false;
    std::optional<std::uint32_t> play_app;
    std::string notice;
  };
  RedistWork redist_work_;
  struct RedistWorkScope;
  // Set by Shutdown: a download in progress gives up.
  std::atomic<bool> cancel_downloads_{false};
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable published_;  // notified on every Publish
  RuntimeSnapshot snapshot_;
  mutable std::shared_ptr<const RuntimeSnapshot> shared_;  // Shared()'s copy of snapshot_ at shared_revision_
  mutable std::uint64_t shared_revision_ = 0;
  std::vector<RuntimePrepareRequest> prepare_requests_;
  struct PlayRequest {
    std::uint32_t app = 0;
    std::string setup_program;  // Play(…, setup_program)
  };
  std::vector<PlayRequest> play_requests_;
  struct RemovalRequest {
    std::uint32_t app = 0;
    bool delete_saves = false;
    std::optional<LeftoverDelete> leftover;
    std::uint64_t order = 0;
  };
  std::vector<RemovalRequest> remove_requests_;
  std::uint64_t next_work_order_ = 0;
  std::map<std::uint32_t, std::uint64_t> prepare_order_;
  std::vector<std::uint32_t> kept_delete_requests_;
  std::vector<std::pair<std::uint32_t, std::string>> scan_requests_;
  std::vector<std::pair<std::uint32_t, RuntimeMaintenance>> maintenance_requests_;
  std::vector<std::uint32_t> activity_requests_;
  std::vector<std::shared_ptr<PendingRelocation>> relocation_requests_;  // guarded by mutex_
  int work_percent_ = -1;  // worker thread only: the published work percent, -1 for none
  bool launch_poll_ = false;
  bool cleanup_requested_ = false;
  bool measure_requested_ = false;
  bool kit_reload_requested_ = false;  // guarded by mutex_
  bool prepares_held_ = false;
  std::vector<PreparedGame> prepared_dismissals_;  // guarded by mutex_
  bool prepared_clear_requested_ = false;          // guarded by mutex_
  std::vector<PreparedGame> prepared_history_;     // worker thread only, oldest first
  std::map<std::uint32_t, std::string> title_names_;  // guarded by mutex_
  std::string steam_account_name_;                     // guarded by mutex_
  std::uint64_t steam_id_ = 0;                         // guarded by mutex_
  bool title_names_changed_ = false;
  bool installs_recheck_requested_ = false;
  // Guards the worker's launch service and preflight cache for Handoff.
  std::mutex launch_mutex_;
  runtime::LaunchService* launch_ = nullptr;
  Composition* launch_composition_ = nullptr;
  pthread_t application_thread_{};
  // Runtime numbers of games from other stores. Lock order: the effective-UID hold (which the
  // worker keeps around startup and each batch, repository use included), then numbers_mutex_,
  // then the repository's own lock (never the other way round), then mutex_. A number lookup that
  // may write the repository takes the hold first: a repository write waits for the hold itself.
  mutable std::mutex numbers_mutex_;
  std::unordered_map<GameId, std::uint32_t> numbers_;     // guarded by numbers_mutex_
  runtime::RuntimeInstallRepository* numbers_repository_ = nullptr;  // guarded by numbers_mutex_
  bool stopping_ = false;
  bool closing_ = false;  // BeginClosing: relocations not started answer at once
  bool started_ = false;
  void* thread_ = nullptr;  // pthread_t storage, owned by the implementation
};

// Canonical random (version 4) UUID from /dev/urandom, or nothing (platform/random_id.h).
std::optional<std::string> RandomUuid();

}  // namespace playden::homebrew
