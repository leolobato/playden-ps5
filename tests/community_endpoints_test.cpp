// community_endpoints_test.cpp - Community recipe endpoints, built twice: without dev endpoints
// (a release build: the dev file is never read, only HTTPS GitHub and api.playden.club, and no
// request asks for plain http) and with them (the file's endpoints, plain http to them only).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/endpoints.h"
#include "steam/backend/http_client.h"

#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace playden::community;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

std::string Folder() {
  char pattern[] = "/tmp/playden-endpoints-XXXXXX";
  const char* made = mkdtemp(pattern);
  return made ? std::filesystem::canonical(made).string() : std::string();
}

void Write(const std::string& path, const std::string& text) {
  if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
  }
}

bool IsRelease(const Endpoints& endpoints) {
  return endpoints.releases_latest_url == kRecipesReleasesLatestUrl && endpoints.api_base_url == kPlaydenApiBaseUrl &&
         endpoints.recipes_auth_header.empty() && !endpoints.allow_plain_http;
}

void Origins() {
  CHECK(UrlOrigin("https://api.github.com/repos/x") == "https://api.github.com:443");
  CHECK(UrlOrigin("HTTP://10.0.1.9:3000/api/v1") == "http://10.0.1.9:3000");
  CHECK(UrlOrigin("http://Example.org") == "http://example.org:80");
  CHECK(UrlOrigin("https://api.playden.club:443/v1") == "https://api.playden.club:443");
  for (const char* bad : {"", "ftp://x", "file:///etc/passwd", "https://", "https://user@host/", "http://host:0/",
                          "http://host:70000/", "http://host:/", "http://[::1]:80/", "http://ho st/", "https:/x"})
    CHECK(UrlOrigin(bad).empty());
}

void Release() {
  const auto endpoints = ReleaseEndpoints();
  CHECK(IsRelease(endpoints));
  CHECK(EndpointUrlAllowed(endpoints, kRecipesReleasesLatestUrl));
  CHECK(EndpointUrlAllowed(endpoints, "https://github.com/leolobato/playden-ps5-recipes/releases/download/r1/index.json"));
  CHECK(EndpointUrlAllowed(endpoints, "https://objects.githubusercontent.com/x"));
  CHECK(EndpointUrlAllowed(endpoints, "https://api.playden.club/v1/summary.json"));
  CHECK(!EndpointUrlAllowed(endpoints, "http://api.playden.club/v1/summary.json"));
  CHECK(!EndpointUrlAllowed(endpoints, "http://api.github.com/repos/x"));
  CHECK(!EndpointUrlAllowed(endpoints, "https://example.org/"));
  CHECK(!EndpointUrlAllowed(endpoints, "https://api.playden.club.example.org/"));
  CHECK(!EndpointPlainHttpAllowed(endpoints, "http://api.playden.club/"));
  CHECK(!EndpointSendsRecipesAuth(endpoints, kRecipesReleasesLatestUrl));
}

// What a dev file does, and that a build without dev endpoints ignores it whatever it says.
void DevFile(const std::string& folder) {
  const std::string path = folder + "/" + kDevEndpointsFileName;
  std::string line;
  // No file: the release endpoints, nothing logged.
  auto endpoints = LoadEndpoints(path, &line);
  CHECK(IsRelease(endpoints) && line.empty());

  Write(path, R"({"recipes_releases":"http://10.0.1.9:3000/api/v1/repos/leolobato/playden-ps5-recipes/releases/latest",)"
              R"("recipes_auth":"token s3cret-value","api_base":"http://10.0.1.80:8787/"})");
  endpoints = LoadEndpoints(path, &line);
  if (!DevEndpointsCompiledIn()) {
    CHECK(IsRelease(endpoints) && line.empty());
    CHECK(!EndpointUrlAllowed(endpoints, "http://10.0.1.9:3000/api/v1/repos/x"));
    CHECK(!EndpointUrlAllowed(endpoints, "http://10.0.1.80:8787/v1/summary.json"));
    // Even endpoints made by hand don't open other hosts or plain http in a release build.
    Endpoints forged = endpoints;
    forged.releases_latest_url = "http://10.0.1.9:3000/x";
    forged.api_base_url = "https://evil.example.org";
    forged.allow_plain_http = true;
    CHECK(!EndpointUrlAllowed(forged, "http://10.0.1.9:3000/x"));
    CHECK(!EndpointUrlAllowed(forged, "https://evil.example.org/v1/summary.json"));
    CHECK(!EndpointPlainHttpAllowed(forged, "http://10.0.1.9:3000/x"));
    return;
  }
  CHECK(endpoints.releases_latest_url ==
        "http://10.0.1.9:3000/api/v1/repos/leolobato/playden-ps5-recipes/releases/latest");
  CHECK(endpoints.recipes_auth_header == "token s3cret-value");
  CHECK(endpoints.api_base_url == "http://10.0.1.80:8787");
  CHECK(endpoints.allow_plain_http);
  // One line, without the auth value.
  CHECK(line.find("dev endpoints in use") != std::string::npos);
  CHECK(line.find("s3cret") == std::string::npos);
  // The configured origins only, with their exact port.
  CHECK(EndpointUrlAllowed(endpoints, "http://10.0.1.9:3000/leolobato/playden-ps5-recipes/releases/download/r1/index.json"));
  CHECK(EndpointUrlAllowed(endpoints, "http://10.0.1.80:8787/v1/summary.json"));
  CHECK(!EndpointUrlAllowed(endpoints, "http://10.0.1.9:3001/x"));
  CHECK(!EndpointUrlAllowed(endpoints, "https://10.0.1.9:3000/x"));  // another scheme is another origin
  CHECK(!EndpointUrlAllowed(endpoints, "http://10.0.1.10:3000/x"));
  CHECK(!EndpointUrlAllowed(endpoints, "http://example.org/"));
  CHECK(EndpointUrlAllowed(endpoints, "https://objects.githubusercontent.com/x"));  // GitHub stays allowed
  CHECK(EndpointPlainHttpAllowed(endpoints, "http://10.0.1.9:3000/x"));
  CHECK(!EndpointPlainHttpAllowed(endpoints, "http://example.org/"));
  CHECK(!EndpointPlainHttpAllowed(endpoints, "http://api.github.com/"));
  // The auth header goes to the recipes origin only, never the API.
  CHECK(EndpointSendsRecipesAuth(endpoints, "http://10.0.1.9:3000/leolobato/x/releases/download/r1/recipes.zip"));
  CHECK(!EndpointSendsRecipesAuth(endpoints, "http://10.0.1.80:8787/v1/summary.json"));
  CHECK(!EndpointSendsRecipesAuth(endpoints, "https://objects.githubusercontent.com/x"));

  // Refused files fall back to the release endpoints and say why.
  for (const char* bad : {"not json", R"({"recipes_releases":"ftp://x/y"})", R"({"api_base":"http://x"})",
                          R"({"recipes_releases":"http://x/y","recipes_auth":"a\r\nX-Evil: 1"})",
                          R"({"recipes_releases":"http://x/y","api_base":"file:///etc"})"}) {
    Write(path, bad);
    endpoints = LoadEndpoints(path, &line);
    CHECK(IsRelease(endpoints));
    CHECK(line.find("ignored") != std::string::npos);
  }
  // Without auth or api_base: the API stays api.playden.club, and https only for it.
  Write(path, R"({"recipes_releases":"https://gitea.example.org/api/v1/repos/x/y/releases/latest"})");
  endpoints = LoadEndpoints(path, &line);
  CHECK(endpoints.api_base_url == kPlaydenApiBaseUrl && endpoints.recipes_auth_header.empty());
  CHECK(!endpoints.allow_plain_http);
  CHECK(EndpointUrlAllowed(endpoints, "https://gitea.example.org/x"));
  CHECK(!EndpointUrlAllowed(endpoints, "http://gitea.example.org/x"));
  // A link is not followed.
  Write(folder + "/real.json", R"({"recipes_releases":"http://x/y"})");
  unlink(path.c_str());
  CHECK(symlink((folder + "/real.json").c_str(), path.c_str()) == 0);
  endpoints = LoadEndpoints(path, &line);
  CHECK(IsRelease(endpoints));
}

// Only a dev-endpoints build asks HttpClient for plain http, and only to a configured origin.
void PlainHttp() {
  Endpoints endpoints;
  endpoints.releases_latest_url = "http://10.0.0.5:3000/api/v1/repos/me/recipes/releases/latest";
  endpoints.api_base_url = "http://10.0.0.5:8787";
  endpoints.allow_plain_http = true;
  CHECK(EndpointPlainHttpAllowed(endpoints, "http://10.0.0.5:3000/x.zip") == DevEndpointsCompiledIn());
  CHECK(EndpointPlainHttpAllowed(endpoints, "http://10.0.0.5:8787/v1/summary.json") == DevEndpointsCompiledIn());
  CHECK(!EndpointPlainHttpAllowed(endpoints, "http://10.0.0.6:3000/x.zip"));
  CHECK(!EndpointPlainHttpAllowed(endpoints, "https://10.0.0.5:3000/x.zip"));
  endpoints.allow_plain_http = false;
  CHECK(!EndpointPlainHttpAllowed(endpoints, "http://10.0.0.5:3000/x.zip"));
  CHECK(!EndpointPlainHttpAllowed(ReleaseEndpoints(), "http://api.playden.club/v1/summary.json"));

  // Without the opt-in HttpClient refuses http.
  playden::steam::HttpClient client("/nonexistent-ca-bundle.pem");
  std::atomic<bool> cancel{false};
  playden::steam::HttpRequest request;
  request.url = "http://127.0.0.1:1/";
  request.timeout_ms = 2000;
  CHECK(client.Perform(request, cancel).error == playden::steam::HttpError::kInvalidRequest);
}

}  // namespace

int main() {
  const std::string folder = Folder();
  CHECK(!folder.empty());
  Origins();
  Release();
  DevFile(folder);
  PlainHttp();
  std::filesystem::remove_all(folder);
  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  else std::printf("community endpoints (%s) ok\n", DevEndpointsCompiledIn() ? "dev" : "release");
  return failures ? 1 : 0;
}
