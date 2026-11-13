// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The signed-in player's GOG username, from embed.gog.com/userData.json (GOG_PROTOCOL.md §User). Synchronous: call it
// on a worker. Nothing here logs; callers log the endpoint's name ("user") and the status, never the URL, the token
// or the username.

#include "gog/http.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace playden::gog {

inline constexpr std::uint32_t kUserTimeoutMs = 20'000;
inline constexpr std::uint32_t kUserConnectTimeoutMs = 10'000;
// userData.json carries much more than the username (prices, updates, flags): the caps leave room for all of it.
inline constexpr std::size_t kMaxUserResponseBytes = 1024 * 1024;
inline constexpr std::size_t kMaxUserValues = 65536;
inline constexpr std::size_t kMaxUsernameBytes = 256;

struct UserResult {
  enum class Kind {
    kOk,
    kRejected,  // 401: the access token is no longer good
    kNetwork,   // try again later: no answer, 5xx, 429, another 4xx
    kProtocol,  // 200 without a usable username, or another status
  };
  Kind kind = Kind::kNetwork;
  std::string username;  // kOk only: 1-256 bytes, no control character. Never logged.
  long status = 0;       // the HTTP status; 0 when there was no answer
};

// GET https://embed.gog.com/userData.json with "Authorization: Bearer <access token>". Reads only "username"; every
// other field is ignored. The user ID comes from the token answer, never from here. The request's header and the
// response body are wiped before it returns.
UserResult FetchUser(Transport& transport, std::string_view access_token, const std::atomic<bool>& cancel);

}  // namespace playden::gog
