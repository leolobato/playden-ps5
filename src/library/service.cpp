#include "library/service.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <unordered_map>

namespace playden {
namespace {
constexpr std::size_t kMaximumPending = 16;
constexpr std::size_t kMaximumEvents = 64;
constexpr std::uint64_t kMaximumTimestamp =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

}  // namespace

LibraryService::LibraryService(steam::LibraryBackend& backend,
                               SessionRepository& sessions,
                               LibraryRepository* cache,
                               std::function<std::uint64_t()> unix_seconds)
    : backend_(backend), sessions_(sessions), cache_(cache),
      unix_seconds_(std::move(unix_seconds)), worker_([this] { Run(); }) {}

LibraryService::~LibraryService() { Shutdown(); }

std::uint64_t LibraryService::Now() const {
  std::uint64_t value = 0;
  if (unix_seconds_) {
    value = unix_seconds_();
  } else {
    value = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
  }
  return std::min(value, kMaximumTimestamp);
}

std::uint64_t LibraryService::Refresh(std::uint64_t account,
                                      std::uint64_t generation) {
  return Submit(Command::kRefresh, account, generation);
}

std::uint64_t LibraryService::LoadCached(std::uint64_t account,
                                         std::uint64_t generation) {
  return Submit(Command::kLoadCached, account, generation);
}

std::uint64_t LibraryService::Clear(std::uint64_t account,
                                    std::uint64_t generation) {
  return Submit(Command::kClear, account, generation);
}

std::uint64_t LibraryService::Submit(Command command, std::uint64_t account,
                                     std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || !account || generation < generation_) return 0;
  if (clear_barrier_) return 0;

  events_.erase(std::remove_if(events_.begin(), events_.end(), [this](const LibraryEvent& event) {
                  return !DestructiveRequest(event.request_id);
                }), events_.end());
  if (events_.size() >= kMaximumEvents) return 0;
  if (active_cancel_ && active_command_ != Command::kClear)
    active_cancel_->store(true);
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Job& job) {
                   return job.command != Command::kClear;
                 }), pending_.end());
  if (pending_.size() >= kMaximumPending) return 0;

  generation_ = generation;
  ++request_;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  pending_.push_back(Job{command, request_, account, generation, std::move(cancel)});
  if (command == Command::kClear) {
    clear_barrier_ = true;
    destructive_requests_.push_back(request_);
  } else {
    LibraryEvent loading;
    loading.kind = LibraryEventKind::kLoading;
    loading.request_id = request_;
    loading.account_id = account;
    loading.account_generation = generation;
    PushEvent(std::move(loading));
  }
  wake_.notify_one();
  return request_;
}

void LibraryService::Invalidate(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation < generation_) return;
  generation_ = generation;
  ++request_;
  if (active_cancel_ && active_command_ != Command::kClear)
    active_cancel_->store(true);
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Job& job) {
                   return job.command != Command::kClear;
                 }), pending_.end());
  events_.erase(std::remove_if(events_.begin(), events_.end(), [this](const LibraryEvent& event) {
                  return !DestructiveRequest(event.request_id);
                }), events_.end());
  wake_.notify_one();
}

bool LibraryService::Current(const Job& job) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopping_ && job.command != Command::kClear &&
         job.request == request_ && job.generation == generation_ &&
         !job.cancel->load();
}

void LibraryService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (joined_) return;
    stopping_ = true;
    if (active_cancel_ && active_command_ != Command::kClear)
      active_cancel_->store(true);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Job& job) {
                     return job.command != Command::kClear;
                   }), pending_.end());
    events_.erase(std::remove_if(events_.begin(), events_.end(), [this](const LibraryEvent& event) {
                    return !DestructiveRequest(event.request_id);
                  }), events_.end());
  }
  wake_.notify_one();
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  joined_ = true;
}

std::vector<LibraryEvent> LibraryService::Pump(std::size_t maximum) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LibraryEvent> result;
  const auto count = std::min(maximum, events_.size());
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto request = events_.front().request_id;
    const auto kind = events_.front().kind;
    result.push_back(std::move(events_.front()));
    events_.pop_front();
    if (kind == LibraryEventKind::kCleared || kind == LibraryEventKind::kFailure) {
      const auto found = std::find(destructive_requests_.begin(),
                                   destructive_requests_.end(), request);
      if (found != destructive_requests_.end()) destructive_requests_.erase(found);
    }
  }
  return result;
}

bool LibraryService::DestructiveRequest(std::uint64_t request) const {
  return std::find(destructive_requests_.begin(), destructive_requests_.end(), request) !=
         destructive_requests_.end();
}

void LibraryService::PushEvent(LibraryEvent event, bool destructive_terminal) {
  if (events_.size() >= kMaximumEvents) {
    const auto disposable = std::find_if(events_.begin(), events_.end(),
        [this](const LibraryEvent& existing) {
          return !DestructiveRequest(existing.request_id);
        });
    if (disposable == events_.end()) {
      if (!destructive_terminal) return;
      // Submit reserves capacity for an accepted destructive command.
      return;
    }
    events_.erase(disposable);
  }
  events_.push_back(std::move(event));
}

void LibraryService::Run() {
  for (;;) {
    std::optional<Job> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (pending_.empty()) {
        if (stopping_) return;
        continue;
      }
      job = std::move(pending_.front());
      pending_.pop_front();
      active_cancel_ = job->cancel;
      active_command_ = job->command;
    }
    try {
      Execute(*job);
    } catch (...) {
      if (job->command == Command::kClear)
        EmitFailure(*job, steam::LibraryError::kPersistence,
                    StorageError::kInvalidData, true);
      else
        EmitFailure(*job, steam::LibraryError::kProtocol);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_cancel_.reset();
      active_command_.reset();
    }
  }
}

void LibraryService::Execute(const Job& job) {
  if (job.command == Command::kRefresh) ExecuteRefresh(job);
  else if (job.command == Command::kLoadCached) ExecuteLoadCached(job);
  else ExecuteClear(job);
}

void LibraryService::ExecuteLoadCached(const Job& job) {
  if (!Current(job)) return;
  if (!cache_) {
    EmitFailure(job, steam::LibraryError::kPersistence, StorageError::kInvalidData);
    return;
  }
  auto loaded = cache_->Load(job.account);
  if (!Current(job)) return;
  if (!loaded.ok()) {
    EmitFailure(job, steam::LibraryError::kPersistence, loaded.error());
    return;
  }
  if (loaded.value().retrieved_at_unix_seconds > kMaximumTimestamp) {
    EmitFailure(job, steam::LibraryError::kPersistence, StorageError::kInvalidData);
    return;
  }
  LibraryEvent event;
  event.kind = LibraryEventKind::kCached;
  event.request_id = job.request;
  event.account_id = job.account;
  event.account_generation = job.generation;
  event.games = std::move(loaded.value().games);
  event.metadata_partial = loaded.value().metadata_partial;
  event.retrieved_at_unix_seconds = loaded.value().retrieved_at_unix_seconds;
  event.cache_available = true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stopping_ && job.request == request_ && job.generation == generation_ &&
      !job.cancel->load()) PushEvent(std::move(event));
}

void LibraryService::ExecuteRefresh(const Job& job) {
  if (!Current(job)) return;
  auto session = sessions_.Load();
  if (!Current(job)) return;
  if (!session.ok()) {
    EmitFailure(job, steam::LibraryError::kPersistence);
    return;
  }
  if (!session.value() || !job.account || session.value()->steam_id != job.account ||
      session.value()->access_token.empty()) {
    EmitFailure(job, steam::LibraryError::kNoSession);
    return;
  }

  bool cache_available = false;
  StorageError cache_error = StorageError::kNone;
  // Acquisition dates from the previous snapshot, kept when a refresh cannot obtain them.
  std::unordered_map<GameId, std::uint64_t> previous_added_at;
  if (cache_) {
    auto loaded = cache_->Load(job.account);
    if (!Current(job)) return;
    if (loaded.ok() && loaded.value().retrieved_at_unix_seconds <= kMaximumTimestamp) {
      cache_available = true;
      for (const auto& game : loaded.value().games)
        if (game.added_at_unix_seconds) previous_added_at.emplace(game.id, *game.added_at_unix_seconds);
      LibraryEvent cached;
      cached.kind = LibraryEventKind::kCached;
      cached.request_id = job.request;
      cached.account_id = job.account;
      cached.account_generation = job.generation;
      cached.games = loaded.value().games;
      cached.metadata_partial = loaded.value().metadata_partial;
      cached.retrieved_at_unix_seconds = loaded.value().retrieved_at_unix_seconds;
      cached.cache_available = true;
      std::lock_guard<std::mutex> lock(mutex_);
      if (!stopping_ && job.request == request_ && job.generation == generation_ &&
          !job.cancel->load()) PushEvent(std::move(cached));
    } else {
      cache_error = loaded.ok() ? StorageError::kInvalidData : loaded.error();
    }
  }
  if (!Current(job)) return;
  auto result = backend_.Fetch(*session.value(), *job.cancel);
  if (!Current(job)) return;
  if (result.error != steam::LibraryError::kNone) {
    EmitFailure(job, result.error, cache_error, cache_available);
    return;
  }

  LibrarySnapshot snapshot;
  snapshot.retrieved_at_unix_seconds = Now();
  snapshot.games = std::move(result.games);
  for (auto& game : snapshot.games) {
    if (game.added_at_unix_seconds) continue;
    const auto previous = previous_added_at.find(game.id);
    if (previous != previous_added_at.end()) game.added_at_unix_seconds = previous->second;
  }
  snapshot.metadata_partial = result.metadata_partial;
  // Only a library with demos needs schema 6, and one with family-shared games schema 7; one without stays
  // readable by older versions.
  if (std::any_of(snapshot.games.begin(), snapshot.games.end(), [](const GameSummary& game) { return game.demo; }))
    snapshot.schema_version = 6;
  if (std::any_of(snapshot.games.begin(), snapshot.games.end(),
                  [](const GameSummary& game) { return game.family_shared; }))
    snapshot.schema_version = 7;
  if (cache_) {
    const auto saved = cache_->Save(job.account, snapshot);
    if (!Current(job)) return;
    if (saved.ok()) {
      cache_available = true;
      cache_error = StorageError::kNone;
    } else {
      cache_error = saved.error();
    }
  }

  LibraryEvent event;
  event.kind = LibraryEventKind::kComplete;
  event.request_id = job.request;
  event.account_id = job.account;
  event.account_generation = job.generation;
  event.games = std::move(snapshot.games);
  event.metadata_partial = snapshot.metadata_partial;
  event.retrieved_at_unix_seconds = snapshot.retrieved_at_unix_seconds;
  event.cache_available = cache_available;
  event.cache_error = cache_error;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stopping_ && job.request == request_ && job.generation == generation_ &&
      !job.cancel->load()) PushEvent(std::move(event));
}

void LibraryService::ExecuteClear(const Job& job) {
  Result<void> cleared = cache_
      ? cache_->Clear(job.account)
      : Result<void>::Failure(StorageError::kInvalidData, "No library cache configured.");
  std::lock_guard<std::mutex> lock(mutex_);
  clear_barrier_ = false;
  LibraryEvent event;
  event.request_id = job.request;
  event.account_id = job.account;
  event.account_generation = job.generation;
  if (cleared.ok()) {
    event.kind = LibraryEventKind::kCleared;
  } else {
    event.kind = LibraryEventKind::kFailure;
    event.error = steam::LibraryError::kPersistence;
    event.cache_error = cleared.error();
    // Removal failed; a configured repository may still retain old data.
    event.cache_available = cache_ != nullptr;
  }
  PushEvent(std::move(event), true);
}

void LibraryService::EmitFailure(const Job& job, steam::LibraryError error,
                                 StorageError cache_error, bool cache_available) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (job.command != Command::kClear &&
      (stopping_ || job.request != request_ || job.generation != generation_ ||
       job.cancel->load())) return;
  if (job.command == Command::kClear) clear_barrier_ = false;
  LibraryEvent event;
  event.kind = LibraryEventKind::kFailure;
  event.request_id = job.request;
  event.account_id = job.account;
  event.account_generation = job.generation;
  event.error = error;
  event.cache_error = cache_error;
  event.cache_available = cache_available;
  PushEvent(std::move(event), job.command == Command::kClear);
}

}  // namespace playden
