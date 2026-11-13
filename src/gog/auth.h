// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GOG sign-in: the login address, the code a player pastes back, and the token calls (GOG_PROTOCOL.md §1).
// Synchronous: call Exchange and Refresh on a worker. Nothing here logs; callers log the endpoint's name ("token")
// and the status, never a URL, a code or a token.

#include "gog/http.h"
#include "steam/backend/http_client.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace playden::gog {

// https://auth.gog.com/auth?client_id=...&redirect_uri=<kRedirectUri, percent-encoded>&response_type=code&layout=galaxy
std::string AuthorizeUrl();

// The sign-in code in what a player pasted, or nothing. Accepted, after trimming ASCII blanks:
// - the address GOG opens after sign-in: exactly https://embed.gog.com/on_login_success, then a query with a code
//   parameter anywhere in it (other parameters, such as origin=client, are allowed). The first non-empty code counts,
//   percent-decoded once;
// - a bare code of 16 to 1024 characters.
// A code is made of [A-Za-z0-9._~-]. Refused: more than 4096 bytes, a control character or NUL, another scheme,
// host, port, userinfo or path, a code only in the fragment, an empty code, a bad percent escape.
std::optional<std::string> CodeFromPaste(std::string_view paste);

enum class Grant {
  kAuthorizationCode,  // secret: the sign-in code
  kRefreshToken,       // secret: the refresh token
};

// GET https://auth.gog.com/token with the client's ID and secret, the grant type and its fields, URL-encoded:
// redirect_uri and code for a code, refresh_token for a refresh. 20 s in all, 10 s to connect, 64 KiB at most.
// The URL holds the secret: wipe it (platform::ZeroizeSecret) when done.
steam::HttpRequest TokenRequest(Grant grant, std::string_view secret);

inline constexpr std::uint32_t kTokenTimeoutMs = 20'000;
inline constexpr std::uint32_t kTokenConnectTimeoutMs = 10'000;
inline constexpr std::size_t kMaxTokenResponseBytes = 64 * 1024;

inline constexpr std::uint32_t kDefaultExpiresIn = 3600;
inline constexpr std::uint32_t kMinExpiresIn = 60;
inline constexpr std::uint32_t kMaxExpiresIn = 86400;

struct Tokens {
  std::string access_token;
  std::string refresh_token;
  std::string user_id;  // canonical decimal: 1 to 20 digits, no leading zero. Never logged.
  std::uint32_t expires_in = kDefaultExpiresIn;  // seconds: 3600 when GOG leaves it out, within 60..86400
  // Zeroes the tokens and the user ID.
  void Wipe();
};

struct TokenResult {
  enum class Kind {
    kOk,
    kRejected,  // the code or refresh token is no longer good: 400 invalid_grant, or any 401
    kNetwork,   // try again later: no answer (timeout, DNS, TLS, too large, canceled), 5xx, 429, another 4xx
    kProtocol,  // 200 with a body that isn't a token answer, or another status
  };
  Kind kind = Kind::kNetwork;
  Tokens tokens;    // kOk only
  long status = 0;  // the HTTP status; 0 when there was no answer
};

// One GET to auth.gog.com/token. The request URL, the code or refresh token passed in and the response body are
// wiped before they return. The JSON parser's own copies of the answer's strings are freed, not wiped.
TokenResult Exchange(Transport& transport, std::string code, const std::atomic<bool>& cancel);
TokenResult Refresh(Transport& transport, std::string refresh_token, const std::atomic<bool>& cancel);

}  // namespace playden::gog
