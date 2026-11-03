#include "steam/app_details_service.h"

#include <algorithm>
#include <iterator>

namespace playden::steam {

AppDetailsService::AppDetailsService(AppDetailsSource& source, SessionRepository& sessions)
    : source_(source), sessions_(sessions), worker_([this] { Run(); }), measure_worker_([this] { RunMeasures(); }) {}

AppDetailsService::~AppDetailsService() { Shutdown(); }

void AppDetailsService::Request(GameId id, std::uint64_t account_generation, bool first) {
  if (!id.steam_app() || account_generation == 0) return;  // Steam games only
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  if (account_generation != generation_) Reset(account_generation);
  if (requested_.count(id)) {
    // Queued already: it goes first. In flight or answered: nothing to do.
    if (const auto queued = std::find(pending_.begin(), pending_.end(), id); first && queued != pending_.end()) {
      pending_.erase(queued);
      pending_.push_front(id);
    }
    return;
  }
  if (pending_.size() >= kMaxPending && !first) return;
  requested_.insert(id);
  if (first) pending_.push_front(id);
  else pending_.push_back(id);
  wake_.notify_all();
}

void AppDetailsService::Refresh(GameId id, std::uint64_t account_generation, bool first) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (account_generation == generation_ && std::find(pending_.begin(), pending_.end(), id) == pending_.end())
      requested_.erase(id);
  }
  Request(id, account_generation, first);
}

void AppDetailsService::Invalidate(std::uint64_t account_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Reset(account_generation);
  wake_.notify_all();
}

void AppDetailsService::Reset(std::uint64_t account_generation) {
  generation_ = account_generation;
  pending_.clear();
  requested_.clear();
  results_.clear();
  check_pending_.clear();
  check_waiting_.clear();
  check_ = {};
  measure_pending_.clear();
  measure_requested_.clear();
  measured_.clear();
  cancel_.store(true, std::memory_order_release);
  measure_cancel_.store(true, std::memory_order_release);
}

bool AppDetailsService::CheckInstalled(const std::vector<GameId>& ids, std::uint64_t account_generation) {
  if (account_generation == 0) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return false;
  if (account_generation != generation_) Reset(account_generation);
  if (check_.total && !check_.done()) return false;
  check_ = {};
  check_pending_.clear();
  check_waiting_.clear();
  ++check_serial_;
  std::set<GameId> seen;
  for (const auto& id : ids)
    if (id.steam_app() && seen.insert(id).second) check_pending_.push_back(id);
  check_.account_generation = account_generation;
  check_.total = check_pending_.size();
  wake_.notify_all();
  return check_.total != 0;
}

InstalledCheck AppDetailsService::CheckProgress() {
  std::lock_guard<std::mutex> lock(mutex_);
  return check_;
}

bool AppDetailsService::Measure(SizeRequest request, std::uint64_t account_generation) {
  if (!request.id.steam_app() || account_generation == 0) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return false;
  // Another account: what was asked for the previous one goes, as for Request.
  if (account_generation != generation_) Reset(account_generation);
  if (measure_requested_.count(request)) return true;
  // Each kind has its own room: updates' sizes waiting never keep out the install sheet's.
  const auto same_kind = std::count_if(measure_pending_.begin(), measure_pending_.end(),
                                       [&](const SizeRequest& queued) { return queued.update == request.update; });
  if (static_cast<std::size_t>(same_kind) >= kMaxPending) return false;
  measure_requested_.insert(request);
  // The install sheet's choice is waited for on screen: ahead of the updates' sizes, and the page opened last first.
  if (request.update) {
    measure_pending_.push_back(std::move(request));
  } else {
    measure_pending_.insert(measure_pending_.begin(), std::move(request));
  }
  wake_.notify_all();
  return true;
}

std::vector<MeasuredSize> AppDetailsService::PumpMeasured() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MeasuredSize> output(std::make_move_iterator(measured_.begin()), std::make_move_iterator(measured_.end()));
  measured_.clear();
  return output;
}

std::vector<AppDetails> AppDetailsService::Pump(std::size_t maximum) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AppDetails> output;
  while (!results_.empty() && output.size() < maximum) {
    auto& [details, check] = results_.front();
    // The check counts a game once it is handed out, so its progress never runs ahead of the answers. An
    // earlier answer for the same game (a page's, still queued) is not the check's.
    if (check == check_serial_ && check_waiting_.erase(details.id)) {
      ++check_.answered;
      if (details.failed) ++check_.failed;
    }
    output.push_back(std::move(details));
    results_.pop_front();
  }
  return output;
}

void AppDetailsService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ && !worker_.joinable() && !measure_worker_.joinable()) return;
    stopping_ = true;
    cancel_.store(true, std::memory_order_release);
    measure_cancel_.store(true, std::memory_order_release);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (measure_worker_.joinable()) measure_worker_.join();
}

void AppDetailsService::Run() {
  for (;;) {
    std::vector<GameId> batch;
    std::set<GameId> checked;  // the games the check asked for in this batch
    std::uint64_t generation = 0, check = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty() || !check_pending_.empty(); });
      if (stopping_) return;
      while (!pending_.empty() && batch.size() < kBatch) {
        batch.push_back(pending_.front());
        pending_.pop_front();
      }
      // The installed check fills what the queue leaves: it waits behind every request, a page's included.
      while (!check_pending_.empty() && batch.size() < kBatch) {
        const auto id = check_pending_.front();
        check_pending_.pop_front();
        ++check_.asked;
        check_waiting_.insert(id);
        checked.insert(id);
        requested_.insert(id);
        if (std::find(batch.begin(), batch.end(), id) == batch.end()) batch.push_back(id);
      }
      generation = generation_;
      check = check_serial_;
      cancel_.store(false, std::memory_order_release);
    }
    std::vector<AppDetails> fetched;
    const auto session = sessions_.Load();
    if (session.ok() && session.value()) {
      fetched = source_.Fetch(*session.value(), batch, cancel_);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    if (generation != generation_ || cancel_.load(std::memory_order_acquire)) continue;
    if (fetched.empty() && !check_pending_.empty()) {
      // Nothing answered (no session, Steam unreachable): the rest of the check would fail the same way.
      check_.asked += check_pending_.size();
      check_.answered += check_pending_.size();
      check_.failed += check_pending_.size();
      check_pending_.clear();
    }
    for (const auto& id : batch) {
      const auto found = std::find_if(fetched.begin(), fetched.end(),
                                      [id](const AppDetails& details) { return details.id == id; });
      AppDetails result = found != fetched.end() ? *found : AppDetails{};
      result.id = id;
      result.account_generation = generation;
      if (found == fetched.end()) {
        result.available = false;
        result.failed = true;
        result.reason = "Steam details are unavailable right now.";
        requested_.erase(id);  // allow a later retry
      }
      results_.emplace_back(std::move(result), checked.count(id) ? check : 0);
    }
  }
}

// Size checks have their own worker: one that waits on manifests never holds up a game's page.
void AppDetailsService::RunMeasures() {
  for (;;) {
    SizeRequest measure;
    std::uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !measure_pending_.empty(); });
      if (stopping_) return;
      measure = std::move(measure_pending_.front());
      measure_pending_.pop_front();
      generation = generation_;
      measure_cancel_.store(false, std::memory_order_release);
    }
    MeasuredSize result{measure};
    const auto session = sessions_.Load();
    if (session.ok() && session.value()) result = source_.Measure(*session.value(), measure, measure_cancel_);
    else result.failed = true;
    result.request = measure;
    result.account_generation = generation;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    measure_requested_.erase(measure);
    if (generation != generation_ || measure_cancel_.load(std::memory_order_acquire)) continue;
    measured_.push_back(std::move(result));
  }
}

}  // namespace playden::steam
