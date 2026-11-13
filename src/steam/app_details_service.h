#pragma once

#include "library/model.h"
#include "stores/game_details.h"
#include "steam/session_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace playden::steam {

// Steam's game details are the store-neutral ones (stores/game_details.h).
using LaunchOption = stores::LaunchOption;
using AppVersion = stores::AppVersion;
using DlcChoice = stores::DlcChoice;
using AppDetails = stores::GameDetails;

// A check of every installed game for updates (AppDetailsService::CheckInstalled). Counts are games:
// `asked` were sent to Steam, `answered` were handed out by Pump, `failed` of those got no answer.
struct InstalledCheck {
  std::uint64_t account_generation = 0;
  std::size_t total = 0;  // 0: no check (none started, or Invalidate dropped it)
  std::size_t asked = 0;
  std::size_t answered = 0;
  std::size_t failed = 0;
  bool done() const { return total != 0 && answered >= total; }
};

// One install choice whose exact size is asked for: a version (Steam branch, "public" by default) and a
// content language. With `update`, an installed game's update instead: the version and language are its
// install record's, and branch and language stay empty.
struct SizeRequest {
  GameId id;
  std::string branch;
  std::string language;
  bool update = false;
  std::vector<std::uint32_t> excluded_dlc;  // DLC turned off for this install, sorted
  bool operator==(const SizeRequest& other) const {
    return id == other.id && branch == other.branch && language == other.language && update == other.update &&
           excluded_dlc == other.excluded_dlc;
  }
  bool operator<(const SizeRequest& other) const {
    if (!(id == other.id)) return id < other.id;
    if (update != other.update) return other.update;
    if (branch != other.branch) return branch < other.branch;
    return language != other.language ? language < other.language : excluded_dlc < other.excluded_dlc;
  }
};

// The size an install of that choice has once its depots are merged, from their manifests. Steam's figures add
// up each depot, so depots holding the same files count them twice.
struct MeasuredSize {
  SizeRequest request;
  std::uint64_t account_generation = 0;
  // False: Steam's figures are right as they are (nothing overlaps enough to matter), or the check failed.
  bool measured = false;
  bool failed = false;  // the check could not finish (no session, the network); asking again may work
  std::uint64_t install_bytes = 0;
  std::uint64_t download_bytes = 0;
  // An update's: the changed files' bytes (staged beside the game), and the selection measured
  // (AppDetails::Update::target); `measured` false when the install keeps no file tree or isn't there.
  std::uint64_t staged_bytes = 0;
  std::uint64_t target = 0;
};

// Runs on the service worker; may block on the network.
class AppDetailsSource {
 public:
  virtual ~AppDetailsSource() = default;
  virtual std::vector<AppDetails> Fetch(const StoredSession& session, const std::vector<GameId>& ids,
                                        const std::atomic<bool>& cancel) = 0;
  // Runs on its own worker, at the same time as Fetch. Default: Steam's figures stand.
  virtual MeasuredSize Measure(const StoredSession&, const SizeRequest& request, const std::atomic<bool>&) {
    return {request};
  }
};

// Fetches app details for games the player opens. Requests are deduplicated and cached for the
// current account generation; Invalidate drops everything for a new account. The source and the
// repository outlive the service.
class AppDetailsService {
 public:
  AppDetailsService(AppDetailsSource& source, SessionRepository& sessions);
  ~AppDetailsService();
  AppDetailsService(const AppDetailsService&) = delete;
  AppDetailsService& operator=(const AppDetailsService&) = delete;

  // `first`: ahead of what is queued (a game the player opened while installed games are checked for updates);
  // one already queued moves to the front.
  void Request(GameId id, std::uint64_t account_generation, bool first = false);
  // Asks again for a game already answered in this generation (an installed game whose content changed).
  void Refresh(GameId id, std::uint64_t account_generation, bool first = false);
  void Invalidate(std::uint64_t account_generation);
  // Asks Steam again for every game in `ids` (installed games, for updates), even those answered in this
  // generation. They are fed in as the queue drains, behind anything Request asked for, so a page opened
  // meanwhile still goes first; there is no limit on how many. False while a check of this generation runs.
  // Invalidate and another account drop it. When a whole batch gets no answer (Steam unreachable), the
  // games not yet asked are counted as failed without asking.
  bool CheckInstalled(const std::vector<GameId>& ids, std::uint64_t account_generation);
  InstalledCheck CheckProgress();
  std::vector<AppDetails> Pump(std::size_t maximum = 8);
  // Asks for the exact size of one install choice, after the details already asked for, or of an update
  // (`request.update`). Install choices go ahead of updates. A request already queued or being checked is not
  // asked twice. False when it can't be queued (stopping, or kMaxPending of its kind already wait).
  bool Measure(SizeRequest request, std::uint64_t account_generation);
  std::vector<MeasuredSize> PumpMeasured();
  void Shutdown();

  static constexpr std::size_t kMaxPending = 64;
  static constexpr std::size_t kBatch = 8;

 private:
  void Run();
  void RunMeasures();
  void Reset(std::uint64_t account_generation);  // mutex_ held

  AppDetailsSource& source_;
  SessionRepository& sessions_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::uint64_t generation_ = 0;
  std::deque<GameId> pending_;
  std::set<GameId> requested_;  // queued, in flight or answered for this generation
  // Each answer with the check that asked for it (check_serial_ then), 0 for a page's: only the running
  // check's own answers count for it.
  std::deque<std::pair<AppDetails, std::uint64_t>> results_;
  std::deque<GameId> check_pending_;  // the installed check's games not yet asked
  std::set<GameId> check_waiting_;    // asked by the check, not yet handed out by Pump
  InstalledCheck check_;
  std::uint64_t check_serial_ = 0;  // counts the checks started
  std::deque<SizeRequest> measure_pending_;
  std::set<SizeRequest> measure_requested_;  // queued or in flight
  std::deque<MeasuredSize> measured_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> measure_cancel_{false};
  bool stopping_ = false;
  std::thread worker_;
  std::thread measure_worker_;  // size checks: the source's Measure runs here, beside Fetch
};

}  // namespace playden::steam
