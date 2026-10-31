#include "steam/backend/entitled_library.h"

#include "steam/backend/app_info.h"
#include "steam/backend/app_info_http.h"

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

EntitledLibraryBackend::~EntitledLibraryBackend() {
  StopLookups();
  StopFamilyProbe();
}

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
  family_no_group_.store(0);
  family_ca_bundle_ = std::move(ca_bundle);
  family_perform_ = nullptr;
  family_log_ = Serialized(std::move(log));
}

void EntitledLibraryBackend::SetFamilyDiagnostics(FamilyPerform perform, FamilyLog log) {
  StopFamilyProbe();
  family_answered_.store(false);
  family_no_group_.store(0);
  family_ca_bundle_.clear();
  family_perform_ = std::move(perform);
  family_log_ = Serialized(std::move(log));
}

// Once per run Steam's family service is asked what the family shares. The probe never decides or holds up
// a load: it runs on its own copies, one at a time, and a failed one is tried again by the next load.
bool EntitledLibraryBackend::StartFamilyProbe(const StoredSession& session) {
  if (!family_log_ || (!family_perform_ && family_ca_bundle_.empty())) return false;
  std::lock_guard<std::mutex> guard(family_start_mutex_);
  if (family_running_.load() || family_no_group_.load() == session.steam_id) return false;
  if (family_thread_.joinable()) family_thread_.join();  // finished
  {
    std::lock_guard<std::mutex> lock(family_mutex_);
    family_licenses_published_ = false;
    family_probe_done_ = false;
    family_web_fresh_ = false;
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
    // The family's games, for the load waiting on them (and later loads, until the next answer).
    {
      std::lock_guard<std::mutex> lock(family_mutex_);
      if (probe.outcome == FamilyProbe::Outcome::kGroup && probe.library) {
        family_web_account_ = session.steam_id;
        family_web_fresh_ = true;
        family_web_apps_.clear();
        for (const auto& app : probe.library->apps) family_web_apps_[app.appid] = app;
      }
      family_probe_done_ = true;
    }
    family_cv_.notify_all();
    // No family ends the probing for this run and account. A family is asked again by every load, since a
    // member can share a game at any time; it is compared with known licenses in the log once per run.
    if (probe.outcome == FamilyProbe::Outcome::kNoGroup) family_no_group_.store(session.steam_id);
    if (probe.outcome == FamilyProbe::Outcome::kGroup && probe.library && !family_answered_.load()) {
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
  // The background lookup shares the CM client: it stops here, and the load starts it again with what is
  // still unknown.
  StopLookups();
  const auto started = std::chrono::steady_clock::now();
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
  if (licensed_) licensed_->Set(session.steam_id, acquisitions);
  result.licenses_read = true;
  for (auto& game : result.games) {
    const auto found = acquisitions.acquired_at.find(*game.id.steam_app());
    if (found != acquisitions.acquired_at.end()) game.added_at_unix_seconds = found->second;
  }
  if (probing) PublishFamilyLicenses(&acquisitions);
  bool family_fresh = false;
  const auto family = FamilyApps(session.steam_id, probing, started, &family_fresh);
  AddLicensedGames(session, acquisitions, family ? &*family : nullptr, family_fresh, cancel, &result);
  return result;
}

std::optional<std::map<std::uint32_t, SharedLibraryApp>> EntitledLibraryBackend::FamilyApps(
    std::uint64_t steam_id, bool probing, std::chrono::steady_clock::time_point started, bool* fresh) {
  *fresh = false;
  if (!family_log_) return std::nullopt;
  std::unique_lock<std::mutex> lock(family_mutex_);
  // A load waits a little for its own probe (counted from the load's start); a slower answer is used by the
  // next one. A load that started none uses the last answer.
  if (probing)
    family_cv_.wait_until(lock, started + family_wait_, [&] { return family_stop_.load() || family_probe_done_; });
  if (family_web_account_ != steam_id) return std::nullopt;
  *fresh = probing && family_web_fresh_;
  return family_web_apps_;
}

namespace {

std::uint64_t UnixNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// The Library's entry for a licensed app it knows the name of.
GameSummary LicensedGame(std::uint32_t app, const AppName& name, bool shared,
                         const std::map<std::uint32_t, std::uint32_t>& acquired_at, std::uint32_t family_acquired) {
  GameSummary game{GameId::Steam(app), name.name, {}, {}, {}};
  game.demo = name.kind == AppName::Kind::kDemo;
  game.family_shared = shared;
  if (!name.icon.empty())
    game.artwork_source = "https://media.steampowered.com/steamcommunity/public/images/apps/" + std::to_string(app) +
                          "/" + name.icon + ".jpg";
  if (const auto added = acquired_at.find(app); added != acquired_at.end()) game.added_at_unix_seconds = added->second;
  else if (family_acquired) game.added_at_unix_seconds = family_acquired;
  return game;
}

// What PICS says an app is: a named game or demo, or something the Library leaves out (and why).
std::optional<AppName> NameFromAppInfo(std::uint32_t app, const std::string& text, std::string* reason) {
  const auto info = ParseAppInfo(app, text);
  if (!info) {
    *reason = "app_info_unreadable";
    return std::nullopt;
  }
  std::string type = info->type;
  std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
  AppName name;
  if ((type == "game" || type == "demo") && !info->name.empty() && info->name != "app_" + std::to_string(app)) {
    name.kind = type == "demo" ? AppName::Kind::kDemo : AppName::Kind::kGame;
    name.name = info->name;
    name.icon = info->icon.size() <= 64 ? info->icon : std::string();
  } else {
    *reason = type != "game" && type != "demo" ? "type=" + (type.empty() ? std::string("none") : type.substr(0, 32))
                                               : "no_name";
    name.dlc_for = info->dlc_for_app_id;
  }
  name.learned_at = UnixNow();
  return name;
}

}  // namespace

void EntitledLibraryBackend::SetAppNameStorage(const Storage* storage) {
  StopLookups();
  std::lock_guard<std::mutex> lock(names_mutex_);
  names_storage_ = storage;
  names_loaded_ = false;
}

void EntitledLibraryBackend::SetLateGamesCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(late_callback_mutex_);
  late_callback_ = std::move(callback);
}

std::vector<GameSummary> EntitledLibraryBackend::TakeLateGames(std::uint64_t steam_id) {
  std::lock_guard<std::mutex> lock(names_mutex_);
  if (late_context_.steam_id != steam_id) return {};
  return std::exchange(late_games_, {});
}

void EntitledLibraryBackend::LoadNamesLocked() {
  if (names_loaded_) return;
  names_loaded_ = true;
  if (names_storage_) names_ = LoadAppNames(*names_storage_);
}

void EntitledLibraryBackend::SaveNames() {
  AppNames copy;
  const Storage* storage = nullptr;
  {
    std::lock_guard<std::mutex> lock(names_mutex_);
    if (!names_dirty_ || !names_storage_) return;
    names_dirty_ = false;
    copy = names_;
    storage = names_storage_;
  }
  if (!SaveAppNames(*storage, copy) && family_log_) family_log_("family names save=failed");
}

void EntitledLibraryBackend::AddLicensedGames(const StoredSession& session,
                                              const AcquisitionResult& acquisitions,
                                              const std::map<std::uint32_t, SharedLibraryApp>* family,
                                              bool family_fresh, const std::atomic<bool>& cancel,
                                              LibraryResult* result) {
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

  std::size_t added = 0;
  std::size_t left_out = 0;
  std::size_t named_family = 0;
  std::size_t named_saved = 0;
  // Per-app lines are capped per load so a big family library can't rotate the license dump out of the
  // log. An app's line counts as logged only once written, so the rest follow on later loads.
  std::size_t written = 0;
  std::size_t truncated = 0;
  const auto detail = [&](std::set<std::uint32_t>& logged, std::uint32_t app, const std::string& line) {
    if (!family_log_ || logged.count(app)) return;
    if (written == kFamilyAppLineLimit) {
      ++truncated;
      return;
    }
    ++written;
    logged.insert(app);
    family_log_(line);
  };
  std::vector<std::uint32_t> unknown;
  std::vector<std::uint32_t> dlc_lookups;
  {
    std::lock_guard<std::mutex> lock(names_mutex_);
    LoadNamesLocked();
    for (const auto app : candidates) {
      const bool is_shared = shared.count(app) != 0;
      std::uint32_t family_acquired = 0;
      if (is_shared && family) {
        // Steam's family service lists the family's games (and demos) by name. A shared app this load's
        // answer leaves out is DLC or another non-game, and is not looked up. An older answer only names
        // games: a game shared since then is looked up.
        const auto found = family->find(app);
        const bool game = found != family->end() && (found->second.app_type == kFamilyAppTypeGame ||
                                                     found->second.app_type == kFamilyAppTypeDemo);
        if (game && !found->second.name.empty()) {
          AppName& name = names_[app];
          const auto kind = found->second.app_type == kFamilyAppTypeDemo ? AppName::Kind::kDemo : AppName::Kind::kGame;
          if (name.kind != kind || name.name != found->second.name) {
            name.kind = kind;
            name.name = found->second.name;
            name.learned_at = UnixNow();
            names_dirty_ = true;
          }
          ++added;
          ++named_family;
          if (name.kind == AppName::Kind::kDemo) demos_.insert(app);
          result->games.push_back(LicensedGame(app, name, true, acquisitions.acquired_at, found->second.rt_time_acquired));
          detail(family_logged_, app, "family library added app=" + std::to_string(app) + " demo=" +
                                          (name.kind == AppName::Kind::kDemo ? "1" : "0") + " source=family");
          continue;
        }
        if (!game && !names_.count(app) && family_fresh) {
          ++left_out;
          detail(family_logged_, app, "family library left_out app=" + std::to_string(app) + " reason=" +
                                          (found == family->end() ? std::string("not_in_family_list")
                                                                  : "app_type=" + std::to_string(found->second.app_type)));
          continue;
        }
        family_acquired = found != family->end() ? found->second.rt_time_acquired : 0;
      }
      const auto known = names_.find(app);
      if (known == names_.end()) {
        unknown.push_back(app);
        continue;
      }
      if (known->second.kind == AppName::Kind::kOther) {
        if (is_shared) {
          ++left_out;
          const auto reason = family_left_out_.find(app);
          detail(family_logged_, app, "family library left_out app=" + std::to_string(app) + " reason=" +
                                          (reason != family_left_out_.end() ? reason->second : std::string("not_a_game")));
        }
        continue;
      }
      if (is_shared) {
        ++added;
        ++named_saved;
        detail(family_logged_, app, "family library added app=" + std::to_string(app) + " demo=" +
                                        (known->second.kind == AppName::Kind::kDemo ? "1" : "0") + " source=saved");
      }
      result->games.push_back(LicensedGame(app, known->second, is_shared, acquisitions.acquired_at, family_acquired));
    }
    // What the background lookup still has to name, and what the Library needs to add each one.
    late_context_ = {};
    late_context_.steam_id = session.steam_id;
    late_context_.shared = shared;
    late_context_.acquired_at = acquisitions.acquired_at;
    late_context_.wanted.insert(unknown.begin(), unknown.end());
    if (family)
      for (const auto app : unknown)
        if (const auto found = family->find(app); found != family->end())
          late_context_.family_acquired[app] = found->second.rt_time_acquired;
    late_games_.clear();
    // The other licensed apps no lookup has named, a bought game's DLC among them, are looked up too: not for
    // the Library, but for the game each DLC names (DlcOf).
    std::set<std::uint32_t> queued(unknown.begin(), unknown.end());
    for (const auto& [package, apps] : acquisitions.package_apps) {
      if (package == 0) continue;
      for (const auto app : apps)
        if (!listed.count(app) && !names_.count(app) && queued.insert(app).second) dlc_lookups.push_back(app);
    }
  }
  if (family_log_) {
    if (truncated) family_log_("family library truncated=" + std::to_string(truncated));
    std::size_t in_owned_list = 0;
    for (const auto app : shared) in_owned_list += listed.count(app);
    std::size_t pending = 0;
    for (const auto app : unknown) pending += shared.count(app);
    family_log_("family library shared=" + std::to_string(shared.size()) + " added=" + std::to_string(added) +
                " left_out=" + std::to_string(left_out) + " pending=" + std::to_string(pending) +
                " in_owned_list=" + std::to_string(in_owned_list) + " named_family=" + std::to_string(named_family) +
                " named_saved=" + std::to_string(named_saved) +
                " family_list=" + (!family ? "n" : family_fresh ? "y" : "stale") +
                " lookups=" + std::to_string(unknown.size()) + " dlc_lookups=" + std::to_string(dlc_lookups.size()));
  }
  if ((unknown.empty() && dlc_lookups.empty()) || cancel.load()) {
    SaveNames();
    return;
  }
  // The Library's own lookups first: they name games, which it waits for.
  unknown.insert(unknown.end(), dlc_lookups.begin(), dlc_lookups.end());
  StartLookups(session, std::move(unknown));
}

std::vector<std::uint32_t> EntitledLibraryBackend::DlcOf(std::uint32_t app) {
  std::lock_guard<std::mutex> lock(names_mutex_);
  LoadNamesLocked();
  std::vector<std::uint32_t> out;
  for (const auto& [id, name] : names_)
    if (name.kind == AppName::Kind::kOther && name.dlc_for == app) out.push_back(id);
  std::sort(out.begin(), out.end());
  return out;
}

void EntitledLibraryBackend::StopLookups() {
  std::lock_guard<std::mutex> guard(lookup_start_mutex_);
  lookup_stop_.store(true);
  if (lookup_thread_.joinable()) lookup_thread_.join();
  lookup_stop_.store(false);
}

void EntitledLibraryBackend::StartLookups(const StoredSession& session, std::vector<std::uint32_t> apps) {
  std::lock_guard<std::mutex> guard(lookup_start_mutex_);
  if (lookup_thread_.joinable()) lookup_thread_.join();
  lookup_running_.store(true);
  lookup_thread_ = std::thread([this, session, apps = std::move(apps)] {
    RunLookups(session, apps);
    lookup_running_.store(false);
  });
}

bool EntitledLibraryBackend::WaitForLookups(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (lookup_running_.load()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Names one app from its app info, and queues it for the Library when it is a game this load wanted.
bool EntitledLibraryBackend::Record(std::uint32_t app, const std::string& text) {
  std::string reason;
  auto name = NameFromAppInfo(app, text, &reason);
  std::lock_guard<std::mutex> lock(names_mutex_);
  if (!name) {
    if (late_context_.shared.count(app)) family_left_out_[app] = reason;
    return false;
  }
  if (name->kind == AppName::Kind::kOther && late_context_.shared.count(app)) family_left_out_[app] = reason;
  if (name->kind == AppName::Kind::kDemo) demos_.insert(app);
  const bool wanted = late_context_.wanted.erase(app) != 0;
  names_[app] = *name;
  names_dirty_ = true;
  if (!wanted || name->kind == AppName::Kind::kOther) return false;
  const auto family_acquired = late_context_.family_acquired.find(app);
  late_games_.push_back(LicensedGame(app, *name, late_context_.shared.count(app) != 0, late_context_.acquired_at,
                                     family_acquired != late_context_.family_acquired.end() ? family_acquired->second
                                                                                             : 0));
  return true;
}

void EntitledLibraryBackend::RecordOther(std::uint32_t app, const std::string& reason) {
  std::lock_guard<std::mutex> lock(names_mutex_);
  if (late_context_.shared.count(app)) family_left_out_[app] = reason;
  late_context_.wanted.erase(app);
  AppName name;
  name.learned_at = UnixNow();
  names_[app] = std::move(name);
  names_dirty_ = true;
}

void EntitledLibraryBackend::PublishLateGames() {
  {
    std::lock_guard<std::mutex> lock(names_mutex_);
    if (late_games_.empty()) return;
  }
  std::lock_guard<std::mutex> lock(late_callback_mutex_);
  if (late_callback_) late_callback_();
}

// Names the apps a load could not, off the load: PICS in batches, then the hash-only ones over HTTP a few at
// a time. Games are handed to the Library after each batch. A failed batch ends the pass; what is still
// unknown is asked again by the next load.
void EntitledLibraryBackend::RunLookups(const StoredSession& session, const std::vector<std::uint32_t>& apps) {
  const auto start = std::chrono::steady_clock::now();
  if (family_log_) family_log_("family lookup start apps=" + std::to_string(apps.size()));
  std::size_t games = 0;
  std::size_t by_hash = 0;
  std::size_t unanswered = 0;
  std::string error;
  const auto make_perform = [this]() -> AppInfoPerform {
    if (family_perform_) return family_perform_;
    if (family_ca_bundle_.empty()) return nullptr;
    auto client = std::make_shared<HttpClient>(family_ca_bundle_);
    return [client](const HttpRequest& request, const std::atomic<bool>& stop) { return client->Perform(request, stop); };
  };
  for (std::size_t first = 0; first < apps.size() && !lookup_stop_.load(); first += kMaxAppInfoBatch) {
    const std::vector<std::uint32_t> batch(
        apps.begin() + static_cast<std::ptrdiff_t>(first),
        apps.begin() + static_cast<std::ptrdiff_t>(std::min(first + kMaxAppInfoBatch, apps.size())));
    const auto fetched = acquisitions_.FetchAppInfoPics(session, batch, lookup_stop_);
    if (!fetched.ok()) {
      if (fetched.error != CmError::kCanceled) error = CmErrorName(fetched.error);
      break;
    }
    std::size_t answered = fetched.buffers.size();
    for (const auto& [app, text] : fetched.buffers) games += Record(app, text);
    // Apps Steam doesn't know, or describes with nothing, are no games; a refused token is asked again.
    for (const auto& [app, reason] : fetched.unanswered)
      if (reason == "unknown" || reason == "empty") {
        RecordOther(app, "no_app_info:" + reason);
        ++answered;
      }
    if (!fetched.hashed.empty()) {
      AppInfoHttpStats http;
      const auto texts = FetchAppInfoByHash(make_perform, fetched.http_host, fetched.hashed, kAppInfoHttpWorkers,
                                            lookup_stop_, &http, [&](std::uint32_t app, const std::string& text) {
                                              games += Record(app, text);
                                            });
      by_hash += texts.size();
      answered += texts.size();
      if (family_log_)
        family_log_("family lookup http=" + std::to_string(fetched.hashed.size()) + " fetched=" +
                    std::to_string(http.fetched) + " failed=" + std::to_string(http.failed) + " skipped=" +
                    std::to_string(http.skipped) + " connections=" + std::to_string(http.new_connections) +
                    " ms=" + std::to_string(http.total_ms) + (http.error.empty() ? "" : " error=" + http.error));
    }
    unanswered += batch.size() - answered;
    SaveNames();
    PublishLateGames();
  }
  SaveNames();
  PublishLateGames();
  if (family_log_)
    family_log_("family lookup end games=" + std::to_string(games) + " by_hash=" + std::to_string(by_hash) +
                " unanswered=" + std::to_string(unanswered) + (error.empty() ? "" : " error=" + error) +
                (lookup_stop_.load() ? " stopped=1" : "") + " elapsed_ms=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - start).count()));
}

}  // namespace playden::steam
