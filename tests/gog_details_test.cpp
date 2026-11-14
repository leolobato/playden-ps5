// SPDX-License-Identifier: GPL-3.0-or-later
// GOG's details source (stores/gog_details) against the stub CDN (gog_cdn_stub.h): what installing a game takes. The
// Elder Scrolls: Arena's recorded public meta with DOSBox from the recorded public dependency store, a synthetic game
// naming __redist installers from the same store, a synthetic game with a DLC owned or not, Monkey Island 2's recorded
// gen 1 build and repository with its redists and language_setup from the same store, no Windows build, the network
// failing, and Invalidate dropping an answer that comes late.
// Usage: gog_details_test <fixtures/gog> <scratch folder>.

#include "gog/session.h"
#include "gog/session_store.h"
#include "gog_cdn_stub.h"
#include "gog_stub_transport.h"
#include "gog_synthetic_game.h"
#include "stores/gog_details.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace playden;
using namespace gog_test;
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
  int transports = 0;
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
  // No line names the account, a token or the code.
  void CheckLogs() {
    std::lock_guard<std::mutex> lock(log_mutex);
    CHECK(!logs.empty());
    for (const auto& line : logs) {
      CHECK(line.rfind("details result=", 0) == 0);
      CHECK(line.find(kUserId) == std::string::npos && line.find(kUsername) == std::string::npos);
      CHECK(line.find(AccessText(1)) == std::string::npos && line.find("http") == std::string::npos);
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
  fs::remove_all(g_scratch);
  std::puts("gog_details: ok");
  return 0;
}
