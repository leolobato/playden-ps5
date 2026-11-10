#pragma once

#include "library/model.h"
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

// One Windows launch option from Steam's app info, as Steam wrote it.
struct LaunchOption {
  std::string executable;
  std::string arguments;
  std::string working_directory;
  std::string description;
};

// A version (Steam branch) an install can choose, with what installing it takes.
struct AppVersion {
  std::string name;  // the branch as Steam names it; "public" for the default
  std::string description;
  std::int64_t time_updated = 0;  // unix seconds; 0 = unknown
  std::uint64_t build_id = 0;
  std::uint64_t download_bytes = 0;  // 0 = unknown
  std::uint64_t install_bytes = 0;
  // The sizes for each content language the version carries, when an install picks another one.
  struct LanguageSize {
    std::string language;
    std::uint64_t download_bytes = 0;
    std::uint64_t install_bytes = 0;
  };
  std::vector<LanguageSize> languages;
};

// A DLC an install can include or leave out. Its sizes are for the public branch in the Settings language.
struct DlcChoice {
  std::uint32_t app_id = 0;
  std::string name;  // empty: Steam doesn't describe it
  std::uint64_t download_bytes = 0;
  std::uint64_t install_bytes = 0;
  bool free = false;  // not owned yet: an install claims it
  // Where another version or language differs. No install bytes: this choice doesn't have the DLC.
  struct OtherSize {
    std::string branch;
    std::string language;
    std::uint64_t download_bytes = 0;
    std::uint64_t install_bytes = 0;
    bool optional = false;  // Steam doesn't download it by itself on this choice
  };
  std::vector<OtherSize> other_sizes;
  bool optional = false;  // Steam doesn't download it by itself: an install starts with it off, if it is owned
};

// What a game's Steam app info says about installing it on this console.
struct AppDetails {
  GameId id;
  std::uint64_t account_generation = 0;
  bool available = false;  // false: Steam had no usable app info (see reason)
  std::optional<std::uint64_t> download_bytes;
  std::optional<std::uint64_t> install_bytes;
  bool windows_installable = false;  // depots and a launch entry exist for Windows/English
  std::string reason;                // short, user-facing, no identifiers
  std::vector<LaunchOption> launches;  // Windows launch options, default first
  // No option was for the public branch, so `launches` are every branch's: Steam doesn't say
  // which program starts the game.
  bool launches_any_branch = false;
  // The content languages the game's Windows depots carry, as Steam names them ("german"), in
  // listing order; empty when nothing is localised. An install can pick one of them.
  std::vector<std::string> languages;
  // The DLC with files an install can leave out, owned or free to claim (OfferedDlc). An installed game's are its
  // record's branch and language, with no other sizes.
  std::vector<DlcChoice> dlc;
  // The versions an install can choose: the public branch first, then branches with no password
  // that carry the game's own content, the most recently updated first. One entry: nothing to choose.
  std::vector<AppVersion> versions;
  // An installed game: what Steam has now against its install record (selected with the record's branch
  // and language). `newer`: a depot has another manifest, is new, or left the selection; `new_dlc`: DLC
  // the account owns now that the install doesn't hold. `target` names the selection updated to
  // (UpdateTarget): its download size is measured from the manifests (a SizeRequest with `update`). All
  // false when the licenses or the record's depots are unknown.
  struct Update {
    bool newer = false;
    bool new_dlc = false;
    std::uint64_t target = 0;
    bool available() const { return newer || new_dlc; }
  };
  Update update;
  // Steam gave no answer for the game (no session, the network): asking again may work. Not set when Steam
  // answered that it has no usable app info.
  bool failed = false;
};

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
