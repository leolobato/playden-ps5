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

// What the Steam store says about a game, for its details page: the short
// description and genres as plain text, and the controller support Steam lists
// ("full", "partial", or empty).
struct StoreDetails {
  GameId id;
  std::string description;
  std::vector<std::string> genres;
  std::string controller_support;
  // Steam lists category 8, "Valve Anti-Cheat enabled": its servers can ban an account for a changed game.
  bool vac = false;
};

// The store's appdetails URL for one game, in English.
std::string StoreDetailsUrl(std::uint32_t app);
// Parses a store appdetails response; nothing when it is not a successful answer for this game.
std::optional<StoreDetails> ParseStoreDetails(std::uint32_t app, std::string_view body);

// Fetches store details for games the player opens, one at a time on its own worker.
// Each game is asked once per run; a failed answer is not retried until Forget.
class StoreDetailsService {
 public:
  // Returns the response body of a GET, or nothing (network failure, non-200 status).
  using Fetch = std::function<std::optional<std::string>(const std::string& url, const std::atomic<bool>& cancel)>;

  explicit StoreDetailsService(Fetch fetch);
  ~StoreDetailsService();
  StoreDetailsService(const StoreDetailsService&) = delete;
  StoreDetailsService& operator=(const StoreDetailsService&) = delete;

  void Request(GameId id);
  std::vector<StoreDetails> Pump();
  void Shutdown();

  static constexpr std::size_t kMaxPending = 32;

 private:
  void Run();

  Fetch fetch_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<GameId> pending_;
  std::set<GameId> requested_;
  std::deque<StoreDetails> results_;
  std::atomic<bool> cancel_{false};
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace playden::steam
