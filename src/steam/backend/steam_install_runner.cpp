#include "steam/backend/steam_install_runner.h"

#include "runtime/steamworks.h"
#include "steam/backend/app_info.h"

#include <set>
#include <sys/stat.h>
#include <charconv>

namespace playden::steam {

void LogInstallTrace(const InstallLogFn& log, std::uint32_t app, const char* marker) {
  if (!log) return;
  if (std::string(marker) == "journal-reset") log(app, "journal", "reset", "");
  else if (std::string(marker) == "journal-legacy") log(app, "journal", "legacy", "");
  else log(app, "trace", marker, "");
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

void SteamInstallRunner::DiscardUpdate(const std::string& root) {
  const bool ok = steam::DiscardUpdate(root);
  if (log_) log_(0, "update-discard", ok ? "done" : "kept", "");
}

std::optional<std::vector<std::uint32_t>> SteamInstallRunner::FindAppUpdates(
    const StoredSession& session, const std::map<std::uint32_t, std::string>& apps, const std::atomic<bool>& cancel) {
  // PICS answers for many apps at once; batches keep each answer small.
  constexpr std::size_t kBatch = 50;
  std::vector<std::uint32_t> ids;
  for (const auto& [app, root] : apps) ids.push_back(app);
  std::vector<std::uint32_t> updates;
  for (std::size_t start = 0; start < ids.size(); start += kBatch) {
    const std::vector<std::uint32_t> batch(ids.begin() + static_cast<std::ptrdiff_t>(start),
                                           ids.begin() + static_cast<std::ptrdiff_t>(std::min(ids.size(), start + kBatch)));
    const auto info = cm_.FetchAppInfo(session, batch, cancel);
    if (cancel.load() || !info.ok()) {
      if (log_ && !cancel.load()) log_(0, "update-check", CmErrorName(info.error), "");
      return std::nullopt;
    }
    for (const auto app_id : batch) {
      const auto buffer = info.buffers.find(app_id);
      const auto app = buffer == info.buffers.end() ? std::nullopt : ParseAppInfo(app_id, buffer->second);
      if (!app) continue;
      // A recorded depot whose public manifest moved on is an update; depots Steam no longer lists
      // for the public branch don't count.
      bool newer = false;
      for (const auto& [depot_id, manifest] : RecordedDepots(apps.at(app_id)))
        for (const auto& depot : app->depots)
          if (depot.id == depot_id && depot.manifest_gid && *depot.manifest_gid != 0 && *depot.manifest_gid != manifest)
            newer = true;
      if (newer) updates.push_back(app_id);
    }
  }
  if (log_) log_(0, "update-check", "done", ("apps=" + std::to_string(ids.size()) + " updates=" + std::to_string(updates.size())).c_str());
  return updates;
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
  }
  return "Install failed.";
}

InstallRunner::Result SteamInstallRunner::InstallApp(const StoredSession& session, std::uint32_t id, const std::string& root,
                                                  const std::atomic<bool>& cancel,
                                                  const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                                                  const std::function<void(InstallStep)>& step) {
  Result result;
  current_app_.store(id);
  installer_.SetTuning(tuning_ ? tuning_() : DownloadTuning{});
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
  InstallTarget target;
  target.app_id = id;
  target.install_root = root;
  // A complete install, or an update that stopped part way, is updated in place of a new install.
  struct stat folder {};
  target.update = stat((root + "/" + DepotInstaller::kInstallRecord).c_str(), &folder) == 0 ||
                  stat((root + "/" + DepotInstaller::kUpdateDir).c_str(), &folder) == 0;
  if (target.update) log("update", "start", "");
  std::string reason;
  // Like the reference, only depots this account's licenses grant are installed. Accounts with
  // thousands of licenses take a minute here (issue #47), so the player sees this step.
  if (step) step(InstallStep::kCheckingLicenses);
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
  std::set<std::uint32_t> owned_apps = entitlements.owned_apps;
  owned_apps.insert(id);
  if (!SelectDepots(*app, owned_apps, entitlements.owned_depots, &target.depots, &reason)) {
    log("depots", "none", reason.c_str());
    result.failure = "No Windows content to install.";
    return result;
  }
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
  // Keep Steam's listing order: it is the depot mounting order.
  std::vector<DepotInfo> ordered;
  for (const auto& depot : app->depots)
    for (const auto& selected : target.depots)
      if (selected.id == depot.id) ordered.push_back(selected);
  target.depots = ordered;
  std::string selected_ids;
  for (const auto& depot : target.depots) selected_ids += (selected_ids.empty() ? "" : " ") + std::to_string(depot.id);
  log("depots", "selected", selected_ids.c_str());
  if (step) step(InstallStep::kDownloading);
  bool checking = false;
  const auto outcome = installer_.Run(session, target, cancel, [&](const InstallProgress& state) {
    if (state.checking != checking) {
      checking = state.checking;
      if (step) step(checking ? InstallStep::kCheckingFiles : InstallStep::kDownloading);
    }
    if (!progress) return;
    if (state.checking) progress(state.check_done, state.check_total, 0);
    else progress(state.download_done, state.download_total, state.held_bytes);
  });
  if (!outcome.skipped_depots.empty()) {
    std::string skipped;
    for (const auto id : outcome.skipped_depots) skipped += (skipped.empty() ? "" : " ") + std::to_string(id);
    log("depots", "skipped", ("borrowed-refused=" + skipped).c_str());
  }
  log("install", InstallErrorName(outcome.error), outcome.detail.c_str());
  result.ok = outcome.error == InstallError::kNone;
  if (result.ok) {
    // What the Steamworks emulator needs later, while the licenses are at hand: installed depots and owned DLC.
    std::vector<std::uint32_t> depots, dlc;
    for (const auto& depot : target.depots)
      if (std::find(outcome.skipped_depots.begin(), outcome.skipped_depots.end(), depot.id) == outcome.skipped_depots.end())
        depots.push_back(depot.id);
    for (const auto app_id : app->dlc_app_ids)
      if (entitlements.owned_apps.count(app_id)) dlc.push_back(app_id);
    if (!runtime::WriteSteamworksFacts(root, depots, dlc)) log("steamworks_facts", "write-failed", root.c_str());
  }
  result.canceled = outcome.error == InstallError::kCanceled;
  result.retry_later = outcome.error == InstallError::kNetwork;
  result.failure = InstallFailureText(outcome.error);
  result.install_bytes = outcome.progress.install_total;
  return result;
}

}  // namespace playden::steam
