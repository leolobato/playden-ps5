#pragma once

#include "library/model.h"
#include "steam/download_rate.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace playden::installs {

struct InstallRequest {
  enum class Action { kInstall, kUninstall, kCancel, kImport, kCopy, kMove };
  Action action = Action::kInstall;
  std::uint32_t app = 0;  // a Steam game's app ID; 0 for other games
  GameId game;            // the game (kImport: none yet)
  // kImport: the drive (install volume id), the game's folder and the program, as LocalLibrarySource::Add takes them.
  // kCopy: the install volume to copy to (empty: the default).
  std::string volume;
  std::string folder;
  std::string program;
};
// Development request file (dev/install-request.txt), one command per line, in order, at most 64:
//   <game>                              Steam: install; Local: prepare (set up) the imported game
//   uninstall <game>                    Steam: uninstall; Local: remove from the library (files stay)
//   cancel <app id>                     Steam only: cancel the download
//   copy <local game> [volume]          copy an imported game playing in place into Playden (volume:
//                                       an install volume id; default: the default volume)
//   move <local game>                   move an imported game from the drop folder into Playden
//   import <volume> <folder> <program>  add a game playing in place (volume: internal, m2, extended,
//                                       usb0, usb1; folder: a name in that drive's import folder, or a
//                                       path from the drive's root; program: inside the folder)
// <game> is a Steam app ID (strict decimal) or "local-<uuid>". Words are separated by spaces or tabs;
// a word with spaces goes in double quotes ("Old School Rally"), which can't hold a quote themselves.
// A line that doesn't parse is skipped.
std::vector<InstallRequest> ParseInstallRequests(const std::string& text);

enum class InstallJobState { kQueued, kRunning, kPaused, kFailed };
// What a running install is doing before its first progress report.
// An update reads the installed files (kCheckingFiles, with its own progress) before it downloads.
enum class InstallStep { kStarting, kCheckingLicenses, kCheckingFiles, kDownloading };

// A place games can be installed: a folder on one drive. `available` means the folder exists and
// Playden can write to it now; installs recorded on a volume that is absent stay listed.
struct InstallVolume {
  std::string id;          // stable across runs, e.g. "internal"
  std::string label;       // shown to the player
  std::string games_root;  // absolute folder holding <app id> folders
  bool available = true;
};

struct InstallJobView {
  GameId id;
  InstallJobState state = InstallJobState::kQueued;
  std::uint64_t done_bytes = 0;
  std::uint64_t total_bytes = 0;
  std::uint64_t held_bytes = 0;  // installed bytes already on disk (see PendingInstallBytes)
  std::string failure;  // short, user-facing
  std::string volume_id;
  std::optional<std::uint64_t> bytes_per_second;
  std::optional<int> eta_seconds;
  InstallStep step = InstallStep::kStarting;  // while running
  std::chrono::steady_clock::time_point step_since{};
  // An update of an installed game, which stays installed (and playable) until the update is swapped in.
  bool update = false;
};

struct InstalledGame {
  GameId id;
  std::uint64_t install_bytes = 0;
  std::string root;
  std::string volume_id;
  bool available = true;  // false while the game's volume, or its folder on that volume, is missing
  std::int64_t finished_at = 0;  // Unix seconds the install finished (its record's time); 0 unknown
  bool update_available = false;  // the last check found a newer version in its store
};

// A recent finished download that Downloads lists after the queue until the player dismisses it.
struct CompletedDownload {
  GameId id;
  std::uint64_t install_bytes = 0;
  std::string volume_id;
  std::int64_t finished_at = 0;
};

// Installs that finished at most `kCompletedDays` before `now` and were not dismissed (a dismissal
// names the finish time, so installing the game again lists it again), newest first, at most
// `kMaxCompleted`.
inline constexpr std::int64_t kCompletedDays = 7;
inline constexpr std::size_t kMaxCompleted = 20;
std::vector<CompletedDownload> CompletedDownloads(const std::vector<InstalledGame>& installed,
                                                  const std::vector<std::pair<GameId, std::int64_t>>& dismissed,
                                                  std::int64_t now);

// An uninstall whose files could not be deleted: the game is listed as installed again.
struct RemovalFailure {
  GameId id;
  std::string reason;  // short, user-facing
};

struct InstallSnapshot {
  std::uint64_t revision = 0;
  std::vector<InstallJobView> jobs;
  std::vector<InstalledGame> installed;
  std::vector<GameId> uninstalling;  // games whose files are being deleted
  std::vector<CompletedDownload> completed;
  std::vector<InstallVolume> volumes;
  std::string default_volume_id;
  bool default_volume_saved = true;  // false: the choice holds until Playden closes
};

// One game's install, run synchronously on the service worker, for the store it is registered
// for. Implementations report progress and honor cancel. A runner that can't start yet (no
// sign-in, the store out of reach) returns `retry_later`: the job keeps its place, waits and is
// tried again by itself while other jobs run. Remove deletes a game's folder and returns an empty
// string on success or a short failure text.
class InstallRunner {
 public:
  virtual ~InstallRunner() = default;
  struct Result {
    bool ok = false;
    bool canceled = false;
    std::string failure;
    std::uint64_t install_bytes = 0;
    bool retry_later = false;  // the job waits and tries again
    std::string waiting;       // with retry_later: what Downloads says meanwhile
  };
  virtual Result Install(const GameId& id, const std::string& root, const std::atomic<bool>& cancel,
                         const std::function<void(std::uint64_t done, std::uint64_t total, std::uint64_t held)>& progress,
                         const std::function<void(InstallStep)>& step) = 0;
  virtual std::string Remove(const std::string& root) = 0;
  // The games (of this runner's store) whose store has a newer version than their folder holds; none
  // when the store has no updates. std::nullopt: not known now (no sign-in, the store out of reach).
  virtual std::optional<std::vector<GameId>> FindUpdates(const std::vector<InstalledGame>& games,
                                                         const std::atomic<bool>& cancel) {
    (void)games; (void)cancel;
    return std::vector<GameId>{};
  }
  // A canceled update: its staged files go, the installed version stays.
  virtual void DiscardUpdate(const std::string& root) { (void)root; }
  // An uninstalled game whose folder is missing: forgotten with nothing to delete (on the worker).
  virtual void Forget(const GameId& id) { (void)id; }
};

// Install queue for real games under <volume games_root>/<key>. One install runs at a time; jobs
// go to the runner of their game's store, and a store without one can't install.
// Pause keeps the partial download for Resume; Cancel and Uninstall delete only Playden-managed
// folders. A deletion never waits for a download: the running download steps back into the queue
// and carries on after it. An uninstall is in the registry until its folder is gone, so one that
// Playden stopped before it finished carries on at the next start; one that fails lists the game
// again and is reported by TakeRemovalFailures. The first volume is primary: it holds the registry of every managed game (with the
// volume each is on) and the default-volume choice. At start the registry is read: complete
// installs are listed and interrupted downloads come back paused. New installs go to the default
// volume. A job whose runner asks to retry later waits (5 s, doubling up to a minute) while the
// worker runs the next job. The runners outlive the service.
class InstallService {
 public:
  using Runners = std::map<Store, InstallRunner*>;
  // Steam's runner only, on one always-available volume, id "internal".
  InstallService(InstallRunner& steam, std::string games_root);
  // `volumes` must be non-empty; volumes[0] is primary and must be available.
  InstallService(InstallRunner& steam, std::vector<InstallVolume> volumes);
  InstallService(Runners runners, std::vector<InstallVolume> volumes);
  ~InstallService();
  InstallService(const InstallService&) = delete;
  InstallService& operator=(const InstallService&) = delete;

  // Installs to `volume_id`, or the default drive when empty; the default is not changed. The
  // estimate is reserved (less what the download already holds on disk) until the job ends, and
  // the runner still checks the selected manifests before writing game data.
  void Install(GameId id, std::optional<std::uint64_t> estimated_bytes = std::nullopt,
               const std::string& volume_id = {});
  void Pause(GameId id);
  void Resume(GameId id);
  // Moves the download to the front of the queue and starts it. A download that was running stops
  // and stays queued right behind it, so it carries on by itself afterwards.
  void DownloadNow(GameId id);
  // Swaps with the previous/next queued download, preserving the running download.
  void MoveQueued(GameId id, bool up);
  void Cancel(GameId id);
  void Uninstall(GameId id);
  // Updates an installed game the last check found an update for. The job runs in the queue like a
  // download; the game stays installed meanwhile, and Cancel keeps the installed version.
  void Update(GameId id);
  // Asks each store whether its installed games have updates (on the worker, between jobs);
  // InstalledGame::update_available then says.
  void CheckUpdates();
  // Uninstalls whose files could not be deleted since the last call.
  std::vector<RemovalFailure> TakeRemovalFailures();
  // Lists a complete managed install that was made outside the queue (an imported game moved
  // into Playden): its folder on `volume_id` holds the install record. True when it is listed now
  // (or was already); false when the game has a job or files being deleted, its store no runner, or
  // the record is missing.
  bool Adopt(GameId id, const std::string& volume_id, std::uint64_t install_bytes);
  // Chooses the volume for installs queued from now on; ignored unless it names an available volume.
  void SetDefaultVolume(const std::string& volume_id);
  // Takes a finished download, or every one, off the Completed list in Downloads.
  void DismissCompleted(GameId id);
  void ClearCompleted();
  InstallSnapshot Snapshot();
  // Checks the volume's games folder (and may fix it) when a download starts; a non-empty answer
  // fails the download with it as the message. Set before the first Install; empty: no check.
  void SetFolderCheck(std::function<std::string(const std::string& games_root)> check) { folder_check_ = std::move(check); }
  // How long a job that asked to retry later waits: `first`, doubling each time up to `most`.
  void SetRetryDelays(std::chrono::milliseconds first, std::chrono::milliseconds most);
  // What the store's jobs waited for may be there now (Steam: a sign-in): the jobs of the store that
  // wait to try again try at once, and a later wait starts short again.
  void RetryWaiting(Store store);
  void Shutdown();

  // The folder a game uses (or would use if installed now); empty if its volume is unknown.
  std::string RootFor(GameId id);
  static constexpr std::size_t kMaxJobs = 64;
  static constexpr const char* kPrimaryVolumeId = "internal";

 private:
  struct Job {
    InstallJobView view;
    steam::DownloadRate rate;
    bool cancel_requested = false;  // delete files when the run stops
    std::optional<std::uint64_t> estimated_bytes;
    // Waiting after a retry_later: not tried again before `not_before`; the next wait is `retry_delay`.
    bool waiting = false;
    std::chrono::steady_clock::time_point not_before{};
    std::chrono::milliseconds retry_delay{0};
  };
  struct Removal {
    Store store = Store::kSteam;
    std::string root;
    std::optional<GameId> forget;  // set: InstallRunner::Forget instead of deleting `root`
    std::optional<InstalledGame> game;  // an uninstall's folder; none for a canceled download
    bool discard_update = false;  // a canceled update: InstallRunner::DiscardUpdate instead of deleting `root`
  };
  void Run();
  void Scan();
  void QueueRemoval(Removal removal);
  Job* Find(GameId id);
  void Publish();
  void SaveRegistry();
  void SaveDefaultVolume();
  void SaveDismissed();
  void Dismiss(GameId id);
  const InstallVolume* FindVolume(const std::string& id) const;
  std::string RootOn(const std::string& volume_id, GameId id) const;
  std::string LockedRootFor(GameId id) const;

  InstallRunner* RunnerFor(Store store) const;
  static void ClearWait(Job& job);

  Runners runners_;
  std::chrono::milliseconds first_retry_{5000};
  std::chrono::milliseconds most_retry_{60000};
  std::function<std::string(const std::string&)> folder_check_;
  std::vector<InstallVolume> volumes_;
  std::string default_volume_id_;
  bool default_volume_saved_ = true;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  std::vector<InstalledGame> installed_;
  std::vector<std::pair<GameId, std::int64_t>> dismissed_;  // finished downloads taken off Completed
  std::deque<Removal> removals_;  // folders to delete, by the runner of their store
  std::vector<InstalledGame> uninstalling_;  // uninstalls whose folder is not deleted yet
  std::vector<RemovalFailure> removal_failures_;
  std::set<GameId> updates_;  // installed games with an update, as the last check found
  bool check_updates_ = false;
  std::optional<GameId> active_;
  std::atomic<bool> active_cancel_{false};
  std::uint64_t revision_ = 0;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace playden::installs
