// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/family_groups.h"

#include "steam/backend/json.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <utility>

namespace playden::steam {
namespace {

constexpr std::size_t kBodyLimit = 8 * 1024 * 1024;
constexpr std::uint32_t kRequestTimeoutMs = 6000;
constexpr std::size_t kMemberLimit = 32;
constexpr std::size_t kAppLimit = 20000;
constexpr std::size_t kOwnerLimit = 32;
constexpr std::size_t kAppLineLimit = 200;

bool Unreserved(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
         byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

std::string PercentEncode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const unsigned char byte : value) {
    if (Unreserved(byte)) {
      encoded.push_back(static_cast<char>(byte));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[byte >> 4]);
      encoded.push_back(hex[byte & 0x0f]);
    }
  }
  return encoded;
}

// Steam's WebAPI sends 64-bit IDs as strings and small numbers as numbers; take either.
std::optional<std::uint64_t> Id(const json::Value* value) {
  if (!value) return std::nullopt;
  if (value->is_number()) return value->AsUint64();
  if (!value->is_string() || value->text().empty() || value->text().size() > 20) return std::nullopt;
  std::uint64_t parsed = 0;
  for (const unsigned char byte : value->text()) {
    if (byte < '0' || byte > '9') return std::nullopt;
    const std::uint64_t digit = byte - '0';
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
    parsed = parsed * 10 + digit;
  }
  return parsed;
}

const json::Value* Payload(std::string_view body, std::optional<json::Value>* root) {
  json::Limits limits;
  limits.max_bytes = kBodyLimit;
  limits.max_depth = 16;
  limits.max_values = 400000;
  limits.max_string_bytes = 65536;
  *root = json::Parse(body, limits);
  if (!*root || !(*root)->is_object()) return nullptr;
  const auto* payload = (*root)->Find("response");
  return payload && payload->is_object() ? payload : nullptr;
}

std::string_view Body(const HttpResponse& response) {
  if (response.body.empty()) return {};
  return {reinterpret_cast<const char*>(response.body.data()), response.body.size()};
}

// One request: the response on success, else a logged failure and nothing.
std::optional<HttpResponse> Get(const std::string& url, const char* step, const FamilyPerform& perform,
                                const FamilyLog& log, const std::atomic<bool>& cancel) {
  HttpRequest request;
  request.method = HttpMethod::kGet;
  request.url = url;
  request.headers = {{"User-Agent", "Playden/1.0"}};
  request.timeout_ms = kRequestTimeoutMs;
  request.max_response_bytes = kBodyLimit;
  auto response = perform(request, cancel);
  if (cancel.load()) return std::nullopt;
  if (!response.ok()) {
    log(std::string("family webapi step=") + step + " error=" + HttpFailureName(response));
    return std::nullopt;
  }
  if (response.status_code != 200) {
    std::string line = std::string("family webapi step=") + step + " http=" + std::to_string(response.status_code);
    for (const auto& [name, value] : response.headers) {
      std::string lower = name;
      for (auto& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      if (lower == "x-eresult" && value.size() <= 8) line += " eresult=" + value;
    }
    log(line);
    return std::nullopt;
  }
  return response;
}

}  // namespace

std::optional<FamilyGroup> ParseFamilyGroup(std::string_view body) {
  std::optional<json::Value> root;
  const auto* payload = Payload(body, &root);
  if (!payload) return std::nullopt;
  FamilyGroup group;
  group.family_groupid = Id(payload->Find("family_groupid")).value_or(0);
  if (const auto* flag = payload->Find("is_not_member_of_any_group"); flag && flag->is_bool())
    group.not_member = flag->boolean();
  group.role = Id(payload->Find("role"));
  const auto* family = payload->Find("family_group");
  const auto* members = family && family->is_object() ? family->Find("members") : nullptr;
  if (members && members->is_array()) {
    for (const auto& item : members->array()) {
      if (group.members.size() >= kMemberLimit) break;
      if (!item.is_object()) continue;
      FamilyMember member;
      member.steamid = Id(item.Find("steamid")).value_or(0);
      member.role = Id(item.Find("role"));
      group.members.push_back(member);
    }
  }
  return group;
}

std::optional<SharedLibrary> ParseSharedLibraryApps(std::string_view body) {
  std::optional<json::Value> root;
  const auto* payload = Payload(body, &root);
  if (!payload) return std::nullopt;
  SharedLibrary library;
  const auto* apps = payload->Find("apps");
  if (!apps || !apps->is_array()) return library;
  library.apps.reserve(std::min(apps->array().size(), kAppLimit));
  for (const auto& item : apps->array()) {
    if (library.apps.size() >= kAppLimit) break;
    if (!item.is_object()) continue;
    const auto appid = Id(item.Find("appid"));
    if (!appid || !*appid || *appid > std::numeric_limits<std::uint32_t>::max()) continue;
    SharedLibraryApp app;
    app.appid = static_cast<std::uint32_t>(*appid);
    if (const auto* owners = item.Find("owner_steamids"); owners && owners->is_array()) {
      for (const auto& owner : owners->array()) {
        if (app.owners.size() >= kOwnerLimit) break;
        if (const auto id = Id(&owner)) app.owners.push_back(*id);
      }
    }
    app.exclude_reason = Id(item.Find("exclude_reason")).value_or(0);
    library.apps.push_back(std::move(app));
  }
  return library;
}

FamilyProbe ProbeFamily(const StoredSession& session, const FamilyPerform& perform, const FamilyLog& log,
                        const std::atomic<bool>& cancel) {
  FamilyProbe probe;
  if (cancel.load()) {
    probe.outcome = FamilyProbe::Outcome::kCanceled;
    return probe;
  }
  if (!session.steam_id || session.access_token.empty()) {
    log("family webapi step=group error=no_session");
    return probe;
  }
  const std::string token = PercentEncode(session.access_token);
  const std::string steamid = std::to_string(session.steam_id);
  const auto group_response =
      Get("https://api.steampowered.com/IFamilyGroupsService/GetFamilyGroupForUser/v1/?access_token=" + token +
              "&steamid=" + steamid + "&include_family_group_response=true",
          "group", perform, log, cancel);
  if (cancel.load()) {
    probe.outcome = FamilyProbe::Outcome::kCanceled;
    return probe;
  }
  if (!group_response) return probe;
  const auto group = ParseFamilyGroup(Body(*group_response));
  if (!group) {
    log("family webapi step=group error=unreadable bytes=" + std::to_string(group_response->body.size()));
    return probe;
  }
  probe.group = *group;
  if (!group->family_groupid) {
    probe.outcome = FamilyProbe::Outcome::kNoGroup;
    log(std::string("family webapi group=n not_member=") + (group->not_member ? "1" : "0"));
    return probe;
  }
  std::string line = "family webapi group=y family_groupid=" + std::to_string(group->family_groupid) +
                     " role=" + (group->role ? std::to_string(*group->role) : std::string("?")) +
                     " members=" + std::to_string(group->members.size());
  for (const auto& member : group->members)
    line += " member=" + std::to_string(member.steamid) + ":" +
            (member.role ? std::to_string(*member.role) : std::string("?"));
  log(line);
  probe.outcome = FamilyProbe::Outcome::kGroup;

  const auto apps_response =
      Get("https://api.steampowered.com/IFamilyGroupsService/GetSharedLibraryApps/v1/?access_token=" + token +
              "&family_groupid=" + std::to_string(group->family_groupid) + "&steamid=" + steamid +
              "&include_own=false&include_excluded=true&include_free=false&include_non_games=false",
          "apps", perform, log, cancel);
  if (cancel.load()) {
    probe.outcome = FamilyProbe::Outcome::kCanceled;
    return probe;
  }
  // Without the app list the probe failed, and is tried again.
  if (!apps_response) {
    probe.outcome = FamilyProbe::Outcome::kFailed;
    return probe;
  }
  probe.library = ParseSharedLibraryApps(Body(*apps_response));
  if (!probe.library) {
    probe.outcome = FamilyProbe::Outcome::kFailed;
    log("family webapi step=apps error=unreadable bytes=" + std::to_string(apps_response->body.size()));
  }
  return probe;
}

void LogFamilySharedApps(const FamilyProbe& probe, const AcquisitionResult* acquisitions, std::uint64_t elapsed_ms,
                         const FamilyLog& log) {
  if (probe.outcome != FamilyProbe::Outcome::kGroup || !probe.library) return;
  const bool known = acquisitions && acquisitions->ok();
  std::size_t excluded = 0;
  std::size_t unlicensed = 0;
  std::set<std::uint64_t> owners;
  for (const auto& app : probe.library->apps) {
    owners.insert(app.owners.begin(), app.owners.end());
    if (app.exclude_reason) {
      ++excluded;
    } else if (known && !acquisitions->shared_apps.count(app.appid) && !acquisitions->owned_apps.count(app.appid)) {
      ++unlicensed;
    }
  }
  log("family webapi shared_apps=" + std::to_string(probe.library->apps.size()) +
      " excluded=" + std::to_string(excluded) + " owners=" + std::to_string(owners.size()) +
      " unlicensed=" + (known ? std::to_string(unlicensed) : std::string("?")) +
      " licenses=" + (known ? "known" : "unknown") +
      (known ? " license_shared_apps=" + std::to_string(acquisitions->shared_apps.size()) : std::string()) +
      " elapsed_ms=" + std::to_string(elapsed_ms));
  std::size_t written = 0;
  for (const auto& app : probe.library->apps) {
    if (written == kAppLineLimit) {
      log("family webapi truncated=" + std::to_string(probe.library->apps.size() - written));
      break;
    }
    std::string line = "family webapi app=" + std::to_string(app.appid) + " owners=";
    for (std::size_t i = 0; i < app.owners.size(); ++i) line += (i ? "," : "") + std::to_string(app.owners[i]);
    if (app.owners.empty()) line += "-";
    line += " exclude_reason=" + std::to_string(app.exclude_reason) + " licensed=";
    if (!known) line += "?";
    else if (acquisitions->shared_apps.count(app.appid)) line += "shared";
    else if (acquisitions->owned_apps.count(app.appid)) line += "own";
    else line += "n";
    log(line);
    ++written;
  }
}

void RunFamilyDiagnostics(const StoredSession& session, const AcquisitionResult* acquisitions,
                          const FamilyPerform& perform, const FamilyLog& log, const std::atomic<bool>& cancel) {
  const auto start = std::chrono::steady_clock::now();
  const auto probe = ProbeFamily(session, perform, log, cancel);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  LogFamilySharedApps(probe, acquisitions, static_cast<std::uint64_t>(elapsed), log);
}

}  // namespace playden::steam
