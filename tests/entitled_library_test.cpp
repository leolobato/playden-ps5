#include "steam/backend/entitled_library.h"

#include <unistd.h>

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
  std::map<std::uint32_t, std::string> unanswered;  // why the CM sent no buffer, for apps not in app_info
  std::map<std::uint32_t, HashedAppInfo> hashed;     // apps PICS answers with a hash only
  std::mutex requests_mutex;
  std::vector<std::vector<std::uint32_t>> app_info_requests;
  std::atomic<bool> fail_app_info{false};
  AppInfoBuffers FetchAppInfo(const StoredSession&, const std::vector<std::uint32_t>& ids,
                              const std::atomic<bool>&) override {
    {
      std::lock_guard<std::mutex> lock(requests_mutex);
      app_info_requests.push_back(ids);
    }
    if (fail_app_info) return {CmError::kNetwork, {}};
    AppInfoBuffers result{CmError::kNone, {}};
    for (const auto id : ids)
      if (const auto found = app_info.find(id); found != app_info.end()) result.buffers[id] = found->second;
      else if (const auto hash = hashed.find(id); hash != hashed.end()) {
        result.unanswered[id] = "http";
        result.hashed[id] = hash->second;
      } else if (const auto reason = unanswered.find(id); reason != unanswered.end()) result.unanswered[id] = reason->second;
    return result;
  }
  std::vector<std::vector<std::uint32_t>> Requests() {
    std::lock_guard<std::mutex> lock(requests_mutex);
    return app_info_requests;
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

std::string AppInfoText(std::uint32_t id, const std::string& name, const std::string& type) {
  return "\"appinfo\"\n{\n\t\"appid\"\t\"" + std::to_string(id) + "\"\n\t\"common\"\n\t{\n\t\t\"name\"\t\"" + name +
         "\"\n\t\t\"type\"\t\"" + type + "\"\n\t\t\"icon\"\t\"abc123\"\n\t}\n}\n";
}

// A DLC's app info, naming its game the way Steam does.
std::string DlcInfoText(std::uint32_t id, std::uint32_t game) {
  return "\"appinfo\"\n{\n\t\"appid\"\t\"" + std::to_string(id) + "\"\n\t\"common\"\n\t{\n\t\t\"type\"\t\"DLC\"\n\t}\n" +
         "\t\"extended\"\n\t{\n\t\t\"dlcforappid\"\t\"" + std::to_string(game) + "\"\n\t}\n}\n";
}

// A load, the background lookup it started, then a load that lists what the lookup named.
LibraryResult Settled(EntitledLibraryBackend& backend, const StoredSession& session = Session()) {
  std::atomic<bool> cancel{false};
  backend.Fetch(session, cancel);
  assert(backend.WaitForLookups(5s));
  return backend.Fetch(session, cancel);
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


HttpResponse Json(const std::string& body, long status = 200) {
  HttpResponse response;
  response.status_code = status;
  response.body.assign(body.begin(), body.end());
  return response;
}


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
  assert(result.licenses_read);
}

// The licenses go to size estimates and update checks for the account that read them only.
void LicensesBelongToTheirAccount() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.owned_apps = {10};
  acquisitions.result.owned_depots = {11};
  LicensedDepots licensed;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetLicensedDepots(&licensed);
  std::atomic<bool> cancel{false};
  assert(backend.Fetch(Session(), cancel).licenses_read);
  assert(licensed.Get(123) && licensed.Get(123)->depots == std::set<std::uint32_t>{11});
  // Another account whose license read fails has none: the first account's don't stand in.
  acquisitions.result = {CmError::kNetwork, {}};
  auto other = Session();
  other.steam_id = 456;
  assert(!backend.Fetch(other, cancel).licenses_read);
  assert(!licensed.Get(456));
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
  assert(!result.licenses_read);  // the games are listed, but what they own is unknown
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

// A free-to-play game added with "Add to library" and never played: GetOwnedGames leaves it out,
// its free license names it. Packages with a listed game, DLC and tools add nothing. The load never waits for
// the lookup: it lists the game once the background lookup named it.
void LicensedFreeGamesAreAdded() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.acquired_at[2767030] = 1700000000u;
  acquisitions.result.package_apps = {{0, {480}},               // Steam's basic free package
                                      {100, {10, 11}},          // a bought game and its DLC
                                      {200, {2767030}},         // the free game
                                      {300, {30}},              // DLC bought on its own
                                      {400, {40}}};             // an app Steam doesn't know
  acquisitions.app_info = {{2767030, AppInfoText(2767030, "Synthetic Rivals", "Game")},
                           {30, AppInfoText(30, "Synthetic DLC", "DLC")},
                           {11, DlcInfoText(11, 10)},
                           {480, AppInfoText(480, "Spacewar", "Game")}};
  acquisitions.unanswered = {{40, "unknown"}};
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 2);
  assert(backend.WaitForLookups(5s));
  // Handed over once, for the load's account only.
  assert(backend.TakeLateGames(456).empty());
  auto late = backend.TakeLateGames(123);
  assert(late.size() == 1 && backend.TakeLateGames(123).empty());
  const auto& added = late.back();
  assert(added.id == SteamId(2767030) && added.title == "Synthetic Rivals" && added.added_at_unix_seconds == 1700000000u);
  assert(added.artwork_source == "https://media.steampowered.com/steamcommunity/public/images/apps/2767030/abc123.jpg");
  // The listed game's DLC is looked up after the Library's apps, for the game it names, not for the Library.
  assert((acquisitions.Requests() == std::vector<std::vector<std::uint32_t>>{{30, 40, 2767030, 11}}));
  assert((backend.DlcOf(10) == std::vector<std::uint32_t>{11}) && backend.DlcOf(20).empty() && !Find(result, 11));
  result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 3 && Find(result, 2767030));

  // A refresh asks only about apps it doesn't know.
  acquisitions.result.package_apps[500] = {50};
  acquisitions.app_info[50] = AppInfoText(50, "Synthetic New", "game");
  result = Settled(backend);
  assert(result.games.size() == 4 && Find(result, 50) && Find(result, 50)->title == "Synthetic New");
  assert(acquisitions.Requests().size() == 2 && (acquisitions.Requests()[1] == std::vector<std::uint32_t>{50}));
  result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 4 && acquisitions.Requests().size() == 2);
}

// A demo comes in a free package of its own, which GetOwnedGames leaves out. It is added and marked,
// so the Library can hide it; games stay unmarked. A known name keeps the mark.
void LicensedDemosAreAddedAndMarked() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{600, {60}}, {700, {70}}};
  acquisitions.app_info = {{60, AppInfoText(60, "Synthetic Demo", "Demo")},
                           {70, AppInfoText(70, "Synthetic Free", "game")}};
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  for (int pass = 0; pass < 2; ++pass) {
    const auto result = pass ? backend.Fetch(Session(), cancel) : Settled(backend);
    assert(result.error == LibraryError::kNone && result.games.size() == 4);
    const auto demo = std::find_if(result.games.begin(), result.games.end(),
                                   [](const auto& game) { return game.id == SteamId(60); });
    assert(demo != result.games.end() && demo->title == "Synthetic Demo" && demo->demo);
    assert(std::none_of(result.games.begin(), result.games.end(),
                        [](const auto& game) { return game.demo && game.id != SteamId(60); }));
  }
  assert(acquisitions.Requests().size() == 1);
}

// Apps only a Steam family member's licenses grant can be hash-only in PICS: the lookup fetches them over HTTP.
void HashOnlyAppsAreFetchedOverHttp() {
  // gzip of AppInfoText(90, "Synthetic Hashed", "game") plus its NUL (107 bytes), and the SHA-1 of the text.
  const std::vector<std::uint8_t> gz = {
      0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x53, 0x4a, 0x2c, 0x28, 0xc8, 0xcc, 0x4b, 0xcb,
      0x57, 0xe2, 0xaa, 0xe6, 0xe2, 0x54, 0x02, 0x71, 0x52, 0x94, 0x38, 0x95, 0x2c, 0x0d, 0x94, 0x80, 0xbc, 0xe4,
      0xfc, 0xdc, 0xdc, 0xfc, 0x3c, 0x20, 0x0b, 0x28, 0xc5, 0xa9, 0x94, 0x97, 0x98, 0x9b, 0x0a, 0x94, 0x0a, 0xae,
      0xcc, 0x2b, 0xc9, 0x48, 0x2d, 0xc9, 0x4c, 0x56, 0xf0, 0x48, 0x2c, 0xce, 0x48, 0x4d, 0x51, 0x02, 0xc9, 0x95,
      0x54, 0x16, 0x80, 0xe4, 0xd2, 0x41, 0x4a, 0x40, 0xfc, 0xcc, 0x64, 0xa0, 0x3e, 0xa0, 0x71, 0x49, 0xc9, 0x86,
      0x46, 0xc6, 0x40, 0x91, 0x5a, 0xae, 0x5a, 0x2e, 0x06, 0x00, 0x10, 0x55, 0xd6, 0x54, 0x6b, 0x00, 0x00, 0x00};
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{900, {90}}};
  acquisitions.result.shared_apps = {90};
  acquisitions.hashed[90] = HashedAppInfo{{0x3d, 0xa8, 0xeb, 0x83, 0x16, 0xb6, 0x68, 0x11, 0xd5, 0x09,
                                           0x97, 0x05, 0x6d, 0x5a, 0x97, 0xc2, 0x72, 0xe3, 0xeb, 0x0f},
                                          107};
  Lines lines;
  std::atomic<int> fetches{0};
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [&](const HttpRequest& request, const std::atomic<bool>&) {
        HttpResponse response;
        if (request.url.find("/appinfo/90/sha/3da8eb8316b66811d50997056d5a97c272e3eb0f.txt.gz") == std::string::npos) {
          response.error = HttpError::kConnectionFailed;  // no family service
          return response;
        }
        ++fetches;
        response.status_code = 200;
        response.body = gz;
        return response;
      },
      lines.Log());
  const auto result = Settled(backend);
  assert(Find(result, 90) && Find(result, 90)->title == "Synthetic Hashed" && Find(result, 90)->family_shared);
  assert(fetches == 1 && lines.Count("family lookup http=1 fetched=1 failed=0 skipped=0") == 1);
}

// Each named game reaches the Library through the callback, after the load returned.
void LateGamesReachTheCallback() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{700, {70}}};
  acquisitions.app_info = {{70, AppInfoText(70, "Synthetic Free", "game")}};
  std::atomic<int> calls{0};
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetLateGamesCallback([&calls] { ++calls; });
  std::atomic<bool> cancel{false};
  assert(backend.Fetch(Session(), cancel).games.size() == 2);
  assert(backend.WaitForLookups(5s));
  assert(calls >= 1 && backend.TakeLateGames(123).size() == 1);
  backend.SetLateGamesCallback(nullptr);
}

// The names file holds its entries exactly; a damaged one reads as empty, and the newest entries are kept.
void AppNamesFileRoundTrips() {
  char root[] = "/tmp/playden-app-names-XXXXXX";
  assert(mkdtemp(root));
  Storage storage(root);
  AppNames names;
  names[10] = AppName{AppName::Kind::kGame, "Synthetic Game", "abc", 100};
  names[11] = AppName{AppName::Kind::kDemo, "Synthetic Demo", "", 101};
  names[12] = AppName{AppName::Kind::kOther, "", "", 102, 10};
  assert(SaveAppNames(storage, names));
  auto loaded = LoadAppNames(storage);
  assert(loaded.size() == 3 && loaded.at(10).name == "Synthetic Game" && loaded.at(10).icon == "abc" &&
         loaded.at(11).kind == AppName::Kind::kDemo && loaded.at(12).kind == AppName::Kind::kOther &&
         loaded.at(12).learned_at == 102 && loaded.at(12).dlc_for == 10 && loaded.at(10).dlc_for == 0);
  // A file from before DLC kept their game: its games load, its other apps are looked up again.
  Bytes v1 = {'P', 'D', 'N', 'A', 'M', 'E', 0, 1, 2, 0, 0, 0};
  const auto entry = [&v1](std::uint32_t id, std::uint8_t kind, const std::string& name) {
    for (int i = 0; i < 4; ++i) v1.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    v1.push_back(kind);
    for (int i = 0; i < 8; ++i) v1.push_back(i == 0 ? 7 : 0);
    v1.push_back(static_cast<std::uint8_t>(name.size()));
    v1.push_back(0);
    v1.insert(v1.end(), name.begin(), name.end());
    v1.push_back(0);
    v1.push_back(0);
  };
  entry(10, 1, "Old Game");
  entry(12, 0, "");
  std::uint32_t crc = 0xffffffffu;
  for (const auto byte : v1) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  crc = ~crc;
  for (int i = 0; i < 4; ++i) v1.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
  assert(storage.WriteAtomic("app-names.cache", v1).ok());
  loaded = LoadAppNames(storage);
  assert(loaded.size() == 1 && loaded.at(10).name == "Old Game" && loaded.at(10).learned_at == 7);
  assert(SaveAppNames(storage, names));
  auto bytes = storage.Read("app-names.cache").value();
  bytes[bytes.size() / 2] ^= 1;
  assert(storage.WriteAtomic("app-names.cache", bytes).ok());
  assert(LoadAppNames(storage).empty());
  AppNames many;
  for (std::uint32_t id = 1; id <= kMaxAppNames + 5; ++id) many[id] = AppName{AppName::Kind::kGame, "G", "", id};
  assert(SaveAppNames(storage, many));
  loaded = LoadAppNames(storage);
  assert(loaded.size() == kMaxAppNames && !loaded.count(5) && loaded.count(6) && loaded.count(kMaxAppNames + 5));
  // Other apps have a cap of their own: a library's DLC neither pushes games out nor is pushed out by them.
  constexpr std::uint32_t kOthers = kMaxOtherApps + 5;
  for (std::uint32_t id = 1; id <= kOthers; ++id)
    many[1'000'000 + id] = AppName{AppName::Kind::kOther, "", "", 1'000'000 + id, 7};
  assert(SaveAppNames(storage, many));
  loaded = LoadAppNames(storage);
  std::size_t games = 0;
  for (const auto& [id, name] : loaded) games += name.kind == AppName::Kind::kGame;
  assert(games == kMaxAppNames && loaded.size() == kMaxAppNames + kMaxOtherApps);
  assert(!loaded.count(1'000'005) && loaded.count(1'000'006) && loaded.at(1'000'000 + kOthers).dlc_for == 7);
  // Long names: what is saved still fits what Load reads, newest first.
  AppNames long_names;
  for (std::uint32_t id = 1; id <= 5000; ++id)
    long_names[id] = AppName{AppName::Kind::kGame, std::string(1000, 'x'), "", id};
  assert(SaveAppNames(storage, long_names));
  loaded = LoadAppNames(storage);
  assert(loaded.size() > 3000 && loaded.size() < 5000 && loaded.count(5000) && !loaded.count(1));
  std::remove((std::string(root) + "/app-names.cache").c_str());
  rmdir(root);
}

// Names are saved: the next run lists the game at once, without a lookup.
void NamesAreSaved() {
  char root[] = "/tmp/playden-app-names-XXXXXX";
  assert(mkdtemp(root));
  Storage storage(root);
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{700, {70}}, {300, {30}}};
  acquisitions.app_info = {{70, AppInfoText(70, "Synthetic Free", "game")}, {30, AppInfoText(30, "Synthetic DLC", "DLC")}};
  {
    EntitledLibraryBackend backend(owned, acquisitions);
    backend.SetAppNameStorage(&storage);
    assert(Settled(backend).games.size() == 3);
  }
  const auto names = LoadAppNames(storage);
  assert(names.size() == 2 && names.at(70).kind == AppName::Kind::kGame && names.at(70).name == "Synthetic Free");
  assert(names.at(30).kind == AppName::Kind::kOther && names.at(30).name.empty());
  Acquisitions next;
  next.result = acquisitions.result;
  EntitledLibraryBackend backend(owned, next);
  backend.SetAppNameStorage(&storage);
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 3 && Find(result, 70) && Find(result, 70)->title == "Synthetic Free");
  assert(backend.WaitForLookups(5s) && next.Requests().empty());
  std::remove((std::string(root) + "/app-names.cache").c_str());
  rmdir(root);
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
    const auto result = pass ? backend.Fetch(Session(), cancel) : Settled(backend);
    assert(result.error == LibraryError::kNone && result.games.size() == 5);
    assert(Find(result, 80) && Find(result, 80)->family_shared && Find(result, 80)->title == "Synthetic Shared");
    assert(Find(result, 90) && Find(result, 90)->family_shared);
    assert(Find(result, 95) && !Find(result, 95)->family_shared);
    assert(!Find(result, 10)->family_shared && !Find(result, 20)->family_shared && !Find(result, 11));
  }
  // The package with an owned game is passed over; the borrowed bundle only skips the owned game. Its DLC
  // is looked up last, only for the game it names.
  assert((acquisitions.Requests() == std::vector<std::vector<std::uint32_t>>{{80, 90, 95, 11}}));
  // Each shared app is logged once per run; the summary once per load.
  assert(lines.Count("family library added app=80 demo=0 source=saved") == 1);
  assert(lines.Count("family library added app=90 demo=0 source=saved") == 1);
  assert(lines.Count("family library shared=2 added=0 left_out=0 pending=2 in_owned_list=0") == 1);
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
  assert(acquisitions.Requests().empty());
  assert(lines.Count("family library shared=1 added=0 left_out=0 pending=0 in_owned_list=1") == 1);
}

// Shared apps PICS doesn't call a named game or demo stay out, with the reason logged. Apps without an answer
// stay pending and are asked again.
void SharedAppsLeftOutAreLogged() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{810, {81}}, {820, {82}}, {830, {83}}, {840, {84}}, {850, {85}}};
  acquisitions.result.shared_apps = {81, 82, 83, 84, 85};
  acquisitions.app_info = {{81, AppInfoText(81, "Synthetic DLC", "DLC")},
                           {83, AppInfoText(83, "app_83", "game")},
                           {84, "not app info"}};
  acquisitions.unanswered = {{85, "missing_token"}};
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(NoFamilyService(), lines.Log());
  const auto result = Settled(backend);
  assert(result.games.size() == 2);
  assert(lines.Count("family library left_out app=81 reason=type=dlc") == 1);
  assert(lines.Count("family library left_out app=83 reason=no_name") == 1);
  assert(lines.Count("family library shared=5 added=0 left_out=2 pending=3") == 1);
  // The second load asked again about the three it couldn't name.
  assert(backend.WaitForLookups(5s));
  assert(lines.Count("family lookup end games=0 by_hash=0 unanswered=2") == 2);
  assert((acquisitions.Requests().back() == std::vector<std::uint32_t>{82, 84, 85}));
}

// A shared app Steam sent no app info for is named by a later load's lookup.
void SharedAppWithoutAppInfoIsAskedAgain() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{800, {80}}, {810, {81}}};
  acquisitions.result.shared_apps = {80, 81};
  acquisitions.app_info = {{81, AppInfoText(81, "Synthetic DLC", "DLC")}};
  acquisitions.unanswered = {{80, "missing_token"}};
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(NoFamilyService(), lines.Log());
  auto result = Settled(backend);
  assert(!Find(result, 80));
  assert(backend.WaitForLookups(5s));
  acquisitions.app_info[80] = AppInfoText(80, "Synthetic Shared", "game");
  result = Settled(backend);
  assert(Find(result, 80) && Find(result, 80)->family_shared);
  // Only the unanswered app is asked again; the DLC's answer is kept.
  assert((acquisitions.Requests() == std::vector<std::vector<std::uint32_t>>{{80, 81}, {80}, {80}}));
  assert(lines.Count("family library added app=80 demo=0") == 1);
}

// When PICS fails the lookup stops, and a later load asks again.
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
  auto result = Settled(backend);
  assert(result.games.size() == 2 && !Find(result, 80));
  assert(backend.WaitForLookups(5s));
  assert(lines.Count("family lookup end games=0 by_hash=0 unanswered=0 error=network") == 2);
  acquisitions.fail_app_info = false;
  result = Settled(backend);
  assert(Find(result, 80) && Find(result, 80)->family_shared);
  assert(lines.Count("family library added app=80") == 1);
}

// An older family answer (this load's probe failed) still names games, but a shared app it doesn't list may
// have been shared since: it is looked up instead of left out.
void StaleFamilyListDoesNotHideNewGames() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{800, {80}}};
  acquisitions.result.shared_apps = {80};
  acquisitions.app_info = {{81, AppInfoText(81, "Synthetic Newly Shared", "game")}};
  Lines lines;
  std::atomic<bool> fail{false};
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [&fail](const HttpRequest& request, const std::atomic<bool>&) {
        if (fail) {
          HttpResponse response;
          response.error = HttpError::kTimeout;
          return response;
        }
        if (request.url.find("GetFamilyGroupForUser") != std::string::npos)
          return Json(R"({"response":{"family_groupid":"42"}})");
        return Json(R"({"response":{"apps":[{"appid":80,"name":"Synthetic Family Game","app_type":1}]}})");
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  assert(Find(backend.Fetch(Session(), cancel), 80));
  assert(WaitFor(lines, "family webapi app=80 "));
  fail = true;
  acquisitions.result.package_apps[810] = {81};
  acquisitions.result.shared_apps.insert(81);
  const auto result = Settled(backend);
  assert(Find(result, 80) && Find(result, 81) && Find(result, 81)->title == "Synthetic Newly Shared");
  assert(lines.Count("family_list=stale") >= 1 && lines.Count("not_in_family_list") == 0);
}

// Steam's family service names the family's games: the load lists them at once, without a lookup. Shared
// apps it leaves out are not games and are looked up only for the game a DLC names; a game it lists without
// a name is looked up for the Library.
void FamilyServiceNamesSharedGames() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result.package_apps = {{800, {80, 81}}, {820, {82}}, {830, {83}}, {840, {84}}};
  acquisitions.result.shared_apps = {80, 81, 82, 83, 84};
  acquisitions.app_info = {{84, AppInfoText(84, "Synthetic Unnamed", "game")}};
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [](const HttpRequest& request, const std::atomic<bool>&) {
        if (request.url.find("GetFamilyGroupForUser") != std::string::npos)
          return Json(R"({"response":{"family_groupid":"42"}})");
        return Json(R"({"response":{"apps":[
          {"appid":80,"owner_steamids":["5"],"name":"Synthetic Family Game","app_type":1,"rt_time_acquired":1600000000},
          {"appid":82,"owner_steamids":["5"],"name":"Synthetic Family Demo","app_type":8},
          {"appid":83,"owner_steamids":["5"],"name":"Synthetic Family Tool","app_type":4},
          {"appid":84,"owner_steamids":["5"]}]}})");
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.games.size() == 4);
  assert(Find(result, 80)->title == "Synthetic Family Game" && Find(result, 80)->family_shared &&
         Find(result, 80)->added_at_unix_seconds == 1600000000u && !Find(result, 80)->demo);
  assert(Find(result, 82)->title == "Synthetic Family Demo" && Find(result, 82)->demo);
  assert(lines.Count("family library left_out app=81 reason=not_in_family_list") == 1);
  assert(lines.Count("family library left_out app=83 reason=app_type=4") == 1);
  assert(lines.Count("named_family=2 named_saved=0 family_list=y lookups=1") == 1);
  // The game listed without a name is looked up first; the apps the service didn't name follow, for DlcOf.
  assert(backend.WaitForLookups(5s));
  assert((acquisitions.Requests() == std::vector<std::vector<std::uint32_t>>{{84, 81, 83}}));
  assert(backend.TakeLateGames(123).size() == 1);
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

// The family service is asked by every load; its comparison with the licenses is logged once per run.
void FamilyServiceIsAskedByEveryLoad() {
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
  auto result = Settled(backend);
  assert(result.error == LibraryError::kNone && result.games.size() == 3);
  assert(WaitFor(lines, "family webapi app=81 "));
  assert(requests == 4);
  assert(lines.Count("family webapi group=y family_groupid=42 role=2 members=1 member=5:1") == 2);
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
  backend->SetFamilyWait(50ms);
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
  backend.SetFamilyWait(50ms);
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
  assert(lines.Count("family_list=n") == 1);
  release = true;
  // The last line the probe writes.
  assert(WaitFor(lines, "family webapi app=81 owners=5 exclude_reason=0 licensed=n"));
  assert(lines.Count("family webapi shared_apps=2 excluded=0 owners=1 unlicensed=1 licenses=known") == 1);
  assert(lines.Count("family webapi app=80 owners=5 exclude_reason=0 licensed=shared") == 1);
}

// Per-app family lines stay bounded per load, and each app is logged once.
void FamilyAppLinesAreCapped() {
  Owned owned;
  Acquisitions acquisitions;
  std::string apps;
  for (std::uint32_t app = 1000; app < 1300; ++app) {
    acquisitions.result.package_apps[app] = {app};
    acquisitions.result.shared_apps.insert(app);
    apps += std::string(apps.empty() ? "" : ",") + R"({"appid":)" + std::to_string(app) + R"(,"name":"Synthetic )" +
            std::to_string(app) + R"(","app_type":1})";
  }
  Lines lines;
  EntitledLibraryBackend backend(owned, acquisitions);
  backend.SetFamilyDiagnostics(
      [apps](const HttpRequest& request, const std::atomic<bool>&) {
        if (request.url.find("GetFamilyGroupForUser") != std::string::npos)
          return Json(R"({"response":{"family_groupid":"42"}})");
        return Json(R"({"response":{"apps":[)" + apps + "]}}");
      },
      lines.Log());
  std::atomic<bool> cancel{false};
  const auto load = [&] {
    const auto before = lines.Count("family library ");
    assert(backend.Fetch(Session(), cancel).games.size() == 302);
    return lines.Count("family library ") - before;
  };
  // Each load writes at most 100 per-app lines, a truncated line and the summary; no app repeats.
  assert(load() == 102 && lines.Count("family library added app=") == 100);
  assert(lines.Count("family library truncated=200") == 1);
  assert(load() == 102 && lines.Count("family library added app=") == 200);
  assert(load() == 101 && lines.Count("family library added app=") == 300);
  assert(load() == 1);
  for (std::uint32_t app = 1000; app < 1300; ++app)
    assert(lines.Count("family library added app=" + std::to_string(app) + " ") == 1);
  assert(lines.Count("family library shared=300 added=300 left_out=0 pending=0") == 4);
  assert(acquisitions.Requests().empty());
}

}  // namespace

int main() {
  FamilySharedAppsAreMarked();
  OwnedAndBorrowedIsNotShared();
  SharedAppsLeftOutAreLogged();
  SharedAppWithoutAppInfoIsAskedAgain();
  SharedAppInfoFailureIsRetried();
  FamilyServiceNamesSharedGames();
  StaleFamilyListDoesNotHideNewGames();
  FamilyServiceIsAskedByEveryLoad();
  FamilyServiceNeverFailsTheLoad();
  FamilyWithoutLicensesIsAskedAgain();
  StalledFamilyServiceHoldsNothingUp();
  LateFamilyAnswerIsLogged();
  FamilyAppLinesAreCapped();
  CmFailureIsLoggedForFamily();
  LicensedFreeGamesAreAdded();
  LicensedDemosAreAddedAndMarked();
  HashOnlyAppsAreFetchedOverHttp();
  LateGamesReachTheCallback();
  NamesAreSaved();
  AppNamesFileRoundTrips();
  DatesMergeIntoOwnedGames();
  LicensesBelongToTheirAccount();
  CmFailureIsNeverFatal();
  OwnedFailureStopsTheCmSession();
  CancelReachesARunningCmSession();
}
