#pragma once

#include "platform/storage.h"
#include "steam/backend/app_info.h"
#include "steam/backend/cdn_client.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/content_language.h"
#include "steam/backend/depot_installer.h"
#include "steam/install_service.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>

namespace playden::steam {

// The Fastest race's winner (a host), kept in memory so a queue of games races once. It belongs to one
// Steam account and lasts kMinutes; another account, an empty host or Clear forgets it.
class FastestServerCache {
 public:
  static constexpr int kMinutes = 45;
  using Now = std::function<std::chrono::steady_clock::time_point()>;
  explicit FastestServerCache(Now now = {}) : now_(std::move(now)) {}
  std::optional<std::string> Get(std::uint64_t account);
  void Remember(std::uint64_t account, const std::string& host);
  void Clear();

 private:
  std::chrono::steady_clock::time_point Time() const { return now_ ? now_() : std::chrono::steady_clock::now(); }
  Now now_;
  std::mutex mutex_;
  std::uint64_t account_ = 0;
  std::string host_;
  std::chrono::steady_clock::time_point at_{};
};

std::string FormatInstallStats(const char* phase, const InstallStats& stats);
std::string FormatInstallServerList(const std::vector<ContentServer>& servers, const char* preference);
std::vector<std::string> FormatInstallServerStats(const std::vector<InstallServerStats>& servers);
using InstallLogFn = std::function<void(std::uint32_t app, const char* step, const char* category, const char* detail)>;
void LogInstallTrace(const InstallLogFn& log, std::uint32_t app, const char* marker);

// Free DLC: at most this many apps are asked for per install, in one request, and the wait for the
// license list after a grant.
inline constexpr std::size_t kMaxFreeDlcClaim = 20;
inline constexpr std::uint32_t kFreeDlcLicenseWaitMs = 10'000;

// Whether an update of the install `record` describes can't be selected without the appinfo of a DLC app in
// `unfetched` (one whose batch Steam didn't answer): a recorded depot is listed by that app. The update would
// otherwise leave that DLC's depots out and delete their files.
bool UpdateNeedsUnfetchedDlc(const installs::InstalledRecord& record, std::uint32_t app,
                             const std::set<std::uint32_t>& unfetched);

// The DLC an install asks Steam to grant for free: the game's listofdlc apps, in that order, that the
// account doesn't own, that have a Windows depot (one the game lists for that DLC, or one in the DLC's
// own appinfo), and that Steam hasn't refused for this account before. At most `limit`.
std::vector<std::uint32_t> FreeDlcCandidates(const AppInfo& app, const std::vector<AppInfo>& dlc_apps,
                                             const std::set<std::uint32_t>& owned_apps,
                                             const std::set<std::uint32_t>& refused,
                                             std::size_t limit = kMaxFreeDlcClaim);
// The asked-for DLC Steam answered for without granting, that the licenses read after the request
// don't hold either (a stale library list asks for owned DLC). Nothing when the request failed, or
// when Steam granted packages without naming apps, which can't be told apart.
std::vector<std::uint32_t> FreeDlcRefusals(const std::vector<std::uint32_t>& claimed, const FreeLicenseResult& claim,
                                           const std::set<std::uint32_t>& owned_apps);
// The DLC Steam refused for free for one account, kept so they are not asked for again: a file in the
// user data root named after the account, one `app\trefused` line each. Lines it can't read are skipped.
std::string FreeDlcMemoryName(std::uint64_t steam_id);
std::set<std::uint32_t> ParseFreeDlcMemory(std::string_view text);
std::string FormatFreeDlcMemory(const std::set<std::uint32_t>& refused);
std::set<std::uint32_t> LoadFreeDlcMemory(const Storage& storage, std::uint64_t steam_id);
bool SaveFreeDlcMemory(const Storage& storage, std::uint64_t steam_id, const std::set<std::uint32_t>& refused);

// Installs a Steam game: PICS app info for the depot selection (Windows, the install's content
// language and branch, by default the Settings language and the public branch), then the resumable
// depot installer.
class SteamInstallRunner final : public SessionInstallRunner {
 public:
  SteamInstallRunner(SessionRepository& sessions, CmClient& cm, CdnClient& cdn, FastestServerCache::Now now = {})
      : SessionInstallRunner(sessions), cm_(cm), installer_(cm, cdn), fastest_(std::move(now)) {}
  Result InstallApp(const StoredSession& session, std::uint32_t id, const std::string& root,
                 const InstallOptions& options, const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                 const std::function<void(InstallStep)>& step) override;
  // An update selects depots as an install does, with the options (the service fills in the record's
  // branch and language), and downloads only the files that changed.
  Result UpdateApp(const StoredSession& session, std::uint32_t id, const std::string& root,
                   const InstallOptions& options, const std::atomic<bool>& cancel,
                   const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                   const std::function<void(InstallStep)>& step, const std::function<bool()>& may_apply) override;
  std::string Remove(const std::string& root) override;
  std::string DiscardUpdate(const std::string& root) override;
  std::string DownloadServerName() const override { return server_name_; }
  FastestServerCache& fastest_server() { return fastest_; }
  void SetTuning(std::function<DownloadTuning()> tuning) { tuning_ = std::move(tuning); }
  // The content language an install selects depots for when its options name none, read when the
  // install starts so a change to the setting reaches the next download. English when unset.
  void SetContentLanguage(std::function<std::string()> language) { language_ = std::move(language); }
  // The licenses the library last read, to choose which DLC to ask Steam for free before the
  // install reads the licenses itself. Without it no free DLC is asked for.
  void SetLicensedDepots(const LicensedDepots* licensed) { licensed_ = licensed; }
  // Where the free DLC Steam refused are remembered (the user data root). Without it they are
  // asked for again on every install.
  void SetFreeDlcMemory(const Storage* storage) { free_dlc_memory_ = storage; }
  // Optional sanitized log: the Steam app ID, step, error category and a short detail; never
  // tokens or account identifiers. Called on the install worker.
  using LogFn = InstallLogFn;
  void SetLog(LogFn log) {
    log_ = log;
    installer_.SetStats([this, log](const char* phase, const InstallStats& stats) {
      if (log) {
        if (std::string(phase) == "servers") {
          for (const auto& detail : FormatInstallServerStats(stats.server_stats))
            log(current_app_.load(), "stats", "servers", detail.c_str());
          return;
        }
        const auto detail = FormatInstallStats(phase, stats);
        log(current_app_.load(), "stats", phase, detail.c_str());
      }
    });
    installer_.SetServerList([this, log](const std::vector<ContentServer>& servers, const char* preference) {
      if (log) {
        const auto detail = FormatInstallServerList(servers, preference);
        log(current_app_.load(), "servers", "listed", detail.c_str());
      }
    });
    installer_.SetTrace([this, log](const char* step) { LogInstallTrace(log, current_app_.load(), step); });
    installer_.SetMerge([this, log](const char* result, const std::string& detail) {
      if (log) log(current_app_.load(), "merge", result, detail.c_str());
    });
    installer_.SetRace([this, log](const char* step, const char* result, const std::string& detail) {
      if (log) log(current_app_.load(), step, result, detail.c_str());
    });
  }

 protected:
  void SignedOut() override { fastest_.Clear(); }
  std::optional<Result> FinishUpdate(const std::string& root, const std::atomic<bool>& cancel,
                                     const std::function<void(InstallStep)>& step) override;

 private:
  Result Run(const StoredSession& session, std::uint32_t id, const std::string& root, const InstallOptions& options,
             const std::atomic<bool>& cancel,
             const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
             const std::function<void(InstallStep)>& step, bool update, const std::function<bool()>& may_apply);
  CmClient& cm_;
  DepotInstaller installer_;
  FastestServerCache fastest_;
  std::string server_name_;  // the install worker's own: set and read during InstallApp's progress
  LogFn log_;
  std::function<DownloadTuning()> tuning_;
  std::function<std::string()> language_;
  const LicensedDepots* licensed_ = nullptr;
  const Storage* free_dlc_memory_ = nullptr;
  std::atomic<std::uint32_t> current_app_{0};
};

// User-facing text for an install failure category.
const char* InstallFailureText(InstallError error);

}  // namespace playden::steam
