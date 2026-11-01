#pragma once

#include "library/model.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace playden {
class Storage;
}

namespace playden::steam {

// One app from IStoreBrowseService/GetItems: user tag names, screenshot URLs, and the
// highlight trailer's thumbnail and file. Public store data, not tied to an account.
struct StoreBrowseItem {
  std::uint32_t app = 0;
  bool tags_known = false;
  std::vector<std::string> tags;
  std::vector<std::string> screenshots;
  std::string trailer_thumbnail;
  std::string trailer_url;
  std::uint64_t fetched_at = 0;
  // From GetItems basic_info / controller categories, or filled later from appdetails.
  bool description_known = false;
  std::string description;
  std::string controller_support;  // "full", "partial", or empty
  // Genre names are not in GetItems. appdetails writes them onto this record.
  bool genres_known = false;
  std::vector<std::string> genres;
};

// A saved record is reused for this long, the same age as the cover-art cache.
constexpr std::uint64_t kStoreBrowseTtlSeconds = 14ull * 24ull * 3600ull;

std::string StoreTagListUrl();
std::string StoreBrowseItemsUrl(const std::vector<std::uint32_t>& apps);

// Names for tag ids, from IStoreService/GetTagList. Unknown ids are absent.
std::map<std::uint32_t, std::string> ParseStoreTagList(std::string_view body);
// Items in response order. Tag names come from `tags`; a missing dictionary leaves tags_known false.
std::vector<StoreBrowseItem> ParseStoreBrowseItems(std::string_view body,
                                                   const std::map<std::uint32_t, std::string>& tags);

// Fetches store browse data in batches. The game on screen is the next batch's first app.
// A record younger than kStoreBrowseTtlSeconds is answered from the local file and sends nothing.
class StoreBrowseService {
 public:
  using Fetch = std::function<std::optional<std::string>(const std::string& url, const std::atomic<bool>& cancel)>;

  explicit StoreBrowseService(Fetch fetch, std::chrono::milliseconds pause = std::chrono::milliseconds(350));
  ~StoreBrowseService();
  StoreBrowseService(const StoreBrowseService&) = delete;
  StoreBrowseService& operator=(const StoreBrowseService&) = delete;

  // The file that holds every app's record. Set before the first request.
  void SetStorage(const Storage* storage);
  // Unix seconds for the cache age. The system clock when unset.
  void SetNow(std::function<std::uint64_t()> now);

  void Request(GameId id);
  // The open game: next after whatever batch is already downloading.
  void RequestFirst(GameId id);
  // The saved record while it is under kStoreBrowseTtlSeconds, including one that has no description yet.
  std::optional<StoreBrowseItem> Cached(GameId id);
  // Writes genres onto the saved record. Description and controller support fill in only when absent.
  void RememberDetails(GameId id, std::string description, std::vector<std::string> genres,
                       std::string controller);
  std::vector<StoreBrowseItem> Pump(std::size_t maximum = 8);
  void Shutdown();

  static constexpr std::size_t kMaxPending = 256;
  static constexpr std::size_t kBatch = 50;

 private:
  void Run();
  void EnsureLoaded();
  void SaveLocked(std::unique_lock<std::mutex>& lock);
  std::uint64_t Now() const;
  bool Current(const StoreBrowseItem& item, std::uint64_t now) const;
  bool Fresh(const StoreBrowseItem& item, std::uint64_t now) const;
  void Enqueue(GameId id, bool first);

  Fetch fetch_;
  std::chrono::milliseconds pause_;
  const Storage* storage_ = nullptr;
  std::function<std::uint64_t()> now_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<GameId> pending_;
  std::set<GameId> requested_;
  std::deque<StoreBrowseItem> results_;
  std::map<std::uint32_t, StoreBrowseItem> cached_;
  std::map<std::uint32_t, std::string> tag_names_;
  std::uint64_t tag_names_at_ = 0;
  bool cache_loaded_ = false;
  std::atomic<bool> cancel_{false};
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace playden::steam
