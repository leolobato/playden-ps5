#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace playden::steam {

enum class HttpMethod {
  kGet,
  kPost,
  kPut,  // body sent as-is; no default Content-Type is added
};

enum class HttpError {
  kNone,
  kInvalidRequest,
  kInitializationFailed,
  kNameResolutionFailed,
  kConnectionFailed,
  kTlsVerificationFailed,
  kTimeout,
  kCanceled,
  kResponseTooLarge,
  kResponseHeadersTooLarge,
  kProtocolError,
};

const char* HttpErrorName(HttpError error);

inline constexpr std::uint32_t kMaxHttpTimeoutMs = 120'000;
inline constexpr std::size_t kMaxHttpResponseBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxHttpResponseHeaderBytes = 16 * 1024;
inline constexpr std::size_t kMaxHttpResponseHeaderCount = 64;

struct HttpRequest {
  HttpMethod method = HttpMethod::kGet;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;
  std::uint32_t timeout_ms = 10'000;
  std::size_t max_response_bytes = 1024 * 1024;
  // Also accept an http:// URL. Only for content checked by other means, such as Steam CDN chunks.
  bool allow_plain_http = false;
};

struct HttpTiming {
  std::uint32_t resolve_ms = 0;
  std::uint32_t connect_ms = 0;
  std::uint32_t tls_ms = 0;
  std::uint32_t first_byte_ms = 0;
  std::uint32_t total_ms = 0;
  std::uint32_t new_connections = 0;
  std::uint64_t body_bytes = 0;
};

struct HttpResponse {
  HttpError error = HttpError::kNone;
  long status_code = 0;
  // Header names preserve server casing and duplicates preserve wire order. Protocol consumers
  // decide whether a duplicated semantic header such as X-EResult is acceptable.
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::uint8_t> body;

  HttpTiming timing;  // Retained when a failed response discards its payload.
  // The transport library's numeric result code when the transfer failed (0 otherwise), for logs:
  // several codes share one HttpError, such as a TLS handshake failure and kProtocolError.
  int transport_code = 0;
  // The server's numeric address from the console's resolver, for logs (empty on host builds, where
  // curl resolves): a DNS server that blocks a host answers one no server listens on.
  std::string address;

  bool ok() const { return error == HttpError::kNone; }
};

// HttpErrorName plus the transport code and the server's address when there are, such as
// "connection_failed curl=7 addr=203.0.113.5".
std::string HttpFailureName(const HttpResponse& response);

// One worker thread owns each instance and calls Perform synchronously. The cancel flag may be
// changed by another thread. Responses contain stable categories and never expose URLs, headers,
// bodies, or transport-library error text.
class HttpClient {
 public:
  explicit HttpClient(std::string ca_bundle);
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;
  HttpClient(HttpClient&&) noexcept;
  HttpClient& operator=(HttpClient&&) noexcept;

  HttpResponse Perform(const HttpRequest& request, const std::atomic<bool>& cancel);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playden::steam
