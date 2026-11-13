#pragma once

#include "library/repository.h"
#include "steam/library_backend.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace playden {

// kUpdated follows a kComplete of the same request: the whole library again, with games the backend named
// in the background since.
enum class LibraryEventKind { kLoading, kCached, kComplete, kFailure, kCleared, kUpdated };

struct LibraryEvent {
  LibraryEventKind kind = LibraryEventKind::kFailure;
  std::uint64_t request_id = 0;
  std::uint64_t account_id = 0;
  std::uint64_t account_generation = 0;
  LibraryError error = LibraryError::kNone;
  std::vector<GameSummary> games;
  bool metadata_partial = false;
  std::uint64_t retrieved_at_unix_seconds = 0;
  bool cache_available = false;
  StorageError cache_error = StorageError::kNone;
  // kComplete: this load read the account's licenses too (never for a cached library).
  bool licenses_read = false;
};

// A single worker owns session, network, and cache IO. Dependencies outlive the
// service. Refresh replacement is cancelable; an accepted Clear is an ordered
// destructive barrier and cannot be replaced or dropped by Shutdown.
class LibraryService {
 public:
  LibraryService(steam::LibraryBackend&, SessionRepository&,
                 LibraryRepository* cache = nullptr,
                 std::function<std::uint64_t()> unix_seconds = {});
  ~LibraryService();
  LibraryService(const LibraryService&) = delete;
  LibraryService& operator=(const LibraryService&) = delete;

  // Generations are monotonic. Zero means stale, stopped, barrier-blocked, or
  // rejected by a bounded queue. LoadCached never reads credentials or network.
  std::uint64_t Refresh(std::uint64_t account_id, std::uint64_t account_generation);
  std::uint64_t LoadCached(std::uint64_t account_id,
                           std::uint64_t account_generation);
  std::uint64_t Clear(std::uint64_t account_id, std::uint64_t account_generation);
  void Invalidate(std::uint64_t account_generation);
  // From any thread (the backend's late-games callback): queues adding the backend's late games to the last
  // completed refresh, unless something else is queued or that refresh is no longer current.
  void NotifyLateGames();
  void Shutdown();
  std::vector<LibraryEvent> Pump(std::size_t max_events);

 private:
  enum class Command { kRefresh, kLoadCached, kClear, kMerge };
  struct Job {
    Command command;
    std::uint64_t request;
    std::uint64_t account;
    std::uint64_t generation;
    std::shared_ptr<std::atomic<bool>> cancel;
  };

  std::uint64_t Submit(Command, std::uint64_t, std::uint64_t);
  bool Current(const Job&) const;
  void Run();
  void Execute(const Job&);
  void ExecuteRefresh(const Job&);
  void ExecuteLoadCached(const Job&);
  void ExecuteClear(const Job&);
  void ExecuteMerge(const Job&);
  void QueueMergeLocked();
  void EmitFailure(const Job&, steam::LibraryError, StorageError = StorageError::kNone,
                   bool cache_available = false);
  void PushEvent(LibraryEvent, bool destructive_terminal = false);
  bool DestructiveRequest(std::uint64_t request) const;
  std::uint64_t Now() const;

  steam::LibraryBackend& backend_;
  SessionRepository& sessions_;
  LibraryRepository* cache_;
  std::function<std::uint64_t()> unix_seconds_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> pending_;
  std::shared_ptr<std::atomic<bool>> active_cancel_;
  std::optional<Command> active_command_;
  std::deque<LibraryEvent> events_;
  std::deque<std::uint64_t> destructive_requests_;
  std::uint64_t request_ = 0;
  std::uint64_t generation_ = 0;
  bool clear_barrier_ = false;
  bool stopping_ = false;
  bool joined_ = false;
  // The last refresh that completed (worker thread only), for late games.
  std::optional<LibrarySnapshot> completed_;
  std::uint64_t completed_request_ = 0;
  std::uint64_t completed_account_ = 0;
  bool completed_cache_available_ = false;
  std::thread worker_;
};

}  // namespace playden
