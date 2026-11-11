#pragma once

#include "library/model.h"
#include "steam/session_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace playden::steam {

enum class CloudState {
  kIdle,
  kChecking,
  kSyncing,
  kSynced,        // the last sync finished; message says what moved
  kUpToDate,
  kConflict,      // the player picks local or Cloud saves
  kNeedsAccount,  // Cloud changes wait for the player to confirm this Steam account
  kUnsupported,   // no Steam Cloud, or save locations Playden cannot map
  kFailed,
  kWaiting,       // transfers that need no choice, left for the player (a check-only location)
};

enum class CloudResolution { kAutomatic, kUseLocal, kUseCloud };

// Where one game's saves live: the install, its optional explicit Wine bottle,
// and a state folder holding the sync baseline/backups that outlives uninstall.
struct CloudLocation {
  GameId id;
  std::string game_root;
  std::string state_root;
  // Managed games use their actual bottle; empty retains the legacy
  // state_root/prefix mapping for unprepared installs.
  std::string prefix_root;
  // Checked but never changed by a job (an uninstalled game's kept bottle): transfers wait as kWaiting
  // until the player asks with Sync.
  bool check_only = false;
};

// One check or sync, as the player sees it.
struct CloudReport {
  CloudState state = CloudState::kFailed;
  std::string message;  // short, user-facing, no identifiers
  std::size_t downloads = 0, uploads = 0, conflicts = 0;
  std::size_t local_files = 0, remote_files = 0;
  std::uint64_t local_bytes = 0, remote_bytes = 0, local_latest = 0, remote_latest = 0;
  bool first_sync = false;     // a conflict with no earlier sync on this device: both sides already had saves
  bool first_overwrite = false;  // a conflict because this console's first upload would replace Cloud saves
  bool canceled = false;  // failed only because the service canceled it (Playden quitting): never saved
  // Filled in by the service.
  std::int64_t last_sync = 0;  // Unix time saves were last in step (synced or found up to date)
  std::int64_t checked = 0;    // Unix time of the last finished check
  bool queued = false;         // waiting in the current job
  std::uint64_t moved_bytes = 0, total_bytes = 0;  // the transfer in progress
  std::uint64_t serial = 0;    // the service revision this report was set at
};

// What the quick check found for one game; see cloud::CloudSync::QuickCheck.
enum class CloudDrift { kUnknown, kUnchanged, kLocal, kRemote, kBoth };

// Runs on the service worker and may block on the network. Check remembers its plan per game for
// the following Apply, and reports kSyncing when that plan's transfers need no choice (the service
// then applies it at once).
class CloudRunner {
 public:
  virtual ~CloudRunner() = default;
  virtual CloudReport Check(const StoredSession& session, const CloudLocation& location, const std::atomic<bool>& cancel) = 0;
  virtual CloudReport Apply(const StoredSession& session, const CloudLocation& location, CloudResolution resolution,
                            bool confirm_account, const std::atomic<bool>& cancel) = 0;
  // One entry per location; unknown makes the service run a full check.
  virtual std::vector<CloudDrift> QuickCheck(const StoredSession&, const std::vector<CloudLocation>& locations,
                                             const std::atomic<bool>&) {
    return std::vector<CloudDrift>(locations.size(), CloudDrift::kUnknown);
  }
  // Deletes the game's save files on this device (backed up first).
  virtual bool RemoveLocal(const CloudLocation&, std::string* message) {
    *message = "Saves cannot be deleted here.";
    return false;
  }
  // Called during Apply with the bytes moved so far.
  virtual void SetProgress(std::function<void(std::uint64_t done, std::uint64_t total)>) {}
};

struct CloudGameView {
  GameId id;
  CloudReport report;
};

enum class CloudJobKind { kNone, kQuickCheck, kSyncAll };

// A check or sync over many games, one at a time.
struct CloudJob {
  CloudJobKind kind = CloudJobKind::kNone;
  std::size_t done = 0, total = 0;
  GameId current;
  bool stopping = false;
};

struct CloudSnapshot {
  std::uint64_t revision = 0;
  std::vector<CloudGameView> games;
  CloudJob job;
};

// Steam Cloud save sync on demand: Sync checks a game and carries out the transfers when they need
// no choice; a conflict or an unconfirmed account waits for Resolve. One operation runs at a time.
// Jobs check or sync many games in one go: the quick check fully checks only the games that drifted
// since their last check (or whose last result was not settled); Sync all checks every game. Neither
// settles a choice. Each game's last result is kept in its state folder and restored at start.
// The runner and repository outlive the service.
class CloudService {
 public:
  CloudService(CloudRunner& runner, SessionRepository& sessions);
  ~CloudService();
  CloudService(const CloudService&) = delete;
  CloudService& operator=(const CloudService&) = delete;

  // Sync, Resolve and RemoveLocal return the serial after which a report for the game is this
  // request's answer (0 when the request was refused, e.g. the queue is full).
  std::uint64_t Sync(const CloudLocation& location);
  // Checks again, then applies the choice while it still applies: a choice restored from an earlier
  // run has no plan in memory, and Cloud may have moved on since it was shown.
  std::uint64_t Resolve(const CloudLocation& location, CloudResolution resolution, bool confirm_account);
  // Reads the last results kept under saves_root/<id> for games not known yet.
  void Restore(const std::string& saves_root);
  // One job at a time; a second is ignored while one runs.
  void StartJob(CloudJobKind kind, std::vector<CloudLocation> locations);
  // Stops the running job after the game it is on.
  void StopJob();
  // Deletes a game's saves on this device, then checks it again.
  std::uint64_t RemoveLocal(const CloudLocation& location);
  // Asked on the worker just before a job checks a game: false when its saves must not move now
  // (its title runs). Such games are skipped and listed by TakeDeferred.
  void SetGate(std::function<bool(GameId)> may_move);
  std::vector<GameId> TakeDeferred();
  // Forgets a game: its last result and its state folder (baseline, index, backups).
  void Forget(const CloudLocation& location);
  void SetClock(std::function<std::int64_t()> now);
  CloudSnapshot Snapshot();
  void Shutdown();
  static constexpr std::size_t kMaxPending = 16;

 private:
  enum class Kind { kSync, kResolve, kRestore, kJob, kRemove, kForget };
  struct Request {
    Kind kind = Kind::kSync;
    CloudLocation location;
    CloudResolution resolution = CloudResolution::kAutomatic;
    bool confirm_account = false;
    std::string saves_root;
    CloudJobKind job = CloudJobKind::kNone;
    std::vector<CloudLocation> locations;
  };
  void Run();
  void Set(GameId id, CloudReport report);
  // Settles a finished report: timestamps carried over, kept in the state folder.
  void Finish(const CloudLocation& location, CloudReport report);
  // Check, then Apply when no choice is needed (check-only locations wait instead).
  CloudReport CheckAndApply(const StoredSession& session, const CloudLocation& location, const std::atomic<bool>& cancel);
  void RunJob(const StoredSession& session, const Request& request);
  std::int64_t Now() const;

  CloudRunner& runner_;
  SessionRepository& sessions_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Request> pending_;
  std::map<GameId, CloudReport> games_;
  std::uint64_t revision_ = 0;
  CloudJob job_;
  bool job_pending_ = false;
  GameId active_;  // the game Apply is moving saves for
  std::function<std::int64_t()> now_;
  std::function<bool(GameId)> gate_;
  std::vector<GameId> deferred_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> stop_job_{false};
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace playden::steam
