#include "steam/backend/steam_install_runner.h"

#include "runtime/steamworks.h"
#include "steam/backend/app_info.h"

#include <algorithm>
#include <set>
#include <charconv>
#include <string_view>

namespace playden::steam {

void LogInstallTrace(const InstallLogFn& log, std::uint32_t app, const char* marker) {
  if (!log) return;
  if (std::string(marker) == "journal-reset") log(app, "journal", "reset", "");
  else if (std::string(marker) == "journal-legacy") log(app, "journal", "legacy", "");
  else log(app, "trace", marker, "");
}

std::string FormatInstallServerList(const std::vector<ContentServer>& servers, const char* preference) {
  const std::string pref = preference ? preference : "auto";
  std::string detail = "n=" + std::to_string(servers.size()) + " https=" +
      std::to_string(std::count_if(servers.begin(), servers.end(), [](const ContentServer& server) { return server.https; })) +
      " pref=" + (pref == "chosen" || pref == "missing" || pref == "fastest" ? pref : "auto") + " first=" +
      (servers.empty() ? "none" : CdnServerLabel(servers.front().host) + "@0") + " order=";
  for (std::size_t i = 0; i < std::min<std::size_t>(servers.size(), 8); ++i) {
    if (i) detail += ',';
    detail += CdnServerLabel(servers[i].host) + ':' + std::to_string(std::clamp(servers[i].priority, 0, 9));
  }
  return detail;
}

std::vector<std::string> FormatInstallServerStats(const std::vector<InstallServerStats>& servers) {
  std::vector<std::string> lines;
  std::string detail;
  for (std::size_t i = 0; i < std::min<std::size_t>(servers.size(), 9); ++i) {
    const auto& server = servers[i];
    if (!server.requests) continue;
    const auto& label = server.label;
    const bool known = label == "akamai" || label == "fastly" || label == "google" || label == "alibaba" ||
                       label == "edgenext" || label == "valve" || label == "cache" || label == "other";
    const auto token = std::string(known ? label : "cache") + '@' + std::to_string(std::min<std::size_t>(server.index, 8)) +
        ":req=" + std::to_string(server.requests) + ",fail=" + std::to_string(server.failures) +
        ",mb=" + std::to_string(server.bytes / 1000000) + ",ms=" + std::to_string(server.fetch_ms);
    // Leave room for timestamp, app, step and result in the native 640-byte log buffer.
    if (!detail.empty() && detail.size() + 1 + token.size() > 360) { lines.push_back(std::move(detail)); detail.clear(); }
    if (!detail.empty()) detail += ' ';
    detail += token;
  }
  if (!detail.empty()) lines.push_back(std::move(detail));
  if (lines.empty()) lines.push_back("n=0");
  return lines;
}

std::string FormatInstallStats(const char* phase, const InstallStats& stats) {
  std::string detail;
  const auto add = [&](const char* key, std::uint64_t value) {
    if (!detail.empty()) detail += ' ';
    detail += key; detail += '='; detail += std::to_string(value);
  };
  add("files_total", stats.files_total); add("download_total", stats.download_total);
  if (std::string(phase) == "shape") {
    add("chunks_pending", stats.chunks_pending); add("files_pending", stats.files_pending);
    add("bytes_pending", stats.bytes_pending);
    add("slots", stats.slots); add("ahead", stats.ahead_files);
    return detail;
  }
  add("dl_ms", stats.download_ms); add("cb", stats.compressed_bytes); add("ob", stats.original_bytes);
  add("ch", stats.chunks_done); add("sk", stats.chunks_skipped);
  add("fc", stats.files_created); add("dc", stats.folders_created);
  add("fetch", stats.fetch_ms); add("proc", stats.process_ms); add("create", stats.create_ms);
  add("write", stats.write_ms); add("jrnl", stats.journal_ms); add("back", stats.backoff_ms);
  add("req", stats.requests); add("conn", stats.new_connections);
  add("res", stats.resolve_ms); add("tcp", stats.connect_ms); add("tls", stats.tls_ms); add("ttfb", stats.first_byte_ms);
  add("retry", stats.chunk_retries); add("bad", stats.corrupt_retries);
  add("slots", stats.slots); add("reuse", stats.reuse);
  add("waiting", stats.peak_waiting); add("active", stats.peak_active);
  add("access", stats.access_ms); add("servers", stats.servers_ms); add("manifest", stats.manifests_ms); add("prep", stats.prepare_ms);
  add("http_ms", stats.total_ms); add("http_b", stats.bytes); add("fail", stats.failures);
  return detail;
}

std::optional<std::string> FastestServerCache::Get(std::uint64_t account) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (host_.empty()) return std::nullopt;
  if (account != account_ || Time() - at_ >= std::chrono::minutes(kMinutes)) {
    host_.clear();
    return std::nullopt;
  }
  return host_;
}

void FastestServerCache::Remember(std::uint64_t account, const std::string& host) {
  std::lock_guard<std::mutex> lock(mutex_);
  account_ = account;
  host_ = host;
  at_ = Time();
}

void FastestServerCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  host_.clear();
}

std::string SteamInstallRunner::Remove(const std::string& root) {
  std::string failure;
  const bool ok = RemoveManagedInstall(root, &failure);
  const auto slash = root.rfind('/');
  const auto name = root.substr(slash == std::string::npos ? 0 : slash + 1);
  std::uint32_t app = 0;
  const auto parsed = std::from_chars(name.data(), name.data() + name.size(), app);
  if (log_ && app != 0 && parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size())
    log_(app, "install-removal", "done", ok ? "ok=1" : ("ok=0 " + failure).c_str());
  return ok ? std::string() : failure;
}

// The app ID a game folder is named after; 0 for another name.
std::uint32_t FolderApp(const std::string& root) {
  const auto slash = root.rfind('/');
  const auto name = root.substr(slash == std::string::npos ? 0 : slash + 1);
  std::uint32_t app = 0;
  const auto parsed = std::from_chars(name.data(), name.data() + name.size(), app);
  return parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() ? app : 0;
}

std::string SteamInstallRunner::DiscardUpdate(const std::string& root) {
  std::string failure;
  const bool ok = DiscardManagedUpdate(root, &failure);
  if (log_ && FolderApp(root) != 0) log_(FolderApp(root), "update-discard", "done", ok ? "ok=1" : ("ok=0 " + failure).c_str());
  return ok ? std::string() : failure;
}

std::optional<InstallRunner::Result> SteamInstallRunner::FinishUpdate(const std::string& root, const std::atomic<bool>& cancel,
                                                                      const std::function<void(InstallStep)>& step) {
  const auto state = installs::ReadUpdateState(root);
  if (!state || state->phase == installs::UpdatePhase::kStaging) return std::nullopt;
  const auto app = FolderApp(root);
  current_app_.store(app);
  if (step) step(InstallStep::kApplying);
  const auto outcome = DepotInstaller::FinishUpdate(root, cancel, [this, app](const char* marker) {
    LogInstallTrace(log_, app, marker);
  });
  if (log_) log_(app, "update-finish", InstallErrorName(outcome.error), outcome.detail.c_str());
  Result result;
  result.ok = outcome.error == InstallError::kNone;
  result.canceled = outcome.error == InstallError::kCanceled;
  result.failure = InstallFailureText(outcome.error);
  result.install_bytes = outcome.progress.install_total;
  return result;
}

const char* InstallFailureText(InstallError error) {
  switch (error) {
    case InstallError::kNone: return "";
    case InstallError::kCanceled: return "";
    case InstallError::kNoSession: return "Sign in to Steam to install.";
    case InstallError::kRejected: return "Steam refused access to this game's content.";
    case InstallError::kNetwork: return "Network problem. Resume to try again.";
    case InstallError::kCorrupt: return "Downloaded content failed verification. Resume to try again.";
    case InstallError::kNoSpace: return "Not enough free space.";
    case InstallError::kStorage: return "Could not write the game files.";
    case InstallError::kProtocol: return "Steam returned unexpected data.";
    case InstallError::kGameRunning: return InstallRunner::kWaitingForGame;
  }
  return "Install failed.";
}

namespace {

// Comma-separated items after a key, split so each line fits the native log buffer with room for the
// timestamp, app and step.
std::vector<std::string> FamilyLines(const char* key, const std::vector<std::string>& items) {
  std::vector<std::string> lines;
  std::string line = key;
  bool first = true;
  for (const auto& item : items) {
    if (!first && line.size() + 1 + item.size() > 360) {
      lines.push_back(std::move(line));
      line = key;
      first = true;
    }
    line += (first ? "" : ",") + item;
    first = false;
  }
  if (first) line += "none";
  lines.push_back(std::move(line));
  return lines;
}

std::string IdText(const std::vector<std::uint32_t>& ids) {
  std::string text;
  for (const auto id : ids) text += (text.empty() ? "" : ",") + std::to_string(id);
  return text.empty() ? "none" : text;
}

}  // namespace

bool UpdateNeedsUnfetchedDlc(const installs::InstalledRecord& record, std::uint32_t app,
                             const std::set<std::uint32_t>& unfetched) {
  return std::any_of(record.depots.begin(), record.depots.end(), [&](const installs::RecordedDepot& depot) {
    const auto source = depot.source == 0 ? app : depot.source;
    return source != app && unfetched.count(source) > 0;
  });
}

std::vector<std::uint32_t> FreeDlcCandidates(const AppInfo& app, const std::vector<AppInfo>& dlc_apps,
                                             const std::set<std::uint32_t>& owned_apps,
                                             const std::set<std::uint32_t>& refused, std::size_t limit) {
  const auto windows = [](const DepotInfo& depot) { return !depot.shared_install && OsListMatchesWindows(depot.os_list); };
  std::vector<std::uint32_t> candidates;
  for (const auto dlc : app.dlc_app_ids) {
    if (candidates.size() >= limit) break;
    if (dlc == app.app_id || owned_apps.count(dlc) || refused.count(dlc) ||
        std::find(candidates.begin(), candidates.end(), dlc) != candidates.end())
      continue;
    bool content = std::any_of(app.depots.begin(), app.depots.end(), [&](const DepotInfo& depot) {
      return depot.dlc_app_id == dlc && windows(depot);
    });
    for (const auto& info : dlc_apps)
      if (info.app_id == dlc) content = content || std::any_of(info.depots.begin(), info.depots.end(), windows);
    if (content) candidates.push_back(dlc);
  }
  return candidates;
}

std::vector<std::uint32_t> FreeDlcRefusals(const std::vector<std::uint32_t>& claimed, const FreeLicenseResult& claim,
                                           const std::set<std::uint32_t>& owned_apps) {
  if (!claim.ok() || claim.eresult != cm::kEResultOk || (claim.granted_apps.empty() && !claim.granted_packages.empty()))
    return {};
  std::vector<std::uint32_t> refused;
  for (const auto dlc : claimed)
    if (std::find(claim.granted_apps.begin(), claim.granted_apps.end(), dlc) == claim.granted_apps.end() &&
        !owned_apps.count(dlc))
      refused.push_back(dlc);
  return refused;
}

std::string FreeDlcMemoryName(std::uint64_t steam_id) {
  return "steam-free-dlc-" + std::to_string(static_cast<std::uint32_t>(steam_id)) + ".txt";
}

std::set<std::uint32_t> ParseFreeDlcMemory(std::string_view text) {
  std::set<std::uint32_t> refused;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const auto tab = line.find('\t');
    if (tab == std::string_view::npos || line.substr(tab + 1) != "refused") continue;
    std::uint32_t app = 0;
    const auto parsed = std::from_chars(line.data(), line.data() + tab, app);
    if (parsed.ec == std::errc{} && parsed.ptr == line.data() + tab && app != 0) refused.insert(app);
  }
  return refused;
}

std::string FormatFreeDlcMemory(const std::set<std::uint32_t>& refused) {
  std::string text;
  for (const auto app : refused) text += std::to_string(app) + "\trefused\n";
  return text;
}

std::set<std::uint32_t> LoadFreeDlcMemory(const Storage& storage, std::uint64_t steam_id) {
  const auto read = storage.Read(FreeDlcMemoryName(steam_id), 256 * 1024);
  if (!read.ok()) return {};
  const auto& bytes = read.value();
  return ParseFreeDlcMemory(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool SaveFreeDlcMemory(const Storage& storage, std::uint64_t steam_id, const std::set<std::uint32_t>& refused) {
  const auto text = FormatFreeDlcMemory(refused);
  return storage.WriteAtomicPrivate(FreeDlcMemoryName(steam_id), Bytes(text.begin(), text.end())).ok();
}

InstallRunner::Result SteamInstallRunner::InstallApp(const StoredSession& session, std::uint32_t id, const std::string& root,
                                                  const InstallOptions& options, const std::atomic<bool>& cancel,
                                                  const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                                                  const std::function<void(InstallStep)>& step) {
  return Run(session, id, root, options, cancel, progress, step, false, {});
}

InstallRunner::Result SteamInstallRunner::UpdateApp(const StoredSession& session, std::uint32_t id, const std::string& root,
                                                 const InstallOptions& options, const std::atomic<bool>& cancel,
                                                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                                                 const std::function<void(InstallStep)>& step,
                                                 const std::function<bool()>& may_apply) {
  return Run(session, id, root, options, cancel, progress, step, true, may_apply);
}

InstallRunner::Result SteamInstallRunner::Run(const StoredSession& session, std::uint32_t id, const std::string& root,
                                           const InstallOptions& options, const std::atomic<bool>& cancel,
                                           const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                                           const std::function<void(InstallStep)>& step, bool update,
                                           const std::function<bool()>& may_apply) {
  Result result;
  current_app_.store(id);
  server_name_.clear();
  installer_.SetTuning(tuning_ ? tuning_() : DownloadTuning{});
  const auto account = session.steam_id;
  installer_.SetFastestCache([this, account] { return fastest_.Get(account); },
                             [this, account](const std::string& host) { fastest_.Remember(account, host); });
  auto log = [this, id](const char* step, const char* category, const char* detail) {
    if (log_) log_(id, step, category, detail);
  };
  const auto info = cm_.FetchAppInfo(session, {id}, cancel);
  if (cancel.load()) {
    result.canceled = true;
    return result;
  }
  if (!info.ok()) {
    log("app_info", CmErrorName(info.error), "");
    result.failure = "Could not reach Steam. Resume to try again.";
    result.retry_later = info.error == CmError::kNetwork || info.error == CmError::kTimeout;
    return result;
  }
  const auto buffer = info.buffers.find(id);
  const auto app = buffer == info.buffers.end() ? std::nullopt : ParseAppInfo(id, buffer->second);
  if (!app) {
    log("app_info", "missing", buffer == info.buffers.end() ? "no buffer" : "unparsable");
    result.failure = "Steam has no install details for this game.";
    return result;
  }
  // The appinfo of the game's DLC, for the depots a DLC app lists itself. Steam leaves out the ones it
  // doesn't describe; a batch that fails only leaves its DLC's own depots out of an install. An update
  // that has such depots installed waits for Steam instead: without them it would delete them.
  std::vector<AppInfo> dlc_apps;
  std::set<std::uint32_t> unfetched;
  CmError dlc_error = CmError::kNone;
  {
    std::vector<std::uint32_t> dlc_ids;
    for (const auto dlc : app->dlc_app_ids)
      if (dlc != id && std::find(dlc_ids.begin(), dlc_ids.end(), dlc) == dlc_ids.end()) dlc_ids.push_back(dlc);
    for (std::size_t start = 0; start < dlc_ids.size(); start += kMaxAppInfoBatch) {
      const std::vector<std::uint32_t> batch(
          dlc_ids.begin() + static_cast<std::ptrdiff_t>(start),
          dlc_ids.begin() + static_cast<std::ptrdiff_t>(std::min(start + kMaxAppInfoBatch, dlc_ids.size())));
      const auto dlc_info = cm_.FetchAppInfo(session, batch, cancel);
      if (cancel.load()) {
        result.canceled = true;
        return result;
      }
      if (!dlc_info.ok()) {
        log("dlc_app_info", CmErrorName(dlc_info.error), ("apps=" + std::to_string(batch.size())).c_str());
        unfetched.insert(batch.begin(), batch.end());
        dlc_error = dlc_info.error;
        continue;
      }
      for (const auto dlc : batch) {
        const auto found = dlc_info.buffers.find(dlc);
        if (found == dlc_info.buffers.end()) continue;
        if (auto parsed = ParseAppInfo(dlc, found->second)) dlc_apps.push_back(std::move(*parsed));
      }
    }
  }
  if (update && !unfetched.empty())
    if (const auto record = installs::ReadInstallRecord(root + "/" + DepotInstaller::kInstallRecord);
        record && UpdateNeedsUnfetchedDlc(*record, id, unfetched)) {
      log("dlc_app_info", "update-blocked", "");
      result.failure = "Could not reach Steam. Resume to try again.";
      result.retry_later = dlc_error == CmError::kNetwork || dlc_error == CmError::kTimeout;
      return result;
    }
  InstallTarget target;
  target.app_id = id;
  target.install_root = root;
  // Like the reference, only depots this account's licenses grant are installed. Accounts with
  // thousands of licenses take a minute here (issue #47), so the player sees this step.
  if (step) step(InstallStep::kCheckingLicenses);
  // Free DLC with Windows content the account doesn't own yet: asked for once, before the licenses
  // are read, so a grant is part of this install. Which DLC the account owns comes from the library's
  // last license read; the licenses read below settle what Steam refused.
  std::vector<std::uint32_t> claimed;
  std::optional<FreeLicenseResult> claim;
  std::set<std::uint32_t> refused_before;
  if (!app->dlc_app_ids.empty()) {
    const auto grant = licensed_ ? licensed_->Get() : std::nullopt;
    if (free_dlc_memory_) refused_before = LoadFreeDlcMemory(*free_dlc_memory_, account);
    if (!grant) {
      log("dlc", "claim", "skipped=no-licenses");
    } else {
      claimed = FreeDlcCandidates(*app, dlc_apps, grant->apps, refused_before);
    }
  }
  if (!claimed.empty()) {
    log("dlc", "claim", ("requested=" + IdText(claimed)).c_str());
    claim = cm_.RequestFreeLicenses(session, claimed, kFreeDlcLicenseWaitMs, cancel);
    if (cancel.load()) {
      result.canceled = true;
      return result;
    }
    if (!claim->ok()) {
      // Not remembered: Steam may grant them another time.
      log("dlc", "claim", (std::string("error=") + CmErrorName(claim->error) + " eresult=" + std::to_string(claim->eresult)).c_str());
    } else {
      log("dlc", "claim", ("granted=" + IdText(claim->granted_apps) + " packages=" + IdText(claim->granted_packages)).c_str());
      if (!claim->granted_apps.empty() || !claim->granted_packages.empty())
        log("licences", claim->licenses_updated ? "waited" : "timeout", ("ms=" + std::to_string(claim->waited_ms)).c_str());
    }
  }
  const auto entitlements = cm_.FetchAcquisitionDates(session, cancel);
  if (cancel.load()) {
    result.canceled = true;
    return result;
  }
  if (!entitlements.ok()) {
    log("entitlements", CmErrorName(entitlements.error), "");
    result.failure = "Could not read this account's licenses. Resume to try again.";
    result.retry_later = entitlements.error == CmError::kNetwork || entitlements.error == CmError::kTimeout;
    return result;
  }
  if (entitlements.skipped_packages)
    log("entitlements", "partial", ("skipped_packages=" + std::to_string(entitlements.skipped_packages)).c_str());
  // A game a Steam family member shares: which borrowed packages carry it. steam-cm.log names each
  // package's owner (family step=license).
  const bool family_shared = entitlements.shared_apps.count(id) > 0;
  if (family_shared) {
    std::vector<std::string> packages;
    for (const auto& [package, apps] : entitlements.package_apps)
      if (std::find(apps.begin(), apps.end(), id) != apps.end()) packages.push_back(std::to_string(package));
    for (const auto& line : FamilyLines("packages=", packages)) log("family", "shared", line.c_str());
  }
  // A DLC Steam answered for without granting it, that the licenses read now don't hold either, is not
  // free for this account: it is not asked for again. A grant of packages without app IDs can't be
  // told apart, so nothing is remembered then.
  if (claim) {
    const auto refused = FreeDlcRefusals(claimed, *claim, entitlements.owned_apps);
    if (!refused.empty()) {
      log("dlc", "claim", ("refused=" + IdText(refused)).c_str());
      auto remembered = refused_before;
      remembered.insert(refused.begin(), refused.end());
      if (free_dlc_memory_ && !SaveFreeDlcMemory(*free_dlc_memory_, account, remembered))
        log("dlc", "memory", "write-failed");
    }
  }
  std::set<std::uint32_t> owned_apps = entitlements.owned_apps;
  owned_apps.insert(id);
  // The install sheet's choice, else the Settings download language.
  const std::string language =
      !options.language.empty() ? options.language : language_ ? language_() : std::string(kDefaultContentLanguage);
  DepotSelection selection;
  selection.language = language;
  if (!options.branch.empty()) selection.branch = options.branch;
  SelectionResult selected;
  const bool any = SelectDepots(*app, dlc_apps, owned_apps, entitlements.owned_depots, selection, &selected);
  // Every depot left out and why, as depot:reason.
  if (!selected.rejected.empty()) {
    std::vector<std::string> rejected;
    for (const auto& depot : selected.rejected) rejected.push_back(std::to_string(depot.id) + ":" + depot.reason);
    for (const auto& line : FamilyLines("", rejected)) log("depots", "rejected", line.c_str());
  }
  if (!any) {
    log("depots", "none", selected.reason.c_str());
    // Only DLC selected: when the licenses leave the game's content out, that is what the player is told.
    result.failure = !MissingBaseDepots(*app, entitlements.owned_depots, selected.depots).empty()
                         ? "Steam didn't grant access to part of this game."
                     : selected.depots.empty() ? "No Windows content to install."
                                               : "This version has no content of the game itself.";
    return result;
  }
  target.depots = std::move(selected.depots);
  // The language the game's depots resolve to, which the Steamworks emulator then reports.
  const std::string effective_language = selected.effective_language;
  target.branch = selection.branch;
  target.language = effective_language;
  if (const auto missing = MissingBaseDepots(*app, entitlements.owned_depots, target.depots); !missing.empty()) {
    std::string ids;
    for (const auto depot : missing) ids += (ids.empty() ? "" : " ") + std::to_string(depot);
    log("depots", "partial", ("unowned=" + ids).c_str());
    // Which licenses carry the game and what they grant, against the app's depots.
    std::string packages;
    for (const auto& [package, apps] : entitlements.package_apps) {
      if (std::find(apps.begin(), apps.end(), id) == apps.end()) continue;
      packages += " " + std::to_string(package) + ":";
      const auto depots = entitlements.package_depots.find(package);
      if (depots != entitlements.package_depots.end())
        for (const auto depot : depots->second) packages += std::to_string(depot) + ",";
    }
    log("depots", "licenses", packages.c_str());
    std::string listed;
    for (const auto& depot : app->depots)
      listed += " " + std::to_string(depot.id) + ":" + std::to_string(depot.size) + ":" + depot.os_list + ":" +
                (depot.depot_from_app ? "from" + std::to_string(*depot.depot_from_app) : "") +
                (depot.shared_install ? "shared" : "") + (depot.is_dlc ? "dlc" : "") + ":" +
                (entitlements.owned_depots.count(depot.id) ? "owned" : "-") + ":" + depot.name;
    log("depots", "listed", listed.c_str());
    result.failure = "Steam didn't grant access to part of this game.";
    return result;
  }
  // The selection keeps Steam's listing order: it is the depot mounting order.
  std::string selected_ids;
  for (const auto& depot : target.depots) selected_ids += (selected_ids.empty() ? "" : " ") + std::to_string(depot.id);
  log("depots", "selected", selected_ids.c_str());
  log("depots", "language", ("requested=" + language + " effective=" + effective_language).c_str());
  if (family_shared) {
    // Which packages grant each selected depot, as depot:package/package.
    std::vector<std::string> grants;
    for (const auto& depot : target.depots) {
      std::string grant = std::to_string(depot.id) + ":";
      std::size_t found = 0;
      for (const auto& [package, depots] : entitlements.package_depots)
        if (std::find(depots.begin(), depots.end(), depot.id) != depots.end())
          grant += (found++ ? "/" : "") + std::to_string(package);
      grants.push_back(found ? grant : grant + "none");
    }
    for (const auto& line : FamilyLines("depots=", grants)) log("family", "depots", line.c_str());
  }
  // What the Steamworks emulator needs later, while the licenses are at hand: installed depots and owned DLC.
  const auto write_facts = [&](const std::string& folder, const std::vector<std::uint32_t>& skipped) {
    std::vector<std::uint32_t> depots, dlc;
    for (const auto& depot : target.depots)
      if (std::find(skipped.begin(), skipped.end(), depot.id) == skipped.end()) depots.push_back(depot.id);
    // Owned DLC, claimed ones included, and any DLC whose content is installed.
    for (const auto app_id : app->dlc_app_ids)
      if (app_id != id && entitlements.owned_apps.count(app_id) && std::find(dlc.begin(), dlc.end(), app_id) == dlc.end())
        dlc.push_back(app_id);
    for (const auto& depot : target.depots)
      if (depot.owner_app != 0 && depot.owner_app != id &&
          std::find(dlc.begin(), dlc.end(), depot.owner_app) == dlc.end())
        dlc.push_back(depot.owner_app);
    const bool written = runtime::WriteSteamworksFacts(folder, depots, dlc, effective_language);
    if (!written) log("steamworks_facts", "write-failed", folder.c_str());
    return written;
  };
  // An update's facts go in place with its record.
  target.update = update;
  if (update) target.before_apply = write_facts, target.may_apply = may_apply;
  if (step) step(InstallStep::kDownloading);
  InstallPhase phase = InstallPhase::kDownloading;
  const auto outcome = installer_.Run(session, target, cancel, [&](const InstallProgress& state) {
    server_name_ = state.server_name;
    if (state.phase != phase) {
      phase = state.phase;
      if (step) step(phase == InstallPhase::kCheckingFiles ? InstallStep::kCheckingFiles
                     : phase == InstallPhase::kApplying  ? InstallStep::kApplying
                                                         : InstallStep::kDownloading);
    }
    if (phase == InstallPhase::kDownloading && progress) progress(state.download_done, state.download_total, state.held_bytes);
  });
  if (!outcome.skipped_depots.empty()) {
    std::string skipped;
    for (const auto id : outcome.skipped_depots) skipped += (skipped.empty() ? "" : " ") + std::to_string(id);
    log("depots", "skipped", ("borrowed-refused=" + skipped).c_str());
  }
  log("install", InstallErrorName(outcome.error), outcome.detail.c_str());
  result.ok = outcome.error == InstallError::kNone;
  if (result.ok && !update) write_facts(root, outcome.skipped_depots);
  result.canceled = outcome.error == InstallError::kCanceled;
  // A running game holds the update back: it waits, its downloads kept, and tries again.
  result.retry_later = outcome.error == InstallError::kNetwork || outcome.error == InstallError::kGameRunning;
  if (outcome.error == InstallError::kGameRunning) result.waiting = kWaitingForGame;
  result.failure = InstallFailureText(outcome.error);
  result.install_bytes = outcome.progress.install_total;
  return result;
}

}  // namespace playden::steam
