#include "steam/app_details_service.h"
#include "steam/backend/cm_app_details.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include "steam_id.h"

using namespace playden;
using namespace playden::steam;
using namespace std::chrono_literals;

namespace {

class Sessions final : public SessionRepository {
 public:
  Result<std::optional<StoredSession>> Load() const override {
    return Result<std::optional<StoredSession>>::Success(StoredSession{123, "synthetic", "refresh", "access", {}});
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};

class Source final : public AppDetailsSource {
 public:
  std::mutex mutex;
  std::vector<std::vector<GameId>> calls;
  std::vector<SizeRequest> measures;
  std::atomic<bool> fail{false};
  std::atomic<bool> hold{false};
  std::atomic<bool> block_fetch{false};  // Fetch waits until released  // a size check that waits (on manifests) until released or canceled
  MeasuredSize Measure(const StoredSession&, const SizeRequest& request, const std::atomic<bool>& cancel) override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      measures.push_back(request);
    }
    while (hold && !cancel) std::this_thread::sleep_for(1ms);
    MeasuredSize result{request};
    if (fail) {
      result.failed = true;
      return result;
    }
    result.measured = true;
    result.install_bytes = *request.id.steam_app() * 7ull;
    return result;
  }
  std::vector<AppDetails> Fetch(const StoredSession&, const std::vector<GameId>& ids,
                                const std::atomic<bool>&) override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      calls.push_back(ids);
    }
    while (block_fetch) std::this_thread::sleep_for(1ms);
    if (fail) return {};
    std::vector<AppDetails> result;
    for (const auto& id : ids) result.push_back({id, 0, true, *id.steam_app() * 10ull, *id.steam_app() * 100ull, true, {}});
    return result;
  }
};

std::vector<AppDetails> Await(AppDetailsService& service, std::size_t count) {
  std::vector<AppDetails> all;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (all.size() < count && std::chrono::steady_clock::now() < deadline) {
    for (auto& details : service.Pump()) all.push_back(std::move(details));
    std::this_thread::sleep_for(1ms);
  }
  assert(all.size() >= count);
  return all;
}

void RequestsAreDeduplicatedPerGeneration() {
  Sessions sessions;
  Source source;
  AppDetailsService service(source, sessions);
  service.Request(SteamId(7), 1);
  service.Request(SteamId(7), 1);
  const auto first = Await(service, 1);
  assert(first[0].id == SteamId(7) && first[0].available && first[0].install_bytes == 700u && first[0].account_generation == 1);
  service.Request(SteamId(7), 1);
  std::this_thread::sleep_for(20ms);
  assert(service.Pump().empty());
  std::lock_guard<std::mutex> lock(source.mutex);
  assert(source.calls.size() == 1);
}

void NewGenerationDropsResultsAndRefetches() {
  Sessions sessions;
  Source source;
  AppDetailsService service(source, sessions);
  service.Request(SteamId(7), 1);
  Await(service, 1);
  service.Invalidate(2);
  service.Request(SteamId(7), 2);
  const auto again = Await(service, 1);
  assert(again[0].account_generation == 2);
}

// A new download language keeps the account: invalidating the same generation asks Steam again.
void SameGenerationInvalidateRefetches() {
  Sessions sessions;
  Source source;
  AppDetailsService service(source, sessions);
  service.Request(SteamId(7), 1);
  Await(service, 1);
  service.Invalidate(1);
  service.Request(SteamId(7), 1);
  const auto again = Await(service, 1);
  assert(again[0].id == SteamId(7) && again[0].account_generation == 1);
  std::lock_guard<std::mutex> lock(source.mutex);
  assert(source.calls.size() == 2);
}

// An installed game whose update finished is asked for again, alone: other answers stay cached.
void RefreshAsksAgainForOneGame() {
  Sessions sessions;
  Source source;
  AppDetailsService service(source, sessions);
  service.Request(SteamId(7), 1);
  service.Request(SteamId(8), 1);
  Await(service, 2);
  service.Refresh(SteamId(7), 1);
  const auto again = Await(service, 1);
  assert(again.size() == 1 && again[0].id == SteamId(7));
  service.Request(SteamId(8), 1);
  std::this_thread::sleep_for(20ms);
  assert(service.Pump().empty());
  // Another generation's refresh is an ordinary request of that generation.
  service.Refresh(SteamId(7), 2);
  assert(Await(service, 1)[0].account_generation == 2);
}

void FailuresAreRetryable() {
  Sessions sessions;
  Source source;
  source.fail = true;
  AppDetailsService service(source, sessions);
  service.Request(SteamId(9), 1);
  const auto failed = Await(service, 1);
  assert(!failed[0].available && !failed[0].reason.empty());
  source.fail = false;
  service.Request(SteamId(9), 1);
  const auto recovered = Await(service, 1);
  assert(recovered[0].available && recovered[0].download_bytes == 90u);
}

std::vector<MeasuredSize> AwaitMeasured(AppDetailsService& service, std::size_t count) {
  std::vector<MeasuredSize> all;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (all.size() < count && std::chrono::steady_clock::now() < deadline) {
    for (auto& size : service.PumpMeasured()) all.push_back(std::move(size));
    std::this_thread::sleep_for(1ms);
  }
  assert(all.size() >= count);
  return all;
}

// An install choice's exact size is asked once while it is queued or checked; answered, it can be asked again (the
// sheet decides when), and so can one that failed.
void SizeChecksAreAnsweredOncePerChoice() {
  Sessions sessions;
  Source source;
  AppDetailsService service(source, sessions);
  service.Measure({SteamId(400), "public", "english"}, 1);
  service.Measure({SteamId(400), "public", "english"}, 1);
  const auto first = AwaitMeasured(service, 1);
  assert(first.size() == 1 && first[0].measured && first[0].install_bytes == 2800u && first[0].account_generation == 1);
  assert(first[0].request == (SizeRequest{SteamId(400), "public", "english"}));
  {
    std::lock_guard<std::mutex> lock(source.mutex);
    assert(source.measures.size() == 1);
  }
  service.Measure({SteamId(400), "beta", "english"}, 1);
  const auto second = AwaitMeasured(service, 1);
  assert(second.size() == 1 && second[0].request.branch == "beta");
  service.Measure({SteamId(400), "public", "english"}, 1);
  assert(AwaitMeasured(service, 1)[0].request.branch == "public");
  source.fail = true;
  service.Measure({SteamId(401), "public", "english"}, 1);
  assert(AwaitMeasured(service, 1)[0].failed);
  source.fail = false;
  service.Measure({SteamId(401), "public", "english"}, 1);
  const auto retried = AwaitMeasured(service, 1);
  assert(retried[0].measured && retried[0].install_bytes == 2807u);
  // Another account asks again.
  service.Measure({SteamId(400), "public", "english"}, 2);
  assert(AwaitMeasured(service, 1)[0].account_generation == 2);
}

// The game the player opens goes ahead of installed games checked for updates, queued or not.
void OpenedGameGoesFirst() {
  Sessions sessions;
  Source source;
  source.hold = false;
  AppDetailsService service(source, sessions);
  {
    std::lock_guard<std::mutex> lock(source.mutex);
    source.block_fetch = true;
  }
  for (std::uint32_t app = 100; app < 140; ++app) service.Request(SteamId(app), 1);  // the first batch blocks the worker
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(source.mutex);
      if (!source.calls.empty()) break;
    }
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
  service.Request(SteamId(620), 1, true);  // not queued yet
  service.Request(SteamId(135), 1, true);  // queued at the back
  source.block_fetch = false;
  Await(service, 40);
  std::lock_guard<std::mutex> lock(source.mutex);
  assert(source.calls.size() >= 2 && source.calls[1].size() >= 2);
  assert(source.calls[1][0] == SteamId(135) && source.calls[1][1] == SteamId(620));
}

// A size check waiting on manifests doesn't hold up a game's details: they have their own worker.
void SlowSizeChecksDontBlockDetails() {
  Sessions sessions;
  Source source;
  source.hold = true;
  AppDetailsService service(source, sessions);
  service.Measure({SteamId(400), "public", "english"}, 1);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(source.mutex);
      if (!source.measures.empty()) break;
    }
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
  service.Request(SteamId(2005010), 1);
  const auto details = Await(service, 1);
  assert(details[0].id == SteamId(2005010) && details[0].available);
  assert(service.PumpMeasured().empty());
  source.hold = false;
  assert(AwaitMeasured(service, 1)[0].measured);
}

// Only a selection whose merged size can be far from Steam's sum is worth its manifests: two depots or more of
// kMeasureDepotBytes. Portal's 401 and 409 hold mostly the same files.
void LargeOverlappingDepotsAreMeasured() {
  const std::string portal = R"("appinfo"
{
  "appid" "400"
  "common" { "name" "Portal" "type" "Game" }
  "config" { "launch" { "0" { "executable" "portal.exe" "config" { "oslist" "windows" } } } }
  "depots"
  {
    "401" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "1" "size" "4184288203" "download" "2520000000" } } }
    "402" { "config" { "oslist" "windows" "language" "russian" } "manifests" { "public" { "gid" "2" "size" "160000000" "download" "90000000" } } }
    "409" { "manifests" { "public" { "gid" "9" "size" "3850000000" "download" "2360000000" } } }
  }
})";
  const auto english = DepotsToMeasure(400, portal, std::nullopt, "public", "english");
  assert(english.size() == 2 && english[0].id == 401 && english[1].id == 409 && english[1].manifest_gid == 9u);
  const auto russian = DepotsToMeasure(400, portal, std::nullopt, "", "russian");
  assert(russian.size() == 3);
  const std::string small = R"("appinfo"
{
  "appid" "440"
  "common" { "name" "Synthetic" "type" "Game" }
  "depots"
  {
    "441" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "1" "size" "4000000000" } } }
    "442" { "config" { "oslist" "windows" "language" "german" } "manifests" { "public" { "gid" "2" "size" "30000000" } } }
  }
})";
  assert(DepotsToMeasure(440, small, std::nullopt, "public", "german").empty());
  assert(DepotsToMeasure(440, "not vdf {", std::nullopt, "public", "english").empty());
}

void AppInfoBecomesWindowsEstimate() {
  const std::string buffer = R"("appinfo"
{
  "appid" "440"
  "common" { "name" "Synthetic" "type" "game" }
  "config"
  {
    "installdir" "Synthetic"
    "launch"
    {
      "0" { "executable" "game.exe" "type" "default" "config" { "oslist" "windows" } }
      "1" { "executable" "bin\\safe.exe" "arguments" "-safe" "description" "Safe mode" "config" { "oslist" "windows" } }
      "2" { "executable" "game.app" "config" { "oslist" "macos" } }
    }
  }
  "depots"
  {
    "441" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "11" "size" "3000" "download" "1200" } } }
    "442" { "manifests" { "public" { "gid" "12" "size" "500" "download" "200" } } }
    "443" { "config" { "oslist" "macos" } "manifests" { "public" { "gid" "13" "size" "999" "download" "999" } } }
  }
})";
  const auto details = DetailsFromAppInfo(440, buffer + std::string(2, '\0'));
  assert(details.available && details.windows_installable);
  assert(details.install_bytes == 3500u && details.download_bytes == 1400u);
  // The Windows launch options, default first, are kept for preparing the game.
  assert(details.launches.size() == 2 && details.launches[0].executable == "game.exe");
  assert(details.launches[1].executable == "bin\\safe.exe" && details.launches[1].arguments == "-safe" &&
         details.launches[1].description == "Safe mode");
  const auto unreadable = DetailsFromAppInfo(440, "not vdf {");
  assert(!unreadable.available && !unreadable.reason.empty());
  const std::string mac_only = R"("appinfo" { "depots" { "1" { "config" { "oslist" "macos" } "manifests" { "public" { "gid" "1" } } } } })";
  const auto none = DetailsFromAppInfo(440, mac_only);
  assert(none.available && !none.windows_installable && !none.install_bytes);
}

// Half-Life 2's appinfo since its 20th anniversary update (trimmed): the public branch's Windows launch option is tagged
// betakey "default", and the other Windows options belong to the steam_legacy and test branches (issue #10).
void DefaultBetaKeyIsThePublicBranch() {
  const std::string buffer = R"("appinfo"
{
  "appid" "220"
  "common" { "name" "Half-Life 2" "type" "Game" }
  "config"
  {
    "installdir" "Half-Life 2"
    "launch"
    {
      "0" { "executable" "hl2.exe" "arguments" "-steam -game hl2_complete" "workingdir" "bin" "config" { "betakey" "default" "oslist" "windows" } }
      "1" { "executable" "hl2.sh" "arguments" "-game hl2 -steam" "config" { "oslist" "macos" } }
      "2" { "executable" "hl2.sh" "arguments" "-steam -game hl2_complete" "config" { "oslist" "linux" } }
      "3" { "executable" "hl2.exe" "arguments" "-steam -game hl2" "workingdir" "bin" "config" { "betakey" "steam_legacy" "oslist" "windows" } }
      "4" { "executable" "hl2.sh" "arguments" "-steam -game hl2" "config" { "betakey" "steam_legacy" "oslist" "linux" } }
      "5" { "executable" "hl2.exe" "arguments" "-steam -game hl2_complete" "workingdir" "bin" "config" { "betakey" "test" "oslist" "windows" } }
      "6" { "executable" "hl2.sh" "arguments" "-steam -game hl2_complete" "config" { "betakey" "test" "oslist" "linux" } }
    }
  }
  "depots"
  {
    "221" { "manifests" { "public" { "download" "2751430624" "gid" "4622516436473900521" "size" "5437140737" } } "systemdefined" "1" }
    "222" { "config" { "oslist" "windows" } "manifests" { "public" { "download" "205502384" "gid" "6432688940779947022" "size" "333424562" } } "systemdefined" "1" }
    "389" { "depotfromapp" "380" "sharedinstall" "2" }
  }
})";
  const auto details = DetailsFromAppInfo(220, buffer);
  assert(details.available && details.windows_installable);
  assert(details.launches.size() == 1 && details.launches[0].executable == "hl2.exe" &&
         details.launches[0].arguments == "-steam -game hl2_complete");
}

// FINAL FANTASY XIII's appinfo (trimmed) reports each depot's download as about 1,700 times its installed size, which
// showed 160 TB on Install (issue #5). A compressed download is never larger than what it installs.
void ImpossibleDownloadSizeFallsBackToInstallSize() {
  const std::string buffer = R"("appinfo"
{
  "appid" "292120"
  "common" { "name" "FINAL FANTASY XIII" "type" "Game" }
  "config" { "launch" { "0" { "executable" "FFXiiiLauncher.exe" "config" { "oslist" "windows" } } } }
  "depots"
  {
    "228990" { "config" { "oslist" "windows" } "depotfromapp" "228980" "sharedinstall" "1" }
    "292121" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "1" "size" "30738676601" "download" "56430184027278" } } }
    "292122" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "2" "size" "61851360699" "download" "103960316066861" } } }
    "292123" { "manifests" { "public" { "gid" "3" "size" "1000" "download" "400" } } }
  }
})";
  const auto details = DetailsFromAppInfo(292120, buffer);
  assert(details.available && details.windows_installable);
  assert(details.install_bytes == 30738676601ull + 61851360699ull + 1000u);
  assert(details.download_bytes == 30738676601ull + 61851360699ull + 400u);
}

// Marvel Rivals lists two copies of its content and a third content depot; the free license grants
// one copy. The page showed 168.5 GB, the sum of all three, for a 74 GB install.
void SizesCountOnlyLicensedDepots() {
  const std::string buffer = R"("appinfo"
{
  "appid" "2767030"
  "common" { "name" "Synthetic Rivals" "type" "Game" }
  "config" { "launch" { "0" { "executable" "Rivals.exe" "config" { "oslist" "windows" } } } }
  "depots"
  {
    "2767031" { "manifests" { "public" { "gid" "1" "size" "74083193346" "download" "67000000000" } } }
    "2767032" { "manifests" { "public" { "gid" "2" "size" "74083193346" "download" "67000000000" } } }
    "2767033" { "manifests" { "public" { "gid" "3" "size" "20311069758" "download" "18000000000" } } }
  }
})";
  const auto unlicensed = DetailsFromAppInfo(2767030, buffer);
  assert(unlicensed.install_bytes == 2 * 74083193346ull + 20311069758ull);
  LicensedDepots::Grant grant{{2767030}, {2767031}};
  const auto licensed = DetailsFromAppInfo(2767030, buffer, grant);
  assert(licensed.available && licensed.windows_installable);
  assert(licensed.install_bytes == 74083193346ull && licensed.download_bytes == 67000000000ull);
  // Licenses that grant none of it (not loaded for this game yet) keep the full estimate.
  const auto none = DetailsFromAppInfo(2767030, buffer, LicensedDepots::Grant{{}, {}});
  assert(none.install_bytes == unlicensed.install_bytes);
}

}  // namespace

// Options only for other branches (RuneScape: Dragonwilds and GUILTY GEAR -STRIVE-, trimmed): a betakey list naming
// "public" is the public branch's; with none, every branch's Windows options are kept and marked.
void OtherBranchOptionsAreCandidates() {
  const std::string listed = R"("appinfo"
{
  "config" { "launch" {
    "0" { "executable" "Dungeons.exe" "type" "default" "config" { "betakey" "public, build_debug, latest\t" "oslist" "windows" } }
  } }
  "depots" { "1912411" { "manifests" { "public" { "gid" "1" "size" "10" "download" "5" } } } }
})";
  const auto dungeons = DetailsFromAppInfo(1912410, listed);
  assert(dungeons.windows_installable && !dungeons.launches_any_branch && dungeons.launches.size() == 1);
  const std::string branches = R"("appinfo"
{
  "config" { "launch" {
    "0" { "executable" "RSDragonwilds.exe" "type" "default" "config" { "betakey" "any-undefined" } }
    "1" { "executable" "Aqua-Coyote.exe" "type" "default" "config" { "betakey" "adventurerbranch" } }
    "3" { "executable" "RSDragonwilds.exe" "type" "option1" "config" { "betakey" "trunk.dev.latest" "ownsdlc" "4179820" } }
  } }
  "depots" { "1374491" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "1" "size" "10" "download" "5" } } } }
})";
  const auto dragonwilds = DetailsFromAppInfo(1374490, branches);
  assert(dragonwilds.windows_installable && dragonwilds.launches_any_branch);
  assert(dragonwilds.launches.size() == 2 && dragonwilds.launches[0].executable == "RSDragonwilds.exe" &&
         dragonwilds.launches[1].executable == "Aqua-Coyote.exe");
}

// The size shown has to count the depots the install will fetch: a localised game is bigger in
// a language it carries, and the free-space check is made against that figure.
void SizesFollowTheContentLanguage() {
  const std::string buffer = R"("appinfo"
{
  "appid" "400"
  "common" { "name" "Portal" "type" "Game" }
  "config" { "launch" { "0" { "executable" "portal.exe" "config" { "oslist" "windows" } } } }
  "depots"
  {
    "401" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "1" "size" "1000" "download" "400" } } }
    "402" { "config" { "oslist" "windows" "language" "russian" } "manifests" { "public" { "gid" "2" "size" "500" "download" "200" } } }
  }
})";
  const auto english = DetailsFromAppInfo(400, buffer);
  assert(english.available && english.install_bytes == 1000u && english.download_bytes == 400u);

  const auto russian = DetailsFromAppInfo(400, buffer, std::nullopt, "russian");
  assert(russian.available && russian.install_bytes == 1500u && russian.download_bytes == 600u);

  // A language the game does not carry is installed in English, and sized as English.
  const auto polish = DetailsFromAppInfo(400, buffer, std::nullopt, "polish");
  assert(polish.available && polish.install_bytes == 1000u && polish.download_bytes == 400u);
  // The languages an install can choose from: the neutral content is the game's English.
  assert((polish.languages == std::vector<std::string>{"english", "russian"}));
}

// The install sheet's "Includes N DLC": owned DLC whose Windows content the install selects.
void IncludedDlcIsCounted() {
  const std::string buffer = R"("appinfo"
{
  "appid" "6000"
  "common" { "name" "DLC" "type" "Game" }
  "config" { "launch" { "0" { "executable" "game.exe" "config" { "oslist" "windows" } } } }
  "depots"
  {
    "6000" { "manifests" { "public" { "gid" "1" "size" "1000" "download" "400" } } }
    "6011" { "dlcappid" "6001" "config" { "oslist" "windows" } "manifests" { "public" { "gid" "2" "size" "10" "download" "5" } } }
    "6012" { "dlcappid" "6001" "config" { "oslist" "windows" } "manifests" { "public" { "gid" "3" "size" "10" "download" "5" } } }
    "6021" { "dlcappid" "6002" "config" { "oslist" "windows" } "manifests" { "public" { "gid" "4" "size" "10" "download" "5" } } }
    "6031" { "dlcappid" "6003" "config" { "oslist" "macos" } "manifests" { "public" { "gid" "5" "size" "10" "download" "5" } } }
  }
})";
  // Without licenses no DLC counts as owned.
  assert(DetailsFromAppInfo(6000, buffer).dlc_count == 0);
  LicensedDepots::Grant grant;
  grant.apps = {6000, 6001, 6003};
  grant.depots = {6000, 6011, 6012, 6021, 6031};
  const auto details = DetailsFromAppInfo(6000, buffer, grant);
  // 6001 counts once for its two depots; 6002 isn't owned and 6003 is Mac only.
  assert(details.dlc_count == 1 && details.install_bytes == 1020u);
}

// The versions an install can choose, with each one's own size; the licenses filter them as they do the sizes.
void VersionsAreOffered() {
  const std::string buffer = R"("appinfo"
{
  "config" { "launch" { "0" { "executable" "game.exe" "config" { "oslist" "windows" } } } }
  "depots"
  {
    "7001" { "manifests" { "public" { "gid" "1" "size" "1000" "download" "400" } "4.04" { "gid" "2" "size" "800" "download" "300" } } }
    "7002" { "config" { "oslist" "windows" } "manifests" { "4.04" { "gid" "3" "size" "100" "download" "50" } }
             "encryptedmanifests" { "beta" { "encrypted_gid_2" "AB" } } }
    "branches"
    {
      "public" { "buildid" "90" "timeupdated" "1760000000" }
      "4.04" { "buildid" "40" "description" "Patch 4.04" "timeupdated" "1670000000" }
      "beta" { "buildid" "95" "description" "Beta" "timeupdated" "1765000000" }
      "private" { "buildid" "96" "pwdrequired" "1" "timeupdated" "1766000000" }
    }
  }
})";
  const auto details = DetailsFromAppInfo(7000, buffer);
  assert(details.available && details.install_bytes == 1000u);
  assert(details.versions.size() == 2 && details.versions[0].name == "public" && details.versions[0].build_id == 90 &&
         details.versions[0].install_bytes == 1000 && details.versions[0].download_bytes == 400);
  assert(details.versions[1].name == "4.04" && details.versions[1].description == "Patch 4.04" &&
         details.versions[1].time_updated == 1670000000 && details.versions[1].install_bytes == 900 &&
         details.versions[1].download_bytes == 350);
  // Licenses granting only the public content: 4.04's own depot is not the account's, but its content is.
  const auto licensed = DetailsFromAppInfo(7000, buffer, LicensedDepots::Grant{{7000}, {7001}});
  assert(licensed.versions.size() == 2 && licensed.versions[1].install_bytes == 800);
}

// An installed game's details compare what Steam selects now with its install record.
void InstalledGamesCompareWithTheirRecord() {
  const std::string buffer = R"("appinfo"
{
  "config" { "launch" { "0" { "executable" "game.exe" "config" { "oslist" "windows" } }
                        "1" { "executable" "beta.exe" "config" { "oslist" "windows" "betakey" "beta" } } } }
  "depots"
  {
    "8001" { "manifests" { "public" { "gid" "11" "size" "1000" "download" "400" } "beta" { "gid" "12" "size" "1000" "download" "500" } } }
    "8002" { "config" { "oslist" "windows" } "manifests" { "public" { "gid" "21" "size" "100" "download" "40" } } }
    "8011" { "dlcappid" "8100" "config" { "oslist" "windows" } "manifests" { "public" { "gid" "31" "size" "10" "download" "5" } } }
    "8012" { "dlcappid" "8101" "manifests" { "public" { "gid" "32" "size" "0" "download" "0" } } }
    "branches" { "public" { "buildid" "2" } "beta" { "buildid" "3" } }
  }
})";
  // 8012 is an empty depot of owned DLC 8101 that the install never held (Steam lists free DLC this way): no update.
  const LicensedDepots::Grant grant{{8000, 8100, 8101}, {8001, 8002, 8011, 8012}};
  installs::InstalledRecord record;
  record.version = 2;
  record.app_id = 8000;
  record.depots = {{8001, 11, 8000, 8000}, {8002, 21, 8000, 8000}, {8011, 31, 8100, 8000}};
  // The same manifests: nothing to do.
  auto details = DetailsFromAppInfo(8000, buffer, grant, "english", record);
  assert(!details.update.available() && details.update.download_bytes == 0);
  // A new manifest: an update with that depot's download.
  record.depots[1].gid = 20;
  details = DetailsFromAppInfo(8000, buffer, grant, "english", record);
  assert(details.update.newer && !details.update.new_dlc && details.update.download_bytes == 40);
  record.depots[1].gid = 21;
  // A recorded depot Steam no longer selects (here: no longer licensed... as a removed one reads).
  record.depots.push_back({8003, 41, 8000, 8000});
  details = DetailsFromAppInfo(8000, buffer, grant, "english", record);
  assert(details.update.newer && details.update.download_bytes == 0);
  record.depots.pop_back();
  // A depot listed by a DLC app whose appinfo wasn't read can't have left.
  record.depots.push_back({8201, 51, 8200, 8200});
  details = DetailsFromAppInfo(8000, buffer, grant, "english", record);
  assert(!details.update.available());
  record.depots.pop_back();
  // DLC bought after the install: its depot is new content of an owner the record doesn't hold.
  const auto bought = record;
  record.depots.pop_back();
  details = DetailsFromAppInfo(8000, buffer, grant, "english", record);
  assert(details.update.new_dlc && !details.update.newer && details.update.download_bytes == 5);
  // A version 1 record names the game as every depot's owner: a held DLC depot is not new DLC.
  record = bought;
  record.version = 1;
  record.depots[2].owner = 8000;
  details = DetailsFromAppInfo(8000, buffer, grant, "english", record);
  assert(!details.update.available());
  // Licenses unknown: nothing is said.
  record.depots[1].gid = 20;
  assert(!DetailsFromAppInfo(8000, buffer, std::nullopt, "english", record).update.available());
  // A record without depots (an imported copy): nothing either.
  installs::InstalledRecord imported;
  imported.app_id = 8000;
  assert(!DetailsFromAppInfo(8000, buffer, grant, "english", imported).update.available());
  // The record's branch selects, and the launch options are that branch's.
  record = bought;
  record.branch = "beta";
  details = DetailsFromAppInfo(8000, buffer, grant, "english", record);
  assert(details.update.newer && details.update.download_bytes == 500);
  const auto has_beta = [](const AppDetails& value) {
    return std::any_of(value.launches.begin(), value.launches.end(),
                       [](const LaunchOption& option) { return option.executable == "beta.exe"; });
  };
  assert(has_beta(details) && !has_beta(DetailsFromAppInfo(8000, buffer, grant, "english", bought)));
}

int main() {
  SizeChecksAreAnsweredOncePerChoice();
  SlowSizeChecksDontBlockDetails();
  OpenedGameGoesFirst();
  LargeOverlappingDepotsAreMeasured();
  SizesCountOnlyLicensedDepots();
  RequestsAreDeduplicatedPerGeneration();
  NewGenerationDropsResultsAndRefetches();
  SameGenerationInvalidateRefetches();
  RefreshAsksAgainForOneGame();
  FailuresAreRetryable();
  AppInfoBecomesWindowsEstimate();
  DefaultBetaKeyIsThePublicBranch();
  OtherBranchOptionsAreCandidates();
  ImpossibleDownloadSizeFallsBackToInstallSize();
  SizesFollowTheContentLanguage();
  IncludedDlcIsCounted();
  VersionsAreOffered();
  InstalledGamesCompareWithTheirRecord();
}
