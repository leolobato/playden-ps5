#pragma once
// asset_download.h - Resumable download of one GitHub release asset to a file.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "steam/backend/http_client.h"

namespace playden::update {

// One HTTP exchange; it should give up soon after cancel turns true. The real one wraps
// HttpClient::Perform; tests pass a fake.
using Fetch = std::function<steam::HttpResponse(const steam::HttpRequest&, const std::atomic<bool>& cancel)>;

struct DownloadOptions {
  Fetch fetch;
  std::string user_agent = "Playden";
  std::uint64_t chunk_bytes = 8 << 20;  // one Range request
  int max_attempts = 5;                 // per chunk, for network errors and server errors
  std::function<void(int attempt)> wait_before_retry;  // default: 2 s per attempt, ending early on cancel
  std::function<void(std::uint64_t done, std::uint64_t total)> progress;
  std::function<void(const std::string& line)> log;
  std::function<std::optional<std::string>(const std::string& path)> hash_file;  // SHA-256 hex
  const std::atomic<bool>* cancel = nullptr;
  // When set, the only URLs fetched (redirects included): those starting with it, over HTTPS. It must end with "/"
  // after the host, e.g. "https://builds.playden.club/packs/". Empty: the GitHub hosts of AllowedDownloadUrl.
  std::string allowed_url_prefix;
};

struct DownloadResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
};

// Downloads url (size bytes) into path. Bytes go to "<path>.part" first; a later call resumes from
// what it holds. Redirects are followed by hand, at most 5, and only to HTTPS GitHub hosts (or to
// options.allowed_url_prefix). When
// sha256 is set the finished file must match it. On success "<path>.part" becomes path.
DownloadResult DownloadAsset(const std::string& url, std::uint64_t size, const std::string& sha256,
                             const std::string& path, const DownloadOptions& options);

// HTTPS on github.com, api.github.com, or a host ending in ".githubusercontent.com".
bool AllowedDownloadUrl(const std::string& url);

// url is one options allow: under allowed_url_prefix when it is set, else AllowedDownloadUrl.
bool AllowedDownloadUrl(const std::string& url, const DownloadOptions& options);

// A JSON or other small GET (at most limit bytes), following allowed redirects. Empty on failure.
std::optional<std::string> FetchSmall(const std::string& url, std::size_t limit, const DownloadOptions& options,
                                      const std::string& accept, std::string* error);

}  // namespace playden::update
