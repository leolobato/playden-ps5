#pragma once
// A store's library for tests: every Refresh answers at once with its games, or with a failure. With hold set,
// the answers wait in held until Release, so a test can deliver them late, change them or drop them.
#include "stores/library_source.h"

#include <deque>
#include <mutex>
#include <vector>

namespace playden::test {

class FakeLibrarySource final : public stores::LibrarySource {
 public:
  explicit FakeLibrarySource(Store store = Store::kLocal) : store_(store) {}
  std::vector<GameSummary> games;
  bool fail = false;
  LibraryError error = LibraryError::kPersistence;  // a failure's error
  StorageError cache_error = StorageError::kNone;
  bool cache_available = false;
  bool fail_clear = false;  // Clear answers kFailure with error
  bool hold = false;
  int refreshes = 0;
  int cached_loads = 0;
  int clears = 0;
  int invalidated = 0;
  std::uint64_t last_account = 0;
  std::uint64_t last_invalidated = 0;  // the generation Invalidate was last given
  std::deque<LibraryEvent> held;

  Store store() const override { return store_; }
  std::uint64_t Refresh(std::uint64_t account, std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++refreshes;
    return Answer(fail ? LibraryEventKind::kFailure : LibraryEventKind::kComplete, account, generation);
  }
  // The saved copy: kCached.
  std::uint64_t LoadCached(std::uint64_t account, std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++cached_loads;
    return Answer(fail ? LibraryEventKind::kFailure : LibraryEventKind::kCached, account, generation);
  }
  std::uint64_t Clear(std::uint64_t account, std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++clears;
    last_account = account;
    LibraryEvent event;
    event.kind = fail_clear ? LibraryEventKind::kFailure : LibraryEventKind::kCleared;
    event.request_id = ++request_;
    event.account_id = account;
    event.account_generation = generation;
    if (fail_clear) event.error = error;
    Queue(std::move(event));
    return request_;
  }
  // Only counted: dropping stale answers is the controller's job, and the tests check it.
  void Invalidate(std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++invalidated;
    last_invalidated = generation;
  }
  void Shutdown() override {}
  std::vector<LibraryEvent> Pump(std::size_t max_events) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LibraryEvent> result;
    while (!events_.empty() && result.size() < max_events) {
      result.push_back(std::move(events_.front()));
      events_.pop_front();
    }
    return result;
  }
  // Delivers the held answers (in order) on the next Pump.
  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!held.empty()) {
      events_.push_back(std::move(held.front()));
      held.pop_front();
    }
  }
  // Delivers an answer the test built.
  void Push(LibraryEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
  }

 private:
  std::uint64_t Answer(LibraryEventKind kind, std::uint64_t account, std::uint64_t generation) {
    last_account = account;
    LibraryEvent event;
    event.kind = kind;
    event.request_id = ++request_;
    event.account_id = account;
    event.account_generation = generation;
    event.cache_available = cache_available;
    event.cache_error = cache_error;
    if (kind == LibraryEventKind::kFailure) {
      event.error = error;
    } else {
      event.games = games;
      event.licenses_read = kind == LibraryEventKind::kComplete;
    }
    Queue(std::move(event));
    return request_;
  }
  void Queue(LibraryEvent event) {
    if (hold) held.push_back(std::move(event));
    else events_.push_back(std::move(event));
  }

  Store store_;
  std::mutex mutex_;
  std::deque<LibraryEvent> events_;
  std::uint64_t request_ = 0;
};

inline GameSummary ImportedGame(const char* uuid, const char* title) {
  GameSummary game;
  game.id = GameId::Local(uuid);
  game.title = title;
  return game;
}

}  // namespace playden::test
