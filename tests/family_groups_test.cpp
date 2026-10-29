// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/family_groups.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace playden;
using namespace playden::steam;

namespace {

const char* kGroup = R"({"response":{"family_groupid":"9876543210","is_not_member_of_any_group":false,
  "latest_time_joined":1700000000,"role":1,"cooldown_seconds_remaining":0,"pending_group_invites":[],
  "family_group":{"name":"Synthetic Family","members":[
    {"steamid":"76561198000000001","role":1,"time_joined":1700000000,"cooldown_seconds_remaining":0},
    {"steamid":76561198000000002,"role":2,"time_joined":1700000001}],
    "free_spots":4,"country":"BR","slot_cooldown_remaining_seconds":0},
  "can_undelete_last_joined_family":false}})";

const char* kApps = R"({"response":{"apps":[
  {"appid":620,"owner_steamids":["76561198000000002"],"name":"Synthetic Shared","capsule_filename":"c.jpg",
   "img_icon_hash":"abc","exclude_reason":0,"rt_time_acquired":1600000000,"app_type":1,"content_descriptors":[]},
  {"appid":"730","owner_steamids":["76561198000000002","76561198000000003"],"exclude_reason":0},
  {"appid":440,"owner_steamids":["76561198000000003"],"exclude_reason":3},
  {"appid":"not a number"},
  {"appid":0},
  "unexpected"],
  "owner_steamid":"76561198000000001"}})";

HttpResponse Json(const std::string& body, long status = 200) {
  HttpResponse response;
  response.status_code = status;
  response.body.assign(body.begin(), body.end());
  return response;
}

StoredSession Session() { return {76561198000000001ULL, "synthetic", "refresh", "secret token/+", 7}; }

struct Fake {
  std::vector<HttpRequest> requests;
  std::vector<HttpResponse> responses;
  std::vector<std::string> lines;
  FamilyPerform Perform() {
    return [this](const HttpRequest& request, const std::atomic<bool>&) {
      requests.push_back(request);
      if (responses.empty()) return Json("", 500);
      auto response = responses.front();
      responses.erase(responses.begin());
      return response;
    };
  }
  FamilyLog Log() {
    return [this](const std::string& line) {
      std::printf("%s\n", line.c_str());
      lines.push_back(line);
    };
  }
  bool Has(const std::string& text) const {
    return std::any_of(lines.begin(), lines.end(), [&](const auto& line) { return line.find(text) != std::string::npos; });
  }
  bool Leaks() const {
    return std::any_of(lines.begin(), lines.end(), [](const auto& line) {
      return line.find("secret") != std::string::npos || line.find("access_token") != std::string::npos ||
             line.find("api.steampowered") != std::string::npos;
    });
  }
};

void ParsesAGroup() {
  const auto group = ParseFamilyGroup(kGroup);
  assert(group && group->family_groupid == 9876543210ULL && !group->not_member && group->role == 1u);
  assert(group->members.size() == 2 && group->members[0].steamid == 76561198000000001ULL &&
         group->members[1].steamid == 76561198000000002ULL && group->members[1].role == 2u);
}

void ParsesNoGroup() {
  auto group = ParseFamilyGroup(R"({"response":{}})");
  assert(group && !group->family_groupid && group->members.empty() && !group->role);
  group = ParseFamilyGroup(R"({"response":{"is_not_member_of_any_group":true,"family_groupid":"0"}})");
  assert(group && !group->family_groupid && group->not_member);
  assert(!ParseFamilyGroup(""));
  assert(!ParseFamilyGroup("<html>"));
  assert(!ParseFamilyGroup(R"({"response":[]})"));
  assert(!ParseFamilyGroup(R"({})"));
}

void ParsesSharedApps() {
  const auto library = ParseSharedLibraryApps(kApps);
  assert(library && library->apps.size() == 3);
  assert(library->apps[0].appid == 620 && library->apps[0].owners == std::vector<std::uint64_t>{76561198000000002ULL});
  assert(library->apps[1].appid == 730 && library->apps[1].owners.size() == 2 && !library->apps[1].exclude_reason);
  assert(library->apps[2].appid == 440 && library->apps[2].exclude_reason == 3);
  const auto empty = ParseSharedLibraryApps(R"({"response":{}})");
  assert(empty && empty->apps.empty());
  assert(!ParseSharedLibraryApps("not json"));
}

// The family lists 620 (licensed here through the family), 730 (no license arrived) and 440 (excluded).
void LogsTheGroupAndTheUnlicensedApps() {
  Fake fake;
  fake.responses = {Json(kGroup), Json(kApps)};
  AcquisitionResult licenses;
  licenses.error = CmError::kNone;
  licenses.owned_apps = {620, 10};
  licenses.shared_apps = {620};
  std::atomic<bool> cancel{false};
  RunFamilyDiagnostics(Session(), &licenses, fake.Perform(), fake.Log(), cancel);
  assert(fake.requests.size() == 2);
  assert(fake.requests[0].url.find("/IFamilyGroupsService/GetFamilyGroupForUser/v1/") != std::string::npos);
  assert(fake.requests[0].url.find("access_token=secret%20token%2F%2B") != std::string::npos);
  assert(fake.requests[0].url.find("steamid=76561198000000001") != std::string::npos);
  assert(fake.requests[1].url.find("/IFamilyGroupsService/GetSharedLibraryApps/v1/") != std::string::npos);
  assert(fake.requests[1].url.find("family_groupid=9876543210") != std::string::npos);
  assert(fake.requests[1].url.find("include_own=false") != std::string::npos);
  assert(fake.requests[1].url.find("include_excluded=true") != std::string::npos);
  assert(fake.requests[0].timeout_ms <= 10000 && fake.requests[1].timeout_ms <= 10000);
  assert(fake.Has("family webapi group=y family_groupid=9876543210 role=1 members=2 member=76561198000000001:1 "
                  "member=76561198000000002:2"));
  assert(fake.Has("family webapi shared_apps=3 excluded=1 owners=2 unlicensed=1 licenses=known license_shared_apps=1"));
  assert(fake.Has("family webapi app=620 owners=76561198000000002 exclude_reason=0 licensed=shared"));
  assert(fake.Has("family webapi app=730 owners=76561198000000002,76561198000000003 exclude_reason=0 licensed=n"));
  assert(fake.Has("family webapi app=440 owners=76561198000000003 exclude_reason=3 licensed=n"));
  assert(!fake.Leaks());
}

void LogsNoGroupWithoutAskingForApps() {
  for (const char* body : {R"({"response":{}})", R"({"response":{"is_not_member_of_any_group":true}})"}) {
    Fake fake;
    fake.responses = {Json(body)};
    std::atomic<bool> cancel{false};
    RunFamilyDiagnostics(Session(), nullptr, fake.Perform(), fake.Log(), cancel);
    assert(fake.requests.size() == 1 && fake.lines.size() == 1);
    assert(fake.Has("family webapi group=n not_member="));
  }
}

void FailuresAreLogLines() {
  {
    Fake fake;
    auto refused = Json("", 403);
    refused.headers = {{"X-eresult", "15"}};
    fake.responses = {refused};
    std::atomic<bool> cancel{false};
    RunFamilyDiagnostics(Session(), nullptr, fake.Perform(), fake.Log(), cancel);
    assert(fake.lines.size() == 1 && fake.Has("family webapi step=group http=403 eresult=15"));
  }
  {
    Fake fake;
    HttpResponse timeout;
    timeout.error = HttpError::kTimeout;
    fake.responses = {timeout};
    std::atomic<bool> cancel{false};
    RunFamilyDiagnostics(Session(), nullptr, fake.Perform(), fake.Log(), cancel);
    assert(fake.lines.size() == 1 && fake.Has("family webapi step=group error=timeout"));
  }
  {
    Fake fake;
    fake.responses = {Json(kGroup), Json("garbage")};
    std::atomic<bool> cancel{false};
    RunFamilyDiagnostics(Session(), nullptr, fake.Perform(), fake.Log(), cancel);
    assert(fake.Has("family webapi step=apps error=unreadable bytes=7") && !fake.Has("shared_apps="));
    assert(!fake.Leaks());
  }
  {
    Fake fake;
    std::atomic<bool> cancel{true};
    RunFamilyDiagnostics(Session(), nullptr, fake.Perform(), fake.Log(), cancel);
    assert(fake.requests.empty() && fake.lines.empty());
  }
}

void AppLinesAreCapped() {
  std::string apps = R"({"response":{"apps":[)";
  for (int i = 1; i <= 250; ++i) apps += (i > 1 ? "," : "") + std::string(R"({"appid":)") + std::to_string(i) + "}";
  apps += "]}}";
  Fake fake;
  fake.responses = {Json(kGroup), Json(apps)};
  std::atomic<bool> cancel{false};
  RunFamilyDiagnostics(Session(), nullptr, fake.Perform(), fake.Log(), cancel);
  const auto app_lines = std::count_if(fake.lines.begin(), fake.lines.end(),
                                       [](const auto& line) { return line.find("family webapi app=") == 0; });
  assert(app_lines == 200 && fake.Has("family webapi truncated=50"));
  assert(fake.Has("family webapi shared_apps=250 excluded=0 owners=0 unlicensed=? licenses=unknown"));
  assert(fake.Has("family webapi app=1 owners=- exclude_reason=0 licensed=?"));
}

}  // namespace

int main() {
  ParsesAGroup();
  ParsesNoGroup();
  ParsesSharedApps();
  LogsTheGroupAndTheUnlicensedApps();
  LogsNoGroupWithoutAskingForApps();
  FailuresAreLogLines();
  AppLinesAreCapped();
  std::puts("family_groups_test passed");
}
