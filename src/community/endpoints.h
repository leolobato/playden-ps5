#pragma once
// endpoints.h - Where community recipes and their vote totals come from. A release build has
// one fixed answer: the recipes repository's latest GitHub release and api.playden.club, over
// HTTPS. A build configured with PLAYDEN_DEV_ENDPOINTS=ON may read other ones from a small file
// (dev-endpoints.json in Playden's user data) to test against a local server; in every other build
// the code that reads that file is not compiled.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

namespace playden::community {

struct Endpoints {
  std::string releases_latest_url;  // GET: the recipes repository's latest release (GitHub API shape)
  // A whole Authorization header value ("token <x>", "Basic <x>"), sent only to the origin of
  // releases_latest_url. Empty in release builds. Never logged.
  std::string recipes_auth_header;
  std::string api_base_url;         // vote totals: <api_base_url>/v1/summary.json
  // Plain http for the configured origins (dev endpoints only, see EndpointPlainHttpAllowed).
  bool allow_plain_http = false;
};

inline constexpr const char* kRecipesReleasesLatestUrl =
    "https://api.github.com/repos/leolobato/playden-ps5-recipes/releases/latest";
inline constexpr const char* kPlaydenApiBaseUrl = "https://api.playden.club";
inline constexpr const char* kDevEndpointsFileName = "dev-endpoints.json";

// The fixed release endpoints: no auth, HTTPS only.
Endpoints ReleaseEndpoints();

// True when this build was configured with PLAYDEN_DEV_ENDPOINTS=ON.
bool DevEndpointsCompiledIn();

// Release builds: always ReleaseEndpoints(), the file is never opened. Dev builds: the endpoints in
// dev_file_path, a JSON object
//   {"recipes_releases": "http://host:port/.../releases/latest",
//    "recipes_auth": "token <x>",            (optional)
//    "api_base": "http://host:port"}         (optional; api.playden.club when absent)
// with http or https URLs; a missing file gives ReleaseEndpoints(), a bad one ReleaseEndpoints()
// and a log line. *log_line gets one line when dev endpoints are used or the file was refused
// (never the auth value); empty otherwise.
Endpoints LoadEndpoints(const std::string& dev_file_path, std::string* log_line);

// Whether a request (or a redirect) may go to url. Always: HTTPS on github.com, api.github.com,
// *.githubusercontent.com (update::AllowedDownloadUrl) and https://api.playden.club. With dev
// endpoints also the exact scheme, host and port of releases_latest_url and api_base_url.
bool EndpointUrlAllowed(const Endpoints& endpoints, const std::string& url);

// Whether url may be fetched over plain http: dev endpoints only, and only on a configured origin.
bool EndpointPlainHttpAllowed(const Endpoints& endpoints, const std::string& url);

// Whether the recipes auth header goes with a request to url: the same scheme, host and port as
// releases_latest_url, and a header is configured.
bool EndpointSendsRecipesAuth(const Endpoints& endpoints, const std::string& url);

// "scheme://host:port" with the scheme and host in lower case and the default port filled in, for
// http and https URLs without user info; empty for anything else.
std::string UrlOrigin(const std::string& url);

}  // namespace playden::community
