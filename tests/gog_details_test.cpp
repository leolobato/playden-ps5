// SPDX-License-Identifier: GPL-3.0-or-later
// GOG's details source (stores/gog_details) against the stub CDN (gog_cdn_stub.h): what installing a game takes. The
// Elder Scrolls: Arena's recorded public meta with DOSBox from the recorded public dependency store, a synthetic game
// naming __redist installers from the same store, a synthetic game with a DLC owned or not, Monkey Island 2's recorded
// gen 1 build and repository with its redists and language_setup from the same store, no Windows build, the network
// failing, and Invalidate dropping an answer that comes late. Installed games: updates from the install record, the
// installed check's progress (also past kMaximumEvents answers), and an update's size measured against a synthetic
// game the install runner put in place, which the measure leaves as it was.
// Usage: gog_details_test <fixtures/gog> <scratch folder>.

#include "gog/session.h"
#include "gog/session_store.h"
#include "gog/update_diff.h"
#include "gog_cdn_stub.h"
#include "gog_stub_transport.h"
#include "gog_synthetic_game.h"
#include "installs/install_record.h"
#include "stores/gog_details.h"
#include "stores/gog_install.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace playden;
using namespace gog_test;
using installs::InstalledRecord;
using stores::GameDetails;
using stores::GogDetailsSource;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string g_fixtures;
fs::path g_scratch;
int g_cases = 0;

constexpr std::uint64_t kArena = 1435828982;
constexpr std::uint64_t kMonkeyIsland2 = 1425039730;

// One case: the CDN, a GOG session (signed in or not), the source and what it logged.
struct World {
  fs::path user;
  CdnStub cdn;
  Stub auth;
  std::unique_ptr<gog::SessionStore> store;
  std::unique_ptr<gog::Session> session;
  std::mutex log_mutex;
  std::vector<std::string> logs;
  std::atomic<int> transports{0};
  // What the install service would say: each installed game's record and folder.
  std::mutex installed_mutex;
  std::map<GameId, InstalledRecord> records;
  std::map<GameId, std::string> roots;
  std::unique_ptr<GogDetailsSource> source;

  explicit World(bool signed_in) {
    user = g_scratch / ("case-" + std::to_string(++g_cases));
    fs::remove_all(user);
    fs::create_directories(user);
    CHECK(::chmod(user.c_str(), 0755) == 0);
    gog::SessionStore::Options options;
    options.user_root = user.string();
    store = std::make_unique<gog::SessionStore>(options);
    session = std::make_unique<gog::Session>(*store, auth.Transport(), [] { return std::int64_t{100000}; });
    if (signed_in) {
      auth.Script(Endpoint::kToken, Tokens(1));
      auth.Script(Endpoint::kUser, Answer(200, ReadFile(g_fixtures + "/user-data.json")));
      const std::atomic<bool> no_cancel{false};
      const auto epoch = session->BeginSignIn();
      CHECK(session->Exchange("SyntheticCode-details-0123456789", epoch, no_cancel).kind ==
            gog::ExchangeOutcome::Kind::kSignedIn);
    }
    source = std::make_unique<GogDetailsSource>(
        *session,
        [this] {
          ++transports;
          return cdn.Transport();
        },
        [] { return std::string("english"); },
        [this](const std::string& line) {
          std::lock_guard<std::mutex> lock(log_mutex);
          logs.push_back(line);
        });
    source->SetInstalled([this](const GameId& id) -> std::optional<InstalledRecord> {
      std::lock_guard<std::mutex> lock(installed_mutex);
      const auto found = records.find(id);
      if (found == records.end()) return std::nullopt;
      return found->second;
    });
    source->SetInstallRoot([this](const GameId& id) -> std::optional<std::string> {
      std::lock_guard<std::mutex> lock(installed_mutex);
      const auto found = roots.find(id);
      if (found == roots.end()) return std::nullopt;
      return found->second;
    });
  }
  void Install(std::uint64_t product, std::optional<InstalledRecord> record, const std::string& root = {}) {
    std::lock_guard<std::mutex> lock(installed_mutex);
    const auto id = GameId::Gog(product);
    if (record) records[id] = *record;
    else records.erase(id);
    if (!root.empty()) roots[id] = root;
  }
  ~World() {
    source.reset();
    fs::remove_all(user);
  }

  // The next answer of the generation, waiting for it.
  GameDetails Next(std::uint64_t generation = 1) {
    const auto until = std::chrono::steady_clock::now() + 30s;
    for (;;) {
      auto answers = source->Pump(8);
      CHECK(answers.size() <= 1);
      if (!answers.empty()) {
        CHECK(answers[0].account_generation == generation);
        return answers[0];
      }
      CHECK(std::chrono::steady_clock::now() < until);
      std::this_thread::sleep_for(1ms);
    }
  }
  GameDetails Ask(std::uint64_t product, std::uint64_t generation = 1) {
    source->RequestFirst(GameId::Gog(product), generation);
    auto details = Next(generation);
    CHECK(details.id == GameId::Gog(product));
    return details;
  }
  // Asked again (the record changed).
  GameDetails Again(std::uint64_t product, std::uint64_t generation = 1) {
    source->Refresh(GameId::Gog(product), generation, true);
    auto details = Next(generation);
    CHECK(details.id == GameId::Gog(product));
    return details;
  }
  // Every answer until the check of `generation` is done, with its progress never ahead of what was handed out.
  std::vector<GameDetails> Drain(std::uint64_t generation = 1) {
    std::vector<GameDetails> answers;
    const auto until = std::chrono::steady_clock::now() + 30s;
    for (;;) {
      for (auto& details : source->Pump(8)) {
        CHECK(details.account_generation == generation);
        answers.push_back(std::move(details));
      }
      const auto check = source->CheckProgress();
      CHECK(check.account_generation == generation && check.total != 0);
      CHECK(check.answered <= check.asked && check.asked <= check.total && check.failed <= check.answered);
      if (check.done()) return answers;
      CHECK(std::chrono::steady_clock::now() < until);
      std::this_thread::sleep_for(1ms);
    }
  }
  // The next update size of the generation, waiting for it.
  steam::MeasuredSize Measured(std::uint64_t product, std::uint64_t generation = 1) {
    CHECK(source->Measure({GameId::Gog(product), {}, {}, true}, generation));
    const auto until = std::chrono::steady_clock::now() + 30s;
    for (;;) {
      auto sizes = source->PumpMeasured();
      CHECK(sizes.size() <= 1);
      if (!sizes.empty()) {
        CHECK(sizes[0].account_generation == generation && sizes[0].request.update);
        CHECK(sizes[0].request.id == GameId::Gog(product));
        return sizes[0];
      }
      CHECK(std::chrono::steady_clock::now() < until);
      std::this_thread::sleep_for(1ms);
    }
  }
  // No line names the account, a token, the code, an address or a folder.
  void CheckLogs() {
    std::lock_guard<std::mutex> lock(log_mutex);
    CHECK(!logs.empty());
    for (const auto& line : logs) {
      CHECK(line.rfind("details result=", 0) == 0 || line.rfind("details update result=", 0) == 0 ||
            line.rfind("details size result=", 0) == 0);
      CHECK(line.find(kUserId) == std::string::npos && line.find(kUsername) == std::string::npos);
      CHECK(line.find(AccessText(1)) == std::string::npos && line.find("http") == std::string::npos);
      CHECK(line.find(g_scratch.string()) == std::string::npos && line.find("content-system") == std::string::npos);
      CHECK(line.find(std::to_string(kBase)) == std::string::npos);
    }
  }
  bool Logged(const std::string& start) {
    std::lock_guard<std::mutex> lock(log_mutex);
    return std::any_of(logs.begin(), logs.end(), [&](const std::string& line) { return line.rfind(start, 0) == 0; });
  }
};

// Arena's recorded meta behind a synthetic builds answer, and the recorded dependency store at the stub's CDN.
void PublishArena(CdnStub& cdn) {
  const auto meta_id = cdn.Put(ReadFile(g_fixtures + "/meta-v2-dosbox-1435828982.zlib"));
  cdn.SetBuilds(kArena, BuildsJson(kArena, "1000000000000000201", meta_id));
  const auto repository_id = cdn.Put(ReadFile(g_fixtures + "/dependencies-manifest.zlib"));
  cdn.SetRepository("{\"repository_manifest\":" +
                    Json(kFastlyBase + "/content-system/v2/dependencies/meta/" + Galaxy(repository_id)) +
                    ",\"build_id\":\"60163282911088704\",\"generation\":2}");
}

// The Elder Scrolls: Arena: its five en-US and "*" depots, plus DOSBox from the dependency store, as installed.
void Gen2SizesCountTheDependencies() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1435828982]}");
  PublishArena(world.cdn);
  const auto details = world.Ask(kArena);
  CHECK(details.available && details.windows_installable && !details.failed && details.reason.empty());
  // Depots (meta-v2-dosbox-1435828982): 104645228 + 9467 + 2421 + 1838 + 12218, compressed 68505734 + 9200 + 539 +
  // 382 + 4437; DOSBox074_2CS (dependencies-manifest): 8868679, compressed 6105676.
  CHECK(details.install_bytes == std::optional<std::uint64_t>(104671172ull + 8868679ull));
  CHECK(details.download_bytes == std::optional<std::uint64_t>(68520292ull + 6105676ull));
  // Installed games' launches come from the install record; nothing else is offered yet.
  CHECK(details.launches.empty() && details.languages.empty() && details.versions.empty() && details.dlc.empty());
  CHECK(details.install_dir.empty() && !details.update.available());
  CHECK(world.Logged("details result=gen2 depots=5 products=1 dependencies=1 owned=list"));
  CHECK(world.transports == 1);
  // Asked again: answered from what was asked in this generation, no request.
  const auto before = world.cdn.requests().size();
  world.source->Request(GameId::Gog(kArena), 1);
  std::this_thread::sleep_for(20ms);
  CHECK(world.source->Pump(8).empty() && world.cdn.requests().size() == before);
  // Refresh asks GOG again; the dependency store and the owned list are not read again.
  world.source->Refresh(GameId::Gog(kArena), 1);
  CHECK(world.Next().install_bytes == details.install_bytes);
  CHECK(world.cdn.Count(Kind::kRepository) == 1 && world.cdn.Count(Kind::kDependencyMeta) == 1);
  CHECK(world.cdn.Count(Kind::kOwned) == 1 && world.cdn.Count(Kind::kBuilds) == 2);
  // No Authorization goes anywhere but the owned list.
  for (const auto& request : world.cdn.requests())
    CHECK(request.authorization.empty() == (request.kind != Kind::kOwned));
  world.CheckLogs();
}

// A meta naming __redist installers (XNA, DirectX, XNA again) and one the store doesn't list: the installers count
// once each, as the installer downloads them into .playden-gog/redist/.
void Gen2SizesCountTheRedists() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1900000001]}");
  PublishArena(world.cdn);  // the dependency store
  Game game;
  GameDepot main;
  main.files.push_back({"DepotFile", "bin\\Game.exe", RandomBytes(40 * 1024, 33)});
  game.depots.push_back(main);
  game.dependencies = {"XNA", "DirectX", "XNA", "NotInTheStore"};
  Publish(world.cdn, game);
  const auto details = world.Ask(kBase);
  CHECK(details.available && details.windows_installable && !details.failed);
  // XNA (dependencies-manifest): 7671808, compressed 6908843; DirectX: 102930955, compressed 100641555.
  CHECK(details.install_bytes == std::optional<std::uint64_t>(40 * 1024 + 7671808ull + 102930955ull));
  CHECK(details.download_bytes && *details.download_bytes > 6908843ull + 100641555ull &&
        *details.download_bytes < 6908843ull + 100641555ull + 40 * 1024 + 1024);
  CHECK(world.Logged("details result=gen2 depots=1 products=1 dependencies=2 owned=list"));
  world.CheckLogs();
}

// A synthetic game with a DLC: the DLC's depot counts only when the account owns it. With no session, only the game.
void OwnedDlcCountsOnlyWhenOwned() {
  Game game;
  GameDepot main;
  main.files.push_back({"DepotFile", "bin\\Game.exe", RandomBytes(40 * 1024, 31)});
  game.depots.push_back(main);
  GameDepot dlc;
  dlc.product = kDlc;
  dlc.files.push_back({"DepotFile", "dlc\\extra.pak", RandomBytes(30 * 1024, 32)});
  game.depots.push_back(dlc);
  const auto sizes = [&](const char* owned, bool signed_in) {
    World world(signed_in);
    world.cdn.SetOwned(owned);
    Publish(world.cdn, game);
    const auto details = world.Ask(kBase);
    CHECK(details.available && details.windows_installable && !details.failed);
    CHECK(world.cdn.Count(Kind::kOwned) == (signed_in ? 1 : 0));
    world.CheckLogs();
    return std::make_pair(*details.install_bytes, *details.download_bytes);
  };
  const auto base = sizes("{\"owned\":[1900000001]}", true);
  const auto with_dlc = sizes("{\"owned\":[1900000001,1900000002]}", true);
  CHECK(base.first == 40 * 1024 && base.second > 0);
  CHECK(with_dlc.first == 70 * 1024 && with_dlc.second > base.second);
  // Signed out: no owned list can be read, so the game alone, and still an answer.
  CHECK(sizes("{\"owned\":[1900000001,1900000002]}", false) == base);
}

// Monkey Island 2's default build is gen 1 only: installable, its sizes from its recorded repository (the Neutral and
// English depots) plus its redists from the dependency store: language_setup (game-folder files) and the MSVC2008,
// dotNet35 and DirectX installers. A game with no builds has no Windows version: installable=false with the reason,
// answered (not failed).
void Gen1AndNoBuild() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1425039730]}");
  PublishArena(world.cdn);  // the dependency store
  world.cdn.SetBuilds(kMonkeyIsland2, ReadFile(g_fixtures + "/builds-v2-gen1-only-1425039730.json"));
  world.cdn.SetV1File("1425039730/windows/32971092/repository.json",
                      ReadFile(g_fixtures + "/repository-v1-1425039730.json"));
  const auto gen1 = world.Ask(kMonkeyIsland2);
  CHECK(gen1.available && gen1.windows_installable && !gen1.failed && gen1.reason.empty());
  // Depots: 1280640 + 1912238511 (stored whole). From dependencies-manifest: MSVC2008 4216840 (compressed 4194895),
  // dotNet35 245612656 (245524799), DirectX 102930955 (100641555), language_setup 6216288 (6133610).
  constexpr std::uint64_t kRedists = 4216840ull + 245612656ull + 102930955ull + 6216288ull;
  constexpr std::uint64_t kRedistsCompressed = 4194895ull + 245524799ull + 100641555ull + 6133610ull;
  CHECK(gen1.install_bytes == std::optional<std::uint64_t>(1280640ull + 1912238511ull + kRedists));
  CHECK(gen1.download_bytes == std::optional<std::uint64_t>(1280640ull + 1912238511ull + kRedistsCompressed));
  CHECK(world.Logged("details result=gen1 depots=2 products=1 dependencies=4 owned=list"));
  // The repository from fastly, unsigned; never a build meta, a depot manifest or gcore.
  CHECK(world.cdn.Count(Kind::kRepositoryV1) == 1 && world.cdn.Count(Kind::kManifestV1) == 0);
  CHECK(world.cdn.Count(Kind::kMeta) == 0 && world.cdn.Count(Kind::kOwned) == 1);
  for (const auto& request : world.cdn.requests()) {
    CHECK(request.host != "gog-cdn.gcdn.co");
    CHECK(request.authorization.empty() == (request.kind != Kind::kOwned));
  }
  // A repository of another build: known, not installable, and why.
  world.cdn.SetV1File("1425039730/windows/32971092/repository.json",
                      std::string(ReadFile(g_fixtures + "/repository-v1-1425039730.json")).replace(
                          ReadFile(g_fixtures + "/repository-v1-1425039730.json").find("32971092"), 8, "32971093"));
  world.source->Refresh(GameId::Gog(kMonkeyIsland2), 1, true);
  const auto other = world.Next();
  CHECK(other.available && !other.windows_installable && other.reason == "GOG returned unexpected data for this game.");
  const auto none = world.Ask(kBase);  // nothing set: 404
  CHECK(none.available && !none.windows_installable && !none.failed);
  CHECK(none.reason == "GOG has no Windows version of this game to install.");
  CHECK(world.Logged("details result=no_build"));
  world.CheckLogs();
  // Other stores' games are not asked for.
  world.source->Request(GameId::Steam(220), 1);
  world.source->Request(GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e"), 1);
  std::this_thread::sleep_for(20ms);
  CHECK(world.source->Pump(8).empty());
}

// GOG answers, but with something Playden can't use (a 403 or 400 for the builds, builds it can't read, a meta for
// another product): known and not installable, with the reason, so the page doesn't stay on "Checking GOG…".
void UnexpectedAnswersSayWhy() {
  World world(true);
  PublishArena(world.cdn);
  std::atomic<long> builds_status{403};
  world.cdn.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind != Kind::kBuilds || builds_status.load() == 0) return std::nullopt;
    Fault fault;
    fault.status = builds_status.load();
    fault.body = "{\"error\":\"refused\"}";
    return fault;
  });
  // An answer that isn't failed stands for the sign-in: each case asks for its own game.
  std::uint64_t product = 1900000100;
  for (const long status : {403L, 400L, 200L}) {  // 200: "{"error":...}" is no builds answer
    builds_status = status;
    const auto details = world.Ask(++product);
    CHECK(details.available && !details.windows_installable && !details.failed);
    CHECK(details.reason == "GOG returned unexpected data for this game.");
    CHECK(!details.download_bytes && !details.install_bytes);
  }
  // A meta whose base product is another game.
  builds_status = 0;
  const auto arena_meta = world.cdn.Put(ReadFile(g_fixtures + "/meta-v2-dosbox-1435828982.zlib"));
  world.cdn.SetBuilds(kBase, BuildsJson(kBase, "1000000000000000202", arena_meta));
  const auto other = world.Ask(kBase);
  CHECK(other.available && !other.windows_installable && !other.failed &&
        other.reason == "GOG returned unexpected data for this game.");
  CHECK(world.Logged("details result=protocol"));
  world.CheckLogs();
}

// No answer from GOG: failed, so the next visit asks again, and then it answers.
void NetworkFailureFails() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1435828982]}");
  PublishArena(world.cdn);
  std::atomic<Kind> failing{Kind::kBuilds};
  world.cdn.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind != failing.load()) return std::nullopt;
    Fault fault;
    fault.response = Failure(HttpError::kTimeout);
    return fault;
  });
  for (const auto kind : {Kind::kBuilds, Kind::kMeta, Kind::kRepository, Kind::kDependencyMeta}) {
    failing = kind;
    const auto details = world.Ask(kArena);
    CHECK(details.failed && !details.available && !details.install_bytes);
  }
  CHECK(world.Logged("details result=network"));
  failing = Kind::kOther;
  const auto answered = world.Ask(kArena);
  CHECK(answered.available && answered.windows_installable && !answered.failed && answered.install_bytes);
  world.CheckLogs();
}

// An answer that comes after Invalidate is dropped; the next generation's request is answered.
void InvalidateDropsALateAnswer() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1435828982]}");
  PublishArena(world.cdn);
  auto gate = std::make_shared<Gate>();
  std::atomic<bool> hold{true};
  world.cdn.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind == Kind::kBuilds && hold.load()) {
      gate->Enter();
      const std::atomic<bool> no_cancel{false};
      gate->Wait(no_cancel);
    }
    return std::nullopt;
  });
  world.source->RequestFirst(GameId::Gog(kArena), 1);
  gate->WaitEntered(1);
  world.source->Invalidate(2);
  hold = false;
  gate->Open();
  // Generation 1's request is gone with it.
  world.source->Request(GameId::Gog(kArena), 1);
  world.source->Request(GameId::Gog(kArena), 2);
  const auto details = world.Next(2);
  CHECK(details.available && details.account_generation == 2);
  std::this_thread::sleep_for(20ms);
  CHECK(world.source->Pump(8).empty());
  // Shutdown is prompt and final.
  world.source->Shutdown();
  world.source->Request(GameId::Gog(kArena), 3);
  CHECK(world.source->Pump(8).empty());
}


// ---- Installed games ----------------------------------------------------------------------------------------------

constexpr char kBuildA[] = "1000000000000000101";
constexpr char kBuildB[] = "1000000000000000202";

// A GOG install's record at `build`.
InstalledRecord GogRecord(const std::string& build, int generation = 2) {
  InstalledRecord record;
  record.version = 2;
  record.language = "english";
  installs::GogRecord gog;
  gog.build_id = build;
  gog.generation = generation;
  gog.products = {kBase};
  gog.language = "en-US";
  gog.meta_id = std::string(32, 'a');
  record.gog = gog;
  return record;
}

Game SyntheticAt(const std::string& build) {
  Game game;
  game.build_id = build;
  GameDepot main;
  main.files.push_back({"DepotFile", "bin\\Game.exe", RandomBytes(40 * 1024, 51)});
  game.depots.push_back(main);
  return game;
}

// The record against the default build: another build, or the same one in another generation, is an update; the same
// build, no install, a record without `gog` or no Windows build now is none.
void UpdatesFromTheRecord() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1900000001]}");
  Publish(world.cdn, SyntheticAt(kBuildB));
  const auto target = gog::UpdateTarget(kBuildB, 2);
  world.Install(kBase, GogRecord(kBuildA));
  const auto newer = world.Ask(kBase);
  CHECK(newer.available && newer.windows_installable && !newer.failed);
  CHECK(newer.update.newer && !newer.update.new_dlc && newer.update.target == target && newer.update.available());
  CHECK(world.Logged("details update result=newer"));

  world.Install(kBase, GogRecord(kBuildB));
  const auto current = world.Again(kBase);
  CHECK(current.windows_installable && !current.update.available() && current.update.target == 0);
  CHECK(world.Logged("details update result=current"));

  // A gen 1 install of the same build ID: the gen 2 build is another one.
  world.Install(kBase, GogRecord(kBuildB, 1));
  const auto generation = world.Again(kBase);
  CHECK(generation.update.newer && generation.update.target == target);

  world.Install(kBase, std::nullopt);
  const auto not_installed = world.Again(kBase);
  CHECK(not_installed.windows_installable && !not_installed.update.available() && not_installed.update.target == 0);

  InstalledRecord steam_style;
  steam_style.version = 2;
  world.Install(kBase, steam_style);
  const auto no_gog = world.Again(kBase);
  CHECK(no_gog.windows_installable && !no_gog.update.available());
  CHECK(world.Logged("details update result=none"));

  // Installed, but GOG has no Windows build of it now.
  constexpr std::uint64_t kGone = 1900000050;
  auto gone_record = GogRecord(kBuildA);
  gone_record.gog->products = {kGone};
  world.Install(kGone, gone_record);
  const auto gone = world.Ask(kGone);
  CHECK(gone.available && !gone.windows_installable && !gone.update.available());
  world.CheckLogs();
}

// The installed check: three games (one GOG doesn't answer for) through Pump, a second check refused while it runs,
// then asked again once done; Invalidate in the middle drops it and its late answers.
void CheckCountsInstalledGames() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1900000001,1435828982]}");
  PublishArena(world.cdn);
  Publish(world.cdn, SyntheticAt(kBuildB));
  constexpr std::uint64_t kUnreachable = 1900000300;
  world.Install(kBase, GogRecord(kBuildA));
  world.Install(kArena, GogRecord("1000000000000000201"));
  world.Install(kUnreachable, GogRecord(kBuildA));
  auto gate = std::make_shared<Gate>();
  std::atomic<bool> hold{true};
  world.cdn.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind != Kind::kBuilds) return std::nullopt;
    if (hold.load()) {
      gate->Enter();
      const std::atomic<bool> no_cancel{false};
      gate->Wait(no_cancel);
    }
    if (request.product != kUnreachable) return std::nullopt;
    Fault fault;
    fault.response = Failure(HttpError::kTimeout);
    return fault;
  });
  const std::vector<GameId> ids{GameId::Gog(kBase), GameId::Gog(kArena), GameId::Steam(220), GameId::Gog(kBase),
                                GameId::Gog(kUnreachable)};
  // Nothing to check: no GOG game, or nothing at all.
  CHECK(!world.source->CheckInstalled({GameId::Steam(220)}, 1) && !world.source->CheckInstalled({}, 1));
  CHECK(!world.source->CheckInstalled(ids, 0));
  CHECK(world.source->CheckInstalled(ids, 1));
  gate->WaitEntered(1);
  auto progress = world.source->CheckProgress();
  CHECK(progress.account_generation == 1 && progress.total == 3 && progress.asked == 1 && progress.answered == 0);
  CHECK(!world.source->CheckInstalled(ids, 1));
  hold = false;
  gate->Open();
  auto answers = world.Drain();
  progress = world.source->CheckProgress();
  CHECK(progress.total == 3 && progress.asked == 3 && progress.answered == 3 && progress.failed == 1);
  CHECK(answers.size() == 3);
  std::set<GameId> answered;
  for (const auto& details : answers) {
    answered.insert(details.id);
    if (details.id == GameId::Gog(kBase)) CHECK(details.update.newer);
    if (details.id == GameId::Gog(kArena)) CHECK(details.windows_installable && !details.update.available());
    if (details.id == GameId::Gog(kUnreachable)) CHECK(details.failed && !details.update.available());
  }
  CHECK(answered.size() == 3);
  // A page asking now has its answer already; another check asks GOG again.
  const auto builds = world.cdn.Count(Kind::kBuilds);
  world.source->Request(GameId::Gog(kBase), 1);
  std::this_thread::sleep_for(20ms);
  CHECK(world.source->Pump(8).empty() && world.cdn.Count(Kind::kBuilds) == builds);
  CHECK(world.source->CheckInstalled(ids, 1));
  CHECK(world.Drain().size() == 3 && world.cdn.Count(Kind::kBuilds) == builds + 3);

  // Invalidate in the middle: the check is gone (total 0), and so are its answers still to come.
  gate = std::make_shared<Gate>();
  hold = true;
  CHECK(world.source->CheckInstalled(ids, 1));
  gate->WaitEntered(1);
  world.source->Invalidate(2);
  CHECK(world.source->CheckProgress().total == 0);
  hold = false;
  gate->Open();
  std::this_thread::sleep_for(50ms);
  CHECK(world.source->Pump(8).empty() && world.source->CheckProgress().total == 0);
  // The next generation's check runs.
  CHECK(world.source->CheckInstalled(ids, 2));
  CHECK(world.Drain(2).size() == 3 && world.source->CheckProgress().failed == 1);
  world.CheckLogs();
}

// More of the check's answers than kMaximumEvents wait for Pump: none is lost, the worker waits for room, and pages
// asked meanwhile are still answered.
void CheckAnswersAreNeverDropped() {
  World world(true);
  constexpr std::size_t kGames = GogDetailsSource::kMaximumEvents + 6;
  std::vector<GameId> ids;
  for (std::size_t i = 0; i < kGames; ++i) ids.push_back(GameId::Gog(1900001000 + i));  // no builds: answered at once
  CHECK(world.source->CheckInstalled(ids, 1));
  const auto until = std::chrono::steady_clock::now() + 30s;
  while (world.source->CheckProgress().asked < GogDetailsSource::kMaximumEvents) {
    CHECK(std::chrono::steady_clock::now() < until);
    std::this_thread::sleep_for(1ms);
  }
  std::this_thread::sleep_for(50ms);
  auto progress = world.source->CheckProgress();
  CHECK(progress.asked == GogDetailsSource::kMaximumEvents && progress.answered == 0);
  for (std::uint64_t i = 0; i < 3; ++i) world.source->Request(GameId::Gog(1900002000 + i), 1);
  std::this_thread::sleep_for(50ms);
  std::set<GameId> checked, pages;
  for (const auto& details : world.Drain()) {
    CHECK(details.available && !details.windows_installable && !details.failed);
    if (details.id.gog_product() && *details.id.gog_product() >= 1900002000) pages.insert(details.id);
    else checked.insert(details.id);
  }
  progress = world.source->CheckProgress();
  CHECK(progress.total == kGames && progress.answered == kGames && progress.failed == 0);
  CHECK(checked.size() == kGames && pages.size() == 3);
  CHECK(world.source->Pump(8).empty());
}

// Everything under root: names and bytes, folders marked.
std::map<std::string, std::string> Snapshot(const fs::path& root) {
  std::map<std::string, std::string> files;
  for (const auto& entry : fs::recursive_directory_iterator(root))
    files[fs::relative(entry.path(), root).string()] =
        entry.is_directory() ? std::string("<folder>") : ReadFile(entry.path().string());
  return files;
}

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file && std::fwrite(text.data(), 1, text.size(), file) == text.size() && std::fclose(file) == 0);
}

std::uint64_t Download(const std::vector<gog::DepotChunk>& chunks) {
  std::uint64_t total = 0;
  for (const auto& chunk : chunks) total += chunk.compressed_size;
  return total;
}

// An update's size: A installed by the runner, B (one file changed into three chunks, one added) planned beside it
// and diffed. The network down fails; the game folder stays as it was; a second measure is the first one's; an update
// under way, or a game not installed, is not measured.
void MeasureAnUpdate() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1900000001]}");
  world.cdn.SetSecureLink(ReadFile(g_fixtures + "/secure-link-200.json"));
  const auto keep = RandomBytes(12 * 1024, 61);
  const auto change = RandomBytes(18 * 1024, 62);
  const auto changed = RandomBytes(24 * 1024, 63);
  const auto add = RandomBytes(9 * 1024, 64);
  Game a;
  GameDepot depot_a;
  depot_a.files.push_back({"DepotFile", "keep.dat", keep});
  depot_a.files.push_back({"DepotFile", "data\\change.dat", change});
  a.depots.push_back(depot_a);
  Publish(world.cdn, a);
  const auto root = world.user / "games" / "gog-1900000001";
  fs::create_directories(root.parent_path());
  {
    stores::GogInstallRunner runner(
        *world.session, [&world] { return world.cdn.Transport(); }, [] { return std::string("english"); }, {},
        [](const std::string&) { return std::optional<std::uint64_t>(1ull << 40); });
    const std::atomic<bool> no_cancel{false};
    CHECK(runner.Install(GameId::Gog(kBase), root.string(), {}, no_cancel, {}, {}).ok);
  }
  const auto record = installs::ReadInstallRecord((root / installs::kInstallRecordName).string());
  CHECK(record && record->gog && record->gog->build_id == kBuildA);
  world.Install(kBase, *record, root.string());

  Game b;
  b.build_id = kBuildB;
  GameDepot depot_b;
  depot_b.files.push_back({"DepotFile", "keep.dat", keep});
  depot_b.files.push_back({"DepotFile", "data\\change.dat", changed, {8 * 1024, 8 * 1024, 8 * 1024}});
  depot_b.files.push_back({"DepotFile", "data\\add.dat", add});
  b.depots.push_back(depot_b);
  const auto published = Publish(world.cdn, b);
  const auto details = world.Ask(kBase);
  CHECK(details.update.newer && details.update.target == gog::UpdateTarget(kBuildB, 2));
  const auto before = Snapshot(root);
  const auto measure_folder = root / ".playden-gog" / "measure";

  // GOG out of reach: failed, and nothing left behind.
  world.cdn.SetFault([](const Request&) -> std::optional<Fault> {
    Fault fault;
    fault.response = Failure(HttpError::kTimeout);
    return fault;
  });
  const auto down = world.Measured(kBase);
  CHECK(down.failed && !down.measured && down.download_bytes == 0);
  CHECK(!fs::exists(measure_folder) && Snapshot(root) == before);
  CHECK(world.Logged("details size result=failed changed=0 removed=0 why=network"));

  world.cdn.SetFault({});
  world.cdn.ClearRequests();
  const auto size = world.Measured(kBase);
  CHECK(size.measured && !size.failed);
  const auto& change_chunks = published.chunks.at("data\\change.dat");
  const auto& add_chunks = published.chunks.at("data\\add.dat");
  CHECK(change_chunks.size() == 3);
  CHECK(size.download_bytes == Download(change_chunks) + Download(add_chunks));
  CHECK(size.staged_bytes == changed.size() + add.size());
  CHECK(size.install_bytes == keep.size() + changed.size() + add.size());
  CHECK(size.target == details.update.target);
  // Planned, never downloaded; the game folder byte for byte as it was, and the measure folder gone.
  CHECK(world.cdn.Count(Kind::kChunk) == 0 && world.cdn.Count(Kind::kMeta) > 0);
  CHECK(!fs::exists(measure_folder) && Snapshot(root) == before);
  CHECK(world.Logged("details size result=measured changed=2 removed=0"));

  // Measured again: the same answer, and GOG is asked nothing.
  world.cdn.ClearRequests();
  const auto again = world.Measured(kBase);
  CHECK(again.measured && again.download_bytes == size.download_bytes && again.staged_bytes == size.staged_bytes);
  CHECK(again.install_bytes == size.install_bytes && again.target == size.target);
  CHECK(world.cdn.requests().empty());
  CHECK(world.Logged("details size result=measured changed=2 removed=0 cached=1"));

  // GOG moves on to C: once a check names it, the size is C's, planned again.
  constexpr char kBuildC[] = "1000000000000000303";
  b.build_id = kBuildC;
  b.depots[0].files.push_back({"DepotFile", "data\more.dat", RandomBytes(5 * 1024, 65)});
  Publish(world.cdn, b);
  const auto moved = world.Again(kBase);
  CHECK(moved.update.newer && moved.update.target == gog::UpdateTarget(kBuildC, 2));
  world.cdn.ClearRequests();
  const auto c = world.Measured(kBase);
  CHECK(c.measured && c.target == moved.update.target && world.cdn.Count(Kind::kMeta) > 0);
  CHECK(c.staged_bytes == changed.size() + add.size() + 5 * 1024);
  CHECK(!fs::exists(measure_folder) && Snapshot(root) == before);

  // An update under way: not measured (nor failed), and its folder left alone.
  WriteText(root / installs::kUpdateDirName / installs::kUpdateStateName, "{\"phase\":\"staging\"}\n");
  const auto updating = world.Measured(kBase);
  CHECK(!updating.measured && !updating.failed && fs::exists(root / installs::kUpdateDirName / installs::kUpdateStateName));
  CHECK(world.Logged("details size result=skipped changed=0 removed=0 why=updating"));
  fs::remove_all(root / installs::kUpdateDirName);
  CHECK(Snapshot(root) == before);

  // Not installed, or no folder: not measured.
  world.Install(kBase, std::nullopt);
  const auto gone = world.Measured(kBase);
  CHECK(!gone.measured && !gone.failed);
  // Only an update's size, of a GOG game.
  CHECK(!world.source->Measure({GameId::Gog(kBase), "public", "english", false}, 1));
  CHECK(!world.source->Measure({GameId::Steam(220), {}, {}, true}, 1));
  CHECK(!world.source->Measure({GameId::Gog(kBase), {}, {}, true}, 0));
  CHECK(Snapshot(root) == before);
  world.CheckLogs();
}

// A newer generation drops a measure still to come.
void InvalidateDropsAMeasure() {
  World world(true);
  world.cdn.SetOwned("{\"owned\":[1900000001]}");
  Publish(world.cdn, SyntheticAt(kBuildB));
  const auto root = world.user / "games" / "gog-measure-dropped";
  fs::create_directories(root);
  world.Install(kBase, GogRecord(kBuildA), root.string());
  auto gate = std::make_shared<Gate>();
  world.cdn.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind == Kind::kBuilds) {
      gate->Enter();
      const std::atomic<bool> no_cancel{false};
      gate->Wait(no_cancel);
    }
    return std::nullopt;
  });
  CHECK(world.source->Measure({GameId::Gog(kBase), {}, {}, true}, 1));
  // Asked twice: queued once.
  CHECK(world.source->Measure({GameId::Gog(kBase), {}, {}, true}, 1));
  gate->WaitEntered(1);
  world.source->Invalidate(2);
  // An earlier generation's request is refused.
  CHECK(!world.source->Measure({GameId::Gog(kBase), {}, {}, true}, 1));
  gate->Open();
  std::this_thread::sleep_for(50ms);
  CHECK(world.source->PumpMeasured().empty());
  // Shutdown waits for the worker: the measure folder is gone with it.
  world.source->Shutdown();
  CHECK(world.source->PumpMeasured().empty());
  CHECK(!fs::exists(root / ".playden-gog" / "measure"));
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  g_fixtures = argv[1];
  g_scratch = argv[2];
  fs::remove_all(g_scratch);
  fs::create_directories(g_scratch);
  Gen2SizesCountTheDependencies();
  Gen2SizesCountTheRedists();
  OwnedDlcCountsOnlyWhenOwned();
  Gen1AndNoBuild();
  NetworkFailureFails();
  UnexpectedAnswersSayWhy();
  InvalidateDropsALateAnswer();
  UpdatesFromTheRecord();
  CheckCountsInstalledGames();
  CheckAnswersAreNeverDropped();
  MeasureAnUpdate();
  InvalidateDropsAMeasure();
  fs::remove_all(g_scratch);
  std::puts("gog_details: ok");
  return 0;
}
