#pragma once

#include "steam/backend/cdn_client.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/content_language.h"
#include "steam/backend/depot_installer.h"
#include "steam/install_service.h"

namespace playden::steam {

std::string FormatInstallStats(const char* phase, const InstallStats& stats);
using InstallLogFn = std::function<void(std::uint32_t app, const char* step, const char* category, const char* detail)>;
void LogInstallTrace(const InstallLogFn& log, std::uint32_t app, const char* marker);

// Installs a Steam game: PICS app info for the depot selection (Windows, the chosen content
// language, public branch), then the resumable depot installer.
class SteamInstallRunner final : public SessionInstallRunner {
 public:
  SteamInstallRunner(SessionRepository& sessions, CmClient& cm, CdnClient& cdn)
      : SessionInstallRunner(sessions), cm_(cm), installer_(cm, cdn) {}
  Result InstallApp(const StoredSession& session, std::uint32_t id, const std::string& root,
                 const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                 const std::function<void(InstallStep)>& step) override;
  std::string Remove(const std::string& root) override;
  void SetTuning(std::function<DownloadTuning()> tuning) { tuning_ = std::move(tuning); }
  // The content language each install selects depots for, read when the install starts so a
  // change to the setting reaches the next download. English when unset.
  void SetContentLanguage(std::function<std::string()> language) { language_ = std::move(language); }
  // Optional sanitized log: the Steam app ID, step, error category and a short detail; never
  // tokens or account identifiers. Called on the install worker.
  using LogFn = InstallLogFn;
  void SetLog(LogFn log) {
    log_ = log;
    installer_.SetStats([this, log](const char* phase, const InstallStats& stats) {
      if (log) {
        const auto detail = FormatInstallStats(phase, stats);
        log(current_app_.load(), "stats", phase, detail.c_str());
      }
    });
    installer_.SetTrace([this, log](const char* step) { LogInstallTrace(log, current_app_.load(), step); });
  }

 private:
  CmClient& cm_;
  DepotInstaller installer_;
  LogFn log_;
  std::function<DownloadTuning()> tuning_;
  std::function<std::string()> language_;
  std::atomic<std::uint32_t> current_app_{0};
};

// User-facing text for an install failure category.
const char* InstallFailureText(InstallError error);

}  // namespace playden::steam
