// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/cm_friends_source.h"

#include <utility>

namespace playden::steam {

CmFriendsSource::CmFriendsSource(CmClient& client, std::shared_ptr<CmSharedSession> shared)
    : client_(client), shared_(std::move(shared)) {}

// A call that does nothing once it has the session: an open one is only taken and given back, a dropped one
// logs on again (and Steam pushes the friends list after the logon).
bool CmFriendsSource::Connect(const StoredSession& session, const std::atomic<bool>& cancel) {
  return client_.WithServiceSession(session, 25'000, cancel,
                                    [](const CmClient::ServiceCall&, std::uint64_t) { return CmError::kNone; }) ==
         CmError::kNone;
}

bool CmFriendsSource::Connected() const { return CmSharedSessionLoggedOn(*shared_); }

std::uint64_t CmFriendsSource::Changes() const { return CmFriendChanges(*shared_); }

FriendsSource::Friends CmFriendsSource::Current() const {
  auto known = CmFriendsOf(*shared_);
  Friends out;
  out.account = known.account;
  out.friends.reserve(known.friends.size());
  for (auto& person : known.friends) {
    FriendView view;
    view.steam_id = person.steam_id;
    view.name = std::move(person.name);
    view.persona_state = person.persona_state;
    view.app_id = person.app_id;
    view.game_name = std::move(person.game_name);
    view.lobby_id = person.lobby_id;
    view.avatar_hash = std::move(person.avatar_hash);
    out.friends.push_back(std::move(view));
  }
  return out;
}

}  // namespace playden::steam
