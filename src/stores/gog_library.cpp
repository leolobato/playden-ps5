// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/gog_library.h"

#include "gog/api.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace playden::stores {
namespace {

constexpr std::size_t kMaximumPending = 16;
constexpr std::size_t kMaximumEvents = 64;
constexpr std::uint64_t kMaximumTimestamp = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
// A library with a hero picture needs schema 8 (LibraryCache writes 7 when no game has one).
constexpr std::uint32_t kSchema = 8;

const char* ErrorName(LibraryError error) {
  switch (error) {
    case LibraryError::kNone: return "none";
    case LibraryError::kCanceled: return "canceled";
    case LibraryError::kNetwork: return "network";
    case LibraryError::kProtocol: return "protocol";
    case LibraryError::kCredentialsRevoked: return "revoked";
    case LibraryError::kNoSession: return "no_session";
    case LibraryError::kPersistence: return "persistence";
  }
  return "failed";
}

GameSummary Summary(std::uint64_t id, const gog::CachedProduct& product) {
  GameSummary game;
  game.id = GameId::Gog(id);
  game.title = product.title;
  game.description = product.summary;
  game.developer = product.developer;
  game.artwork_source = product.cover;
  game.hero_source = product.hero;
  return game;
}

gog::CachedProduct Cached(const gog::ProductInfo& info, std::string etag) {
  gog::CachedProduct product;
  product.etag = std::move(etag);
  product.kind = info.kind;
  if (info.kind == gog::ProductInfo::Kind::kGame) {
    product.title = info.title;
    product.summary = info.summary;
    product.developer = info.developer;
    product.cover = info.cover;
    product.hero = info.hero;
  }
  return product;
}

}  // namespace

GogLibrarySource::GogLibrarySource(gog::Session& session, gog::Transport transport, LibraryCache& gog_cache,
                                   std::string user_root, LogSink log, std::function<std::uint64_t()> unix_seconds,
                                   CollectionsRepository* tags)
    : session_(session),
      transport_(std::move(transport)),
      cache_(gog_cache),
      tags_(tags),
      products_(std::move(user_root)),
      log_(std::move(log)),
      unix_seconds_(std::move(unix_seconds)),
      worker_([this] { Run(); }) {}

GogLibrarySource::~GogLibrarySource() { Shutdown(); }

void GogLibrarySource::Log(const std::string& line) const {
  if (log_) log_(line);
}

std::uint64_t GogLibrarySource::Now() const {
  std::uint64_t value = 0;
  if (unix_seconds_) {
    value = unix_seconds_();
  } else {
    value = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  }
  return std::min(value, kMaximumTimestamp);
}

std::uint64_t GogLibrarySource::Refresh(std::uint64_t account, std::uint64_t generation) {
  return Submit(Command::kRefresh, account, generation);
}

std::uint64_t GogLibrarySource::LoadCached(std::uint64_t account, std::uint64_t generation) {
  return Submit(Command::kLoadCached, account, generation);
}

std::uint64_t GogLibrarySource::Clear(std::uint64_t account, std::uint64_t generation) {
  return Submit(Command::kClear, account, generation);
}

bool GogLibrarySource::DestructiveLocked(std::uint64_t request) const {
  return std::find(destructive_requests_.begin(), destructive_requests_.end(), request) !=
         destructive_requests_.end();
}

void GogLibrarySource::DropEventsLocked() {
  events_.erase(std::remove_if(events_.begin(), events_.end(),
                               [this](const LibraryEvent& event) { return !DestructiveLocked(event.request_id); }),
                events_.end());
}

std::uint64_t GogLibrarySource::Submit(Command command, std::uint64_t account, std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || !account || generation < generation_ || clear_barrier_) return 0;
  DropEventsLocked();
  if (events_.size() >= kMaximumEvents) return 0;
  if (active_cancel_ && active_command_ != Command::kClear) active_cancel_->store(true);
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [](const Job& job) { return job.command != Command::kClear; }),
                 pending_.end());
  if (pending_.size() >= kMaximumPending) return 0;
  generation_ = generation;
  ++request_;
  pending_.push_back(Job{command, request_, account, generation, std::make_shared<std::atomic<bool>>(false)});
  if (command == Command::kClear) {
    clear_barrier_ = true;
    destructive_requests_.push_back(request_);
  } else {
    LibraryEvent loading;
    loading.kind = LibraryEventKind::kLoading;
    loading.request_id = request_;
    loading.account_id = account;
    loading.account_generation = generation;
    PushLocked(std::move(loading));
  }
  wake_.notify_one();
  return request_;
}

void GogLibrarySource::Invalidate(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation < generation_) return;
  generation_ = generation;
  ++request_;
  if (active_cancel_ && active_command_ != Command::kClear) active_cancel_->store(true);
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [](const Job& job) { return job.command != Command::kClear; }),
                 pending_.end());
  DropEventsLocked();
  wake_.notify_one();
}

void GogLibrarySource::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (joined_) return;
    stopping_ = true;
    if (active_cancel_ && active_command_ != Command::kClear) active_cancel_->store(true);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [](const Job& job) { return job.command != Command::kClear; }),
                   pending_.end());
    DropEventsLocked();
  }
  wake_.notify_one();
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  joined_ = true;
}

std::vector<LibraryEvent> GogLibrarySource::Pump(std::size_t max_events) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LibraryEvent> result;
  const auto count = std::min(max_events, events_.size());
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto request = events_.front().request_id;
    const auto kind = events_.front().kind;
    result.push_back(std::move(events_.front()));
    events_.pop_front();
    if (kind == LibraryEventKind::kCleared || kind == LibraryEventKind::kFailure) {
      const auto found = std::find(destructive_requests_.begin(), destructive_requests_.end(), request);
      if (found != destructive_requests_.end()) destructive_requests_.erase(found);
    }
  }
  return result;
}

void GogLibrarySource::PushLocked(LibraryEvent event, bool destructive_terminal) {
  if (events_.size() >= kMaximumEvents) {
    const auto disposable = std::find_if(events_.begin(), events_.end(), [this](const LibraryEvent& existing) {
      return !DestructiveLocked(existing.request_id);
    });
    // Submit keeps room for an accepted destructive command's answer.
    if (disposable == events_.end()) return;
    events_.erase(disposable);
  }
  (void)destructive_terminal;
  events_.push_back(std::move(event));
}

bool GogLibrarySource::CurrentLocked(const Job& job) const {
  return !stopping_ && job.command != Command::kClear && job.request == request_ && job.generation == generation_ &&
         !job.cancel->load();
}

bool GogLibrarySource::Current(const Job& job) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return CurrentLocked(job);
}

LibraryEvent GogLibrarySource::Event(const Job& job, LibraryEventKind kind) const {
  LibraryEvent event;
  event.kind = kind;
  event.request_id = job.request;
  event.account_id = job.account;
  event.account_generation = job.generation;
  return event;
}

void GogLibrarySource::Emit(const Job& job, LibraryEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!CurrentLocked(job)) return;
  if (event.kind == LibraryEventKind::kCached)
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [&](const LibraryEvent& queued) {
                                   return queued.kind == LibraryEventKind::kCached && queued.request_id == job.request;
                                 }),
                  events_.end());
  PushLocked(std::move(event));
}

void GogLibrarySource::EmitFailure(const Job& job, LibraryError error, StorageError cache_error,
                                   bool cache_available) {
  Log(std::string("library result=") + ErrorName(error));
  std::lock_guard<std::mutex> lock(mutex_);
  if (job.command != Command::kClear && !CurrentLocked(job)) return;
  if (job.command == Command::kClear) clear_barrier_ = false;
  LibraryEvent event = Event(job, LibraryEventKind::kFailure);
  event.error = error;
  event.cache_error = cache_error;
  event.cache_available = cache_available;
  PushLocked(std::move(event), job.command == Command::kClear);
}

void GogLibrarySource::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (pending_.empty()) {
        if (stopping_) return;
        continue;
      }
      job = std::move(pending_.front());
      pending_.pop_front();
      active_cancel_ = job.cancel;
      active_command_ = job.command;
    }
    switch (job.command) {
      case Command::kRefresh: ExecuteRefresh(job); break;
      case Command::kLoadCached: ExecuteLoadCached(job); break;
      case Command::kClear: ExecuteClear(job); break;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    active_cancel_.reset();
    active_command_.reset();
  }
}

std::optional<std::vector<LibraryCollection>> GogLibrarySource::SavedTags(std::uint64_t account) const {
  if (!tags_) return std::nullopt;
  auto loaded = tags_->Load(account);
  if (!loaded.ok()) return std::nullopt;
  return std::move(loaded.value());
}

std::optional<std::vector<LibraryCollection>> GogLibrarySource::FetchTags(const Job& job) {
  if (!tags_) return std::nullopt;
  auto token = session_.Token(*job.cancel);
  if (token.status != gog::AccessToken::Status::kOk || token.generation != session_.generation() ||
      session_.identity().account_key != job.account) {
    Log("library tags result=no_token");
    return std::nullopt;
  }
  auto result = gog::FetchLibraryTags(transport_, token.token, *job.cancel);
  if (result.kind != gog::TagsResult::Kind::kOk) {
    Log("library tags status=" + std::to_string(result.status) + " pages=" + std::to_string(result.pages));
    return std::nullopt;
  }
  std::size_t members = 0;
  std::vector<LibraryCollection> tags;
  for (auto& tag : result.tags) {
    LibraryCollection collection;
    collection.id = std::move(tag.id);
    collection.name = std::move(tag.name);
    for (const auto product : tag.products) collection.members.push_back(GameId::Gog(product));
    members += collection.members.size();
    tags.push_back(std::move(collection));
  }
  Log("library tags=" + std::to_string(tags.size()) + " members=" + std::to_string(members) +
      " pages=" + std::to_string(result.pages));
  return tags;
}

void GogLibrarySource::ExecuteLoadCached(const Job& job) {
  if (!Current(job)) return;
  auto loaded = cache_.Load(job.account);
  if (!loaded.ok() || loaded.value().retrieved_at_unix_seconds > kMaximumTimestamp) {
    EmitFailure(job, LibraryError::kPersistence, loaded.ok() ? StorageError::kInvalidData : loaded.error());
    return;
  }
  auto event = Event(job, LibraryEventKind::kCached);
  event.games = std::move(loaded.value().games);
  event.retrieved_at_unix_seconds = loaded.value().retrieved_at_unix_seconds;
  event.cache_available = true;
  if (auto tags = SavedTags(job.account)) {
    event.collections = std::move(*tags);
    event.collections_known = true;
  }
  Emit(job, std::move(event));
}

void GogLibrarySource::ExecuteClear(const Job& job) {
  auto cleared = cache_.Clear(job.account);
  // The saved tags go with the saved library.
  if (tags_) {
    const auto tags_cleared = tags_->Clear(job.account);
    if (cleared.ok() && !tags_cleared.ok()) cleared = tags_cleared;
  }
  const bool products = products_.Clear();
  Log(std::string("library clear result=") + (cleared.ok() && products ? "ok" : "failed"));
  std::lock_guard<std::mutex> lock(mutex_);
  clear_barrier_ = false;
  LibraryEvent event = Event(job, LibraryEventKind::kCleared);
  if (!cleared.ok() || !products) {
    event.kind = LibraryEventKind::kFailure;
    event.error = LibraryError::kPersistence;
    event.cache_error = cleared.ok() ? StorageError::kRemoveFailed : cleared.error();
    event.cache_available = true;  // some of the saved library may remain
  }
  PushLocked(std::move(event), true);
}

void GogLibrarySource::ExecuteRefresh(const Job& job) {
  if (!Current(job)) return;
  // Only the signed-in account's library: a refresh for an account that has signed out (or another one) is refused.
  if (session_.identity().account_key != job.account) {
    EmitFailure(job, LibraryError::kNoSession);
    return;
  }

  bool cache_available = false;
  // The tags saved by an earlier refresh: sent with the saved library and every batch, kept if GOG can't give them.
  std::optional<std::vector<LibraryCollection>> tags = SavedTags(job.account);
  const auto with_tags = [&tags](LibraryEvent& event) {
    if (!tags) return;
    event.collections = *tags;
    event.collections_known = true;
  };
  {
    auto saved = cache_.Load(job.account);
    if (!Current(job)) return;
    if (saved.ok() && saved.value().retrieved_at_unix_seconds <= kMaximumTimestamp) {
      cache_available = true;
      auto cached = Event(job, LibraryEventKind::kCached);
      cached.games = std::move(saved.value().games);
      cached.retrieved_at_unix_seconds = saved.value().retrieved_at_unix_seconds;
      cached.cache_available = true;
      with_tags(cached);
      Emit(job, std::move(cached));
    }
  }

  gog::OwnedResult owned;
  // A 401 can be an access token GOG ended early: the session drops it and refreshes once. A second 401, with a token
  // just refreshed, is the account's.
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto token = session_.Token(*job.cancel);
    if (!Current(job)) return;
    switch (token.status) {
      case gog::AccessToken::Status::kOk: break;
      case gog::AccessToken::Status::kRevoked:
        EmitFailure(job, LibraryError::kCredentialsRevoked, StorageError::kNone, cache_available);
        return;
      case gog::AccessToken::Status::kNetwork:
        EmitFailure(job, LibraryError::kNetwork, StorageError::kNone, cache_available);
        return;
      case gog::AccessToken::Status::kSignedOut:
        EmitFailure(job, LibraryError::kNoSession, StorageError::kNone, cache_available);
        return;
      case gog::AccessToken::Status::kCanceled: return;
    }
    if (token.generation != session_.generation() || session_.identity().account_key != job.account) {
      EmitFailure(job, LibraryError::kNoSession, StorageError::kNone, cache_available);
      return;
    }
    owned = gog::FetchOwned(transport_, token.token, *job.cancel);
    if (!Current(job)) return;
    if (owned.kind != gog::OwnedResult::Kind::kRejected || attempt > 0) break;
    Log("library owned status=" + std::to_string(owned.status) + " retry=refresh");
    session_.InvalidateAccessToken(token.generation, token.token);
  }  // each token wipes itself at the end of its pass
  switch (owned.kind) {
    case gog::OwnedResult::Kind::kOk: break;
    case gog::OwnedResult::Kind::kRejected:
      Log("library owned status=" + std::to_string(owned.status));
      EmitFailure(job, LibraryError::kCredentialsRevoked, StorageError::kNone, cache_available);
      return;
    case gog::OwnedResult::Kind::kNetwork:
      Log("library owned status=" + std::to_string(owned.status));
      EmitFailure(job, LibraryError::kNetwork, StorageError::kNone, cache_available);
      return;
    case gog::OwnedResult::Kind::kProtocol:
      Log("library owned status=" + std::to_string(owned.status));
      EmitFailure(job, LibraryError::kProtocol, StorageError::kNone, cache_available);
      return;
  }

  // What is known of each owned product: its saved entry until it is fetched.
  const auto& ids = owned.ids;
  std::vector<std::optional<gog::CachedProduct>> known(ids.size());
  for (std::size_t i = 0; i < ids.size(); ++i) {
    known[i] = products_.Load(ids[i]);
    if (job.cancel->load()) return;
  }
  const auto games = [&] {
    std::vector<GameSummary> list;
    for (std::size_t i = 0; i < ids.size(); ++i)
      if (known[i] && known[i]->kind == gog::ProductInfo::Kind::kGame) list.push_back(Summary(ids[i], *known[i]));
    return list;
  };

  const auto counts = [&](std::size_t fetched, std::size_t not_modified, std::size_t missing, std::size_t failed) {
    std::size_t hidden = 0;
    for (const auto& product : known)
      if (product && product->kind == gog::ProductInfo::Kind::kHidden) ++hidden;
    Log("library owned=" + std::to_string(ids.size()) + " fetched=" + std::to_string(fetched) +
        " not_modified=" + std::to_string(not_modified) + " missing=" + std::to_string(missing) +
        " hidden=" + std::to_string(hidden) + " failed=" + std::to_string(failed));
  };

  std::size_t fetched = 0, not_modified = 0, missing = 0, failed = 0;
  std::size_t unanswered = 0;  // products in a row with no answer at all
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (!Current(job)) return;
    const std::uint64_t id = ids[i];
    const std::string etag = known[i] ? known[i]->etag : std::string();
    auto answer = gog::FetchProduct(transport_, id, etag, *job.cancel);
    if (!Current(job)) return;
    unanswered = answer.status == 0 ? unanswered + 1 : 0;
    if (unanswered >= kMaxProductsWithoutAnswer) {
      // The network went away: the rest would each wait for their timeout. What came so far was shown in batches; the
      // saved library stays as it was.
      counts(fetched, not_modified, missing, failed + 1);
      Log("library products result=no_answer asked=" + std::to_string(i + 1));
      EmitFailure(job, LibraryError::kNetwork, StorageError::kNone, cache_available);
      return;
    }
    switch (answer.kind) {
      case gog::ProductResult::Kind::kModified: {
        const auto info = gog::ParseProduct(answer.body, id);
        if (info.kind == gog::ProductInfo::Kind::kInvalid) {
          ++failed;  // the saved entry, if any, stays; never saved with this ETag, so the next refresh asks again
          break;
        }
        ++fetched;
        known[i] = Cached(info, std::move(answer.etag));
        products_.Save(id, *known[i]);  // a failed save only costs a full answer next time
        break;
      }
      case gog::ProductResult::Kind::kNotModified:
        if (known[i]) ++not_modified;
        else ++failed;  // nothing to reuse: asked again without an ETag next time
        break;
      case gog::ProductResult::Kind::kMissing:
        ++missing;
        known[i].reset();
        products_.Remove(id);
        break;
      case gog::ProductResult::Kind::kFailed:
        ++failed;  // the saved entry, if any, stays
        break;
    }
    if ((i + 1) % kBatchSize == 0 || i + 1 == ids.size()) {
      auto batch = Event(job, LibraryEventKind::kCached);
      batch.games = games();
      batch.retrieved_at_unix_seconds = Now();
      batch.cache_available = cache_available;
      with_tags(batch);
      Emit(job, std::move(batch));
    }
  }

  counts(fetched, not_modified, missing, failed);

  LibrarySnapshot snapshot;
  snapshot.schema_version = kSchema;
  snapshot.retrieved_at_unix_seconds = Now();
  snapshot.games = games();
  if (!Current(job)) return;
  StorageError cache_error = StorageError::kNone;
  const auto saved = cache_.Save(job.account, snapshot);
  if (saved.ok()) cache_available = true;
  else cache_error = saved.error();
  if (!saved.ok()) Log("library save result=failed");

  if (auto fresh = FetchTags(job)) {
    if (!Current(job)) return;
    if (!tags_->Save(job.account, *fresh).ok()) Log("library tags save result=failed");
    tags = std::move(fresh);
  }
  if (!Current(job)) return;

  auto complete = Event(job, LibraryEventKind::kComplete);
  complete.games = std::move(snapshot.games);
  complete.retrieved_at_unix_seconds = snapshot.retrieved_at_unix_seconds;
  complete.cache_available = cache_available;
  complete.cache_error = cache_error;
  with_tags(complete);
  Emit(job, std::move(complete));
}

}  // namespace playden::stores
