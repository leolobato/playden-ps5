#include "steam/cloud_service.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace playden::steam {
namespace {

constexpr const char* kStatusFile = "cloud-status.txt";

// Each game's last finished result, kept next to its baseline so the Saves tab shows it at once
// after a restart: key=value lines.
const char* StateName(CloudState state) {
  switch (state) {
    case CloudState::kSynced: return "synced";
    case CloudState::kUpToDate: return "up-to-date";
    case CloudState::kConflict: return "conflict";
    case CloudState::kNeedsAccount: return "needs-account";
    case CloudState::kUnsupported: return "unsupported";
    case CloudState::kFailed: return "failed";
    case CloudState::kWaiting: return "waiting";
    default: return "";
  }
}

bool SaveStatus(const std::string& state_root, const CloudReport& report) {
  const char* state = StateName(report.state);
  if (state_root.empty() || !*state) return false;
  struct stat info {};
  if (stat(state_root.c_str(), &info) != 0 && mkdir(state_root.c_str(), 0777) != 0) return false;
  std::string message = report.message;
  std::replace(message.begin(), message.end(), '\n', ' ');
  std::string text = "version=1\nstate=" + std::string(state) + "\nmessage=" + message +
                     "\nfirst_sync=" + (report.first_sync ? "1" : "0") +
                     "\nfirst_overwrite=" + (report.first_overwrite ? "1" : "0") +
                     "\nlast_sync=" + std::to_string(report.last_sync) + "\nchecked=" + std::to_string(report.checked) +
                     "\nlocal_files=" + std::to_string(report.local_files) + "\nremote_files=" + std::to_string(report.remote_files) +
                     "\nlocal_bytes=" + std::to_string(report.local_bytes) + "\nremote_bytes=" + std::to_string(report.remote_bytes) +
                     "\nlocal_latest=" + std::to_string(report.local_latest) +
                     "\nremote_latest=" + std::to_string(report.remote_latest) + "\n";
  const auto path = state_root + "/" + kStatusFile;
  const auto temporary = path + ".playden-partial";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return false;
  const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  if (std::fclose(file) != 0 || !written || std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    return false;
  }
  return true;
}

std::optional<CloudReport> LoadStatus(const std::string& state_root) {
  FILE* file = std::fopen((state_root + "/" + kStatusFile).c_str(), "rb");
  if (!file) return std::nullopt;
  std::vector<char> line(4096);
  CloudReport report;
  bool header = false, stated = false;
  while (std::fgets(line.data(), static_cast<int>(line.size()), file)) {
    std::string text(line.data(), std::strcspn(line.data(), "\r\n"));
    const auto equals = text.find('=');
    if (equals == std::string::npos) continue;
    const auto key = text.substr(0, equals);
    const auto value = text.substr(equals + 1);
    const auto number = [&] { return std::strtoull(value.c_str(), nullptr, 10); };
    if (key == "version") header = value == "1";
    else if (key == "state") {
      for (const auto state : {CloudState::kSynced, CloudState::kUpToDate, CloudState::kConflict, CloudState::kNeedsAccount,
                               CloudState::kUnsupported, CloudState::kFailed, CloudState::kWaiting})
        if (value == StateName(state)) { report.state = state; stated = true; }
    } else if (key == "message") report.message = value;
    else if (key == "first_sync") report.first_sync = value == "1";
    else if (key == "first_overwrite") report.first_overwrite = value == "1";
    else if (key == "last_sync") report.last_sync = static_cast<std::int64_t>(number());
    else if (key == "checked") report.checked = static_cast<std::int64_t>(number());
    else if (key == "local_files") report.local_files = number();
    else if (key == "remote_files") report.remote_files = number();
    else if (key == "local_bytes") report.local_bytes = number();
    else if (key == "remote_bytes") report.remote_bytes = number();
    else if (key == "local_latest") report.local_latest = number();
    else if (key == "remote_latest") report.remote_latest = number();
  }
  std::fclose(file);
  if (!header || !stated) return std::nullopt;
  return report;
}

// Removes a folder tree without following links (the state folder is only ever Playden's files).
void RemoveTree(const std::string& path, int depth = 0) {
  struct stat info {};
  if (depth > 32 || lstat(path.c_str(), &info) != 0) return;
  if (!S_ISDIR(info.st_mode)) {
    unlink(path.c_str());
    return;
  }
  if (DIR* directory = opendir(path.c_str())) {
    std::vector<std::string> names;
    while (dirent* entry = readdir(directory)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") names.push_back(name);
    }
    closedir(directory);
    for (const auto& name : names) RemoveTree(path + "/" + name, depth + 1);
  }
  rmdir(path.c_str());
}

bool Settled(CloudState state) {
  return state == CloudState::kUpToDate || state == CloudState::kSynced || state == CloudState::kConflict ||
         state == CloudState::kNeedsAccount || state == CloudState::kUnsupported || state == CloudState::kWaiting;
}

bool Busy(CloudState state) { return state == CloudState::kChecking || state == CloudState::kSyncing; }

}  // namespace

CloudService::CloudService(CloudRunner& runner, SessionRepository& sessions) : runner_(runner), sessions_(sessions) {
  runner_.SetProgress([this](std::uint64_t done, std::uint64_t total) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = games_.find(active_);
    if (found == games_.end()) return;
    found->second.moved_bytes = done;
    found->second.total_bytes = total;
    ++revision_;
  });
  worker_ = std::thread([this] { Run(); });
}

CloudService::~CloudService() { Shutdown(); }

void CloudService::SetClock(std::function<std::int64_t()> now) {
  std::lock_guard<std::mutex> lock(mutex_);
  now_ = std::move(now);
}

std::int64_t CloudService::Now() const {
  if (now_) return now_();
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void CloudService::Set(GameId id, CloudReport report) {
  report.serial = ++revision_;
  games_[id] = std::move(report);
}

void CloudService::SetGate(std::function<bool(GameId)> may_move) {
  std::lock_guard<std::mutex> lock(mutex_);
  gate_ = std::move(may_move);
}

std::vector<GameId> CloudService::TakeDeferred() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(deferred_, {});
}

void CloudService::Finish(const CloudLocation& location, CloudReport report) {
  const auto previous = games_.find(location.id);
  const auto now = Now();
  report.last_sync = previous == games_.end() ? 0 : previous->second.last_sync;
  if (report.state == CloudState::kSynced || report.state == CloudState::kUpToDate) report.last_sync = now;
  report.checked = now;
  report.queued = false;
  report.moved_bytes = report.total_bytes = 0;
  SaveStatus(location.state_root, report);
  Set(location.id, std::move(report));
}

std::uint64_t CloudService::Sync(const CloudLocation& location) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || !location.id.steam_app() || pending_.size() >= kMaxPending) return 0;
  const auto busy = games_.find(location.id);
  // Already being checked: that check's result answers this request too.
  if (busy != games_.end() && Busy(busy->second.state)) return busy->second.serial;
  CloudReport report = busy == games_.end() ? CloudReport{} : busy->second;
  report.state = CloudState::kChecking;
  report.message = "Checking Steam Cloud…";
  report.queued = false;
  Set(location.id, std::move(report));
  Request request;
  request.location = location;
  pending_.push_back(std::move(request));
  wake_.notify_all();
  return games_[location.id].serial;
}

std::uint64_t CloudService::Resolve(const CloudLocation& location, CloudResolution resolution, bool confirm_account) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || !location.id.steam_app() || pending_.size() >= kMaxPending) return 0;
  const auto current = games_.find(location.id);
  if (current == games_.end() ||
      (current->second.state != CloudState::kConflict && current->second.state != CloudState::kNeedsAccount))
    return 0;
  auto report = current->second;
  report.state = CloudState::kSyncing;
  report.message = "Syncing saves…";
  Set(location.id, std::move(report));
  Request request;
  request.kind = Kind::kResolve;
  request.location = location;
  request.resolution = resolution;
  request.confirm_account = confirm_account;
  pending_.push_back(std::move(request));
  wake_.notify_all();
  return games_[location.id].serial;
}

void CloudService::Restore(const std::string& saves_root) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || saves_root.empty()) return;
  Request request;
  request.kind = Kind::kRestore;
  request.saves_root = saves_root;
  pending_.push_back(std::move(request));
  wake_.notify_all();
}

void CloudService::StartJob(CloudJobKind kind, std::vector<CloudLocation> locations) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || kind == CloudJobKind::kNone || job_pending_ || job_.kind != CloudJobKind::kNone) return;
  locations.erase(std::remove_if(locations.begin(), locations.end(), [](const CloudLocation& location) { return !location.id.steam_app(); }),
                  locations.end());
  job_pending_ = true;
  stop_job_.store(false);
  job_ = {kind, 0, locations.size(), {}, false};
  for (const auto& location : locations) {
    const auto [slot, added] = games_.try_emplace(location.id);
    if (added) slot->second.state = CloudState::kIdle;  // not known yet: the job checks it
    slot->second.queued = true;
  }
  ++revision_;
  Request request;
  request.kind = Kind::kJob;
  request.job = kind;
  request.locations = std::move(locations);
  pending_.push_back(std::move(request));
  wake_.notify_all();
}

void CloudService::StopJob() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (job_.kind == CloudJobKind::kNone || job_.stopping) return;
  stop_job_.store(true);
  job_.stopping = true;
  ++revision_;
}

std::uint64_t CloudService::RemoveLocal(const CloudLocation& location) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || !location.id.steam_app() || pending_.size() >= kMaxPending) return 0;
  const auto current = games_.find(location.id);
  if (current != games_.end() && Busy(current->second.state)) return 0;
  CloudReport report = current == games_.end() ? CloudReport{} : current->second;
  report.state = CloudState::kSyncing;
  report.message = "Deleting saves…";
  Set(location.id, std::move(report));
  Request request;
  request.kind = Kind::kRemove;
  request.location = location;
  pending_.push_back(std::move(request));
  wake_.notify_all();
  return games_[location.id].serial;
}

void CloudService::Forget(const CloudLocation& location) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || !location.id.steam_app() || location.state_root.empty()) return;
  Request request;
  request.kind = Kind::kForget;
  request.location = location;
  pending_.push_back(std::move(request));
  wake_.notify_all();
}

CloudSnapshot CloudService::Snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  CloudSnapshot snapshot;
  snapshot.revision = revision_;
  for (const auto& [id, report] : games_) snapshot.games.push_back({id, report});
  snapshot.job = job_;
  return snapshot;
}

void CloudService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ && !worker_.joinable()) return;
    stopping_ = true;
    cancel_.store(true);
    stop_job_.store(true);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

CloudReport CloudService::CheckAndApply(const StoredSession& session, const CloudLocation& location,
                                        const std::atomic<bool>& cancel) {
  auto report = runner_.Check(session, location, cancel);
  if (report.state != CloudState::kSyncing) return report;
  if (location.check_only) {
    report.state = CloudState::kWaiting;
    return report;
  }
  // Transfers that need no choice run straight away.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto shown = report;
    const auto previous = games_.find(location.id);
    if (previous != games_.end()) shown.last_sync = previous->second.last_sync;
    Set(location.id, std::move(shown));
    active_ = location.id;
  }
  auto applied = runner_.Apply(session, location, CloudResolution::kAutomatic, false, cancel);
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = {};
  return applied;
}

void CloudService::RunJob(const StoredSession& session, const Request& request) {
  const auto& locations = request.locations;
  std::vector<CloudDrift> drift(locations.size(), CloudDrift::kUnknown);
  if (request.job == CloudJobKind::kQuickCheck) {
    drift = runner_.QuickCheck(session, locations, cancel_);
    drift.resize(locations.size(), CloudDrift::kUnknown);
  }
  // Steam unreachable: two failed checks in a row end the job rather than failing every game slowly.
  int failures = 0;
  for (std::size_t index = 0; index < locations.size(); ++index) {
    if (cancel_.load() || stop_job_.load() || failures >= 2) break;
    const auto& location = locations[index];
    CloudReport current;
    bool known = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = games_.find(location.id);
      if (found != games_.end()) {
        current = found->second;
        known = current.state != CloudState::kIdle;
      }
      // A game whose own check is already waiting is left to that check.
      bool skip = known && Busy(current.state);
      if (!skip && request.job == CloudJobKind::kQuickCheck && known && Settled(current.state)) {
        // Unchanged since a settled result needs nothing; a game without Steam Cloud has no index and
        // is only checked again by Sync all.
        skip = drift[index] == CloudDrift::kUnchanged ||
               (drift[index] == CloudDrift::kUnknown && current.state == CloudState::kUnsupported);
      }
      if (skip) {
        if (found != games_.end()) found->second.queued = false;
        ++job_.done;
        ++revision_;
        continue;
      }
      job_.current = location.id;
      auto checking = current;
      checking.state = CloudState::kChecking;
      checking.message = "Checking Steam Cloud…";
      checking.queued = false;
      Set(location.id, std::move(checking));
    }
    // Asked again now, not only when the job started: the game may have been started since.
    std::function<bool(GameId)> gate;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      gate = gate_;
    }
    if (gate && !gate(location.id)) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto left = current;
      left.queued = false;
      Set(location.id, std::move(left));  // as it was: nothing was checked
      deferred_.push_back(location.id);
      ++job_.done;
      continue;
    }
    auto report = CheckAndApply(session, location, cancel_);
    // Playden quitting stopped the check: not a result to keep.
    if (report.state == CloudState::kFailed && report.canceled) break;
    failures = report.state == CloudState::kFailed ? failures + 1 : 0;
    std::lock_guard<std::mutex> lock(mutex_);
    Finish(location, std::move(report));
    ++job_.done;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& location : locations)
    if (const auto found = games_.find(location.id); found != games_.end()) found->second.queued = false;
  job_ = {};
  job_pending_ = false;
  stop_job_.store(false);
  ++revision_;
}

void CloudService::Run() {
  for (;;) {
    Request request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      request = std::move(pending_.front());
      pending_.pop_front();
    }
    if (request.kind == Kind::kRestore) {
      std::vector<std::pair<GameId, CloudReport>> restored;
      if (DIR* directory = opendir(request.saves_root.c_str())) {
        while (dirent* entry = readdir(directory)) {
          char* end = nullptr;
          const auto id = std::strtoull(entry->d_name, &end, 10);
          if (id == 0 || !end || *end != '\0') continue;
          if (auto report = LoadStatus(request.saves_root + "/" + entry->d_name))
            restored.emplace_back(GameId::Steam(static_cast<std::uint32_t>(id)), std::move(*report));
        }
        closedir(directory);
      }
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& [id, report] : restored) {
        // Only games this run knows nothing about yet; a result from this run is newer.
        const auto [slot, added] = games_.try_emplace(id);
        if (!added && slot->second.state != CloudState::kIdle) continue;
        report.queued = slot->second.queued;
        slot->second = std::move(report);
      }
      ++revision_;
      continue;
    }
    if (request.kind == Kind::kForget) {
      RemoveTree(request.location.state_root);
      std::lock_guard<std::mutex> lock(mutex_);
      games_.erase(request.location.id);
      ++revision_;
      continue;
    }
    const auto session = sessions_.Load();
    const bool signed_in = session.ok() && session.value();
    if (request.kind == Kind::kJob) {
      if (signed_in) {
        RunJob(*session.value(), request);
      } else {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& location : request.locations)
          if (const auto found = games_.find(location.id); found != games_.end()) found->second.queued = false;
        job_ = {};
        job_pending_ = false;
        ++revision_;
      }
      continue;
    }
    CloudReport report;
    if (!signed_in) {
      report.message = "Sign in to Steam to sync saves.";
    } else if (request.kind == Kind::kSync) {
      report = CheckAndApply(*session.value(), request.location, cancel_);
    } else if (request.kind == Kind::kRemove) {
      std::string message;
      if (!runner_.RemoveLocal(request.location, &message)) {
        report.message = message;
      } else {
        // Never downloads straight back: the Cloud copies wait until the player syncs or plays.
        report = runner_.Check(*session.value(), request.location, cancel_);
        if (report.state == CloudState::kSyncing) report.state = CloudState::kWaiting;
        report.message = message;
      }
    } else {
      // A fresh plan: the choice applies to what Cloud and this console hold now.
      report = runner_.Check(*session.value(), request.location, cancel_);
      if (report.state == CloudState::kConflict || report.state == CloudState::kNeedsAccount ||
          report.state == CloudState::kSyncing) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          active_ = request.location.id;
        }
        report = runner_.Apply(*session.value(), request.location, request.resolution, request.confirm_account, cancel_);
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = {};
      }
    }
    // Playden quitting stopped the work: a failure it caused is not saved for the next start. Any other
    // failure is kept, even one that lands while Playden quits.
    if (report.state == CloudState::kFailed && report.canceled) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Finish(request.location, std::move(report));
  }
}

}  // namespace playden::steam
