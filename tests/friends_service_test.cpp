// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/friends_service.h"

#include <cassert>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace playden;
using namespace playden::steam;
using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t kAccount = 76561197960269970ull;

class Sessions final : public SessionRepository {
 public:
  mutable std::mutex mutex;
  std::uint64_t steam_id = kAccount;  // 0: signed out
  Result<std::optional<StoredSession>> Load() const override {
    std::lock_guard<std::mutex> lock(mutex);
    if (!steam_id) return Result<std::optional<StoredSession>>::Success(std::nullopt);
    return Result<std::optional<StoredSession>>::Success(StoredSession{steam_id, "user", "refresh", "access", {}});
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
  void Set(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    steam_id = id;
  }
};

class Source final : public FriendsSource {
 public:
  mutable std::mutex mutex;
  bool connected = false;
  bool connect_ok = true;
  int connects = 0;
  std::uint64_t changes = 0;
  Friends friends;

  bool Connect(const StoredSession& session, const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(mutex);
    assert(session.steam_id != 0);
    ++connects;
    connected = connect_ok;
    return connect_ok;
  }
  bool Connected() const override {
    std::lock_guard<std::mutex> lock(mutex);
    return connected;
  }
  std::uint64_t Changes() const override {
    std::lock_guard<std::mutex> lock(mutex);
    return changes;
  }
  Friends Current() const override {
    std::lock_guard<std::mutex> lock(mutex);
    return friends;
  }
  int Connects() const {
    std::lock_guard<std::mutex> lock(mutex);
    return connects;
  }
  // Steam pushed a list or persona state.
  void Push(std::uint64_t account, std::vector<FriendView> list) {
    std::lock_guard<std::mutex> lock(mutex);
    friends = {account, std::move(list)};
    ++changes;
  }
  void Drop() {
    std::lock_guard<std::mutex> lock(mutex);
    connected = false;
  }
};

FriendsTiming Fast() {
  FriendsTiming timing;
  timing.check = 5ms;
  timing.retry = 50ms;
  timing.retry_max = 200ms;
  return timing;
}

template <typename Ready>
FriendsSnapshot Await(FriendsService& service, Ready ready) {
  for (int i = 0; i < 2000; ++i) {
    auto snapshot = service.Snapshot();
    if (ready(snapshot)) return snapshot;
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "friends service did not get there");
  return {};
}

FriendView Mara() {
  FriendView view;
  view.steam_id = 0x0110000100000001ull;
  view.name = "Mara";
  view.persona_state = 1;
  view.app_id = 440;
  view.lobby_id = 0x0186000000000009ull;
  view.avatar_hash = std::string(40, 'a');
  return view;
}

// Not wanted (signed out, or before the controller says so): the session is never touched.
void NothingWhileNotWanted() {
  Sessions sessions;
  Source source;
  FriendsService service(source, sessions, Fast());
  std::this_thread::sleep_for(50ms);
  assert(source.Connects() == 0 && service.Snapshot().friends.empty() && service.Revision() == 0);
}

// Wanted: the session opens once, the pushed friends show, and while it stays logged on nothing more is
// asked of it, however long the service runs.
void ConnectsOnceAndPublishesPushes() {
  Sessions sessions;
  Source source;
  FriendsService service(source, sessions, Fast());
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  service.SetLog([&](const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    lines.push_back(line);
  });
  service.SetWanted(true);
  Await(service, [](const FriendsSnapshot& s) { return s.connected; });
  source.Push(kAccount, {Mara()});
  const auto shown = Await(service, [](const FriendsSnapshot& s) { return s.friends.size() == 1; });
  assert(shown.friends[0].name == "Mara" && shown.friends[0].lobby_id == 0x0186000000000009ull);
  assert(shown.friends[0].avatar_hash.size() == 40 && !shown.needs_sign_in);
  const auto revision = service.Revision();
  std::this_thread::sleep_for(100ms);
  assert(source.Connects() == 1 && service.Revision() == revision);
  // A persona update: a new revision with the new state.
  auto away = Mara();
  away.persona_state = 3;
  away.lobby_id = 0;
  source.Push(kAccount, {away});
  const auto updated = Await(service, [](const FriendsSnapshot& s) { return !s.friends.empty() && s.friends[0].persona_state == 3; });
  assert(updated.revision > revision && updated.friends[0].lobby_id == 0);
  // Names and IDs never reach the log.
  std::lock_guard<std::mutex> lock(lines_mutex);
  assert(!lines.empty());
  for (const auto& line : lines)
    assert(line.rfind("friends ", 0) == 0 && line.find("Mara") == std::string::npos &&
           line.find(std::to_string(Mara().steam_id)) == std::string::npos);
}

// Steam dropped the session: it is opened again, and a failing logon backs off instead of retrying at once.
void ReconnectsWithBackoff() {
  Sessions sessions;
  Source source;
  FriendsService service(source, sessions, Fast());
  service.SetWanted(true);
  Await(service, [](const FriendsSnapshot& s) { return s.connected; });
  {
    std::lock_guard<std::mutex> lock(source.mutex);
    source.connect_ok = false;
  }
  source.Drop();
  Await(service, [](const FriendsSnapshot& s) { return !s.connected; });
  std::this_thread::sleep_for(120ms);
  // 50 ms, then 100 ms: at most three attempts in 120 ms, not one per 5 ms check.
  const auto failed = source.Connects();
  assert(failed >= 2 && failed <= 4);
  {
    std::lock_guard<std::mutex> lock(source.mutex);
    source.connect_ok = true;
  }
  Await(service, [](const FriendsSnapshot& s) { return s.connected; });
}

// Steam takes the logon and drops it again at once: the worker backs off instead of logging on every few ms.
void QuickDropsBackOff() {
  Sessions sessions;
  Source source;
  auto timing = Fast();
  timing.stable = 10s;
  FriendsService service(source, sessions, timing);
  service.SetWanted(true);
  const auto end = std::chrono::steady_clock::now() + 300ms;
  while (std::chrono::steady_clock::now() < end) {
    source.Drop();
    std::this_thread::sleep_for(1ms);
  }
  // 0, 50, 150, 350 ms: three or four logons, not one per 5 ms check.
  const auto connects = source.Connects();
  assert(connects >= 2 && connects <= 4);
}

// Another account's friends never show, and signing out empties the list.
void AccountChangesAndSignOut() {
  Sessions sessions;
  Source source;
  FriendsService service(source, sessions, Fast());
  service.SetWanted(true);
  source.Push(kAccount + 1, {Mara()});
  Await(service, [](const FriendsSnapshot& s) { return s.connected; });
  std::this_thread::sleep_for(30ms);
  assert(service.Snapshot().friends.empty());
  source.Push(kAccount, {Mara()});
  Await(service, [](const FriendsSnapshot& s) { return s.friends.size() == 1; });
  sessions.Set(0);
  service.Invalidate();
  Await(service, [](const FriendsSnapshot& s) { return s.needs_sign_in && s.friends.empty(); });
  sessions.Set(kAccount);
  service.Invalidate();
  Await(service, [](const FriendsSnapshot& s) { return s.friends.size() == 1; });
  service.SetWanted(false);
  Await(service, [](const FriendsSnapshot& s) { return s.friends.empty() && !s.needs_sign_in; });
  service.Shutdown();
}

}  // namespace

int main() {
  NothingWhileNotWanted();
  ConnectsOnceAndPublishesPushes();
  ReconnectsWithBackoff();
  QuickDropsBackOff();
  AccountChangesAndSignOut();
}
