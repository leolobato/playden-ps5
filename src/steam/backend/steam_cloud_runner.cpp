#include "steam/backend/steam_cloud_runner.h"

namespace playden::steam {
namespace {

CloudReport Report(const cloud::CloudPlan& plan) {
  CloudReport report;
  switch (plan.status) {
    case cloud::PlanStatus::kUpToDate: report.state = CloudState::kUpToDate; break;
    case cloud::PlanStatus::kReady: report.state = CloudState::kSyncing; break;
    case cloud::PlanStatus::kConflict: report.state = CloudState::kConflict; break;
    case cloud::PlanStatus::kNeedsAccount: report.state = CloudState::kNeedsAccount; break;
    case cloud::PlanStatus::kUnsupported: report.state = CloudState::kUnsupported; break;
    case cloud::PlanStatus::kFailed: report.state = CloudState::kFailed; break;
  }
  report.message = plan.message;
  report.downloads = plan.downloads;
  report.uploads = plan.uploads;
  report.conflicts = plan.conflicts;
  report.local_files = plan.local_files;
  report.remote_files = plan.remote_files;
  report.local_bytes = plan.local_bytes;
  report.remote_bytes = plan.remote_bytes;
  report.local_latest = plan.local_latest;
  report.remote_latest = plan.remote_latest;
  // No baseline for this account yet and both sides differ: the first sync on this device.
  report.first_sync = plan.status == cloud::PlanStatus::kConflict && plan.requires_account_confirmation;
  report.first_overwrite = plan.status == cloud::PlanStatus::kConflict && plan.first_overwrite;
  report.canceled = plan.status == cloud::PlanStatus::kFailed && plan.canceled;
  return report;
}

const char* StateName(CloudState state) {
  switch (state) {
    case CloudState::kSyncing: return "ready";
    case CloudState::kSynced: return "synced";
    case CloudState::kUpToDate: return "up-to-date";
    case CloudState::kConflict: return "conflict";
    case CloudState::kNeedsAccount: return "needs-account";
    case CloudState::kUnsupported: return "unsupported";
    case CloudState::kFailed: return "failed";
    default: return "other";
  }
}

// The service only hands Steam games to the runner.
std::uint32_t App(const CloudLocation& location) { return location.id.steam_app().value_or(0); }

}  // namespace

SteamCloudRunner::SteamCloudRunner(CmClient& cm, HttpClient& http) : sync_(cm, http) {}

void SteamCloudRunner::SetLog(std::function<void(std::uint32_t app, const char* event, const std::string& message)> log) {
  log_ = std::move(log);
  sync_.SetTrace([this](const char* step) {
    if (log_) log_(current_app_, "trace", step);
  });
}

SteamCloudRunner::SteamCloudRunner(cloud::CloudHooks hooks) : sync_(std::move(hooks)) {}

cloud::GameSaveLocation SteamCloudRunner::SaveLocation(const CloudLocation& location) {
  return {App(location), location.game_root,
          location.prefix_root.empty() ? location.state_root + "/prefix" : location.prefix_root,
          location.state_root};
}

CloudReport SteamCloudRunner::Check(const StoredSession& session, const CloudLocation& location,
                                    const std::atomic<bool>& cancel) {
  current_app_ = App(location);
  auto plan = sync_.Check(session, SaveLocation(location), cancel);
  auto report = Report(plan);
  if (log_)
    log_(App(location), (std::string("check-") + StateName(report.state)).c_str(),
         plan.detail.empty() ? report.message : report.message + " (" + plan.detail + ")");
  plans_[App(location)] = {std::move(plan), SaveLocation(location)};
  return report;
}

CloudReport SteamCloudRunner::Apply(const StoredSession& session, const CloudLocation& location, CloudResolution resolution,
                                    bool confirm_account, const std::atomic<bool>& cancel) {
  const auto found = plans_.find(App(location));
  if (found == plans_.end()) {
    CloudReport report;
    report.message = "Check this game's saves again.";
    return report;
  }
  if (location.prefix_root.empty() && !location.game_root.empty()) {
    // No bottle yet: Windows-folder saves would land where the game never looks.
    CloudReport report;
    report.message = "Saves move once the game is set up. Press Play first.";
    return report;
  }
  const auto requested = SaveLocation(location);
  const auto& checked = found->second.location;
  if (requested.app_id != checked.app_id || requested.game_root != checked.game_root ||
      requested.prefix_root != checked.prefix_root || requested.state_root != checked.state_root) {
    plans_.erase(found);
    CloudReport report;
    report.message = "This game's save location changed. Check its saves again.";
    return report;
  }
  const auto choice = resolution == CloudResolution::kUseLocal   ? cloud::Resolution::kUseLocal
                      : resolution == CloudResolution::kUseCloud ? cloud::Resolution::kUseCloud
                                                                 : cloud::Resolution::kAutomatic;
  current_app_ = App(location);
  auto report = Report(found->second.plan);
  const auto outcome = sync_.Apply(session, checked, found->second.plan, choice, confirm_account, cancel, progress_);
  plans_.erase(found);  // a plan is used once; the next sync checks again
  report.state = outcome.ok ? CloudState::kSynced : CloudState::kFailed;
  report.message = outcome.message;
  report.canceled = !outcome.ok && outcome.canceled;
  if (outcome.ok) {  // both sides as they are now, not as the check found them
    report.local_files = outcome.local_files;
    report.remote_files = outcome.remote_files;
    report.local_bytes = outcome.local_bytes;
    report.remote_bytes = outcome.remote_bytes;
    report.local_latest = outcome.local_latest;
    report.remote_latest = outcome.remote_latest;
  }
  if (log_)
    log_(App(location), outcome.ok ? "apply-ok" : "apply-failed",
         outcome.detail.empty() ? outcome.message : outcome.message + " (" + outcome.detail + ")");
  report.first_sync = false;
  report.first_overwrite = false;
  return report;
}

std::vector<CloudDrift> SteamCloudRunner::QuickCheck(const StoredSession& session, const std::vector<CloudLocation>& locations,
                                                     const std::atomic<bool>& cancel) {
  std::vector<cloud::GameSaveLocation> mapped;
  for (const auto& location : locations) mapped.push_back(SaveLocation(location));
  current_app_ = 0;
  const auto drift = sync_.QuickCheck(session, mapped, cancel);
  std::vector<CloudDrift> out;
  std::size_t changed = 0;
  for (const auto value : drift) {
    switch (value) {
      case cloud::Drift::kUnchanged: out.push_back(CloudDrift::kUnchanged); break;
      case cloud::Drift::kLocal: out.push_back(CloudDrift::kLocal); break;
      case cloud::Drift::kRemote: out.push_back(CloudDrift::kRemote); break;
      case cloud::Drift::kBoth: out.push_back(CloudDrift::kBoth); break;
      default: out.push_back(CloudDrift::kUnknown); break;
    }
    if (out.back() != CloudDrift::kUnchanged) ++changed;
  }
  if (log_) log_(0, "quick-check", std::to_string(changed) + " of " + std::to_string(out.size()) + " to check");
  return out;
}

bool SteamCloudRunner::RemoveLocal(const CloudLocation& location, std::string* message) {
  plans_.erase(App(location));  // a plan made before the delete no longer matches
  const bool ok = sync_.RemoveLocal(SaveLocation(location), message);
  if (log_) log_(App(location), ok ? "remove-local-ok" : "remove-local-failed", *message);
  return ok;
}

}  // namespace playden::steam
