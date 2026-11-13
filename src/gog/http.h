// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One HTTP exchange for the GOG client. Production code makes it from an HttpClient; tests pass stubs.

#include "steam/backend/http_client.h"

#include <atomic>
#include <functional>
#include <string>

namespace playden::gog {

using Transport = std::function<steam::HttpResponse(const steam::HttpRequest&, const std::atomic<bool>&)>;

// Every GOG request says who sends it, generically and honestly: no Galaxy client name, no version, no user data.
inline constexpr char kUserAgent[] = "Playden";

// One HttpClient for one worker thread: call the transport from that worker only. HttpClient never logs or returns
// a URL, a header or a body.
Transport ClientTransport(std::string ca_bundle);

}  // namespace playden::gog
