// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/depot_installer.h"
#include "steam/backend/http_client.h"
#include "steam/backend/workshop_client.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playden::steam {

// Steam Workshop items in a game's bottle (#856), where Steam keeps them: the Workshop folder
// W = <prefix>/drive_c/Program Files (x86)/Steam/steamapps/workshop, item <id> in W/content/<appid>/<id>/.
// W/.playden-workshop-<appid> lists the items Playden put there; any other folder in content/<appid>/ was placed
// by hand and is never touched. W/.playden-download/ is Playden's own workspace: a download goes to
// .playden-download/<id>/ first (DepotInstaller's folder, made empty before each download), then its files move to
// .playden-download/<id>.new/, which takes the place of content/<appid>/<id> while the old copy waits as
// .playden-download/<id>.old/. The only name Playden changes in content/<appid>/ is <id> of an item it holds.

inline constexpr std::size_t kWorkshopStateMaxBytes = 256u << 10;
inline constexpr const char* kWorkshopDownloadDir = ".playden-download";

// W for a bottle at `prefix`.
std::string WorkshopFolder(const std::string& prefix);
// Makes W's chain (Program Files (x86), Steam, steamapps, workshop) as real folders, 0755 like Wine's own, from
// the bottle's drive_c through ManagedDirectory. The title's steam_view_setup makes the first three itself
// (mkdir, EEXIST accepted) and steamapps/common beside them, which this leaves possible.
bool EnsureWorkshopFolder(const std::string& prefix);

// One item Playden put in the bottle. manifest 0: a download that has not finished yet.
struct WorkshopLocalItem {
  std::uint64_t id = 0;
  std::uint64_t manifest = 0;
  std::uint32_t time_updated = 0;
  std::uint64_t bytes = 0;
  bool on = true;
  std::string title;  // SanitiseWorkshopText, at most kWorkshopTitleBytes: never holds a line break
};

struct WorkshopState {
  std::vector<WorkshopLocalItem> items;  // unique IDs, at most kWorkshopMaxItems
  const WorkshopLocalItem* Find(std::uint64_t id) const;
  WorkshopLocalItem* Find(std::uint64_t id);
};

// ".playden-workshop-<appid>"
std::string WorkshopStateName(std::uint32_t app_id);
// One line per item: "item=<id>,<manifest>,<time_updated>,<bytes>,<on|off>,<title>\n". Strict: any line that
// isn't that (a bad number, a zero or repeated ID, a title that isn't sanitised), more than kWorkshopMaxItems
// items or kWorkshopStateMaxBytes make the whole file invalid.
std::optional<WorkshopState> ParseWorkshopState(std::string_view text);
std::string FormatWorkshopState(const WorkshopState& state);
// A missing file is an empty state; an unreadable or invalid one is none.
std::optional<WorkshopState> ReadWorkshopState(const std::string& workshop_folder, std::uint32_t app_id);
// Atomic: a temporary file, synced, renamed over the old one.
bool WriteWorkshopState(const std::string& workshop_folder, std::uint32_t app_id, const WorkshopState& state);

// The items the player turned off, kept beside the state file so the choice outlives any job: W/<WorkshopOffName>,
// one decimal item ID per line ("<id>\n"), sorted. Strict: anything else (a zero, a leading zero, a repeat, a
// missing line end), more than kWorkshopMaxItems IDs, makes the whole file invalid.
std::string WorkshopOffName(std::uint32_t app_id);  // ".playden-workshop-<appid>-off"
std::optional<std::set<std::uint64_t>> ParseWorkshopOff(std::string_view text);
std::string FormatWorkshopOff(const std::set<std::uint64_t>& off);
// A missing file is nothing turned off; an unreadable or invalid one is none.
std::optional<std::set<std::uint64_t>> ReadWorkshopOff(const std::string& workshop_folder, std::uint32_t app_id);
// Atomic, as WriteWorkshopState. False for more than kWorkshopMaxItems IDs or a zero ID.
bool WriteWorkshopOff(const std::string& workshop_folder, std::uint32_t app_id, const std::set<std::uint64_t>& off);

// The file each old-style item Playden holds was saved as (#1319), and its author, kept beside the state file:
// W/<WorkshopFilesName>, "<id>\t<name>[\t<author SteamID>]\n" per item, sorted by ID. The launch names both in
// gbe_fork's mods.json (primary_filename, steam_id_owner): it serves the file when the game asks Steam for it, and the
// game shows who made the item (an author the emulator can't name reads "Unknown User", never the player). Strict:
// anything else (a zero, a leading zero or repeated ID, a name that isn't a plain file name, a zero author), more than
// kWorkshopMaxItems lines, makes the whole file invalid.
struct WorkshopItemFile {
  std::string name;
  std::uint64_t owner = 0;  // 0: unknown
  bool operator==(const WorkshopItemFile& other) const { return name == other.name && owner == other.owner; }
};
using WorkshopFiles = std::map<std::uint64_t, WorkshopItemFile>;
std::string WorkshopFilesName(std::uint32_t app_id);  // ".playden-workshop-<appid>-files"
std::optional<WorkshopFiles> ParseWorkshopFiles(std::string_view text);
std::string FormatWorkshopFiles(const WorkshopFiles& files);
// A missing file is no names; an unreadable or invalid one is none.
std::optional<WorkshopFiles> ReadWorkshopFiles(const std::string& workshop_folder, std::uint32_t app_id);
// Atomic, as WriteWorkshopState. An empty map removes the file.
bool WriteWorkshopFiles(const std::string& workshop_folder, std::uint32_t app_id, const WorkshopFiles& files);

enum class WorkshopAction {
  kDownload,        // subscribed, on, not in the bottle
  kUpdate,          // Playden's copy is of another manifest or time
  kKeep,            // Playden's copy is current
  kRemove,          // Playden's copy of an item turned off or no longer subscribed
  kSkipLegacy,      // an old-style item Playden can't fetch (LegacyWorkshopFileOf): not downloaded
  kSkipHandPlaced,  // a folder Playden didn't make holds the item: left alone
  kSkipOff,         // turned off and not in the bottle
};
const char* WorkshopActionName(WorkshopAction action);

struct WorkshopPlanEntry {
  WorkshopAction action = WorkshopAction::kKeep;
  WorkshopItem item;  // for kRemove of an unsubscribed item: id and title from the state
  std::uint64_t bytes = 0;  // kDownload/kUpdate: file_size; kRemove: the state's bytes
};

struct WorkshopPlan {
  std::vector<WorkshopPlanEntry> entries;  // subscriptions in order, then unsubscribed items Playden holds
  std::uint64_t download_bytes = 0;
  std::uint64_t freed_bytes = 0;
  std::size_t downloads = 0;  // kDownload + kUpdate
  std::size_t removals = 0;
};

// What a sync would do. `present`: item IDs with a folder (or anything) named <id> in content/<appid>/.
// `complete` false (the subscription list was cut short): items Playden holds that the list doesn't name are kept,
// not removed. A banned item is never downloaded or updated: Playden's copy is kept (removed when turned off), and
// one it doesn't hold is left out of the plan.
WorkshopPlan PlanWorkshopSync(const std::vector<WorkshopItem>& subscribed, const WorkshopState& state,
                              const std::set<std::uint64_t>& off, const std::set<std::uint64_t>& present,
                              bool complete = true);

// Items Valve's client downloaded (Steam features = Online) that Playden doesn't list: a subscribed item with a
// downloadable manifest, whose W/content/<appid>/<id> is a real folder and which W/appworkshop_<appid>.acf (read
// only) lists installed with a manifest. Each joins `state` as Playden's own, on, with the client's manifest, size and
// time, so a sync keeps it (or updates it) instead of leaving it as placed by hand. Nothing is written; the number
// added. Without the client's file, or with no entry for the item, it stays a folder placed by hand.
std::size_t AdoptSteamClientItems(const std::string& workshop_folder, std::uint32_t app_id,
                                  const std::vector<WorkshopItem>& subscribed, WorkshopState* state);

// One download. `refused`: the item's content is not something Playden installs (links, a .playden-* name).
struct WorkshopDownloadResult {
  InstallError error = InstallError::kNone;
  bool refused = false;
  std::string detail;  // short and identifier-free
};
using WorkshopProgressFn = std::function<void(const InstallProgress&)>;
using WorkshopDownloader = std::function<WorkshopDownloadResult(
    const WorkshopItem& item, const std::string& staging_root, const std::atomic<bool>& cancel,
    const WorkshopProgressFn& progress)>;

// Why a Workshop manifest is refused (a link, or a first path component starting ".playden-", any case);
// false when it is fine.
bool WorkshopManifestRefused(const DepotManifest& manifest, std::string* reason = nullptr);

// DepotInstaller::Run of the item's manifest in `workshop_depot`, asked for under `app_id`, into the staging
// root. `hooks` as DepotInstaller takes them; their manifest hook is wrapped so a refused manifest stops the
// download before any chunk is fetched. Old-style items go to `legacy` (an HttpClient::Perform), and fail without one.
WorkshopDownloader MakeWorkshopDownloader(
    InstallerHooks hooks, StoredSession session, std::uint32_t app_id, std::uint32_t workshop_depot,
    std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)> legacy = {});
// The same with CmClient and CdnClient (they must outlive the downloader); the manifest is checked the same way.
// Old-style items (WorkshopItem::legacy) download their one file over HTTPS instead (DownloadLegacyWorkshopItem).
WorkshopDownloader MakeWorkshopDownloader(CmClient& cm, CdnClient& cdn, StoredSession session, std::uint32_t app_id,
                                          std::uint32_t workshop_depot);

struct WorkshopSyncProgress {
  std::size_t index = 0;  // the item being downloaded, among `count` downloads
  std::size_t count = 0;
  std::uint64_t item_id = 0;
  std::uint64_t item_bytes = 0;  // the item's file_size
  InstallProgress item;
  std::uint64_t bytes_done = 0;  // file_size of the downloads finished in this sync
  std::uint64_t bytes_total = 0;  // the plan's download_bytes
};

struct WorkshopSyncRequest {
  std::uint32_t app_id = 0;
  std::string workshop_folder;  // W (see EnsureWorkshopFolder)
  std::vector<WorkshopItem> subscribed;
  std::set<std::uint64_t> off;  // items the player turned off
  bool complete = true;          // false: `subscribed` was cut short (see PlanWorkshopSync)
  WorkshopDownloader download;
  // Optional. Asked right before the items folder changes (an item removed, a download put in place, an
  // interrupted swap undone); false (the game runs): the sync stops there with kGameRunning (the next one
  // downloads again). `applied` follows each change that was let through, once the state file says what changed.
  std::function<bool()> may_apply;
  std::function<void()> applied;
  std::function<void(const WorkshopSyncProgress&)> progress;  // optional
  std::function<void(const std::string&)> log;                // optional; counts and kinds, no IDs or titles
};

enum class WorkshopOutcome { kDownloaded, kUpdated, kKept, kRemoved, kSkippedLegacy, kSkippedHandPlaced, kSkippedOff,
                             kRefused, kFailed };
const char* WorkshopOutcomeName(WorkshopOutcome outcome);

struct WorkshopItemOutcome {
  std::uint64_t id = 0;
  WorkshopOutcome outcome = WorkshopOutcome::kFailed;
  InstallError error = InstallError::kNone;
  std::string detail;
};

struct WorkshopSyncResult {
  // kNone when every item settled (refused and skipped ones included); else the first failure, or kCanceled.
  InstallError error = InstallError::kNone;
  std::string detail;
  WorkshopPlan plan;
  // Removals first, then the rest in plan order. Items after a cancel, or after a failure that stops every
  // download (kNoSession, kNetwork, kNoSpace), are left out.
  std::vector<WorkshopItemOutcome> items;
};

// Adopts the client's items first (AdoptSteamClientItems, saved with the state). Each item it deletes also leaves
// the client's appworkshop file's installed list (runtime::RemoveFromAppWorkshopAcf), so the client doesn't take a
// deleted folder for an installed item. Undoes what an interrupted sync left in .playden-download/ (<id>.new, <id>.old, staging of items no longer
// downloaded), for the items the state lists only, then carries out the plan. The state file is written before each
// download starts and after each item settles. A damaged state file (one that reads but doesn't parse, or is
// over its cap) is renamed aside to "<name>.bad" and the sync starts from an empty state: the folders it listed
// are then left alone as if placed by hand.
WorkshopSyncResult SyncWorkshopItems(const WorkshopSyncRequest& request, const std::atomic<bool>& cancel);

// The Queue job that brings a game's Workshop items in line with the player's subscriptions and choice
// (InstallOptions::workshop). The game's own files are not touched.
struct WorkshopJobRequest {
  std::uint32_t app_id = 0;
  std::string prefix;  // the game's bottle; empty when it has none yet (never prepared)
  // The player's choice, saved (WriteWorkshopOff) before anything else; none: the one saved before. With a
  // damaged saved choice the job fails asking for the choice again (every time, until one replaces it), rather
  // than download what the player turned off.
  std::optional<std::set<std::uint64_t>> off;
  // Steam features = Online: Steam installs every item the account subscribes to, so every one is on for this job.
  // The saved choice is neither read nor written (`off` is ignored): it waits for Offline emulation.
  bool all_on = false;
  std::function<WorkshopSubscriptions(const std::atomic<bool>& cancel)> subscriptions;
  WorkshopDownloader download;
  std::function<bool()> may_apply;  // optional, see WorkshopSyncRequest
  std::function<void()> applied;    // optional
  std::function<void(std::uint64_t done, std::uint64_t total)> progress;  // optional; bytes of the plan's downloads
  std::function<void(const std::string&)> log;                           // optional; counts only
};

struct WorkshopJobResult {
  bool ok = false;
  bool canceled = false;
  bool retry_later = false;   // Steam out of reach, or the game runs
  bool game_running = false;  // with retry_later: may_apply said no
  std::string failure;        // for the player; empty when ok or canceled
  WorkshopSyncResult sync;
};

inline constexpr const char* kWorkshopNoBottle = "Start the game once, then download its Workshop items.";
// The sentence a sync failure shows the player.
const char* WorkshopFailureText(InstallError error);
WorkshopJobResult RunWorkshopJob(const WorkshopJobRequest& request, const std::atomic<bool>& cancel);

}  // namespace playden::steam
