#pragma once
#include "steam/service.h"
#include <string>
#include <string_view>
namespace playden::diagnostics {
// Whitelist stable labels, not arbitrary transport/protocol error text.
const char* AuthMessage(steam::AuthError error);
std::string AuthRecord(const steam::SteamEvent& event);
// A log line or message with credentials taken out: every "Bearer <token>" (any case) becomes
// "Bearer <redacted>", and every occurrence of known_secret (when set, at least 8 characters)
// becomes "<redacted>". A bare hex string is left alone: recipe revision tags are 32 hex too.
std::string RedactSecrets(std::string_view text, std::string_view known_secret = {});
}
