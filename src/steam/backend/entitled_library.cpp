#include "steam/backend/entitled_library.h"

#include "steam/backend/app_info.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <set>
#include <thread>
#include <utility>

namespace playden::steam {
namespace {

constexpr std::size_t kFamilyAppLineLimit = 100;

// The probe thread and the library load both write family lines; one at a time.
FamilyLog Serialized(FamilyLog log) {
  if (!log) return log;
  auto mutex = std::make_shared<std::mutex>();
  return [log = std::move(log), mutex](const std::string& line) {
    std::lock_guard<std::mutex> lock(*mutex);
    log(line);
  };
}

}  // namespace

EntitledLibraryBackend::~EntitledLibraryBackend() { StopFamilyProbe(); }

void EntitledLibraryBackend::StopFamilyProbe() {
  std::lock_guard<std::mutex> guard(family_start_mutex_);
  {
    std::lock_guard<std::mutex> lock(family_mutex_);
    family_stop_.store(true);
  }
  family_cv_.notify_all();
  if (family_thread_.joinable()) family_thread_.join();
  family_stop_.store(false);
}

void EntitledLibraryBackend::SetFamilyDiagnostics(std::string ca_bundle, FamilyLog log) {
  StopFamilyProbe();
  family_answered_.store(false);
  family_ca_bundle_ = std::move(ca_bundle);
  family_perform_ = nullptr;
  family_log_ = Serialized(std::move(log));
}

void EntitledLibraryBackend::SetFamilyDiagnostics(FamilyPerform perform, FamilyLog log) {
  StopFamilyProbe();
  family_answered_.store(false);
  family_ca_bundle_.clear();
  family_perform_ = std::move(perform);
  family_log_ = Serialized(std::move(log));
}

// Once per run Steam's family service is asked what the family shares. The probe never decides or holds up
// a load: it runs on its own copies, one at a time, and a failed one is tried again by the next load.
bool EntitledLibraryBackend::StartFamilyProbe(const StoredSession& session) {
  if (!family_log_ || (!family_perform_ && family_ca_bundle_.empty())) return false;
  std::lock_guard<std::mutex> guard(family_start_mutex_);
  if (family_running_.load() || family_answered_.load()) return false;
  if (family_thread_.joinable()) family_thread_.join();  // finished
  {
    std::lock_guard<std::mutex> lock(family_mutex_);
    family_licenses_published_ = false;
  }
  family_running_.store(true);
  family_thread_ = std::thread([this, session, perform = family_perform_, ca_bundle = family_ca_bundle_,
                                log = family_log_] {
    std::unique_ptr<HttpClient> http;
    FamilyPerform request = perform;
    if (!request) {
      http = std::make_unique<HttpClient>(ca_bundle);
      request = [&http](const HttpRequest& r, const std::atomic<bool>& stop) { return http->Perform(r, stop); };
    }
    const auto start = std::chrono::steady_clock::now();
    const FamilyProbe probe = ProbeFamily(session, request, log, family_stop_);
    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    // Only Steam's answer ends the probing for this run: no family, or a family compared with known
    // licenses. A failure, or a family whose load had no licenses, is tried again by the next load.
    if (probe.outcome == FamilyProbe::Outcome::kNoGroup) family_answered_.store(true);
    if (probe.outcome == FamilyProbe::Outcome::kGroup && probe.library) {
      // Compared with the licenses of the load that started the probe, once that load has them.
      AcquisitionResult licenses;
      bool known = false;
      {
        std::unique_lock<std::mutex> lock(family_mutex_);
        family_cv_.wait_for(lock, std::chrono::seconds(30),
                            [&] { return family_stop_.load() || family_licenses_published_; });
        if (family_licenses_published_ && family_licenses_ok_) {
          known = true;
          licenses.error = CmError::kNone;
          licenses.shared_apps = family_shared_apps_;
          licenses.owned_apps = family_owned_apps_;
        }
      }
      if (!family_stop_.load()) LogFamilySharedApps(probe, known ? &licenses : nullptr, elapsed_ms, log);
      if (known) family_answered_.store(true);
    }
    family_running_.store(false);
  });
  return true;
}

void EntitledLibraryBackend::PublishFamilyLicenses(const AcquisitionResult* licenses) {
  {
    std::lock_guard<std::mutex> lock(family_mutex_);
    family_licenses_published_ = true;
    family_licenses_ok_ = licenses && licenses->ok();
    family_shared_apps_ = family_licenses_ok_ ? licenses->shared_apps : std::set<std::uint32_t>{};
    family_owned_apps_ = family_licenses_ok_ ? licenses->owned_apps : std::set<std::uint32_t>{};
  }
  family_cv_.notify_all();
}

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
  const bool probing = StartFamilyProbe(session);

  LibraryResult result = owned_.Fetch(session, cancel);
  if (result.error != LibraryError::kNone) stop.store(true, std::memory_order_release);
  while (!done.load(std::memory_order_acquire)) {
    if (cancel.load(std::memory_order_acquire)) stop.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  worker.join();

  last_error_.store(acquisitions.error);
  if (result.error != LibraryError::kNone || !acquisitions.ok()) {
    if (family_log_ && result.error == LibraryError::kNone)
      family_log_(std::string("family library licenses=unavailable cm_error=") + CmErrorName(acquisitions.error));
    if (probing) PublishFamilyLicenses(nullptr);
    return result;
  }
  if (licensed_) licensed_->Set(acquisitions);
  for (auto& game : result.games) {
    const auto found = acquisitions.acquired_at.find(*game.id.steam_app());
    if (found != acquisitions.acquired_at.end()) game.added_at_unix_seconds = found->second;
  }
  if (probing) PublishFamilyLicenses(&acquisitions);
  AddLicensedGames(session, acquisitions, cancel, &result);
  return result;
}

void EntitledLibraryBackend::AddLicensedGames(const StoredSession& session,
                                              const AcquisitionResult& acquisitions,
                                              const std::atomic<bool>& cancel, LibraryResult* result) {
  const auto& shared = acquisitions.shared_apps;
  std::set<std::uint32_t> listed;
  for (const auto& game : result->games) listed.insert(*game.id.steam_app());
  // A package with a listed app is a bought game (with its DLC) the owned list already covers.
  // Package 0 is Steam's basic free package, which GetOwnedGames leaves out on purpose. A package borrowed
  // from a family member can hold an app the account also owns next to ones only the lender has, so only
  // its listed apps are passed over.
  std::set<std::uint32_t> candidates;
  for (const auto& [package, apps] : acquisitions.package_apps) {
    if (package == 0) continue;
    const bool borrowed = std::any_of(apps.begin(), apps.end(), [&](auto app) { return shared.count(app); });
    if (!borrowed && std::any_of(apps.begin(), apps.end(), [&](auto app) { return listed.count(app); })) continue;
    for (const auto app : apps)
      if (!listed.count(app)) candidates.insert(app);
  }

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
      std::string type;
      const auto found = fetched.buffers.find(app);
      const auto info = found != fetched.buffers.end() ? ParseAppInfo(app, found->second) : std::nullopt;
      if (info) {
        type = info->type;
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
        if ((type == "game" || type == "demo") && info->name != "app_" + std::to_string(app)) name = info->name;
        if (!name.empty() && type == "demo") demos_.insert(app);
        if (!name.empty() && !info->icon.empty())
          icons_[app] = "https://media.steampowered.com/steamcommunity/public/images/apps/" + std::to_string(app) +
                        "/" + info->icon + ".jpg";
      }
      if (name.empty() && shared.count(app)) {
        // Why a shared app stays out of the Library, for the family log.
        std::string reason = found == fetched.buffers.end() ? "no_app_info"
                             : !info                         ? "app_info_unreadable"
                             : type != "game" && type != "demo"
                                 ? "type=" + (type.empty() ? std::string("none") : type.substr(0, 32))
                                 : "no_name";
        family_left_out_[app] = std::move(reason);
      }
      looked_up_[app] = name;
    }
  }
  std::size_t added = 0;
  std::size_t left_out = 0;
  std::size_t pending = 0;
  // Per-app lines are capped per load so a big family library can't rotate the license dump out of the
  // log. An app's line counts as logged only once written, so the rest follow on later loads.
  std::size_t written = 0;
  std::size_t truncated = 0;
  const auto detail = [&](std::set<std::uint32_t>& logged, std::uint32_t app, const std::string& line) {
    if (logged.count(app)) return;
    if (written == kFamilyAppLineLimit) {
      ++truncated;
      return;
    }
    ++written;
    logged.insert(app);
    family_log_(line);
  };
  for (const auto app : candidates) {
    const auto found = looked_up_.find(app);
    const bool is_shared = shared.count(app) != 0;
    if (found == looked_up_.end() || found->second.empty()) {
      if (!is_shared || !family_log_) continue;
      if (found == looked_up_.end()) {
        // PICS did not answer (or the load was canceled): asked again on the next refresh. Logged once per
        // app; a repeated failure only counts as pending.
        ++pending;
        if (!cancel.load())
          detail(family_failed_logged_, app, "family library left_out app=" + std::to_string(app) + " reason=app_info_failed");
        continue;
      }
      ++left_out;
      const auto reason = family_left_out_.find(app);
      detail(family_logged_, app, "family library left_out app=" + std::to_string(app) +
                                      " reason=" + (reason != family_left_out_.end() ? reason->second : std::string("no_name")));
      continue;
    }
    GameSummary game{GameId::Steam(app), found->second, {}, {}, {}};
    game.demo = demos_.count(app) != 0;
    game.family_shared = is_shared;
    if (const auto icon = icons_.find(app); icon != icons_.end()) game.artwork_source = icon->second;
    if (const auto added_at = acquisitions.acquired_at.find(app); added_at != acquisitions.acquired_at.end())
      game.added_at_unix_seconds = added_at->second;
    if (is_shared) {
      ++added;
      if (family_log_)
        detail(family_logged_, app, "family library added app=" + std::to_string(app) + " demo=" + (game.demo ? "1" : "0"));
    }
    result->games.push_back(std::move(game));
  }
  if (family_log_) {
    if (truncated) family_log_("family library truncated=" + std::to_string(truncated));
    std::size_t in_owned_list = 0;
    for (const auto app : shared) in_owned_list += listed.count(app);
    family_log_("family library shared=" + std::to_string(shared.size()) + " added=" + std::to_string(added) +
                " left_out=" + std::to_string(left_out) + " pending=" + std::to_string(pending) +
                " in_owned_list=" + std::to_string(in_owned_list));
  }
}

}  // namespace playden::steam
