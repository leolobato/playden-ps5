#include "steam/backend/cdn_client.h"

#include "steam/backend/json.h"

#include <algorithm>

namespace playden::steam {
namespace {

CdnError FromHttp(const HttpResponse& response) {
  if (response.error == HttpError::kCanceled) return CdnError::kCanceled;
  if (!response.ok()) return CdnError::kNetwork;
  if (response.status_code == 404) return CdnError::kNotFound;
  if (response.status_code == 429 || response.status_code >= 500) return CdnError::kNetwork;
  if (response.status_code != 200) return CdnError::kProtocol;
  return CdnError::kNone;
}

std::string Scheme(const ContentServer& server) { return server.https ? "https://" : "http://"; }

}  // namespace

bool ValidCdnHost(const std::string& host) {
  if (host.empty() || host.size() > 253) return false;
  return std::all_of(host.begin(), host.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
  });
}

std::string CdnServerLabel(const std::string& host) {
  std::string lower = host;
  for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  if (lower == "steampipe.akamaized.net") return "akamai";
  if (lower == "fastly.cdn.steampipe.steamcontent.com") return "fastly";
  if (lower == "alibaba.cdn.steampipe.steamcontent.com") return "alibaba";
  if (lower == "edgenext.cdn.steampipe.steamcontent.com") return "edgenext";
  const std::string suffix = ".cdn.steampipe.steamcontent.com";
  if (lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
    const auto prefix = lower.substr(0, lower.size() - suffix.size());
    if (prefix.compare(0, 6, "google") == 0 &&
        std::all_of(prefix.begin() + 6, prefix.end(), [](char c) { return c >= '0' && c <= '9'; }))
      return "google";
  }
  const std::string valve_suffix = ".steamcontent.com";
  if (lower.size() > valve_suffix.size() &&
      lower.compare(lower.size() - valve_suffix.size(), valve_suffix.size(), valve_suffix) == 0) return "valve";
  return "cache";
}

std::string CdnServerDisplayName(const std::string& host) {
  const auto label = CdnServerLabel(host);
  if (label == "akamai") return "Akamai";
  if (label == "fastly") return "Fastly";
  if (label == "google") return "Google Cloud";
  if (label == "alibaba") return "Alibaba Cloud";
  if (label == "edgenext") return "EdgeNext";
  if (label == "valve") return "Steam (Valve)";
  return host;
}

ServerOrder PreferServer(std::vector<ContentServer>* servers, const std::string& preferred_host) {
  if (!servers || preferred_host.empty()) return {};
  const auto found = std::find_if(servers->begin(), servers->end(), [&](const ContentServer& server) {
    return server.host == preferred_host;
  });
  if (found == servers->end()) return {};
  const auto index = static_cast<std::size_t>(found - servers->begin());
  std::rotate(servers->begin(), found, found + 1);
  return {true, index};
}

CdnClient::CdnClient(std::string ca_bundle) : ca_bundle_(std::move(ca_bundle)) {}
CdnClient::CdnClient(Perform perform) : perform_(std::move(perform)) {}
CdnClient::~CdnClient() = default;

CdnError CdnClient::Get(const std::string& url, std::size_t max_bytes, std::uint32_t timeout_ms,
                        const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out, bool plain_http) {
  HttpRequest request;
  request.url = url;
  request.allow_plain_http = plain_http;
  request.headers = {{"User-Agent", "Valve/Steam HTTP Client 1.0"}};
  request.timeout_ms = timeout_ms;
  request.max_response_bytes = max_bytes;
  HttpResponse response;
  if (perform_) {
    response = perform_(request, cancel);
  } else {
    if (ca_bundle_.empty()) return CdnError::kProtocol;
    if (!http_) http_ = std::make_unique<HttpClient>(ca_bundle_);
    response = http_->Perform(request, cancel);
  }
  const auto error = FromHttp(response);
  ++stats_.requests;
  if (error != CdnError::kNone) ++stats_.failures;
  stats_.resolve_ms += response.timing.resolve_ms;
  stats_.connect_ms += response.timing.connect_ms;
  stats_.tls_ms += response.timing.tls_ms;
  stats_.first_byte_ms += response.timing.first_byte_ms;
  stats_.total_ms += response.timing.total_ms;
  stats_.new_connections += response.timing.new_connections;
  stats_.bytes += std::max<std::uint64_t>(response.timing.body_bytes, response.body.size());
  if (error == CdnError::kNone) {
    *out = std::move(response.body);
  } else {
    last_failure_ = response.ok() ? "http=" + std::to_string(response.status_code)
                                  : std::string("transport=") + HttpFailureName(response);
  }
  return error;
}

CdnError CdnClient::Servers(std::uint32_t cell_id, const std::atomic<bool>& cancel,
                            std::vector<ContentServer>* out) {
  std::vector<std::uint8_t> body;
  const auto error = Get(
      "https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id=" +
          std::to_string(cell_id) + "&format=json",
      kMaxServerListDownloadBytes, 15'000, cancel, &body);
  if (error != CdnError::kNone) return error;
  const std::string_view text(body.empty() ? "" : reinterpret_cast<const char*>(body.data()), body.size());
  const auto root = json::Parse(text, json::Limits{512 * 1024, 8, 8192, 4096});
  const auto* response = root ? root->Find("response") : nullptr;
  const auto* servers = response ? response->Find("servers") : nullptr;
  if (!servers || !servers->is_array()) {
    last_failure_ = "servers=malformed";
    return CdnError::kProtocol;
  }
  std::vector<ContentServer> usable, plain;
  std::size_t listed = 0;
  for (const auto& server : servers->array()) {
    ++listed;
    const auto* type = server.Find("type");
    const auto* host = server.Find("host");
    const auto* vhost = server.Find("vhost");
    const auto* https = server.Find("https_support");
    const auto* load = server.Find("load");
    const auto* priority = server.Find("priority_class");
    if (!type || !type->is_string() || (type->text() != "SteamCache" && type->text() != "CDN")) continue;
    if (!host || !host->is_string() || !ValidCdnHost(host->text())) continue;
    ContentServer entry;
    entry.host = host->text();
    entry.vhost = vhost && vhost->is_string() && ValidCdnHost(vhost->text()) ? vhost->text() : entry.host;
    entry.https = !(https && https->is_string() && https->text() == "unavailable");
    if (load) {
      if (const auto value = load->AsUint64(); value && *value < 100000) entry.load = static_cast<int>(*value);
    }
    if (priority) {
      if (const auto value = priority->AsUint64()) entry.priority = static_cast<int>(std::min<std::uint64_t>(*value, 9));
    }
    (entry.https ? usable : plain).push_back(std::move(entry));
  }
  // Plain HTTP only when the directory offers nothing better.
  if (usable.empty()) usable = std::move(plain);
  if (usable.empty()) {
    last_failure_ = "servers=" + std::to_string(listed) + " usable=0";
    return CdnError::kProtocol;
  }
  std::stable_sort(usable.begin(), usable.end(),
                   [](const ContentServer& left, const ContentServer& right) { return left.load < right.load; });
  *out = std::move(usable);
  return CdnError::kNone;
}

CdnError CdnClient::Manifest(const ContentServer& server, std::uint32_t depot_id, std::uint64_t gid,
                             std::uint64_t request_code, const std::atomic<bool>& cancel,
                             std::vector<std::uint8_t>* out, std::size_t max_bytes) {
  if (!ValidCdnHost(server.vhost) || !max_bytes) return CdnError::kProtocol;
  return Get(Scheme(server) + server.vhost + "/depot/" + std::to_string(depot_id) + "/manifest/" +
                 std::to_string(gid) + "/5/" + std::to_string(request_code),
             std::min(max_bytes, kMaxManifestDownloadBytes), 60'000, cancel, out, !server.https);
}

CdnError CdnClient::Chunk(const ContentServer& server, std::uint32_t depot_id, const std::string& sha_hex,
                          std::size_t max_bytes, const std::atomic<bool>& cancel,
                          std::vector<std::uint8_t>* out) {
  if (!ValidCdnHost(server.vhost) || sha_hex.size() != 40 ||
      !std::all_of(sha_hex.begin(), sha_hex.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
    return CdnError::kProtocol;
  return Get(Scheme(server) + server.vhost + "/depot/" + std::to_string(depot_id) + "/chunk/" + sha_hex,
             max_bytes, 30'000, cancel, out, !server.https);
}

}  // namespace playden::steam
