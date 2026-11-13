#pragma once
#include "steam/service.h"
#include "stores/account.h"
#include <string>
#include <string_view>
namespace playden::diagnostics {
// Whitelist stable labels, not arbitrary transport/protocol error text.
const char* AuthMessage(steam::AuthError error);
std::string AuthRecord(const steam::SteamEvent& event);
// Another store's account event: {"store":"gog","kind":"signed-in","request":N,"generation":G}, with
// "restoring":true when set. Never the name, the user ID, the account key or the error text.
std::string AccountRecord(const stores::AccountEvent& event);
// A log line or message with credentials taken out: every "Bearer <token>" (any case) becomes
// "Bearer <redacted>", and every occurrence of known_secret (when set, at least 8 characters)
// becomes "<redacted>". Then the shapes in diagnostics/credential_patterns.h: OAuth codes, CDN tokens and
// access and refresh tokens keep their key and lose their value. A bare hex string is left alone: recipe revision
// tags are 32 hex too.
std::string RedactSecrets(std::string_view text, std::string_view known_secret = {});
}
