#pragma once

#include "steam/backend/http_client.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playden::steam {

struct ContentServer {
  std::string host;
  std::string vhost;  // the name used in URLs and TLS
  int load = 100;
  bool https = true;  // false for a server the directory lists as plain HTTP only
  int priority = 0;  // directory priority_class, 0..9; does not affect Automatic order
};

struct CdnStats {
  std::uint64_t requests = 0, failures = 0;
  std::uint64_t resolve_ms = 0, connect_ms = 0, tls_ms = 0, first_byte_ms = 0, total_ms = 0;
  std::uint64_t new_connections = 0, bytes = 0;
};

enum class CdnError { kNone, kCanceled, kNetwork, kNotFound, kProtocol };

// Steam CDN access over the verified HTTPS client: the content server list for the account's
// cell, manifests and chunks. HTTPS-capable SteamCache/CDN servers are used when the directory
// lists any. Some regions (China) only get plain-HTTP caches; those are then used over http://.
// Chunks stay safe there: they are encrypted with the depot key from the CM and checked after
// decryption. The manifest signature is not verified, so a plain-HTTP manifest is trusted as
// received: someone on the path could drop files or move genuine chunks to other genuine paths,
// but could not add content of their own. One worker thread owns each instance.
class CdnClient {
 public:
  using Perform = std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)>;
  explicit CdnClient(std::string ca_bundle);
  explicit CdnClient(Perform perform);  // tests
  ~CdnClient();

  // Servers sorted by load, from IContentServerDirectoryService/GetServersForSteamPipe. When the
  // list holds no usable server, last_failure() says how many were listed and why none fit.
  CdnError Servers(std::uint32_t cell_id, const std::atomic<bool>& cancel, std::vector<ContentServer>* out);
  CdnError Manifest(const ContentServer& server, std::uint32_t depot_id, std::uint64_t gid,
                    std::uint64_t request_code, const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out,
                    std::size_t max_bytes = kMaxHttpResponseBytes);
  CdnError Chunk(const ContentServer& server, std::uint32_t depot_id, const std::string& sha_hex,
                 std::size_t max_bytes, const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out);

  // Bounded by the HTTP client's response cap.
  static constexpr std::size_t kMaxManifestDownloadBytes = kMaxHttpResponseBytes;
  static constexpr std::size_t kMaxServerListDownloadBytes = 512 * 1024;
  // The last failure as "http=<status>" or "transport=<category>", for sanitized logs.
  const CdnStats& stats() const { return stats_; }
  const std::string& last_failure() const { return last_failure_; }
  const std::string& ca_bundle() const { return ca_bundle_; }

 private:
  CdnError Get(const std::string& url, std::size_t max_bytes, std::uint32_t timeout_ms,
               const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out, bool plain_http = false);
  std::string ca_bundle_;
  Perform perform_;
  std::unique_ptr<HttpClient> http_;
  std::string last_failure_;
  CdnStats stats_;
};

// Fixed provider labels for logs; unknown hosts never appear in the returned label.
std::string CdnServerLabel(const std::string& host);
std::string CdnServerDisplayName(const std::string& host);

struct ServerOrder {
  bool preferred_found = false;
  std::size_t preferred_index = 0;  // original position of the first match
};
// Move the first matching host to the front, preserving every other entry's order.
ServerOrder PreferServer(std::vector<ContentServer>* servers, const std::string& preferred_host);

// A host or vhost from the directory may only contain DNS name characters.
bool ValidCdnHost(const std::string& host);

}  // namespace playden::steam
