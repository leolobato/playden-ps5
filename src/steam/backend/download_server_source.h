// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/download_server_service.h"
#include "steam/backend/cdn_client.h"
#include "steam/backend/cm_client.h"

#include <chrono>
#include <functional>

namespace playden::steam {

class CdnDownloadServerSource final : public DownloadServerSource {
 public:
  using Clock = std::chrono::steady_clock;
  struct Hooks {
    std::function<AppInfoBuffers(const StoredSession&, std::uint32_t, const std::atomic<bool>&)> app_info;
    std::function<DepotAccessResult(const StoredSession&, std::uint32_t, const std::vector<DepotRequest>&,
                                    const std::atomic<bool>&)> access;
    // A separate slot identifies each independently owned transfer client.
    std::function<HttpResponse(std::size_t, const HttpRequest&, const std::atomic<bool>&)> perform;
    std::function<Clock::time_point()> now;
  };
  CdnDownloadServerSource(CmClient& cm, CdnClient& cdn,
                           std::function<std::uint32_t()> newest_installed_app = {});
  CdnDownloadServerSource(CdnClient& cdn, Hooks hooks,
                           std::function<std::uint32_t()> newest_installed_app = {});
  bool List(const std::atomic<bool>& cancel, std::vector<DownloadServerView>* out) override;
  DownloadServerPreparation Prepare(const StoredSession&, const std::atomic<bool>& cancel) override;
  DownloadServerRate Measure(const std::string& id, std::uint64_t byte_budget,
                             const std::atomic<bool>& cancel) override;
  std::string DisplayName(const std::string& id) const override;
  std::string LogLabel(const std::string& id) const override;

 private:
  enum class PayloadResult { kReady, kRejected, kFailed };
  PayloadResult PrepareApp(const StoredSession&, std::uint32_t app, bool common,
                            const std::atomic<bool>& cancel);
  struct Chunk { std::string sha; std::size_t cap = 0; };
  CdnClient& cdn_;
  Hooks hooks_;
  std::function<std::uint32_t()> newest_installed_app_;
  std::vector<ContentServer> servers_;
  std::vector<Chunk> chunks_;
  std::uint32_t depot_ = 0;
  std::uint64_t metadata_budget_ = 0;
};

}  // namespace playden::steam
