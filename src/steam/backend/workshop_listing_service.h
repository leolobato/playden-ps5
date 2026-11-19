// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/workshop_client.h"
#include "steam/backend/workshop_items.h"
#include "steam/session_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace playden::steam {

// What the Workshop items sheet lists for one game (#856): the player's subscriptions next to what Playden holds in
// the game's bottle. Plain values for the UI: no session, no paths.

enum class WorkshopListingError {
  kNone,
  kNoSession,  // no stored Steam sign-in
  kNetwork,    // Steam out of reach
  kSteam,      // Steam refused or sent something Playden can't read
  kNoBottle,   // the game was never prepared: nowhere to put items yet
  kStorage,    // the bottle's Workshop folder can't be read
};

struct WorkshopListingItem {
  std::uint64_t id = 0;
  std::string title;           // sanitised (SanitiseWorkshopText)
  std::uint64_t file_size = 0;  // Steam's size; for an item no longer subscribed, what Playden holds
  bool subscribed = true;       // false: Playden holds it but the player unsubscribed
  bool on = true;               // as the saved choice has it (WorkshopOffName)
  // What a sync does to it with the item on, and with it off (PlanWorkshopSync), and the bytes that moves.
  WorkshopAction on_action = WorkshopAction::kKeep;
  WorkshopAction off_action = WorkshopAction::kSkipOff;
  std::uint64_t on_bytes = 0;
  std::uint64_t off_bytes = 0;
};

struct WorkshopListing {
  std::uint32_t app_id = 0;
  WorkshopListingError error = WorkshopListingError::kNone;
  std::vector<WorkshopListingItem> items;  // subscriptions in Steam's order, then items Playden holds unsubscribed
  std::vector<std::uint64_t> saved_off;    // the saved choice, sorted; also IDs no longer listed
  bool truncated = false;                  // Steam's list was cut short (kWorkshopMaxItems)
};

// The listing from `subscriptions` and the bottle at `prefix` (its Workshop folder may not exist yet: nothing held).
// Reads only. A state file that can't be read or parsed counts as empty (a sync sets it aside); a damaged saved
// choice counts as nothing turned off.
WorkshopListing ListWorkshopItems(std::uint32_t app_id, const std::string& prefix,
                                  const WorkshopSubscriptions& subscriptions);

struct WorkshopListingConfig {
  // The game's bottle (its prefix folder, as this process reaches it); none while the game has none.
  std::function<std::optional<std::string>(std::uint32_t app)> bottle;
  // The player's subscriptions over Steam (FetchWorkshopSubscriptions).
  std::function<WorkshopSubscriptions(const StoredSession& session, std::uint32_t app, const std::atomic<bool>& cancel)>
      subscriptions;
  // Optional: counts and kinds only, never IDs, titles or paths.
  std::function<void(const std::string&)> log;
};

// One worker. Request asks for a game's listing; requests for different games wait their turn, oldest first (a game
// already waiting keeps its place, and past kWorkshopListingMaxPending the oldest waiting one is dropped; a running
// one goes on). Pump hands the finished ones to the main thread. The repository must outlive the service.
inline constexpr std::size_t kWorkshopListingMaxPending = 16;

class WorkshopListingService {
 public:
  WorkshopListingService(SessionRepository& sessions, WorkshopListingConfig config);
  ~WorkshopListingService();
  WorkshopListingService(const WorkshopListingService&) = delete;
  WorkshopListingService& operator=(const WorkshopListingService&) = delete;
  void Request(std::uint32_t app_id);
  // The account changed: a running listing is canceled and nothing asked before reaches Pump.
  void Invalidate();
  std::vector<WorkshopListing> Pump();
  void Shutdown();

 private:
  void Run();
  SessionRepository& sessions_;
  WorkshopListingConfig config_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::uint32_t> pending_;
  std::vector<WorkshopListing> done_;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
};

}  // namespace playden::steam
