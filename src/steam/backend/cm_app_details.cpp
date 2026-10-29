#include "steam/backend/cm_app_details.h"

#include "steam/backend/app_info.h"

#include <algorithm>
#include <chrono>

namespace playden::steam {

AppDetails::Update CompareWithRecord(std::uint32_t id, const std::vector<DepotInfo>& selected,
                                     const installs::InstalledRecord& record, const std::set<std::uint32_t>& listed) {
  AppDetails::Update update;
  if (record.depots.empty()) return update;  // an imported copy, or a record that names no depots
  std::set<std::uint32_t> owners{id};
  for (const auto& depot : record.depots) owners.insert(depot.owner == 0 ? id : depot.owner);
  std::vector<DepotInfo> download;
  for (const auto& depot : selected) {
    const auto recorded = std::find_if(record.depots.begin(), record.depots.end(),
                                       [&](const installs::RecordedDepot& entry) { return entry.id == depot.id; });
    if (recorded == record.depots.end()) {
      // An empty one adds no files: Steam lists free DLC (and DLC folded into the game) as empty depots, which an
      // install from an older Playden doesn't record.
      if (depot.size == 0) continue;
      // A depot the install doesn't hold: content of DLC bought since, or new content of the game.
      const auto owner = depot.owner_app == 0 ? id : depot.owner_app;
      if (owner != id && !owners.count(owner)) update.new_dlc = true;
      else update.newer = true;
      download.push_back(depot);
    } else if (depot.manifest_gid.value_or(0) != recorded->gid) {
      update.newer = true;
      download.push_back(depot);
    }
  }
  for (const auto& entry : record.depots) {
    const auto source = entry.source == 0 ? id : entry.source;
    if (!listed.count(source)) continue;
    if (std::none_of(selected.begin(), selected.end(), [&](const DepotInfo& depot) { return depot.id == entry.id; }))
      update.newer = true;
  }
  std::uint64_t install = 0;
  DepotBytes(download, &install, &update.download_bytes);
  if (update.download_bytes == 0) update.download_bytes = install;
  return update;
}

AppDetails DetailsFromAppInfo(std::uint32_t id, const std::string& buffer,
                              const std::optional<LicensedDepots::Grant>& licensed,
                              std::string_view language,
                              const std::optional<installs::InstalledRecord>& installed,
                              const std::vector<AppInfo>& dlc_apps) {
  AppDetails details;
  details.id = GameId::Steam(id);
  const auto app = ParseAppInfo(id, buffer);
  if (!app) {
    details.reason = "Steam returned unreadable details for this game.";
    return details;
  }
  details.available = true;
  std::string reason;
  const std::set<std::uint32_t> owned{id};
  DepotSelection selection;
  selection.language = std::string(language);
  // An installed game is selected as an update of it would be: its record's branch and language.
  if (installed) {
    if (!installed->language.empty()) selection.language = installed->language;
    if (!installed->branch.empty()) selection.branch = installed->branch;
  }
  SelectionResult selected;
  // As an install selects them; without licenses, or when they grant none of it, every eligible depot.
  bool any = false;
  std::set<std::uint32_t> apps = owned;
  std::optional<std::set<std::uint32_t>> depots_owned;
  if (licensed) {
    apps.insert(licensed->apps.begin(), licensed->apps.end());
    depots_owned = licensed->depots;
    any = SelectDepots(*app, dlc_apps, apps, depots_owned, selection, &selected);
  }
  // Only a selection the licenses made can be compared with the record: without them every depot passes.
  if (any && installed) {
    std::set<std::uint32_t> listed{id};
    for (const auto& dlc : dlc_apps) listed.insert(dlc.app_id);
    details.update = CompareWithRecord(id, selected.depots, *installed, listed);
  }
  if (!any) {
    apps = owned;
    depots_owned.reset();
    if (!SelectDepots(*app, dlc_apps, owned, std::nullopt, selection, &selected)) {
      details.reason = "No Windows content to install.";
      return details;
    }
  }
  details.languages = selected.languages;
  const auto& depots = selected.depots;
  std::set<std::uint32_t> dlc;
  for (const auto& depot : depots)
    if (depot.owner_app != 0 && depot.owner_app != id) dlc.insert(depot.owner_app);
  details.dlc_count = static_cast<std::uint32_t>(dlc.size());
  std::uint64_t install = 0;
  std::uint64_t download = 0;
  DepotBytes(depots, &install, &download);
  if (install) details.install_bytes = install;
  if (download) details.download_bytes = download;
  // Versions, selected the same way as the public branch above.
  for (auto& offer : OfferedBranches(*app, dlc_apps, apps, depots_owned, selection.language)) {
    AppVersion version{std::move(offer.name), std::move(offer.description), offer.time_updated, offer.build_id,
                       offer.download_bytes, offer.install_bytes, {}};
    for (const auto& bytes : offer.languages)
      version.languages.push_back({bytes.language, bytes.download_bytes, bytes.install_bytes});
    details.versions.push_back(std::move(version));
  }
  std::vector<AppLaunch> launches;
  // An installed game starts as its branch does.
  const std::string branch = installed && !installed->branch.empty() ? installed->branch : std::string("public");
  details.windows_installable = EligibleLaunches(*app, owned, branch, &launches, &reason, &details.launches_any_branch);
  if (!details.windows_installable) details.reason = "No Windows launch configuration.";
  for (const auto& launch : launches)
    details.launches.push_back({launch.executable, launch.arguments, launch.working_directory,
                                launch.description.value_or(std::string())});
  return details;
}

std::vector<AppInfo> CmAppDetailsSource::FetchDlcApps(CmClient& client, const StoredSession& session, std::uint32_t id,
                                                     const std::string& buffer, const std::atomic<bool>& cancel) {
  std::vector<AppInfo> dlc_apps;
  const auto app = ParseAppInfo(id, buffer);
  if (!app) return dlc_apps;
  std::vector<std::uint32_t> dlc_ids;
  for (const auto dlc : app->dlc_app_ids)
    if (dlc != id && std::find(dlc_ids.begin(), dlc_ids.end(), dlc) == dlc_ids.end()) dlc_ids.push_back(dlc);
  for (std::size_t start = 0; start < dlc_ids.size() && !cancel.load(); start += kMaxAppInfoBatch) {
    const std::vector<std::uint32_t> batch(
        dlc_ids.begin() + static_cast<std::ptrdiff_t>(start),
        dlc_ids.begin() + static_cast<std::ptrdiff_t>(std::min(start + kMaxAppInfoBatch, dlc_ids.size())));
    const auto dlc_info = client.FetchAppInfo(session, batch, cancel);
    if (!dlc_info.ok()) continue;
    for (const auto dlc : batch)
      if (const auto found = dlc_info.buffers.find(dlc); found != dlc_info.buffers.end())
        if (auto parsed = ParseAppInfo(dlc, found->second)) dlc_apps.push_back(std::move(*parsed));
  }
  return dlc_apps;
}

std::vector<AppDetails> CmAppDetailsSource::Fetch(const StoredSession& session,
                                                  const std::vector<GameId>& ids,
                                                  const std::atomic<bool>& cancel) {
  // The service only queues Steam games.
  std::vector<std::uint32_t> apps;
  for (const auto& id : ids)
    if (const auto app = id.steam_app()) apps.push_back(*app);
  std::vector<std::uint32_t> app_ids(apps);
  if (app_ids.size() > kMaxAppInfoBatch) app_ids.resize(kMaxAppInfoBatch);
  const auto started = std::chrono::steady_clock::now();
  const auto fetched = client_.FetchAppInfo(session, app_ids, cancel);
  const auto ms = std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
  std::vector<AppDetails> result;
  if (!fetched.ok()) {
    if (log_) for (const auto id : app_ids) log_(id, "details", "failed", "batch=" + std::to_string(app_ids.size()) + " ms=" + ms);
    return result;  // the service reports these as retryable
  }
  if (log_)
    for (const auto id : app_ids)
      log_(id, "details", fetched.buffers.count(id) ? "ok" : "missing", "batch=" + std::to_string(app_ids.size()) + " ms=" + ms);
  const auto language = language_ ? language_() : std::string(kDefaultContentLanguage);
  for (const auto id : apps) {
    const auto found = fetched.buffers.find(id);
    if (found == fetched.buffers.end()) {
      AppDetails missing;
      missing.id = GameId::Steam(id);
      missing.reason = "Steam has no details for this game.";
      result.push_back(std::move(missing));
      continue;
    }
    const auto record = installed_ ? installed_(id) : std::nullopt;
    // An installed game's DLC apps list depots of their own: an update selects them too. A batch Steam
    // doesn't answer only leaves those DLC out of the comparison.
    std::vector<AppInfo> dlc_apps;
    if (record && !record->depots.empty()) dlc_apps = FetchDlcApps(client_, session, id, found->second, cancel);
    if (cancel.load()) return {};
    result.push_back(DetailsFromAppInfo(id, found->second, licensed_ ? licensed_->Get() : std::nullopt, language, record,
                                        dlc_apps));
  }
  return result;
}

std::vector<DepotInfo> DepotsToMeasure(std::uint32_t id, const std::string& buffer,
                                       const std::optional<LicensedDepots::Grant>& licensed, const std::string& branch,
                                       std::string_view language, const std::vector<AppInfo>& dlc_apps) {
  const auto app = ParseAppInfo(id, buffer);
  if (!app) return {};
  DepotSelection selection;
  selection.language = std::string(language);
  if (!branch.empty()) selection.branch = branch;
  const std::set<std::uint32_t> owned{id};
  SelectionResult selected;
  bool any = false;
  if (licensed) {
    std::set<std::uint32_t> apps = owned;
    apps.insert(licensed->apps.begin(), licensed->apps.end());
    any = SelectDepots(*app, dlc_apps, apps, licensed->depots, selection, &selected);
  }
  if (!any && !SelectDepots(*app, dlc_apps, owned, std::nullopt, selection, &selected)) return {};
  const auto large = std::count_if(selected.depots.begin(), selected.depots.end(),
                                   [](const DepotInfo& depot) { return depot.size >= kMeasureDepotBytes; });
  if (large < 2) return {};
  return selected.depots;
}

MeasuredSize CmAppDetailsSource::Measure(const StoredSession& session, const SizeRequest& request,
                                         const std::atomic<bool>& cancel) {
  MeasuredSize result{request};
  const auto app = request.id.steam_app();
  if (!measurer_ || !measure_client_ || !app) return result;
  const auto started = std::chrono::steady_clock::now();
  const auto language = !request.language.empty() ? request.language
                        : language_ ? language_() : std::string(kDefaultContentLanguage);
  const std::string branch = request.branch.empty() ? "public" : request.branch;
  std::size_t depot_count = 0;
  const auto log = [&](const char* outcome, const std::string& extra) {
    if (!log_) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::string detail = "branch=" + branch + " language=" + language + " depots=" + std::to_string(depot_count);
    if (result.measured)
      detail += " install=" + std::to_string(result.install_bytes) + " download=" + std::to_string(result.download_bytes);
    if (!extra.empty()) detail += " " + extra;
    log_(*app, "size", outcome, detail + " ms=" + std::to_string(ms));
  };
  const auto fetched = measure_client_->FetchAppInfo(session, {*app}, cancel);
  if (!fetched.ok()) {
    result.failed = true;
    log("failed", "why=appinfo");
    return result;
  }
  const auto buffer = fetched.buffers.find(*app);
  if (buffer == fetched.buffers.end()) {
    log("failed", "why=no-appinfo");
    return result;
  }
  // As an install selects: DLC that only its own appinfo lists counts too. Free DLC an install would claim first
  // can't be known yet; the download's own space check still has the last word.
  const auto dlc_apps = FetchDlcApps(*measure_client_, session, *app, buffer->second, cancel);
  if (cancel.load()) {
    result.failed = true;
    log("failed", "why=canceled");
    return result;
  }
  const auto depots = DepotsToMeasure(*app, buffer->second, licensed_ ? licensed_->Get() : std::nullopt,
                                      request.branch, language, dlc_apps);
  depot_count = depots.size();
  if (depots.empty()) {
    log("skipped", {});
    return result;
  }
  // What an account is granted can differ: its answers are its own.
  std::string key = std::to_string(session.steam_id) + " " + std::to_string(*app);
  for (const auto& depot : depots) key += " " + std::to_string(depot.id) + ":" + std::to_string(depot.manifest_gid.value_or(0));
  if (const auto known = measured_.find(key); known != measured_.end()) {
    result.measured = true;
    result.install_bytes = known->second.first;
    result.download_bytes = known->second.second;
    log("cached", {});
    return result;
  }
  // As an install asks: the branch its manifests come from, "public" by default.
  const auto measure = measurer_->Measure(session, *app, depots, branch, cancel);
  if (measure.error != InstallError::kNone || measure.install_bytes == 0) {
    // Refused or unmergeable: an install would say so itself; Steam's figures stand meanwhile.
    result.failed = measure.error == InstallError::kNetwork || measure.error == InstallError::kCanceled ||
                    measure.error == InstallError::kNoSession;
    log("failed", std::string("why=") + InstallErrorName(measure.error) + (measure.detail.empty() ? "" : " (" + measure.detail + ")"));
    return result;
  }
  // Without a depot the account was refused, the size holds for these grants only: not kept.
  if (!measure.partial) measured_[key] = {measure.install_bytes, measure.download_bytes};
  result.measured = true;
  result.install_bytes = measure.install_bytes;
  result.download_bytes = measure.download_bytes;
  std::uint64_t summed = 0;
  for (const auto& depot : depots) summed += depot.size;
  log("fetched", "steam_sum=" + std::to_string(summed) + (measure.partial ? " partial=1" : ""));
  return result;
}

}  // namespace playden::steam
