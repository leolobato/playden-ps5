#include "steam/backend/entitled_library.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include "steam_id.h"

using namespace playden;
using namespace playden::steam;
using namespace std::chrono_literals;

namespace {

struct Owned final : LibraryBackend {
  LibraryResult result{LibraryError::kNone, {{SteamId(10), "Ten", {}, {}, {}}, {SteamId(20), "Twenty", {}, {}, {}}}, false};
  LibraryResult Fetch(const StoredSession&, const std::atomic<bool>&) override { return result; }
};

struct Acquisitions final : AcquisitionSource {
  AcquisitionResult result{CmError::kNone, {{10, 1500000000u}, {99, 1400000000u}}};
  bool block_until_stopped = false;
  std::atomic<bool> saw_stop{false};
  std::map<std::uint32_t, std::string> app_info;
  std::vector<std::vector<std::uint32_t>> app_info_requests;
  bool fail_app_info = false;
  AppInfoBuffers FetchAppInfo(const StoredSession&, const std::vector<std::uint32_t>& ids,
                              const std::atomic<bool>&) override {
    app_info_requests.push_back(ids);
    if (fail_app_info) return {CmError::kNetwork, {}};
    AppInfoBuffers result{CmError::kNone, {}};
    for (const auto id : ids)
      if (const auto found = app_info.find(id); found != app_info.end()) result.buffers[id] = found->second;
    return result;
  }
  AcquisitionResult FetchAcquisitionDates(const StoredSession&, const std::atomic<bool>& cancel) override {
    while (block_until_stopped) {
      if (cancel) {
        saw_stop = true;
        return {CmError::kCanceled, {}};
      }
      std::this_thread::sleep_for(1ms);
    }
    return result;
  }
};

StoredSession Session() { return StoredSession{123, "synthetic", "refresh", "access", {}}; }

void DatesMergeIntoOwnedGames() {
  Owned owned;
  Acquisitions acquisitions;
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 2);
  assert(result.games[0].added_at_unix_seconds == 1500000000u);
  // App 99 is licensed but not in the owned list; app 20 has no license date.
  assert(!result.games[1].added_at_unix_seconds);
}

void CmFailureIsNeverFatal() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result = {CmError::kRejected, {}};
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 2);
  assert(!result.games[0].added_at_unix_seconds && !result.games[1].added_at_unix_seconds);
  assert(backend.last_acquisition_error() == CmError::kRejected);
}


void OwnedFailureStopsTheCmSession() {
  Owned owned;
  owned.result = {LibraryError::kCredentialsRevoked, {}, false};
  Acquisitions acquisitions;
  acquisitions.block_until_stopped = true;
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  const auto start = std::chrono::steady_clock::now();
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kCredentialsRevoked && acquisitions.saw_stop);
  assert(std::chrono::steady_clock::now() - start < 1s);
}

void CancelReachesARunningCmSession() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.block_until_stopped = true;
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  std::thread canceler([&cancel] {
    std::this_thread::sleep_for(30ms);
    cancel = true;
  });
  const auto result = backend.Fetch(Session(), cancel);
  canceler.join();
  assert(acquisitions.saw_stop && result.error == LibraryError::kNone);
  assert(!result.games[0].added_at_unix_seconds);
}

std::string AppInfoText(std::uint32_t id, const std::string& name, const std::string& type) {
  return "\"appinfo\"\n{\n\t\"appid\"\t\"" + std::to_string(id) + "\"\n\t\"common\"\n\t{\n\t\t\"name\"\t\"" + name +
         "\"\n\t\t\"type\"\t\"" + type + "\"\n\t\t\"icon\"\t\"abc123\"\n\t}\n}\n";
}

// A free-to-play game added with "Add to library" and never played: GetOwnedGames leaves it out,
// its free license names it. Packages with a listed game, DLC and tools add nothing.
void LicensedFreeGamesAreAdded() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.acquired_at[2767030] = 1700000000u;
  acquisitions.result.package_apps = {{0, {480}},               // Steam's basic free package
                                      {100, {10, 11}},          // a bought game and its DLC
                                      {200, {2767030}},         // the free game
                                      {300, {30}},              // DLC bought on its own
                                      {400, {40}}};             // an app Steam doesn't describe
  acquisitions.app_info = {{2767030, AppInfoText(2767030, "Synthetic Rivals", "Game")},
                           {30, AppInfoText(30, "Synthetic DLC", "DLC")},
                           {480, AppInfoText(480, "Spacewar", "Game")}};
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 3);
  const auto& added = result.games.back();
  assert(added.id == SteamId(2767030) && added.title == "Synthetic Rivals" && added.added_at_unix_seconds == 1700000000u);
  assert(added.artwork_source == "https://media.steampowered.com/steamcommunity/public/images/apps/2767030/abc123.jpg");
  assert(acquisitions.app_info_requests.size() == 1);
  assert((acquisitions.app_info_requests[0] == std::vector<std::uint32_t>{30, 40, 2767030}));

  // A refresh asks only about apps it hasn't seen this run.
  acquisitions.result.package_apps[500] = {50};
  acquisitions.app_info[50] = AppInfoText(50, "Synthetic New", "game");
  result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 4 && std::any_of(result.games.begin(), result.games.end(),
                                                  [](const auto& game) { return game.title == "Synthetic New"; }));
  assert(acquisitions.app_info_requests.size() == 2 &&
         (acquisitions.app_info_requests[1] == std::vector<std::uint32_t>{50}));
  result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 4 && acquisitions.app_info_requests.size() == 2);
}

// A demo comes in a free package of its own, which GetOwnedGames leaves out. It is added and marked,
// so the Library can hide it; games stay unmarked. A cached lookup keeps the mark.
void LicensedDemosAreAddedAndMarked() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{600, {60}}, {700, {70}}};
  acquisitions.app_info = {{60, AppInfoText(60, "Synthetic Demo", "Demo")},
                           {70, AppInfoText(70, "Synthetic Free", "game")}};
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  for (int pass = 0; pass < 2; ++pass) {
    const auto result = backend.Fetch(Session(), cancel);
    assert(result.error == LibraryError::kNone && result.games.size() == 4);
    const auto demo = std::find_if(result.games.begin(), result.games.end(),
                                   [](const auto& game) { return game.id == SteamId(60); });
    assert(demo != result.games.end() && demo->title == "Synthetic Demo" && demo->demo);
    assert(std::none_of(result.games.begin(), result.games.end(),
                        [](const auto& game) { return game.demo && game.id != SteamId(60); }));
  }
  assert(acquisitions.app_info_requests.size() == 1);
}

// Declared before the backend in each test: the family probe thread may still log while the backend stops it.
struct Lines {
  std::mutex mutex;
  std::vector<std::string> lines;
  FamilyLog Log() {
    return [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mutex);
      std::printf("%s\n", line.c_str());
      lines.push_back(line);
    };
  }
  std::size_t Count(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(), [&](const auto& line) {
      return line.find(text) != std::string::npos;
    }));
  }
};

// Waits for a line written by the family probe thread.
bool WaitFor(Lines& lines, const std::string& text, std::size_t count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (lines.Count(text) < count) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

const GameSummary* Find(const LibraryResult& result, std::uint32_t app) {
  const auto found = std::find_if(result.games.begin(), result.games.end(),
                                  [&](const auto& game) { return game.id == SteamId(app); });
  return found == result.games.end() ? nullptr : &*found;
}

// The family probe is off in these: no requests are answered.
FamilyPerform NoFamilyService() {
  return [](const HttpRequest&, const std::atomic<bool>&) {
    HttpResponse response;
    response.error = HttpError::kConnectionFailed;
    return response;
  };
}

// Apps only a family member's licenses grant are added and marked; owned games never are.
void FamilySharedAppsAreMarked() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{100, {10, 11}},  // the account's own game and its DLC
                                      {800, {80}},      // a game borrowed from a family member
                                      {900, {20, 90}},  // a borrowed bundle with a game the account also owns
                                      {950, {95}}};     // a free game of the account's own
  acquisitions.result.shared_apps = {80, 90};
  acquisitions.app_info = {{11, AppInfoText(11, "Synthetic DLC", "DLC")},
                           {80, AppInfoText(80, "Synthetic Shared", "Game")},
                           {90, AppInfoText(90, "Synthetic Bundled", "game")},
                           {95, AppInfoText(95, "Synthetic Free", "game")}};
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(NoFamilyService(), lines.Log());
  std::atomic<bool> cancel{false};
  for (int pass = 0; pass < 2; ++pass) {
    const auto result = backend.Fetch(Session(), cancel);
    assert(result.error == LibraryError::kNone && result.games.size() == 5);
    assert(Find(result, 80) && Find(result, 80)->family_shared && Find(result, 80)->title == "Synthetic Shared");
    assert(Find(result, 90) && Find(result, 90)->family_shared);
    assert(Find(result, 95) && !Find(result, 95)->family_shared);
    assert(!Find(result, 10)->family_shared && !Find(result, 20)->family_shared && !Find(result, 11));
  }
  // The package with an owned game is passed over; the borrowed bundle only skips the owned game.
  assert((acquisitions.app_info_requests == std::vector<std::vector<std::uint32_t>>{{80, 90, 95}}));
  // Each shared app is logged once per run; the summary once per load.
  assert(lines.Count("family library added app=80 demo=0") == 1);
  assert(lines.Count("family library added app=90 demo=0") == 1);
  assert(lines.Count("family library shared=2 added=2 left_out=0 pending=0 in_owned_list=0") == 2);
}

// An app the account owns stays unmarked even when a family member's license also grants it.
void OwnedAndBorrowedIsNotShared() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{800, {20}}};
  acquisitions.result.shared_apps = {20};
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(NoFamilyService(), lines.Log());
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 2 && !Find(result, 20)->family_shared);
  assert(acquisitions.app_info_requests.empty());
  assert(lines.Count("family library shared=1 added=0 left_out=0 pending=0 in_owned_list=1") == 1);
}

// Shared apps PICS doesn't call a named game or demo stay out, with the reason logged.
void SharedAppsLeftOutAreLogged() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{810, {81}}, {820, {82}}, {830, {83}}, {840, {84}}};
  acquisitions.result.shared_apps = {81, 82, 83, 84};
  acquisitions.app_info = {{81, AppInfoText(81, "Synthetic DLC", "DLC")},
                           {83, AppInfoText(83, "app_83", "game")},
                           {84, "not app info"}};
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(NoFamilyService(), lines.Log());
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 2);
  assert(lines.Count("family library left_out app=81 reason=type=dlc") == 1);
  assert(lines.Count("family library left_out app=82 reason=no_app_info") == 1);
  assert(lines.Count("family library left_out app=83 reason=no_name") == 1);
  assert(lines.Count("family library left_out app=84 reason=app_info_unreadable") == 1);
  assert(lines.Count("family library shared=4 added=0 left_out=4 pending=0") == 1);
}

// When PICS fails the shared apps are logged as pending and asked again on the next load.
void SharedAppInfoFailureIsRetried() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{800, {80}}};
  acquisitions.result.shared_apps = {80};
  acquisitions.app_info = {{80, AppInfoText(80, "Synthetic Shared", "game")}};
  acquisitions.fail_app_info = true;
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(NoFamilyService(), lines.Log());
  std::atomic<bool> cancel{false};
  auto result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 2 && !Find(result, 80));
  assert(lines.Count("family library left_out app=80 reason=app_info_failed") == 1);
  assert(lines.Count("family library shared=1 added=0 left_out=0 pending=1") == 1);
  acquisitions.fail_app_info = false;
  result = backend.Fetch(Session(), cancel);
  assert(Find(result, 80) && Find(result, 80)->family_shared);
  assert(lines.Count("family library added app=80") == 1);
}

HttpResponse Json(const std::string& body, long status = 200) {
  HttpResponse response;
  response.status_code = status;
  response.body.assign(body.begin(), body.end());
  return response;
}

// Without licenses the family log says so, and the probe still reports without comparing.
void CmFailureIsLoggedForFamily() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result = {CmError::kRejected, {}};
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [](const HttpRequest& request, const std::atomic<bool>&) {
        if (request.url.find("GetFamilyGroupForUser") != std::string::npos)
          return Json(R"({"response":{"family_groupid":"42"}})");
        return Json(R"({"response":{"apps":[{"appid":80,"owner_steamids":["5"]}]}})");
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 2);
  assert(lines.Count("family library licenses=unavailable cm_error=rejected") == 1);
  assert(WaitFor(lines, "family webapi app=80 owners=5 exclude_reason=0 licensed=?"));
  assert(lines.Count("family webapi shared_apps=1 excluded=0 owners=1 unlicensed=? licenses=unknown") == 1);
  assert(lines.Count("family webapi app=80 owners=5 exclude_reason=0 licensed=?") == 1);
}

// The family service is asked once per run while the library loads; what it says only reaches the log.
void FamilyServiceIsAskedOncePerRun() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{800, {80}}};
  acquisitions.result.owned_apps = {10, 80};
  acquisitions.result.shared_apps = {80};
  acquisitions.app_info = {{80, AppInfoText(80, "Synthetic Shared", "game")}};
  Lines lines;
  std::atomic<int> requests{0};
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [&requests](const HttpRequest& request, const std::atomic<bool>&) {
        ++requests;
        if (request.url.find("GetFamilyGroupForUser") != std::string::npos)
          return Json(R"({"response":{"family_groupid":"42","role":2,"family_group":{"members":[{"steamid":"5","role":1}]}}})");
        return Json(R"({"response":{"apps":[{"appid":80,"owner_steamids":["5"]},{"appid":81,"owner_steamids":["5"]}]}})");
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 3);
  assert(WaitFor(lines, "family webapi app=81 "));
  result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 3 && requests == 2);
  assert(lines.Count("family webapi group=y family_groupid=42 role=2 members=1 member=5:1") == 1);
  assert(lines.Count("family webapi shared_apps=2 excluded=0 owners=1 unlicensed=1 licenses=known") == 1);
  assert(lines.Count("family webapi app=80 owners=5 exclude_reason=0 licensed=shared") == 1);
  assert(lines.Count("family webapi app=81 owners=5 exclude_reason=0 licensed=n") == 1);
  assert(lines.Count("access") == 0);
}

// A family compared with no licenses (the load that started the probe had none) is asked again by the next
// load, which compares it with that load's own licenses.
void FamilyWithoutLicensesIsAskedAgain() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result = {CmError::kNetwork, {}};
  Lines lines;
  std::atomic<int> requests{0};
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [&requests](const HttpRequest& request, const std::atomic<bool>&) {
        ++requests;
        if (request.url.find("GetFamilyGroupForUser") != std::string::npos)
          return Json(R"({"response":{"family_groupid":"42","role":2}})");
        return Json(R"({"response":{"apps":[{"appid":80,"owner_steamids":["5"]}]}})");
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  backend.Fetch(Session(), cancel);
  assert(WaitFor(lines, "licenses=unknown"));
  while (requests < 2) std::this_thread::sleep_for(1ms);
  std::this_thread::sleep_for(100ms);  // the probe thread finishes after its last line
  acquisitions.result = AcquisitionResult{CmError::kNone, {}};
  acquisitions.result.package_apps = {{800, {80}}};
  acquisitions.result.owned_apps = {80};
  acquisitions.result.shared_apps = {80};
  acquisitions.app_info = {{80, AppInfoText(80, "Synthetic Shared", "game")}};
  backend.Fetch(Session(), cancel);
  assert(WaitFor(lines, "family webapi app=80 owners=5 exclude_reason=0 licensed=shared"));
  assert(requests == 4 && lines.Count("licenses=known") == 1);
}

// A failing family service never fails a load and is tried again by a later one, one probe at a time.
void FamilyServiceNeverFailsTheLoad() {
  Owned owned;
  Acquisitions acquisitions;
  Lines lines;
  std::atomic<int> requests{0};
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [&](const HttpRequest&, const std::atomic<bool>&) {
        ++requests;
        HttpResponse response;
        response.error = HttpError::kTimeout;
        return response;
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 2);
  assert(WaitFor(lines, "family webapi step=group error=timeout"));
  // A failure is tried again by a later load, never twice by one.
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  int loads = 1;
  while (requests < 2 && std::chrono::steady_clock::now() < deadline) {
    result = backend.Fetch(Session(), cancel);
    ++loads;
    assert(result.error == LibraryError::kNone && requests <= loads);
  }
  assert(WaitFor(lines, "family webapi step=group error=timeout", 2));
}

// A stalled family service holds up neither a load (not even an uncanceled one) nor the backend's
// destruction, and a second probe never starts beside it.
void StalledFamilyServiceHoldsNothingUp() {
  Owned owned;
  Acquisitions acquisitions;
  Lines lines;
  std::atomic<int> requests{0};
  auto backend = std::make_unique<EntitledLibraryBackend>(owned, acquisitions);
  backend->SetFamilyDiagnostics(
      [&](const HttpRequest&, const std::atomic<bool>& stop) {
        ++requests;
        while (!stop) std::this_thread::sleep_for(1ms);
        HttpResponse response;
        response.error = HttpError::kCanceled;
        return response;
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  auto start = std::chrono::steady_clock::now();
  auto result = backend->Fetch(Session(), cancel);
  assert(std::chrono::steady_clock::now() - start < 1s);
  assert(result.error == LibraryError::kNone && result.games.size() == 2);
  while (requests == 0) std::this_thread::sleep_for(1ms);
  result = backend->Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && requests == 1);
  start = std::chrono::steady_clock::now();
  backend.reset();
  assert(std::chrono::steady_clock::now() - start < 1s);
  assert(requests == 1 && lines.Count("family webapi") == 0);
}

// A family service that answers after the load returned is still logged and compared with its licenses.
void LateFamilyAnswerIsLogged() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.owned_apps = {10, 80};
  acquisitions.result.shared_apps = {80};
  Lines lines;
  std::atomic<bool> release{false};
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [&release](const HttpRequest& request, const std::atomic<bool>& stop) {
        if (request.url.find("GetFamilyGroupForUser") != std::string::npos) {
          while (!release && !stop) std::this_thread::sleep_for(1ms);
          return Json(R"({"response":{"family_groupid":"42"}})");
        }
        return Json(R"({"response":{"apps":[{"appid":80,"owner_steamids":["5"]},{"appid":81,"owner_steamids":["5"]}]}})");
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && lines.Count("family webapi") == 0);
  release = true;
  // The last line the probe writes.
  assert(WaitFor(lines, "family webapi app=81 owners=5 exclude_reason=0 licensed=n"));
  assert(lines.Count("family webapi shared_apps=2 excluded=0 owners=1 unlicensed=1 licenses=known") == 1);
  assert(lines.Count("family webapi app=80 owners=5 exclude_reason=0 licensed=shared") == 1);
}

// Per-app family lines stay bounded per load, and a lookup that keeps failing is logged once per app.
void FamilyAppLinesAreCapped() {
  Owned owned;
  Acquisitions acquisitions;
  for (std::uint32_t app = 1000; app < 1300; ++app) {
    acquisitions.result.package_apps[app] = {app};
    acquisitions.result.shared_apps.insert(app);
    acquisitions.app_info[app] = AppInfoText(app, "Synthetic " + std::to_string(app), "game");
  }
  acquisitions.fail_app_info = true;
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(NoFamilyService(), lines.Log());
  std::atomic<bool> cancel{false};
  const auto load = [&] {
    const auto before = lines.Count("family library ");
    backend.Fetch(Session(), cancel);
    return lines.Count("family library ") - before;
  };
  // Each load writes at most 100 per-app lines, a truncated line and the summary; no app repeats.
  assert(load() == 102 && lines.Count("reason=app_info_failed") == 100);
  assert(lines.Count("family library truncated=200") == 1);
  assert(lines.Count("family library shared=300 added=0 left_out=0 pending=300") == 1);
  assert(load() == 102 && lines.Count("reason=app_info_failed") == 200 &&
         lines.Count("family library truncated=100") == 1);
  assert(load() == 101 && lines.Count("reason=app_info_failed") == 300);
  assert(load() == 1 && lines.Count("reason=app_info_failed") == 300);
  for (std::uint32_t app = 1000; app < 1300; ++app)
    assert(lines.Count("family library left_out app=" + std::to_string(app) + " reason=app_info_failed") == 1);
  assert(lines.Count("family library shared=300 added=0 left_out=0 pending=300") == 4);
  acquisitions.fail_app_info = false;
  assert(load() == 102 && lines.Count("family library added app=") == 100);
  assert(lines.Count("family library shared=300 added=300 left_out=0 pending=0") == 1);
}

}  // namespace

int main() {
  FamilySharedAppsAreMarked();
  OwnedAndBorrowedIsNotShared();
  SharedAppsLeftOutAreLogged();
  SharedAppInfoFailureIsRetried();
  FamilyServiceIsAskedOncePerRun();
  FamilyServiceNeverFailsTheLoad();
  FamilyWithoutLicensesIsAskedAgain();
  StalledFamilyServiceHoldsNothingUp();
  LateFamilyAnswerIsLogged();
  FamilyAppLinesAreCapped();
  CmFailureIsLoggedForFamily();
  LicensedFreeGamesAreAdded();
  LicensedDemosAreAddedAndMarked();
  DatesMergeIntoOwnedGames();
  CmFailureIsNeverFatal();
  OwnedFailureStopsTheCmSession();
  CancelReachesARunningCmSession();
}
