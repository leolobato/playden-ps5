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
  if (account_generation != generation_) {
    generation_ = account_generation;
    pending_.clear();
    requested_.clear();
    results_.clear();
    measure_pending_.clear();
    measure_requested_.clear();
    measured_.clear();
    cancel_.store(true, std::memory_order_release);
    measure_cancel_.store(true, std::memory_order_release);
  }
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
  generation_ = account_generation;
  pending_.clear();
  requested_.clear();
  results_.clear();
  measure_pending_.clear();
  measure_requested_.clear();
  measured_.clear();
  cancel_.store(true, std::memory_order_release);
  measure_cancel_.store(true, std::memory_order_release);
  wake_.notify_all();
}

void AppDetailsService::Measure(SizeRequest request, std::uint64_t account_generation) {
  if (!request.id.steam_app() || account_generation == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  if (account_generation != generation_) {
    // Another account: what was asked for the previous one goes, as for Request.
    generation_ = account_generation;
    pending_.clear();
    requested_.clear();
    results_.clear();
    measure_pending_.clear();
    measure_requested_.clear();
    measured_.clear();
    cancel_.store(true, std::memory_order_release);
    measure_cancel_.store(true, std::memory_order_release);
  }
  if (measure_requested_.count(request) || measure_pending_.size() >= kMaxPending) return;
  measure_requested_.insert(request);
  measure_pending_.push_back(std::move(request));
  wake_.notify_all();
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
    output.push_back(std::move(results_.front()));
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
    std::uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      while (!pending_.empty() && batch.size() < kBatch) {
        batch.push_back(pending_.front());
        pending_.pop_front();
      }
      generation = generation_;
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
    for (const auto& id : batch) {
      const auto found = std::find_if(fetched.begin(), fetched.end(),
                                      [id](const AppDetails& details) { return details.id == id; });
      AppDetails result = found != fetched.end() ? *found : AppDetails{};
      result.id = id;
      result.account_generation = generation;
      if (found == fetched.end()) {
        result.available = false;
        result.reason = "Steam details are unavailable right now.";
        requested_.erase(id);  // allow a later retry
      }
      results_.push_back(std::move(result));
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
