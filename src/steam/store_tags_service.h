#pragma once

#include "library/model.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace playden::steam {

// User-defined store tags from a game's store page HTML (the "tags":["…"] JSON
// blob Steam embeds). Not the official genres or feature categories.
struct StoreTags {
  GameId id;
  std::vector<std::string> tags;
};

std::string StorePageUrl(std::uint32_t app);

// Parses the first JSON string array assigned to "tags" in the page body.
std::vector<std::string> ParseStoreTagsFromPage(std::string_view body);

// Fetches store pages on a worker; each app is requested once per run until Forget.
class StoreTagsService {
 public:
  using Fetch = std::function<std::optional<std::string>(const std::string& url, const std::atomic<bool>& cancel)>;

  explicit StoreTagsService(Fetch fetch);
  ~StoreTagsService();
  StoreTagsService(const StoreTagsService&) = delete;
  StoreTagsService& operator=(const StoreTagsService&) = delete;

  void Request(GameId id);
  void Forget(GameId id);
  std::vector<StoreTags> Pump();
  void Shutdown();

  static constexpr std::size_t kMaxPending = 256;

 private:
  void Run();

  Fetch fetch_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<GameId> pending_;
  std::set<GameId> requested_;
  std::deque<StoreTags> results_;
  std::atomic<bool> cancel_{false};
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace playden::steam
