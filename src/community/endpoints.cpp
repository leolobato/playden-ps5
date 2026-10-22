// endpoints.cpp - Where community recipes and their vote totals come from.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/endpoints.h"

#include "update/asset_download.h"

#if PLAYDEN_DEV_ENDPOINTS
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

#include "steam/backend/json.h"
#endif

namespace playden::community {
namespace {

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool StartsWithNoCase(const std::string& text, const char* prefix) {
  std::size_t i = 0;
  for (; prefix[i]; ++i)
    if (i >= text.size() || Lower(text[i]) != prefix[i]) return false;
  return true;
}

}  // namespace

std::string UrlOrigin(const std::string& url) {
  if (url.size() > 2048) return {};
  std::string scheme;
  std::size_t start = 0;
  if (StartsWithNoCase(url, "https://")) scheme = "https", start = 8;
  else if (StartsWithNoCase(url, "http://")) scheme = "http", start = 7;
  else return {};
  const auto end = url.find_first_of("/?#", start);
  const std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  if (authority.empty()) return {};
  std::string host = authority;
  std::string port = scheme == "https" ? "443" : "80";
  if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
    host = authority.substr(0, colon);
    const std::string digits = authority.substr(colon + 1);
    if (digits.empty() || digits.size() > 5) return {};
    unsigned long number = 0;
    for (char c : digits) {
      if (c < '0' || c > '9') return {};
      number = number * 10 + static_cast<unsigned long>(c - '0');
    }
    if (number == 0 || number > 65535) return {};
    port = std::to_string(number);
  }
  if (host.empty() || host.size() > 253) return {};
  for (auto& c : host) {
    c = Lower(c);
    // Letters, digits, dots and hyphens: names and IPv4 addresses (no user info, no IPv6 brackets).
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return {};
  }
  return scheme + "://" + host + ":" + port;
}

Endpoints ReleaseEndpoints() {
  Endpoints endpoints;
  endpoints.releases_latest_url = kRecipesReleasesLatestUrl;
  endpoints.api_base_url = kPlaydenApiBaseUrl;
  return endpoints;
}

bool DevEndpointsCompiledIn() {
#if PLAYDEN_DEV_ENDPOINTS
  return true;
#else
  return false;
#endif
}

#if PLAYDEN_DEV_ENDPOINTS
namespace {

bool PlainText(const std::string& text, std::size_t max_bytes) {
  if (text.empty() || text.size() > max_bytes) return false;
  for (char c : text)
    if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7e) return false;
  return true;
}

// The dev file, at most 4 KB; nothing when it is missing (*missing set) or unreadable.
std::optional<std::string> ReadDevFile(const std::string& path, bool* missing) {
  *missing = false;
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    *missing = errno == ENOENT;
    return std::nullopt;
  }
  std::string text;
  char buffer[512];
  for (;;) {
    const ssize_t count = read(fd, buffer, sizeof buffer);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || text.size() + static_cast<std::size_t>(count) > 4096) {
      close(fd);
      return std::nullopt;
    }
    if (count == 0) break;
    text.append(buffer, static_cast<std::size_t>(count));
  }
  close(fd);
  return text;
}

}  // namespace
#endif

Endpoints LoadEndpoints(const std::string& dev_file_path, std::string* log_line) {
  if (log_line) log_line->clear();
#if PLAYDEN_DEV_ENDPOINTS
  auto refuse = [&](const char* reason) {
    if (log_line) *log_line = std::string("community recipes: ") + kDevEndpointsFileName + " ignored: " + reason;
    return ReleaseEndpoints();
  };
  if (dev_file_path.empty() || dev_file_path.front() != '/') return ReleaseEndpoints();
  bool missing = false;
  const auto text = ReadDevFile(dev_file_path, &missing);
  if (!text) return missing ? ReleaseEndpoints() : refuse("unreadable or larger than 4 KB");
  const auto doc = steam::json::Parse(*text);
  if (!doc || !doc->is_object()) return refuse("not a JSON object");
  const auto* releases = doc->Find("recipes_releases");
  if (!releases || !releases->is_string() || !PlainText(releases->text(), 2048) || UrlOrigin(releases->text()).empty())
    return refuse("recipes_releases must be an http or https URL");
  Endpoints endpoints;
  endpoints.releases_latest_url = releases->text();
  if (const auto* auth = doc->Find("recipes_auth"); auth && !auth->is_null()) {
    // A header value: printable ASCII, single spaces allowed inside ("token <x>").
    bool ok = auth->is_string() && !auth->text().empty() && auth->text().size() <= 4096 &&
              auth->text().front() != ' ' && auth->text().back() != ' ';
    if (ok)
      for (char c : auth->text())
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) ok = false;
    if (!ok) return refuse("recipes_auth must be a header value");
    endpoints.recipes_auth_header = auth->text();
  }
  endpoints.api_base_url = kPlaydenApiBaseUrl;
  if (const auto* api = doc->Find("api_base"); api && !api->is_null()) {
    if (!api->is_string() || !PlainText(api->text(), 2048) || UrlOrigin(api->text()).empty())
      return refuse("api_base must be an http or https URL");
    endpoints.api_base_url = api->text();
    while (endpoints.api_base_url.size() > 1 && endpoints.api_base_url.back() == '/') endpoints.api_base_url.pop_back();
  }
  endpoints.allow_plain_http = StartsWithNoCase(endpoints.releases_latest_url, "http://") ||
                               StartsWithNoCase(endpoints.api_base_url, "http://");
  if (log_line)
    *log_line = "community recipes: dev endpoints in use: recipes " + UrlOrigin(endpoints.releases_latest_url) +
                (endpoints.recipes_auth_header.empty() ? " (no auth)" : " (with auth)") + ", api " +
                UrlOrigin(endpoints.api_base_url);
  return endpoints;
#else
  (void)dev_file_path;
  return ReleaseEndpoints();
#endif
}

bool EndpointUrlAllowed(const Endpoints& endpoints, const std::string& url) {
  if (update::AllowedDownloadUrl(url)) return true;
  const std::string origin = UrlOrigin(url);
  if (origin.empty()) return false;
  if (origin == UrlOrigin(kPlaydenApiBaseUrl)) return true;
#if PLAYDEN_DEV_ENDPOINTS
  if (origin == UrlOrigin(endpoints.releases_latest_url) || origin == UrlOrigin(endpoints.api_base_url)) return true;
#else
  (void)endpoints;
#endif
  return false;
}

bool EndpointPlainHttpAllowed(const Endpoints& endpoints, const std::string& url) {
#if PLAYDEN_DEV_ENDPOINTS
  if (!endpoints.allow_plain_http || !StartsWithNoCase(url, "http://")) return false;
  const std::string origin = UrlOrigin(url);
  return !origin.empty() &&
         (origin == UrlOrigin(endpoints.releases_latest_url) || origin == UrlOrigin(endpoints.api_base_url));
#else
  (void)endpoints;
  (void)url;
  return false;
#endif
}

bool EndpointSendsRecipesAuth(const Endpoints& endpoints, const std::string& url) {
  if (endpoints.recipes_auth_header.empty()) return false;
  const std::string origin = UrlOrigin(url);
  return !origin.empty() && origin == UrlOrigin(endpoints.releases_latest_url);
}

}  // namespace playden::community
