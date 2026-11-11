// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/session_store.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace playden::steam {

// One Steam friend for the UI: no credentials, nothing but what the friends list shows.
struct FriendView {
  std::uint64_t steam_id = 0;
  std::string name;
  std::uint32_t persona_state = 0;  // EPersonaState: 0 offline, 1 online, 2 busy, 3 away, ...
  std::uint32_t app_id = 0;         // the Steam game being played, 0 for none
  std::string game_name;            // a non-Steam game's name
  std::uint64_t lobby_id = 0;
  std::string avatar_hash;          // 40 lowercase hex digits, or empty for none
};

struct FriendsSnapshot {
  std::uint64_t revision = 0;  // changes whenever anything below does
  bool needs_sign_in = false;
  bool connected = false;      // the Steam session is logged on
  std::vector<FriendView> friends;
};

// What the friends come from: Playden's shared Steam session. Connect runs on the service worker; the
// rest must be cheap and safe from any thread.
class FriendsSource {
 public:
  struct Friends {
    std::uint64_t account = 0;  // the SteamID these are the friends of; 0 before any list
    std::vector<FriendView> friends;
  };
  virtual ~FriendsSource() = default;
  // Makes sure the session is logged on with `session`. Nothing goes to Steam when it already is.
  virtual bool Connect(const StoredSession& session, const std::atomic<bool>& cancel) = 0;
  virtual bool Connected() const = 0;
  // Changes whenever the friends do.
  virtual std::uint64_t Changes() const = 0;
  virtual Friends Current() const = 0;
};

struct FriendsTiming {
  std::chrono::milliseconds check{1000};       // how often the worker looks at the session (no Steam traffic)
  std::chrono::milliseconds retry{5000};       // between reconnects, doubling
  std::chrono::milliseconds retry_max{300'000};
  // A session that stayed logged on this long resets the retry delay; one Steam drops sooner keeps doubling it.
  std::chrono::milliseconds stable{60'000};
};

// One worker. While friends are wanted (signed in), it keeps the shared session logged on, reopening it
// after Steam drops it, and publishes the friends Steam pushed. It never asks Steam for them again on a
// timer: the session asks once per logon. Source and repository must outlive the service. Create it only
// with online-steam.
class FriendsService {
 public:
  using LogFn = std::function<void(const std::string&)>;
  FriendsService(FriendsSource& source, SessionRepository& sessions, FriendsTiming timing = {});
  ~FriendsService();
  FriendsService(const FriendsService&) = delete;
  FriendsService& operator=(const FriendsService&) = delete;

  // Signed in: friends are wanted. Off, the snapshot empties and the session is left alone.
  void SetWanted(bool wanted);
  // The account changed: the stored session is read again.
  void Invalidate();
  FriendsSnapshot Snapshot();
  // The snapshot's revision, without copying it.
  std::uint64_t Revision() const { return revision_.load(std::memory_order_acquire); }
  // Counts only: connects, failures and how many friends; never names or IDs.
  void SetLog(LogFn log);
  void Shutdown();

 private:
  void Run();
  void Publish(FriendsSnapshot next);
  void Log(const std::string& line);
  FriendsSource& source_;
  SessionRepository& sessions_;
  const FriendsTiming timing_;
  std::mutex mutex_;
  std::condition_variable wake_;
  FriendsSnapshot snapshot_;
  std::atomic<std::uint64_t> revision_{0};
  LogFn log_;
  bool wanted_ = false;
  bool invalidated_ = true;
  bool stopping_ = false;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
};

}  // namespace playden::steam
