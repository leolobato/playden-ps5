// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A stub GOG content system and CDN for the downloader's tests: builds, build metas and depot manifests, the owned
// list, secure links, the open link, the dependency repository and chunks, every one answered from memory. Bodies go
// through HttpRequest::body_sink in small blocks under the same rules as HttpClient: a 2xx body reaches the sink, any
// other keeps its body, cut at kMaxHttpStreamErrorBodyBytes whatever max_response_bytes says; the byte limit is
// checked before each 2xx block (kResponseTooLarge); a sink that returns false
// gives kSinkFailed; cancel stops it between blocks. A stall is answered at once with kTimeout, after the bytes the
// test asks for. A test changes any answer through a fault hook. Synthetic data and public answers only.

#include "gog/hash.h"
#include "gog_stub_transport.h"

#include <miniz.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gog_test {

inline std::string Md5Hex(const std::string& data) {
  playden::gog::Md5 md5;
  md5.Update(data.data(), data.size());
  return md5.FinalHex();
}

inline std::string Sha256Hex(const std::string& data) {
  playden::gog::Sha256 sha;
  sha.Update(data.data(), data.size());
  return sha.FinalHex();
}

// A zlib stream (header and Adler-32) of `raw`; level 0 stores it, so its size is about the input's.
inline std::string Zlib(const std::string& raw, int level = 6) {
  mz_ulong size = mz_compressBound(static_cast<mz_ulong>(raw.size()));
  std::string out(size, '\0');
  CHECK(mz_compress2(reinterpret_cast<unsigned char*>(&out[0]), &size,
                     reinterpret_cast<const unsigned char*>(raw.data()), static_cast<mz_ulong>(raw.size()),
                     level) == MZ_OK);
  out.resize(size);
  return out;
}

inline std::string Galaxy(const std::string& md5) { return md5.substr(0, 2) + "/" + md5.substr(2, 2) + "/" + md5; }

inline const std::string kFastlyBase = "https://cdn.example.invalid";
inline const std::string kGcoreBase = "https://gcore.example.invalid";
inline const std::string kContentPrefix = "https://content-system.gog.com/";

enum class Kind {
  kBuilds,
  kMeta,              // a build meta or depot manifest: /content-system/v2/meta/
  kDependencyMeta,    // /content-system/v2/dependencies/meta/
  kOwned,
  kSecureLink,
  kOpenLink,
  kRepository,
  kChunk,             // a secure-link chunk: /content-system/v2/store/<product>/
  kDependencyChunk,   // /content-system/v2/dependencies/store/
  kOther,
};

struct Request {
  Kind kind = Kind::kOther;
  std::string url;
  std::string authorization;  // empty when the request had no Authorization header
  std::string host;           // "fastly", "gcore" or the host name
  std::string hash;           // the MD5 the URL names (metas, manifests, chunks)
  std::uint64_t product = 0;  // builds, secure links and chunks
  bool streamed = false;
  std::size_t max_bytes = 0;
};

// What a fault does instead of the normal answer: a whole response (status or transport error), or another body for a
// 200, or a stall after some bytes.
struct Fault {
  std::optional<playden::steam::HttpResponse> response;
  std::optional<std::string> body;
  std::optional<long> status;  // with `body`, answered through the client's rules (unlike `response`)
  std::optional<std::size_t> stall_after;
};

class CdnStub {
 public:
  // Content addressed by its MD5 (metas, manifests and chunks, compressed).
  std::string Put(const std::string& compressed) {
    const auto md5 = Md5Hex(compressed);
    std::lock_guard<std::mutex> lock(mutex_);
    blobs_[md5] = compressed;
    return md5;
  }
  void SetBuilds(std::uint64_t product, std::string body) {
    std::lock_guard<std::mutex> lock(mutex_);
    builds_[product] = std::move(body);
  }
  void SetOwned(std::string body) {
    std::lock_guard<std::mutex> lock(mutex_);
    owned_ = std::move(body);
  }
  // The secure link answer for product 1900000001; other products get it with their ID in place.
  void SetSecureLink(std::string body) {
    std::lock_guard<std::mutex> lock(mutex_);
    secure_link_ = std::move(body);
  }
  void SetOpenLink(std::string body) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_link_ = std::move(body);
  }
  void SetRepository(std::string body) {
    std::lock_guard<std::mutex> lock(mutex_);
    repository_ = std::move(body);
  }
  void SetFault(std::function<std::optional<Fault>(const Request&)> fault) {
    std::lock_guard<std::mutex> lock(mutex_);
    fault_ = std::move(fault);
  }
  void SetBlock(std::size_t block) { block_ = block; }

  int Count(Kind kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& request : requests_) count += request.kind == kind;
    return count;
  }
  int CountHash(const std::string& hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& request : requests_) count += request.hash == hash;
    return count;
  }
  std::vector<Request> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }
  void ClearRequests() {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.clear();
  }

  playden::gog::Transport Transport() {
    return [this](const HttpRequest& request, const std::atomic<bool>& cancel) { return Perform(request, cancel); };
  }

 private:
  static std::string HashAtEnd(const std::string& url) {
    const auto path = url.substr(0, url.find('?'));
    if (path.size() < 32) return {};
    const auto hash = path.substr(path.size() - 32);
    return playden::gog::IsMd5Hex(hash) ? hash : std::string();
  }

  static Request Classify(const HttpRequest& http) {
    Request request;
    request.url = http.url;
    request.streamed = static_cast<bool>(http.body_sink);
    request.max_bytes = http.max_response_bytes;
    for (const auto& header : http.headers)
      if (header.first == "Authorization") request.authorization = header.second;
    const std::string& url = http.url;
    if (url.rfind(kFastlyBase, 0) == 0) request.host = "fastly";
    else if (url.rfind(kGcoreBase, 0) == 0) request.host = "gcore";
    else if (url.rfind("https://", 0) == 0) request.host = url.substr(8, url.find('/', 8) - 8);
    if (url.rfind(kContentPrefix + "products/", 0) == 0) {
      request.product = std::strtoull(url.c_str() + kContentPrefix.size() + 9, nullptr, 10);
      if (url.find("/os/windows/builds?generation=2&_version=2") != std::string::npos) request.kind = Kind::kBuilds;
      else if (url.find("/secure_link?_version=2&generation=2&path=/") != std::string::npos)
        request.kind = Kind::kSecureLink;
      return request;
    }
    if (url == kContentPrefix + "open_link?generation=2&_version=2&path=/dependencies/store/") {
      request.kind = Kind::kOpenLink;
      return request;
    }
    if (url == kContentPrefix + "dependencies/repository?generation=2") {
      request.kind = Kind::kRepository;
      return request;
    }
    if (url == "https://embed.gog.com/user/data/games") {
      request.kind = Kind::kOwned;
      return request;
    }
    request.hash = HashAtEnd(url);
    if (request.hash.empty()) return request;
    if (url.find("/content-system/v2/meta/" + Galaxy(request.hash)) != std::string::npos) request.kind = Kind::kMeta;
    else if (url.find("/content-system/v2/dependencies/meta/" + Galaxy(request.hash)) != std::string::npos)
      request.kind = Kind::kDependencyMeta;
    else if (url.find("/content-system/v2/dependencies/store/" + Galaxy(request.hash)) != std::string::npos)
      request.kind = Kind::kDependencyChunk;
    else if (const auto at = url.find("content-system/v2/store/"); at != std::string::npos &&
                                                                    url.find("/" + Galaxy(request.hash)) != std::string::npos) {
      request.kind = Kind::kChunk;
      request.product = std::strtoull(url.c_str() + at + 24, nullptr, 10);
    }
    return request;
  }

  // The answer as the real client would hand it back.
  HttpResponse Serve(const HttpRequest& http, long status, const std::string& body,
                     std::optional<std::size_t> stall_after, const std::atomic<bool>& cancel) const {
    HttpResponse response;
    response.status_code = status;
    if (!http.body_sink || status < 200 || status > 299) {
      if (!http.body_sink && body.size() > http.max_response_bytes) return Failure(HttpError::kResponseTooLarge);
      const std::size_t kept =
          http.body_sink ? std::min(body.size(), playden::steam::kMaxHttpStreamErrorBodyBytes) : body.size();
      response.body.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(kept));
      if (stall_after) return Failure(HttpError::kTimeout);
      return response;
    }
    CHECK(http.stall_timeout_ms != 0 && http.max_response_bytes != 0 &&
          http.max_response_bytes <= playden::steam::kMaxHttpStreamResponseBytes);
    const std::size_t end = stall_after ? std::min(*stall_after, body.size()) : body.size();
    std::size_t sent = 0;
    while (sent < end) {
      if (cancel.load()) return Failure(HttpError::kCanceled);
      const std::size_t length = std::min(block_, end - sent);
      if (length > http.max_response_bytes - sent) return Failure(HttpError::kResponseTooLarge);
      if (!http.body_sink(reinterpret_cast<const std::uint8_t*>(body.data()) + sent, length))
        return Failure(HttpError::kSinkFailed);
      sent += length;
    }
    if (stall_after) return Failure(HttpError::kTimeout);
    response.timing.body_bytes = sent;
    return response;
  }

  HttpResponse Perform(const HttpRequest& http, const std::atomic<bool>& cancel) {
    const Request request = Classify(http);
    std::function<std::optional<Fault>(const Request&)> fault;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(request);
      fault = fault_;
    }
    std::optional<Fault> chosen = fault ? fault(request) : std::nullopt;
    if (chosen && chosen->response) return *chosen->response;
    std::string body;
    long status = 200;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      switch (request.kind) {
        case Kind::kBuilds: {
          const auto found = builds_.find(request.product);
          if (found == builds_.end()) status = 404;
          else body = found->second;
          break;
        }
        case Kind::kOwned: body = owned_; break;
        case Kind::kOpenLink: body = open_link_; break;
        case Kind::kRepository: body = repository_; break;
        case Kind::kSecureLink: {
          body = secure_link_;
          const std::string product = std::to_string(request.product);
          for (auto at = body.find("1900000001"); at != std::string::npos; at = body.find("1900000001", at + 10))
            body.replace(at, 10, product);
          break;
        }
        case Kind::kMeta:
        case Kind::kDependencyMeta:
        case Kind::kChunk:
        case Kind::kDependencyChunk: {
          const auto found = blobs_.find(request.hash);
          if (found == blobs_.end()) status = 404;
          else body = found->second;
          break;
        }
        case Kind::kOther: status = 404; break;
      }
    }
    if (chosen && chosen->body) body = *chosen->body;
    if (chosen && chosen->status) status = *chosen->status;
    return Serve(http, status, body, chosen ? chosen->stall_after : std::nullopt, cancel);
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::string> blobs_;
  std::map<std::uint64_t, std::string> builds_;
  std::string owned_;
  std::string secure_link_;
  std::string open_link_;
  std::string repository_;
  std::function<std::optional<Fault>(const Request&)> fault_;
  std::vector<Request> requests_;
  std::size_t block_ = 7919;
};

}  // namespace gog_test
