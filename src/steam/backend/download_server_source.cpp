// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/download_server_source.h"

#include "steam/backend/app_info.h"
#include "steam/backend/depot_manifest.h"
#include "steam/backend/gzip.h"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <thread>

namespace playden::steam {
namespace {
std::string Hex(const std::array<std::uint8_t, 20>& bytes) {
  std::string result;
  for (const auto byte : bytes) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
  return result;
}
// The part of a host that tells servers of one provider apart: cache5-fra1 for cache5-fra1.steamcontent.com.
std::string ShortHost(const std::string& host) {
  for (const std::string suffix : {".cdn.steampipe.steamcontent.com", ".steamcontent.com"})
    if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
      return host.substr(0, host.size() - suffix.size());
  return host;
}
}

CdnDownloadServerSource::CdnDownloadServerSource(CmClient& cm, CdnClient& cdn,
    std::function<std::uint32_t()> newest_installed_app)
    : CdnDownloadServerSource(cdn, Hooks{}, std::move(newest_installed_app)) {
  hooks_.app_info = [&cm](const StoredSession& session, std::uint32_t app, const std::atomic<bool>& cancel) {
    return cm.FetchAppInfo(session, {app}, cancel);
  };
  hooks_.access = [&cm](const StoredSession& session, std::uint32_t app, const std::vector<DepotRequest>& depots,
                         const std::atomic<bool>& cancel) { return cm.FetchDepotAccess(session, app, depots, cancel); };
}
CdnDownloadServerSource::CdnDownloadServerSource(CdnClient& cdn, Hooks hooks,
    std::function<std::uint32_t()> newest_installed_app)
    : cdn_(cdn), hooks_(std::move(hooks)), newest_installed_app_(std::move(newest_installed_app)) {
  if (!hooks_.now) hooks_.now = [] { return Clock::now(); };
}
std::string CdnDownloadServerSource::DisplayName(const std::string& id) const { return CdnServerDisplayName(id); }
std::string CdnDownloadServerSource::LogLabel(const std::string& id) const { return CdnServerLabel(id); }
bool CdnDownloadServerSource::List(const std::atomic<bool>& cancel, std::vector<DownloadServerView>* out) {
  std::vector<ContentServer> listed;
  if (cdn_.Servers(0, cancel, &listed) != CdnError::kNone || cancel) return false;
  servers_ = std::move(listed);
  out->clear();
  for (const auto& server : servers_) out->push_back({server.host, DisplayName(server.host), !server.https});
  // Steam often lists several servers of one provider: name each by its host so they can be told apart.
  std::map<std::string, int> names;
  for (const auto& view : *out) ++names[view.name];
  for (auto& view : *out)
    if (names[view.name] > 1 && view.name != view.id) view.name += " · " + ShortHost(view.id);
  return !out->empty();
}
DownloadServerPreparation CdnDownloadServerSource::Prepare(const StoredSession& session, const std::atomic<bool>& cancel) {
  chunks_.clear(); depot_ = 0;
  // Also cover the directory request when Test had to list first. Reserve its complete cap
  // even when the directory was already cached, so every test has the same hard byte ceiling.
  metadata_budget_ = DownloadServerService::kTotalByteLimit - CdnClient::kMaxServerListDownloadBytes;
  if (!hooks_.app_info || !hooks_.access || servers_.empty() || cancel) return {};
  const auto primary = PrepareApp(session, 228980, true, cancel);
  if (primary == PayloadResult::kReady) return {DownloadServerPreparation::Error::kNone, 228980, metadata_budget_};
  if (primary != PayloadResult::kRejected || cancel) return {};
  const auto fallback = newest_installed_app_ ? newest_installed_app_() : 0;
  if (!fallback) return {DownloadServerPreparation::Error::kNeedsInstalledGame, 228980};
  const auto result = PrepareApp(session, fallback, false, cancel);
  return {result == PayloadResult::kReady ? DownloadServerPreparation::Error::kNone : DownloadServerPreparation::Error::kFailed, fallback, metadata_budget_};
}
CdnDownloadServerSource::PayloadResult CdnDownloadServerSource::PrepareApp(
    const StoredSession& session, std::uint32_t app_id, bool common, const std::atomic<bool>& cancel) {
  const auto fetched = hooks_.app_info(session, app_id, cancel);
  if (!fetched.ok() || cancel) return PayloadResult::kFailed;
  const auto buffer = fetched.buffers.find(app_id);
  const auto app = buffer == fetched.buffers.end() ? std::nullopt : ParseAppInfo(app_id, buffer->second);
  if (!app) return PayloadResult::kFailed;
  std::vector<DepotInfo> depots;
  if (common) {
    for (const auto& depot : app->depots)
      if (depot.id == 228990 && depot.manifest_gid) depots.push_back(depot);
  } else {
    SelectionResult selected;
    if (!SelectDepots(*app, {}, {app_id}, std::nullopt, DepotSelection{}, &selected)) return PayloadResult::kFailed;
    depots = std::move(selected.depots);
  }
  if (depots.empty()) return PayloadResult::kFailed;
  // Prefer the game's own depots over borrowed content. A refused depot does not prevent
  // another readable depot of the installed game from supplying the sample.
  std::stable_sort(depots.begin(), depots.end(), [](const DepotInfo& a, const DepotInfo& b) {
    return !a.depot_from_app && b.depot_from_app.has_value();
  });
  for (const auto& depot : depots) {
    if (!metadata_budget_ || cancel) return PayloadResult::kFailed;
    const auto access = hooks_.access(session, app_id, {{depot.id, *depot.manifest_gid, false}}, cancel);
    if (cancel) return PayloadResult::kFailed;
    if (!access.ok()) {
      if (access.error != CmError::kRejected) return PayloadResult::kFailed;
      if (common) return PayloadResult::kRejected;
      continue;
    }
    if (access.grants.size() != 1 || access.grants[0].depot_id != depot.id ||
        access.grants[0].manifest_gid != *depot.manifest_gid) return PayloadResult::kFailed;
    const auto& grant = access.grants[0];
    for (std::size_t attempt = 0; attempt < std::min<std::size_t>(servers_.size(), 6) && !cancel; ++attempt) {
      std::vector<std::uint8_t> zip, raw;
      DepotManifest manifest;
      if (!metadata_budget_) return PayloadResult::kFailed;
      const auto cap = static_cast<std::size_t>(std::min<std::uint64_t>(metadata_budget_, CdnClient::kMaxManifestDownloadBytes));
      const auto before = cdn_.stats().bytes;
      const auto outcome = cdn_.Manifest(servers_[attempt], grant.depot_id, grant.manifest_gid,
          grant.manifest_request_code, cancel, &zip, cap);
      // HttpClient retains partial body byte counts on transport errors. All attempts and
      // fallback depots draw from this ledger, and the next request is capped before it starts.
      metadata_budget_ -= std::min(metadata_budget_, cdn_.stats().bytes - before);
      if (outcome != CdnError::kNone ||
          !UnzipFirstEntry(zip, &InflateRaw, &raw) || !ParseDepotManifest(raw, &manifest) ||
          manifest.depot_id != grant.depot_id || manifest.gid != grant.manifest_gid) continue;
      // Names and content remain encrypted. Only bounded chunk hashes and compressed sizes are used.
      for (const auto& file : manifest.files) {
        for (const auto& chunk : file.chunks) {
          const std::uint64_t cap = static_cast<std::uint64_t>(chunk.compressed_size) + 4096;
          if (!chunk.compressed_size || !chunk.original_size || cap > kMaxHttpResponseBytes) continue;
          Chunk candidate{Hex(chunk.sha), static_cast<std::size_t>(cap)};
          const auto pos = std::find_if(chunks_.begin(), chunks_.end(), [&](const Chunk& c) { return c.cap < candidate.cap; });
          chunks_.insert(pos, std::move(candidate));
          if (chunks_.size() > 4) chunks_.pop_back();
        }
      }
      if (!chunks_.empty()) { depot_ = grant.depot_id; return PayloadResult::kReady; }
    }
  }
  return PayloadResult::kFailed;
}
DownloadServerRate CdnDownloadServerSource::Measure(const std::string& id, std::uint64_t byte_budget,
                                                    const std::atomic<bool>& cancel) {
  DownloadServerRate rate{id, 0, true};
  byte_budget = std::min(byte_budget, DownloadServerService::kServerByteLimit);
  const auto found = std::find_if(servers_.begin(), servers_.end(), [&](const ContentServer& s) { return s.host == id; });
  if (found == servers_.end() || chunks_.empty() || !depot_ || !ValidCdnHost(found->vhost) || !byte_budget || cancel) return rate;
  const ContentServer server = *found;
  const auto started = hooks_.now();
  const auto deadline = started + std::chrono::seconds(8);
  std::mutex mutex;
  std::uint64_t reserved = 0, bytes = 0;
  std::size_t cursor = 0;
  unsigned failures = 0;
  std::vector<std::thread> transfers;
  for (std::size_t slot = 0; slot < 4; ++slot) {
    transfers.emplace_back([&, slot] {
      // Each transfer owns an independent connection and DNS cache. Nothing touches the CDN
      // directory/manifest client while these transfers are running.
      std::unique_ptr<HttpClient> http;
      if (!hooks_.perform) http = std::make_unique<HttpClient>(cdn_.ca_bundle());
      for (;;) {
        Chunk chunk;
        std::uint32_t timeout = 0;
        {
          std::lock_guard<std::mutex> lock(mutex);
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - hooks_.now()).count();
          if (cancel || failures >= 2 || remaining <= 0) return;
          bool fits = false;
          for (std::size_t n = 0; n < chunks_.size(); ++n) {
            const auto& candidate = chunks_[cursor++ % chunks_.size()];
            if (candidate.cap <= byte_budget - reserved) { chunk = candidate; fits = true; break; }
          }
          if (!fits) return;
          reserved += chunk.cap;
          timeout = static_cast<std::uint32_t>(std::min<std::int64_t>(remaining, 8000));
        }
        HttpRequest request;
        request.url = std::string(server.https ? "https://" : "http://") + server.vhost + "/depot/" +
            std::to_string(depot_) + "/chunk/" + chunk.sha;
        request.headers = {{"User-Agent", "Valve/Steam HTTP Client 1.0"}};
        request.max_response_bytes = chunk.cap;
        request.timeout_ms = timeout;
        request.allow_plain_http = !server.https;
        const auto response = hooks_.perform ? hooks_.perform(slot, request, cancel) : http->Perform(request, cancel);
        std::lock_guard<std::mutex> lock(mutex);
        // Reserve the full cap even for failed transfers: retries cannot spend the budget twice.
        bytes += std::min<std::uint64_t>(chunk.cap, std::max<std::uint64_t>(response.body.size(), response.timing.body_bytes));
        if (!response.ok() || response.status_code != 200 || response.body.empty() || response.body.size() > chunk.cap) ++failures;
      }
    });
  }
  for (auto& transfer : transfers) transfer.join();
  const auto seconds = std::chrono::duration<double>(hooks_.now() - started).count();
  rate.failed = failures >= 2 || !bytes;
  if (seconds > 0) rate.mb_per_s = static_cast<double>(bytes) / 1'000'000 / seconds;
  return rate;
}

}  // namespace playden::steam
