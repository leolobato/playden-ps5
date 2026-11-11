// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/cm_client.h"
#include "steam/friends_service.h"

#include <memory>

namespace playden::steam {

// The friends of Playden's shared CM session (made with CmSessionOptions::friends). The client must run its
// calls on that session (CmClient::SetSharedSession); both outlive the source.
class CmFriendsSource final : public FriendsSource {
 public:
  CmFriendsSource(CmClient& client, std::shared_ptr<CmSharedSession> shared);
  bool Connect(const StoredSession& session, const std::atomic<bool>& cancel) override;
  bool Connected() const override;
  std::uint64_t Changes() const override;
  Friends Current() const override;

 private:
  CmClient& client_;
  std::shared_ptr<CmSharedSession> shared_;
};

}  // namespace playden::steam
