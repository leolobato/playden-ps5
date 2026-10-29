#pragma once

#include "installs/install_record.h"
#include "steam/app_details_service.h"
#include "steam/backend/app_info.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/content_language.h"
#include "steam/backend/depot_installer.h"

#include <functional>
#include <map>
#include <utility>

namespace playden::steam {

// App details from PICS: totals of the depots Playden would install (Windows, the chosen content
// language, public branch) and whether a Windows launch entry exists. Sizes are Steam's per-depot
// figures, so they are an estimate until the manifests are fetched.
class CmAppDetailsSource final : public AppDetailsSource {
 public:
  explicit CmAppDetailsSource(CmClient& client) : client_(client) {}
  // The licenses the library last read: sizes then count only the depots they grant.
  void SetLicensedDepots(const LicensedDepots* licensed) { licensed_ = licensed; }
  // The content language sizes are counted for; it has to be the one an install would use or
  // the free-space check is made against the wrong depots. Read on the details worker when a
  // fetch starts, so a change in Settings reaches the next one.
  void SetContentLanguage(std::function<std::string()> language) { language_ = std::move(language); }
  // An installed game's install record (none: not installed). Its details are selected with the
  // record's branch and language, its DLC apps' appinfo is read too, and they say whether an update
  // or new DLC is there (AppDetails::update). Called on the details worker.
  void SetInstalled(std::function<std::optional<installs::InstalledRecord>(std::uint32_t app)> installed) {
    installed_ = std::move(installed);
  }
  // What measures an install choice from its manifests (MeasuredSize), and the connection its size checks read
  // appinfo on: Measure runs on its own worker beside Fetch, so not the details one. Without them Steam's figures
  // stand.
  void SetMeasurer(DepotInstaller* measurer, CmClient* client) {
    measurer_ = measurer;
    measure_client_ = client;
  }
  // Optional: lines for the install log, never identifiers beyond app and depot IDs. Step "details": one per game a
  // fetch answers (result ok, missing or failed, with the time Steam took). Step "size": one per size check, result
  // fetched (the manifests were read), cached, skipped (Steam's sum stands: fewer than two large depots) or failed,
  // with the choice, the sizes and the time taken.
  void SetLog(std::function<void(std::uint32_t app, const char* step, const char* result, const std::string& detail)> log) {
    log_ = std::move(log);
  }
  std::vector<AppDetails> Fetch(const StoredSession& session, const std::vector<GameId>& ids,
                                const std::atomic<bool>& cancel) override;
  MeasuredSize Measure(const StoredSession& session, const SizeRequest& request,
                       const std::atomic<bool>& cancel) override;

 private:
  // The game's DLC apps' appinfo (a batch Steam doesn't answer is left out).
  std::vector<AppInfo> FetchDlcApps(CmClient& client, const StoredSession& session, std::uint32_t id,
                                    const std::string& buffer, const std::atomic<bool>& cancel);
  CmClient& client_;
  const LicensedDepots* licensed_ = nullptr;
  std::function<std::string()> language_;
  std::function<std::optional<installs::InstalledRecord>(std::uint32_t)> installed_;
  DepotInstaller* measurer_ = nullptr;
  CmClient* measure_client_ = nullptr;
  std::function<void(std::uint32_t, const char*, const char*, const std::string&)> log_;
  // Measured sizes (the size worker's only) by the depots and manifests measured: a manifest never changes, so neither does its size.
  std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> measured_;
};

// The depots an install of `branch` in `language` selects, as DetailsFromAppInfo selects them, when their
// merged size can differ enough from Steam's sum to be worth fetching their manifests: at least two of them
// hold kMeasureDepotBytes or more. Empty: Steam's figures stand.
inline constexpr std::uint64_t kMeasureDepotBytes = 64ull * 1000 * 1000;
// `dlc_apps`: the game's DLC apps' appinfo, for the depots only they list, as an install reads them.
std::vector<DepotInfo> DepotsToMeasure(std::uint32_t id, const std::string& buffer,
                                       const std::optional<LicensedDepots::Grant>& licensed, const std::string& branch,
                                       std::string_view language, const std::vector<AppInfo>& dlc_apps = {});

// Pure part, for tests: turns one parsed app buffer into details. With `licensed`, sizes count
// only the depots those licenses grant, as an install selects them. With `installed` (the game's
// install record), the record's branch and language select, and `update` compares the selection
// with the record; `dlc_apps` are the game's DLC apps' appinfo, for the depots only they list.
AppDetails DetailsFromAppInfo(std::uint32_t id, const std::string& buffer,
                              const std::optional<LicensedDepots::Grant>& licensed = std::nullopt,
                              std::string_view language = kDefaultContentLanguage,
                              const std::optional<installs::InstalledRecord>& installed = std::nullopt,
                              const std::vector<AppInfo>& dlc_apps = {});

// What `selected` (as an install of the record's branch and language selects now) changes against
// `record`. Recorded depots listed by an app whose appinfo is not in `listed` are not compared: they
// can't have left. Nothing for a record without depots.
AppDetails::Update CompareWithRecord(std::uint32_t id, const std::vector<DepotInfo>& selected,
                                     const installs::InstalledRecord& record, const std::set<std::uint32_t>& listed);

}  // namespace playden::steam
