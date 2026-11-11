// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/friends_service.h"

#include <algorithm>
#include <optional>

namespace playden::steam {

FriendsService::FriendsService(FriendsSource& source, SessionRepository& sessions, FriendsTiming timing)
    : source_(source), sessions_(sessions), timing_(timing), worker_([this] { Run(); }) {}

FriendsService::~FriendsService() { Shutdown(); }

void FriendsService::SetWanted(bool wanted) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (wanted_ == wanted) return;
  wanted_ = wanted;
  invalidated_ = true;
  wake_.notify_all();
}

void FriendsService::Invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  invalidated_ = true;
  wake_.notify_all();
}

FriendsSnapshot FriendsService::Snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void FriendsService::SetLog(LogFn log) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_ = std::move(log);
}

void FriendsService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    cancel_.store(true);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void FriendsService::Log(const std::string& line) {
  LogFn log;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    log = log_;
  }
  if (log) log("friends " + line);
}

// Bumps the revision only when something the UI shows changed.
void FriendsService::Publish(FriendsSnapshot next) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto same = [](const FriendView& a, const FriendView& b) {
    return a.steam_id == b.steam_id && a.name == b.name && a.persona_state == b.persona_state && a.app_id == b.app_id &&
           a.game_name == b.game_name && a.lobby_id == b.lobby_id && a.avatar_hash == b.avatar_hash;
  };
  if (next.needs_sign_in == snapshot_.needs_sign_in && next.connected == snapshot_.connected &&
      std::equal(next.friends.begin(), next.friends.end(), snapshot_.friends.begin(), snapshot_.friends.end(), same))
    return;
  next.revision = snapshot_.revision + 1;
  snapshot_ = std::move(next);
  revision_.store(snapshot_.revision, std::memory_order_release);
}

void FriendsService::Run() {
  using Clock = std::chrono::steady_clock;
  std::uint64_t account = 0;
  std::optional<std::uint64_t> seen_changes;
  auto retry_delay = timing_.retry;
  Clock::time_point retry_at{};
  Clock::time_point connected_at{};
  bool was_connected = false;
  std::optional<std::size_t> logged_count;
  bool logged_connected = false;
  for (;;) {
    bool wanted = false;
    bool invalidated = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, timing_.check, [this] { return stopping_ || invalidated_; });
      if (stopping_) return;
      wanted = wanted_;
      invalidated = invalidated_;
      invalidated_ = false;
    }
    if (!wanted) {
      account = 0;
      seen_changes.reset();
      Publish({});
      continue;
    }
    if (invalidated) {
      // Read again for every account change; the tokens themselves are read at each reconnect.
      const auto loaded = sessions_.Load();
      account = loaded.ok() && loaded.value() ? loaded.value()->steam_id : 0;
      seen_changes.reset();
      retry_at = {};
      retry_delay = timing_.retry;
    }
    if (account == 0) {
      FriendsSnapshot none;
      none.needs_sign_in = true;
      Publish(std::move(none));
      continue;
    }
    // Steam dropped the session (or it never opened): log on again, backing off while that fails.
    if (!source_.Connected() && Clock::now() >= retry_at) {
      const auto loaded = sessions_.Load();
      const bool ok = loaded.ok() && loaded.value() && loaded.value()->steam_id == account &&
                      source_.Connect(*loaded.value(), cancel_);
      if (cancel_.load()) return;
      // The next reconnect waits either way; only a session that lasts resets the delay (below).
      retry_at = Clock::now() + retry_delay;
      Log(std::string("connect result=") + (ok ? "ok" : "failed") + " retry_s=" + std::to_string(retry_delay.count() / 1000));
      retry_delay = std::min(retry_delay * 2, timing_.retry_max);
      if (ok) connected_at = Clock::now();
    }
    const bool connected = source_.Connected();
    if (connected && Clock::now() - connected_at >= timing_.stable) retry_delay = timing_.retry;
    const auto changes = source_.Changes();
    if (seen_changes && *seen_changes == changes && connected == was_connected) continue;
    seen_changes = changes;
    was_connected = connected;
    // The friends are read again only when the session published a change (or the connection changed).
    auto current = source_.Current();
    FriendsSnapshot next;
    next.connected = connected;
    // Another account's friends (the session not yet logged on again) are never shown.
    if (current.account == account) next.friends = std::move(current.friends);
    const auto count = next.friends.size();
    Publish(std::move(next));
    // Persona updates change the snapshot often; the log notes only a new count or connection state.
    if (logged_count != count || logged_connected != connected) {
      logged_count = count;
      logged_connected = connected;
      Log("snapshot friends=" + std::to_string(count) + " connected=" + (connected ? "1" : "0"));
    }
  }
}

}  // namespace playden::steam
