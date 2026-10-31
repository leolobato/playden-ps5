#include "steam/backend/http_client.h"

#include <strings.h>

#include "steam/backend/native_network.h"
#include "steam/backend/native_resolution_cache.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <string_view>

namespace playden::steam {
namespace {

using Clock = std::chrono::steady_clock;

struct CurlGlobal {
  CurlGlobal() : ready(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
  ~CurlGlobal() {
    if (ready) curl_global_cleanup();
  }
  bool ready;
};

CurlGlobal& GlobalCurl() {
  static CurlGlobal global;
  return global;
}

struct TransferBuffer {
  HttpResponse response;
  std::size_t body_limit = 0;
  std::size_t header_bytes = 0;
  std::size_t header_count = 0;
  bool body_overflow = false;
  bool header_overflow = false;
  bool malformed_header = false;
};

bool MultiplyWithin(std::size_t left, std::size_t right, std::size_t limit,
                    std::size_t& product) {
  if (right != 0 && left > limit / right) return false;
  product = left * right;
  return true;
}

size_t WriteBody(char* bytes, size_t size, size_t count, void* context) {
  auto& transfer = *static_cast<TransferBuffer*>(context);
  std::size_t length = 0;
  if (!MultiplyWithin(size, count, transfer.body_limit, length) ||
      length > transfer.body_limit - transfer.response.body.size()) {
    transfer.body_overflow = true;
    return 0;
  }
  const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes);
  transfer.response.body.insert(transfer.response.body.end(), begin, begin + length);
  return length;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
          value.back() == '\n'))
    value.remove_suffix(1);
  return value;
}

size_t WriteHeader(char* bytes, size_t size, size_t count, void* context) {
  auto& transfer = *static_cast<TransferBuffer*>(context);
  std::size_t length = 0;
  if (!MultiplyWithin(size, count, kMaxHttpResponseHeaderBytes, length) ||
      length > kMaxHttpResponseHeaderBytes - transfer.header_bytes) {
    transfer.header_overflow = true;
    return 0;
  }
  transfer.header_bytes += length;
  std::string_view line(bytes, length);
  if (line == "\r\n" || line == "\n") return length;
  if (line.rfind("HTTP/", 0) == 0) {
    transfer.response.headers.clear();
    return length;
  }
  const auto colon = line.find(':');
  if (colon == std::string_view::npos || colon == 0 ||
      transfer.header_count >= kMaxHttpResponseHeaderCount) {
    transfer.header_overflow = transfer.header_count >= kMaxHttpResponseHeaderCount;
    transfer.malformed_header = !transfer.header_overflow;
    return 0;
  }
  const std::string_view name = Trim(line.substr(0, colon));
  const std::string_view value = Trim(line.substr(colon + 1));
  if (name.empty() || name.find('\0') != std::string_view::npos ||
      value.find('\0') != std::string_view::npos) {
    transfer.malformed_header = true;
    return 0;
  }
  ++transfer.header_count;
  transfer.response.headers.emplace_back(name, value);
  return length;
}

bool IsTokenCharacter(unsigned char c) {
  return c != 0 && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || std::strchr("!#$%&'*+-.^_`|~", c));
}

bool ValidRequestHeader(const std::pair<std::string, std::string>& header) {
  if (header.first.empty() || header.first.size() > 1024 || header.second.size() > 8192)
    return false;
  for (unsigned char c : header.first)
    if (!IsTokenCharacter(c)) return false;
  for (unsigned char c : header.second)
    if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
  return true;
}

bool ValidUrl(const std::string& url, bool allow_plain_http) {
  if (url.empty() || url.size() > 8192 || url.find('\0') != std::string::npos) return false;
  CURLU* parsed = curl_url();
  if (!parsed) return false;
  char* scheme = nullptr;
  char* host = nullptr;
  const CURLUcode set = curl_url_set(parsed, CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER);
  bool valid = set == CURLUE_OK &&
               curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
               curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
               scheme && (std::strcmp(scheme, "https") == 0 || (allow_plain_http && std::strcmp(scheme, "http") == 0)) &&
               host && *host;
  curl_free(scheme);
  curl_free(host);
  curl_url_cleanup(parsed);
  return valid;
}

bool ValidRequest(const HttpRequest& request) {
  if (!ValidUrl(request.url, request.allow_plain_http) || request.timeout_ms == 0 ||
      request.timeout_ms > kMaxHttpTimeoutMs || request.max_response_bytes == 0 ||
      request.max_response_bytes > kMaxHttpResponseBytes ||
      request.body.size() > kMaxHttpResponseBytes ||
      request.headers.size() > kMaxHttpResponseHeaderCount)
    return false;
  if ((request.method != HttpMethod::kGet && request.method != HttpMethod::kPost && request.method != HttpMethod::kPut) ||
      (request.method == HttpMethod::kGet && !request.body.empty()))
    return false;
  std::size_t total_header_bytes = 0;
  for (const auto& header : request.headers) {
    constexpr std::size_t kWireOverhead = 4;  // colon, space, CRLF
    if (!ValidRequestHeader(header) ||
        kWireOverhead > kMaxHttpResponseHeaderBytes - total_header_bytes)
      return false;
    total_header_bytes += kWireOverhead;
    if (header.first.size() > kMaxHttpResponseHeaderBytes - total_header_bytes) return false;
    total_header_bytes += header.first.size();
    if (header.second.size() > kMaxHttpResponseHeaderBytes - total_header_bytes) return false;
    total_header_bytes += header.second.size();
  }
  return true;
}

HttpError MapError(CURLcode code, const TransferBuffer& transfer, bool canceled) {
  if (canceled) return HttpError::kCanceled;
  if (transfer.body_overflow) return HttpError::kResponseTooLarge;
  if (transfer.header_overflow) return HttpError::kResponseHeadersTooLarge;
  if (transfer.malformed_header) return HttpError::kProtocolError;
  switch (code) {
    case CURLE_OK:
      return HttpError::kNone;
    case CURLE_OPERATION_TIMEDOUT:
      return HttpError::kTimeout;
    case CURLE_COULDNT_RESOLVE_HOST:
      return HttpError::kNameResolutionFailed;
    case CURLE_COULDNT_CONNECT:
      return HttpError::kConnectionFailed;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
      return HttpError::kTlsVerificationFailed;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
      return HttpError::kInvalidRequest;
    default:
      return HttpError::kProtocolError;
  }
}

}  // namespace

const char* HttpErrorName(HttpError error) {
  switch (error) {
    case HttpError::kNone: return "none";
    case HttpError::kInvalidRequest: return "invalid_request";
    case HttpError::kInitializationFailed: return "initialization_failed";
    case HttpError::kNameResolutionFailed: return "name_resolution_failed";
    case HttpError::kConnectionFailed: return "connection_failed";
    case HttpError::kTlsVerificationFailed: return "tls_verification_failed";
    case HttpError::kTimeout: return "timeout";
    case HttpError::kCanceled: return "canceled";
    case HttpError::kResponseTooLarge: return "response_too_large";
    case HttpError::kResponseHeadersTooLarge: return "response_headers_too_large";
    case HttpError::kProtocolError: return "protocol_error";
  }
  return "protocol_error";
}

std::string HttpFailureName(const HttpResponse& response) {
  std::string name = HttpErrorName(response.error);
  if (response.transport_code != 0) name += " curl=" + std::to_string(response.transport_code);
  if (!response.ok() && !response.address.empty()) name += " addr=" + response.address;
  return name;
}

class HttpClient::Impl {
 public:
  explicit Impl(std::string ca_bundle) : ca_bundle_(std::move(ca_bundle)) {
    native_ready_ = internal::InitializeNativeNetwork();
    curl_ready_ = native_ready_ && GlobalCurl().ready;
    if (curl_ready_) multi_ = internal::NativeResolutionCache::CreateTransport();
  }

  ~Impl() {
    if (multi_) curl_multi_cleanup(multi_);
    curl_slist_free_all(resolve_entries_);
  }

  HttpResponse Perform(const HttpRequest& request, const std::atomic<bool>& cancel) {
    if (!curl_ready_ || !multi_ || ca_bundle_.empty())
      return {HttpError::kInitializationFailed, 0, {}, {}};
    if (!ValidRequest(request)) return {HttpError::kInvalidRequest, 0, {}, {}};
    if (cancel.load(std::memory_order_acquire)) return {HttpError::kCanceled, 0, {}, {}};
    const auto request_start = Clock::now();
    const auto deadline = request_start + std::chrono::milliseconds(request.timeout_ms);

    CURL* easy = curl_easy_init();
    CURLM* multi = multi_;
    curl_slist* request_headers = nullptr;
    auto transfer_buffer = std::make_unique<TransferBuffer>();
    auto& transfer = *transfer_buffer;
    transfer.body_limit = request.max_response_bytes;
    CURLcode result = CURLE_FAILED_INIT;
    internal::NativeResolveError resolve_error = internal::NativeResolveError::kFailed;
    bool added = false;
    bool canceled = false;

    if (!easy || !multi) goto cleanup;
    for (const auto& header : request.headers) {
      const std::string line = header.first + ": " + header.second;
      curl_slist* appended = curl_slist_append(request_headers, line.c_str());
      if (!appended) goto cleanup;
      request_headers = appended;
    }
    if (request.method == HttpMethod::kPut) {
      // A signed upload carries exactly the headers it was given: drop curl's form Content-Type and
      // the Expect: 100-continue handshake unless the caller set them.
      bool has_content_type = false;
      for (const auto& header : request.headers)
        if (header.first.size() == 12 && strncasecmp(header.first.c_str(), "Content-Type", 12) == 0) has_content_type = true;
      for (const char* removal : {has_content_type ? nullptr : "Content-Type:", "Expect:"}) {
        if (!removal) continue;
        curl_slist* appended = curl_slist_append(request_headers, removal);
        if (!appended) goto cleanup;
        request_headers = appended;
      }
    }

#define PLAYDEN_SETOPT(option, value) \
    do { if (curl_easy_setopt(easy, option, value) != CURLE_OK) goto cleanup; } while (false)
    PLAYDEN_SETOPT(CURLOPT_URL, request.url.c_str());
    PLAYDEN_SETOPT(CURLOPT_CAINFO, ca_bundle_.c_str());
    PLAYDEN_SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
    PLAYDEN_SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
    PLAYDEN_SETOPT(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    PLAYDEN_SETOPT(CURLOPT_PROXY, "");
    PLAYDEN_SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
    PLAYDEN_SETOPT(CURLOPT_MAXREDIRS, 0L);
    PLAYDEN_SETOPT(CURLOPT_PROTOCOLS_STR, request.allow_plain_http ? "http,https" : "https");
    PLAYDEN_SETOPT(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    PLAYDEN_SETOPT(CURLOPT_NOSIGNAL, 1L);
    PLAYDEN_SETOPT(CURLOPT_TCP_KEEPALIVE, 1L);
    if (request.method != HttpMethod::kGet) {
      PLAYDEN_SETOPT(CURLOPT_FRESH_CONNECT, 1L);
      PLAYDEN_SETOPT(CURLOPT_FORBID_REUSE, 1L);
    }
    PLAYDEN_SETOPT(CURLOPT_WRITEFUNCTION, WriteBody);
    PLAYDEN_SETOPT(CURLOPT_WRITEDATA, &transfer);
    PLAYDEN_SETOPT(CURLOPT_HEADERFUNCTION, WriteHeader);
    PLAYDEN_SETOPT(CURLOPT_HEADERDATA, &transfer);
    if (request_headers) PLAYDEN_SETOPT(CURLOPT_HTTPHEADER, request_headers);
    if (request.method == HttpMethod::kPost || request.method == HttpMethod::kPut) {
      PLAYDEN_SETOPT(CURLOPT_POST, 1L);
      if (request.method == HttpMethod::kPut) PLAYDEN_SETOPT(CURLOPT_CUSTOMREQUEST, "PUT");
      PLAYDEN_SETOPT(CURLOPT_POSTFIELDS,
                     request.body.empty() ? "" : reinterpret_cast<const char*>(request.body.data()));
      PLAYDEN_SETOPT(CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.body.size()));
    } else {
      PLAYDEN_SETOPT(CURLOPT_HTTPGET, 1L);
    }
#undef PLAYDEN_SETOPT

    {
      const auto start = Clock::now();
      resolve_error = resolution_cache_.Attach(easy, request.url, request.timeout_ms, cancel,
                                               &resolve_entries_, Clock::now());
      transfer.response.address = resolution_cache_.address();
#if PLAYDEN_PLATFORM_PS5
      transfer.response.timing.resolve_ms = static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
#else
      (void)start;
#endif
    }
    if (resolve_error != internal::NativeResolveError::kNone) {
      canceled = resolve_error == internal::NativeResolveError::kCanceled;
      result = canceled ? CURLE_ABORTED_BY_CALLBACK
                        : (resolve_error == internal::NativeResolveError::kTimeout
                               ? CURLE_OPERATION_TIMEDOUT
                               : CURLE_COULDNT_RESOLVE_HOST);
      goto cleanup;
    }
    if (cancel.load(std::memory_order_acquire)) {
      canceled = true;
      result = CURLE_ABORTED_BY_CALLBACK;
      goto cleanup;
    }
    {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - Clock::now()).count();
      if (remaining <= 0) {
        result = CURLE_OPERATION_TIMEDOUT;
        goto cleanup;
      }
      if (curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(remaining)) !=
              CURLE_OK ||
          curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(remaining)) != CURLE_OK)
        goto cleanup;
    }
    if (curl_multi_add_handle(multi, easy) != CURLM_OK) goto cleanup;
    added = true;
    {
      int running = 0;
      do {
        if (cancel.load(std::memory_order_acquire)) {
          canceled = true;
          result = CURLE_ABORTED_BY_CALLBACK;
          break;
        }
        if (Clock::now() >= deadline) {
          result = CURLE_OPERATION_TIMEDOUT;
          break;
        }
        if (curl_multi_perform(multi, &running) != CURLM_OK) break;
        if (!running) {
          int messages = 0;
          CURLMsg* message = nullptr;
          while ((message = curl_multi_info_read(multi, &messages)))
            if (message->msg == CURLMSG_DONE && message->easy_handle == easy)
              result = message->data.result;
          break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        const int wait_ms = static_cast<int>(std::clamp<long long>(remaining, 1, 25));
        if (curl_multi_poll(multi, nullptr, 0, wait_ms, nullptr) != CURLM_OK) break;
      } while (true);
    }
    if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &transfer.response.status_code) !=
        CURLE_OK)
      result = CURLE_FAILED_INIT;
    if (result == CURLE_OK && transfer.response.status_code == 0) result = CURLE_WEIRD_SERVER_REPLY;

  cleanup:
    if (easy) {
      curl_off_t lookup = 0, connect = 0, tls = 0, first = 0;
      long connections = 0;
      curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME_T, &lookup);
      curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME_T, &connect);
      curl_easy_getinfo(easy, CURLINFO_APPCONNECT_TIME_T, &tls);
      curl_easy_getinfo(easy, CURLINFO_STARTTRANSFER_TIME_T, &first);
      curl_easy_getinfo(easy, CURLINFO_NUM_CONNECTS, &connections);
      const auto ms = [](curl_off_t us) { return static_cast<std::uint32_t>(std::max<curl_off_t>(0, us) / 1000); };
      transfer.response.timing.connect_ms = ms(connect - lookup);
      transfer.response.timing.tls_ms = ms(tls - connect);
      transfer.response.timing.first_byte_ms = ms(first);
      transfer.response.timing.new_connections = static_cast<std::uint32_t>(std::max(0L, connections));
    }
    transfer.response.timing.body_bytes = transfer.response.body.size();
    if (added) curl_multi_remove_handle(multi, easy);
    if (easy) curl_easy_cleanup(easy);
    curl_slist_free_all(request_headers);
    transfer.response.error = MapError(result, transfer, canceled);
    if (!transfer.response.ok()) {
      transfer.response.transport_code = result == CURLE_OK ? 0 : static_cast<int>(result);
      // Clearing both caches covers transport failure and abandonment before curl consumes
      // staged DNS removals. Successful GETs retain their multi connection and DNS caches.
      resolution_cache_.ResetTransport(multi_);
      transfer.response.status_code = 0;
      transfer.response.headers.clear();
      transfer.response.body.clear();
    }
    return std::move(transfer.response);
  }

 private:
  std::string ca_bundle_;
  CURLM* multi_ = nullptr;
  curl_slist* resolve_entries_ = nullptr;
  internal::NativeResolutionCache resolution_cache_;
  bool native_ready_ = false;
  bool curl_ready_ = false;
};

HttpClient::HttpClient(std::string ca_bundle)
    : impl_(std::make_unique<Impl>(std::move(ca_bundle))) {}
HttpClient::~HttpClient() = default;
HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

HttpResponse HttpClient::Perform(const HttpRequest& request, const std::atomic<bool>& cancel) {
  const auto start = std::chrono::steady_clock::now();
  auto response = impl_ ? impl_->Perform(request, cancel)
                        : HttpResponse{HttpError::kInitializationFailed, 0, {}, {}};
  response.timing.total_ms = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count());
  return response;
}

}  // namespace playden::steam
