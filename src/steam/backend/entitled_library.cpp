#include "steam/backend/entitled_library.h"

#include "steam/backend/app_info.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <thread>

namespace playden::steam {

LibraryResult EntitledLibraryBackend::Fetch(const StoredSession& session,
                                            const std::atomic<bool>& cancel) {
  // The CM side gets its own flag so a failed or canceled owned fetch stops it promptly.
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};
  AcquisitionResult acquisitions;
  std::thread worker([&] {
    acquisitions = acquisitions_.FetchAcquisitionDates(session, stop);
    done.store(true, std::memory_order_release);
  });

  LibraryResult result = owned_.Fetch(session, cancel);
  if (result.error != LibraryError::kNone) stop.store(true, std::memory_order_release);
  while (!done.load(std::memory_order_acquire)) {
    if (cancel.load(std::memory_order_acquire)) stop.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  worker.join();

  last_error_.store(acquisitions.error);
  if (result.error != LibraryError::kNone || !acquisitions.ok()) return result;
  if (licensed_) licensed_->Set(acquisitions);
  for (auto& game : result.games) {
    const auto found = acquisitions.acquired_at.find(*game.id.steam_app());
    if (found != acquisitions.acquired_at.end()) game.added_at_unix_seconds = found->second;
  }
  AddLicensedGames(session, acquisitions, cancel, &result);
  return result;
}

void EntitledLibraryBackend::AddLicensedGames(const StoredSession& session,
                                              const AcquisitionResult& acquisitions,
                                              const std::atomic<bool>& cancel, LibraryResult* result) {
  std::set<std::uint32_t> listed;
  for (const auto& game : result->games) listed.insert(*game.id.steam_app());
  // A package with a listed app is a bought game (with its DLC) the owned list already covers.
  // Package 0 is Steam's basic free package, which GetOwnedGames leaves out on purpose.
  std::set<std::uint32_t> candidates;
  for (const auto& [package, apps] : acquisitions.package_apps) {
    if (package == 0 || std::any_of(apps.begin(), apps.end(), [&](auto app) { return listed.count(app); })) continue;
    candidates.insert(apps.begin(), apps.end());
  }
  if (candidates.empty()) return;

  std::lock_guard<std::mutex> lock(looked_up_mutex_);
  std::vector<std::uint32_t> unknown;
  for (const auto app : candidates)
    if (!looked_up_.count(app)) unknown.push_back(app);
  for (std::size_t start = 0; start < unknown.size() && !cancel.load(); start += kMaxAppInfoBatch) {
    const std::vector<std::uint32_t> batch(
        unknown.begin() + static_cast<std::ptrdiff_t>(start),
        unknown.begin() + static_cast<std::ptrdiff_t>(std::min(start + kMaxAppInfoBatch, unknown.size())));
    const auto fetched = acquisitions_.FetchAppInfo(session, batch, cancel);
    // Unanswered apps are asked again on the next refresh.
    if (!fetched.ok()) break;
    for (const auto app : batch) {
      std::string name;
      const auto found = fetched.buffers.find(app);
      if (found != fetched.buffers.end()) {
        const auto info = ParseAppInfo(app, found->second);
        std::string type = info ? info->type : std::string();
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
        if (info && (type == "game" || type == "demo") && info->name != "app_" + std::to_string(app)) name = info->name;
        if (!name.empty() && type == "demo") demos_.insert(app);
        if (!name.empty() && info && !info->icon.empty())
          icons_[app] = "https://media.steampowered.com/steamcommunity/public/images/apps/" + std::to_string(app) +
                        "/" + info->icon + ".jpg";
      }
      looked_up_[app] = name;
    }
  }
  for (const auto app : candidates) {
    const auto found = looked_up_.find(app);
    if (found == looked_up_.end() || found->second.empty()) continue;
    GameSummary game{GameId::Steam(app), found->second, {}, {}, {}};
    game.demo = demos_.count(app) != 0;
    if (const auto icon = icons_.find(app); icon != icons_.end()) game.artwork_source = icon->second;
    if (const auto added = acquisitions.acquired_at.find(app); added != acquisitions.acquired_at.end())
      game.added_at_unix_seconds = added->second;
    result->games.push_back(std::move(game));
  }
}

}  // namespace playden::steam
