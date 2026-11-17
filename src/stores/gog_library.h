// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gog/http.h"
#include "gog/product_cache.h"
#include "gog/session.h"
#include "library/cache.h"
#include "library/collections_cache.h"
#include "library/service.h"
#include "stores/library_source.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace playden::stores {

// GOG's library on its own worker thread: the owned IDs from embed.gog.com, then each product's gamesdb entry, one at
// a time, cached per product with its ETag (gog::ProductCache) so an unchanged product costs a 304. The account is
// gog::AccountKey of the signed-in user (the session's identity), never the GOG user ID.
//
// Refresh answers kLoading, kCached with the saved library when there is one, kCached after every batch of
// kBatchSize products (the products fetched so far, plus the saved entries of those not fetched yet), then kComplete
// with every game, saved as library-gog-<account>.cache. A product that fails keeps its saved entry, or waits for the
// next refresh; kMaxProductsWithoutAnswer products in a row with no answer at all end the refresh with a network
// failure (the network went away). A 401 on the owned list makes the session drop its access token and refresh once;
// only a second 401 answers kCredentialsRevoked. Clear is an ordered barrier, as LibraryService's: it runs, and is
// answered, even after Invalidate.
//
// gog.log gets counts only ("library owned=N fetched=N not_modified=N missing=N hidden=N failed=N") and failure
// kinds: never a URL, an ID, a title, a token or the account.
class GogLibrarySource final : public LibrarySource {
 public:
  using LogSink = std::function<void(const std::string&)>;

  // transport belongs to this source's worker (gog::ClientTransport); the session's own transport stays the
  // session's. gog_cache is a LibraryCache with the "library-gog-" prefix. user_root holds gog/products.
  // tags (optional) is a CollectionsCache with the "collections-gog-" prefix: the library tags (#536), read after the
  // products on each Refresh and sent with every kCached and kComplete. A tags read that fails never fails the
  // library and keeps the saved tags. Clear deletes them too.
  GogLibrarySource(gog::Session& session, gog::Transport transport, LibraryCache& gog_cache, std::string user_root,
                   LogSink log = {}, std::function<std::uint64_t()> unix_seconds = {},
                   CollectionsRepository* tags = nullptr);
  ~GogLibrarySource() override;
  GogLibrarySource(const GogLibrarySource&) = delete;
  GogLibrarySource& operator=(const GogLibrarySource&) = delete;

  Store store() const override { return Store::kGog; }
  std::uint64_t Refresh(std::uint64_t account, std::uint64_t generation) override;
  std::uint64_t LoadCached(std::uint64_t account, std::uint64_t generation) override;
  // Deletes the account's saved library and every saved product.
  std::uint64_t Clear(std::uint64_t account, std::uint64_t generation) override;
  void Invalidate(std::uint64_t generation) override;
  void Shutdown() override;
  std::vector<LibraryEvent> Pump(std::size_t max_events) override;

  static constexpr std::size_t kBatchSize = 50;
  static constexpr std::size_t kMaxProductsWithoutAnswer = 3;

 private:
  enum class Command { kRefresh, kLoadCached, kClear };
  struct Job {
    Command command = Command::kRefresh;
    std::uint64_t request = 0;
    std::uint64_t account = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
  };

  std::uint64_t Submit(Command command, std::uint64_t account, std::uint64_t generation);
  bool Current(const Job& job) const;
  bool CurrentLocked(const Job& job) const;
  void Run();
  void ExecuteRefresh(const Job& job);
  void ExecuteLoadCached(const Job& job);
  void ExecuteClear(const Job& job);
  LibraryEvent Event(const Job& job, LibraryEventKind kind) const;
  // Queues event when job is still current; a kCached replaces this request's kCached still waiting (each carries
  // the whole library).
  void Emit(const Job& job, LibraryEvent event);
  void EmitFailure(const Job& job, LibraryError error, StorageError cache_error = StorageError::kNone,
                   bool cache_available = false);
  void PushLocked(LibraryEvent event, bool destructive_terminal = false);
  bool DestructiveLocked(std::uint64_t request) const;
  void DropEventsLocked();
  std::uint64_t Now() const;
  void Log(const std::string& line) const;
  std::optional<std::vector<LibraryCollection>> SavedTags(std::uint64_t account) const;
  // The tags read from GOG now, or none (the saved ones then stay).
  std::optional<std::vector<LibraryCollection>> FetchTags(const Job& job);

  gog::Session& session_;
  gog::Transport transport_;
  LibraryCache& cache_;
  CollectionsRepository* tags_;
  gog::ProductCache products_;
  LogSink log_;
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
  std::thread worker_;
};

}  // namespace playden::stores
