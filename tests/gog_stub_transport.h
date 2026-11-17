// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A stub GOG transport for tests: answers scripted per endpoint (auth.gog.com/token, embed.gog.com/userData.json,
// embed.gog.com/user/data/games), each optionally held at a gate until the test opens it, and gamesdb products
// answered by a handler per product ID. Synthetic tokens and accounts only.

#include "gog/http.h"
#include "steam/backend/http_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace gog_test {

using playden::steam::HttpError;
using playden::steam::HttpRequest;
using playden::steam::HttpResponse;

inline const std::string kUserId = "1000000000000000001";
inline const std::string kUsername = "SyntheticPlayer";

inline std::string ReadFile(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file != nullptr);
  std::string text;
  char chunk[4096];
  for (std::size_t count; (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) text.append(chunk, count);
  std::fclose(file);
  return text;
}

inline HttpResponse Answer(long status, const std::string& body) {
  HttpResponse response;
  response.status_code = status;
  response.body.assign(body.begin(), body.end());
  return response;
}

inline HttpResponse Failure(HttpError error) {
  HttpResponse response;
  response.error = error;
  return response;
}

inline std::string AccessText(int n) { return "synthetic-access-" + std::to_string(1000 + n); }
inline std::string RefreshText(int n) { return "synthetic-refresh-" + std::to_string(1000 + n); }

// A 200 token answer with the n-th synthetic tokens.
inline HttpResponse Tokens(int n, int expires_in = 3600) {
  return Answer(200, "{\"expires_in\":" + std::to_string(expires_in) +
                         ",\"scope\":\"\",\"token_type\":\"bearer\",\"access_token\":\"" + AccessText(n) +
                         "\",\"user_id\":\"" + kUserId + "\",\"refresh_token\":\"" + RefreshText(n) +
                         "\",\"session_id\":\"00000000000000000000\"}");
}

inline HttpResponse InvalidGrant() {
  return Answer(400, "{\"error\":\"invalid_grant\",\"error_description\":\"Invalid refresh token\"}");
}

// Holds a call until Open; Entered counts the calls that reached it.
class Gate {
 public:
  void Enter() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++entered_;
    changed_.notify_all();
  }
  // False when the caller's cancel flag ended the wait.
  bool Wait(const std::atomic<bool>& cancel) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!open_) {
      if (cancel.load()) return false;
      changed_.wait_for(lock, std::chrono::milliseconds(5));
    }
    return true;
  }
  void Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    changed_.notify_all();
  }
  void WaitEntered(int count) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool reached =
        changed_.wait_for(lock, std::chrono::seconds(10), [&] { return entered_ >= count; });
    CHECK(reached);
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  int entered_ = 0;
  bool open_ = false;
};

// kTags: the account library listing (the library tags, #536). Unscripted, it answers 503: a tags read that fails.
enum class Endpoint { kToken, kUser, kOwned, kProduct, kTags, kOther };

inline const std::string kProductPrefix = "https://gamesdb.gog.com/platforms/gog/external_releases/";

class Stub {
 public:
  struct Step {
    HttpResponse response;
    std::shared_ptr<Gate> gate;
  };
  // What a call looked like: copied during the call (the caller wipes its own copies after).
  struct Seen {
    Endpoint endpoint = Endpoint::kOther;
    std::string url;
    std::string authorization;
    std::string if_none_match;
    std::uint64_t product = 0;  // kProduct: the ID asked for
  };
  // Answers a gamesdb product request: its ID and the If-None-Match sent (empty when none).
  using ProductHandler = std::function<Step(std::uint64_t product, const std::string& if_none_match)>;

  void Script(Endpoint endpoint, HttpResponse response, std::shared_ptr<Gate> gate = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    Queue(endpoint).push_back({std::move(response), std::move(gate)});
  }
  int Calls(Endpoint endpoint) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& seen : seen_) count += seen.endpoint == endpoint;
    return count;
  }
  std::vector<Seen> seen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }
  std::size_t Unused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tokens_.size() + users_.size() + owned_.size();
  }
  void SetProducts(ProductHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    products_ = std::move(handler);
  }

  playden::gog::Transport Transport() {
    return [this](const HttpRequest& request, const std::atomic<bool>& cancel) { return Perform(request, cancel); };
  }

 private:
  std::deque<Step>& Queue(Endpoint endpoint) {
    return endpoint == Endpoint::kToken ? tokens_ : endpoint == Endpoint::kOwned ? owned_
           : endpoint == Endpoint::kTags ? tags_ : users_;
  }

  HttpResponse Perform(const HttpRequest& request, const std::atomic<bool>& cancel) {
    Seen seen;
    seen.url = request.url;
    for (const auto& header : request.headers) {
      if (header.first == "Authorization") seen.authorization = header.second;
      if (header.first == "If-None-Match") seen.if_none_match = header.second;
    }
    if (request.url.rfind("https://auth.gog.com/token?", 0) == 0) seen.endpoint = Endpoint::kToken;
    else if (request.url == "https://embed.gog.com/userData.json") seen.endpoint = Endpoint::kUser;
    else if (request.url == "https://embed.gog.com/user/data/games") seen.endpoint = Endpoint::kOwned;
    else if (request.url.rfind("https://embed.gog.com/account/getFilteredProducts?", 0) == 0) seen.endpoint = Endpoint::kTags;
    else if (request.url.rfind(kProductPrefix, 0) == 0) {
      seen.endpoint = Endpoint::kProduct;
      seen.product = std::strtoull(request.url.c_str() + kProductPrefix.size(), nullptr, 10);
    }
    Step step;
    ProductHandler products;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      seen_.push_back(seen);
      if (seen.endpoint == Endpoint::kProduct) {
        products = products_;
      } else if (seen.endpoint == Endpoint::kTags && tags_.empty()) {
        step = {Answer(503, ""), nullptr};
      } else if (seen.endpoint == Endpoint::kOther || Queue(seen.endpoint).empty()) {
        std::fprintf(stderr, "unscripted request\n");
        std::exit(1);
      } else {
        step = Queue(seen.endpoint).front();
        Queue(seen.endpoint).pop_front();
      }
    }
    if (seen.endpoint == Endpoint::kProduct) {
      if (!products) {
        std::fprintf(stderr, "unscripted request\n");
        std::exit(1);
      }
      step = products(seen.product, seen.if_none_match);
    }
    if (step.gate) {
      step.gate->Enter();
      if (!step.gate->Wait(cancel)) return Failure(HttpError::kCanceled);
    }
    return step.response;
  }

  mutable std::mutex mutex_;
  std::deque<Step> tokens_;
  std::deque<Step> users_;
  std::deque<Step> owned_;
  std::deque<Step> tags_;
  ProductHandler products_;
  std::vector<Seen> seen_;
};

// The refresh_token or code parameter of a token request URL.
inline std::string Param(const std::string& url, const std::string& name) {
  const auto at = url.find("&" + name + "=");
  if (at == std::string::npos) return {};
  const auto start = at + name.size() + 2;
  const auto end = url.find('&', start);
  return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace gog_test
