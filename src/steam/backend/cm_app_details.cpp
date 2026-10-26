#include "steam/backend/cm_app_details.h"

#include "steam/backend/app_info.h"

namespace playden::steam {

AppDetails DetailsFromAppInfo(std::uint32_t id, const std::string& buffer,
                              const std::optional<LicensedDepots::Grant>& licensed,
                              std::string_view language) {
  AppDetails details;
  details.id = GameId::Steam(id);
  const auto app = ParseAppInfo(id, buffer);
  if (!app) {
    details.reason = "Steam returned unreadable details for this game.";
    return details;
  }
  details.available = true;
  std::vector<DepotInfo> depots;
  std::string reason;
  const std::set<std::uint32_t> owned{id};
  // As an install selects them; without licenses, or when they grant none of it, every eligible depot.
  bool selected = false;
  if (licensed) {
    auto apps = licensed->apps;
    apps.insert(id);
    selected = SelectDepots(*app, apps, licensed->depots, language, &depots, &reason);
  }
  if (!selected && !SelectDepots(*app, owned, std::nullopt, language, &depots, &reason)) {
    details.reason = "No Windows content to install.";
    return details;
  }
  std::uint64_t install = 0;
  std::uint64_t download = 0;
  for (const auto& depot : depots) {
    install += depot.size;
    // Some appinfo reports a download larger than the install (FINAL FANTASY XIII, issue #5); compressed chunks never are.
    download += depot.size && depot.download_size > depot.size ? depot.size : depot.download_size;
  }
  if (install) details.install_bytes = install;
  if (download) details.download_bytes = download;
  std::vector<AppLaunch> launches;
  details.windows_installable = EligibleLaunches(*app, owned, &launches, &reason, &details.launches_any_branch);
  if (!details.windows_installable) details.reason = "No Windows launch configuration.";
  for (const auto& launch : launches)
    details.launches.push_back({launch.executable, launch.arguments, launch.working_directory,
                                launch.description.value_or(std::string())});
  return details;
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
  const auto fetched = client_.FetchAppInfo(session, app_ids, cancel);
  std::vector<AppDetails> result;
  if (!fetched.ok()) return result;  // the service reports these as retryable
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
    result.push_back(DetailsFromAppInfo(id, found->second, licensed_ ? licensed_->Get() : std::nullopt, language));
  }
  return result;
}

}  // namespace playden::steam
