// SPDX-License-Identifier: GPL-3.0-or-later
// GOG's install runner (stores/gog_install) in a real install service, on two folders standing for console storage
// and a USB drive: a synthetic gen 2 game from the stub CDN (gog_cdn_stub.h, gog_synthetic_game.h) with its
// goggame-<id>.info and .script, a GOG session on the scripted auth stub (gog_stub_transport.h). Install to the end,
// pause and resume, a restart, cancel, signed out and revoked sessions, uninstall, a move to the USB drive, free space,
// a synthetic gen 1 game (ranges of a main.bin) to the end, moved and uninstalled, a game with __redist installers
// installed, moved and uninstalled, and updates from build A to build B (waiting for the game, cancel, pause, restarts
// while staging and while putting files in place, signed out, uninstall and move in the middle, the same build), and
// Manage DLC's changes as updates (a DLC turned on and off, with a newer build, interrupted, restarted, a staging of
// another choice, no owned list, signed out, an unowned DLC, uninstalled in the middle). Built twice: with the host's
// walks, and with the console's walk by path.
// Usage: gog_install_runner_test <fixtures/gog> <scratch folder>.

#include "gog/dependencies.h"
#include "gog/info_file.h"
#include "gog/plan_file.h"
#include "gog/session.h"
#include "gog/session_store.h"
#include "gog_cdn_stub.h"
#include "gog_stub_transport.h"
#include "gog_synthetic_game.h"
#include "installs/game_move.h"
#include "installs/install_record.h"
#include "installs/install_service.h"
#include "stores/gog_install.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
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
using installs::InstallJobState;
using installs::InstallService;
using installs::InstallSnapshot;
using installs::InstallVolume;
using stores::GogInstallRunner;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string g_fixtures;
fs::path g_scratch;
int g_cases = 0;

const GameId kGame = GameId::Gog(kBase);
const std::string kExe = RandomBytes(150 * 1024, 21);
const std::string kData = RandomBytes(3 * 64 * 1024 + 999, 22);
const std::string kMusic = RandomBytes(40 * 1024, 23);
const std::string kDefaults = "[defaults]\nvolume=7\n";
// The script: an INI key naming the game folder as Wine sees it, and the support folder's save copied into the game.
const std::string kScript = R"({"actions":[
 {"install":{"action":"setIni","arguments":{"filename":"{app}\\synthetic.cfg","keyName":"path","keyType":"string","keyValue":"{app}\\data","section":"system","utf8":false}},"languages":["*"],"name":"ini"},
 {"install":{"action":"supportData","arguments":{"overwrite":false,"source":"{supportDir}/save","target":"{app}/save","type":"folder"}},"languages":["*"],"name":"copySave"}
],"productId":"1900000001"})";

bool Exists(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0;
}

std::string Text(const fs::path& path) { return Exists(path) ? ReadFile(path.string()) : std::string(); }

void WaitFor(const std::function<bool()>& done, const char* what) {
  const auto until = std::chrono::steady_clock::now() + 30s;
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) {
      std::fprintf(stderr, "timed out waiting for %s\n", what);
      std::exit(1);
    }
    std::this_thread::sleep_for(2ms);
  }
}

Game SyntheticGame() {
  Game game;
  GameDepot main;
  main.files.push_back({"DepotFile", "bin\\Game.exe", kExe, {50 * 1024, 50 * 1024, 50 * 1024}});
  main.files.push_back({"DepotFile", "data\\level.dat", kData, {64 * 1024, 64 * 1024, 64 * 1024, 999}});
  main.files.push_back({"DepotFile", "Music/theme.ogg", kMusic});
  main.files.push_back({"DepotFile", "save\\defaults.cfg", kDefaults, {}, true});  // a support file
  main.files.push_back({"DepotDirectory", "logs", ""});
  game.depots.push_back(main);
  GameDepot gog;
  gog.languages = {"en-US"};
  gog.gog = true;
  gog.files.push_back({"DepotFile", "goggame-1900000001.info", ReadFile(g_fixtures + "/goggame-1900000001.info")});
  gog.files.push_back({"DepotFile", "GOGGAME-1900000001.SCRIPT", kScript});
  game.depots.push_back(gog);
  return game;
}

// B, a newer build of the same game: level.dat changed, theme.ogg dropped (its folder with it), new.dat added, and an
// info file whose primary task is another program.
const std::string kDataB = RandomBytes(2 * 64 * 1024 + 500, 24);
const std::string kNewFile = RandomBytes(30 * 1024, 25);
const std::string kBuildB = "1000000000000000202";

std::string InfoB() {
  auto text = ReadFile(g_fixtures + "/goggame-1900000001.info");
  const std::string from = "\"path\": \"SyntheticLauncher.exe\"";
  const auto at = text.find(from);
  CHECK(at != std::string::npos);
  text.replace(at, from.size(), "\"path\": \"bin/SyntheticLauncherB.exe\"");
  return text;
}

Game SyntheticGameB() {
  auto game = SyntheticGame();
  game.build_id = kBuildB;
  auto& files = game.depots[0].files;
  files[1] = {"DepotFile", "data\\level.dat", kDataB, {64 * 1024, 64 * 1024, 500}};
  files.erase(files.begin() + 2);  // Music/theme.ogg
  files.push_back({"DepotFile", "data\\new.dat", kNewFile});
  game.depots[1].files[0].bytes = InfoB();
  return game;
}

int ChunkCount(const Published& published) {
  int count = 0;
  for (const auto& file : published.chunks) count += static_cast<int>(file.second.size());
  return count;
}

std::string WindowsApp(const std::string& root) {
  std::string path = "Z:";
  for (const char c : root) path += c == '/' ? '\\' : c;
  return path;
}

// One case's world: the two volumes, the CDN, the session and its auth stub, the runner and the service.
struct World {
  fs::path base, internal, usb, user;
  CdnStub cdn;
  Stub auth;
  std::unique_ptr<gog::SessionStore> store;
  std::unique_ptr<gog::Session> session;
  std::atomic<std::int64_t> clock{100000};
  Published published;

  std::mutex log_mutex;
  std::vector<std::string> logs;
  std::atomic<std::uint64_t> free{1ull << 40};
  int parallel = 4;

  // Chunk requests past `hold_after` wait for their cancel flag (they never reach the CDN); past `revoke_after`, GOG
  // refuses secure links (401) and chunks (403).
  std::mutex hold_mutex;
  int hold_after = -1;
  int revoke_after = -1;
  int served = 0;
  int held = 0;
  int in_flight = 0;
  int waiting = 0;  // chunk requests held right now
  std::atomic<bool> revoked{false};
  // GOG's content system (builds) or its CDN (chunks) out of reach: every such request fails to connect.
  std::atomic<bool> builds_down{false};
  std::atomic<bool> chunks_down{false};
  // GOG's owned list out of reach: it answers 503.
  std::atomic<bool> owned_down{false};

  // What the apply gate answers; while `block_scripts`, an update's script step waits (its files are in place, its
  // record not yet).
  std::atomic<installs::ApplyAnswer> gate{installs::ApplyAnswer::kApply};
  std::atomic<bool> block_scripts{false};
  std::atomic<bool> scripts_blocked{false};

  std::unique_ptr<GogInstallRunner> runner;
  std::unique_ptr<installs::GameMover> mover;
  std::unique_ptr<InstallService> service;

  World() {
    base = g_scratch / ("case-" + std::to_string(++g_cases));
    fs::remove_all(base);
    internal = base / "data/playden-games";
    usb = base / "mnt/usb0/playden/games";
    user = base / "user";
    fs::create_directories(internal);
    fs::create_directories(usb);
    fs::create_directories(user);
    CHECK(::chmod(user.c_str(), 0755) == 0);
    gog::SessionStore::Options options;
    options.user_root = user.string();
    store = std::make_unique<gog::SessionStore>(options);
    session = std::make_unique<gog::Session>(*store, auth.Transport(), [this] { return clock.load(); });
    cdn.SetOwned("{\"owned\":[1900000001,1900000002]}");
    cdn.SetSecureLink(ReadFile(g_fixtures + "/secure-link-200.json"));
    cdn.SetSecureLinkV1(ReadFile(g_fixtures + "/secure-link-gen1-200.json"));
    cdn.SetFault([this](const Request& request) -> std::optional<Fault> {
      if ((builds_down.load() && request.kind == Kind::kBuilds) || (chunks_down.load() && request.kind == Kind::kChunk)) {
        Fault fault;
        fault.response = Failure(HttpError::kConnectionFailed);
        return fault;
      }
      if (owned_down.load() && request.kind == Kind::kOwned) {
        Fault fault;
        fault.response = Answer(503, "");
        return fault;
      }
      if (!revoked.load()) return std::nullopt;
      Fault fault;
      if (request.kind == Kind::kSecureLink) fault.response = Answer(401, ReadFile(g_fixtures + "/secure-link-401.json"));
      else if (request.kind == Kind::kChunk) fault.response = Answer(403, "");
      else return std::nullopt;
      return fault;
    });
    published = Publish(cdn, SyntheticGame());
  }
  ~World() {
    service.reset();
    fs::remove_all(base);
  }

  // A saved session restored: GOG answers the refresh with synthetic tokens.
  void SignIn(int tokens = 1) {
    if (session->signed_in()) return;
    auth.Script(Endpoint::kToken, Tokens(tokens));
    auth.Script(Endpoint::kUser, Answer(200, ReadFile(g_fixtures + "/user-data.json")));
    const std::atomic<bool> no_cancel{false};
    const auto epoch = session->BeginSignIn();
    const auto outcome = session->Exchange("SyntheticCode-runner-0123456789", epoch, no_cancel);
    CHECK(outcome.kind == gog::ExchangeOutcome::Kind::kSignedIn);
  }

  gog::Transport Transport() {
    auto inner = cdn.Transport();
    return [this, inner](const HttpRequest& request, const std::atomic<bool>& cancel) mutable {
      const bool chunk = request.url.find("/content-system/v2/store/") != std::string::npos;
      if (!chunk) return inner(request, cancel);
      {
        std::unique_lock<std::mutex> lock(hold_mutex);
        if (revoke_after >= 0 && served >= revoke_after) revoked = true;
        if (hold_after >= 0 && served >= hold_after) {
          ++held;
          ++waiting;
          while (hold_after >= 0 && served >= hold_after && !cancel.load()) {
            lock.unlock();
            std::this_thread::sleep_for(1ms);
            lock.lock();
          }
          --waiting;
          if (cancel.load()) return Failure(HttpError::kCanceled);
        }
        ++served;
        ++in_flight;
      }
      auto response = inner(request, cancel);
      std::lock_guard<std::mutex> lock(hold_mutex);
      --in_flight;
      return response;
    };
  }

  void Hold(int after) {
    std::lock_guard<std::mutex> lock(hold_mutex);
    hold_after = after;
    held = 0;
  }
  // A pause or cancel reached every held request (the job's state changes before its run stops).
  void WaitReleased() {
    WaitFor([&] {
      std::lock_guard<std::mutex> lock(hold_mutex);
      return waiting == 0;
    }, "the held chunks to stop");
  }
  // Every chunk before the hold answered and journaled (the job's progress counts it), and a later one waiting.
  void WaitHeld() {
    WaitFor([&] {
      {
        std::lock_guard<std::mutex> lock(hold_mutex);
        if (held == 0 || served != hold_after || in_flight != 0) return false;
      }
      std::map<std::string, std::uint64_t> sizes;
      for (const auto& file : published.chunks)
        for (const auto& chunk : file.second) sizes[chunk.compressed_md5] = chunk.compressed_size;
      std::uint64_t answered = 0;
      for (const auto& request : cdn.requests())
        if (request.kind == Kind::kChunk) answered += sizes[request.hash];
      const auto snapshot = service->Snapshot();
      const auto* job = Job(snapshot);
      return job && job->done_bytes >= answered;
    }, "a held chunk");
  }

  void StartRunner() {
    runner = std::make_unique<GogInstallRunner>(
        *session, [this] { return Transport(); }, [] { return std::string("english"); },
        [this](const std::string& line) {
          if (line.find("gog update ") == 0 && line.find(" step=script") != std::string::npos)
            while (block_scripts.load()) {
              scripts_blocked = true;
              std::this_thread::sleep_for(1ms);
            }
          std::lock_guard<std::mutex> lock(log_mutex);
          logs.push_back(line);
        },
        [this](const std::string&) { return std::optional<std::uint64_t>(free.load()); });
    GogInstallRunner::Tuning tuning;
    tuning.parallel = parallel;
    tuning.pause = [](std::uint32_t, const std::atomic<bool>&) {};
    tuning.now_seconds = [] { return std::int64_t{1800000000}; };
    runner->SetTuning(tuning);
  }

  void Start(std::chrono::milliseconds retry = 10min) {
    StartRunner();
    mover = std::make_unique<installs::GameMover>();
    service = std::make_unique<InstallService>(
        InstallService::Runners{{Store::kGog, runner.get()}},
        std::vector<InstallVolume>{{InstallService::kPrimaryVolumeId, "Console storage", internal.string(), true},
                                   {"usb0", "USB drive", usb.string(), true}});
    service->SetSpaceQuery([](const std::string&) { return std::optional<std::uint64_t>(1ull << 40); });
    service->SetRetryDelays(retry, retry);
    service->SetMoveRunner(mover.get());
    service->SetApplyGate([this](GameId) { return gate.load(); });
  }
  // Playden closes and starts again: the service and the runner are made again, the session stays.
  void Restart() {
    service.reset();
    mover.reset();
    runner.reset();
    Start();
  }

  fs::path Root() const { return internal / "gog-1900000001"; }

  const installs::InstallJobView* Job(const InstallSnapshot& snapshot) const {
    for (const auto& job : snapshot.jobs)
      if (job.id == kGame) return &job;
    return nullptr;
  }
  std::optional<installs::InstalledGame> Installed() {
    for (const auto& game : service->Snapshot().installed)
      if (game.id == kGame) return game;
    return std::nullopt;
  }
  void WaitInstalled() {
    WaitFor([&] {
      const auto snapshot = service->Snapshot();
      if (const auto* job = Job(snapshot); job && job->state == InstallJobState::kFailed) {
        std::fprintf(stderr, "job failed: %s\n", job->failure.c_str());
        std::exit(1);
      }
      return !Job(snapshot) && Installed().has_value();
    }, "the install");
  }
  void WaitState(InstallJobState state, const std::string& failure) {
    WaitFor([&] {
      const auto snapshot = service->Snapshot();
      const auto* job = Job(snapshot);
      return job && job->state == state && job->failure == failure;
    }, failure.c_str());
  }

  // Each chunk reached the CDN at most once.
  void CheckNoChunkTwice() const {
    std::map<std::string, int> counts;
    for (const auto& request : cdn.requests())
      if (request.kind == Kind::kChunk) ++counts[request.hash];
    CHECK(static_cast<int>(counts.size()) == ChunkCount(published));
    for (const auto& entry : counts) CHECK(entry.second == 1);
  }

  // The runner's lines name no account, token, signed link or address.
  void CheckLogs() {
    std::lock_guard<std::mutex> lock(log_mutex);
    CHECK(!logs.empty());
    for (const auto& line : logs)
      for (const std::string& secret :
           {kUsername, kUserId, std::string("synthetic-access-"), std::string("synthetic-refresh-"),
            std::string("SyntheticCode"), std::string("signature"), std::string("wsSecret"), std::string("nva="),
            std::string("https://"), std::string("example.invalid")})
        if (line.find(secret) != std::string::npos) {
          std::fprintf(stderr, "log line names %s: %s\n", secret.c_str(), line.c_str());
          std::exit(1);
        }
  }
  bool Logged(const std::string& text) {
    std::lock_guard<std::mutex> lock(log_mutex);
    for (const auto& line : logs)
      if (line.find(text) != std::string::npos) return true;
    return false;
  }

  // A complete install at `root`: files, record, launches, the script's work, and no journal.
  void CheckInstalledAt(const fs::path& root, std::uint64_t redist_bytes = 0) {
    CHECK(Text(root / "bin/Game.exe") == kExe && Text(root / "data/level.dat") == kData &&
          Text(root / "Music/theme.ogg") == kMusic);
    CHECK(Exists(root / ".playden-files") && Exists(root / ".playden-gog/files"));
    CHECK(!Exists(root / ".playden-download"));
    CHECK(!Exists(root / ".playden-install.json.tmp"));
    const auto text = Text(root / installs::kInstallRecordName);
    CHECK(text.find("\"store\":\"gog\"") != std::string::npos && text.find("\"game\":\"1900000001\"") != std::string::npos);
    const auto record = installs::ReadInstallRecord((root / installs::kInstallRecordName).string());
    CHECK(record && record->gog);
    const std::uint64_t size = kExe.size() + kData.size() + kMusic.size() + kDefaults.size() +
                               ReadFile(g_fixtures + "/goggame-1900000001.info").size() + kScript.size() +
                               redist_bytes;
    CHECK(record->install_bytes == size);
    CHECK(record->gog->build_id == "1000000000000000101" && record->gog->generation == 2);
    // The meta's owned DLC counts as installed with the game, even without depots of its own.
    CHECK((record->gog->products == std::vector<std::uint64_t>{kBase, kDlc}));
    CHECK(record->gog->language == "en-US" && record->language == "english");
    CHECK(record->gog->meta_id == published.meta_id);
    const auto info = gog::ParseInfo(ReadFile(g_fixtures + "/goggame-1900000001.info"));
    CHECK(info);
    const auto expected = gog::LaunchesFromInfo(*info);
    CHECK(!expected.empty() && record->launches.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
      CHECK(record->launches[index].executable == expected[index].executable);
      CHECK(record->launches[index].arguments == expected[index].arguments);
      CHECK(record->launches[index].working_directory == expected[index].working_directory);
      CHECK(record->launches[index].label == expected[index].description);
    }
    // The support file stays in .playden-gog/support; the script copied it into the game.
    CHECK(Text(root / ".playden-gog/support/1900000001/save/defaults.cfg") == kDefaults);
    CHECK(!Exists(root / "save/defaults.cfg") || Text(root / "save/defaults.cfg") == kDefaults);
  }

  // ---- Updates ----

  // A installed through the queue, signed in.
  void InstallA() {
    SignIn();
    Start();
    service->Install(kGame);
    WaitInstalled();
    CheckInstalledAt(Root());
  }
  // B published: `published` is B's from here (WaitHeld counts its chunks), and the CDN's requests start afresh.
  Published PublishB() {
    a_meta = published.meta_id;
    published = Publish(cdn, SyntheticGameB());
    cdn.ClearRequests();
    return published;
  }
  std::string a_meta;
  int Served() {
    std::lock_guard<std::mutex> lock(hold_mutex);
    return served;
  }
  // install.log's finished updates so far.
  std::size_t Finishes() {
    std::lock_guard<std::mutex> lock(log_mutex);
    return static_cast<std::size_t>(std::count_if(logs.begin(), logs.end(), [](const std::string& line) {
      return line.find("gog update game=gog-1900000001 step=finish result=ok") == 0;
    }));
  }
  // An update finished after `before` others: no job, the game listed.
  void WaitFinished(std::size_t before) {
    WaitFor([&] {
      const auto snapshot = service->Snapshot();
      if (const auto* job = Job(snapshot); job && job->state == InstallJobState::kFailed) {
        std::fprintf(stderr, "update failed: %s\n", job->failure.c_str());
        std::exit(1);
      }
      return !Job(snapshot) && Finishes() > before && Installed().has_value();
    }, "the update");
  }
  // The update finished: no job, and the listed record names B.
  void WaitUpdated() {
    WaitFor([&] {
      const auto snapshot = service->Snapshot();
      if (const auto* job = Job(snapshot); job && job->state == InstallJobState::kFailed) {
        std::fprintf(stderr, "update failed: %s\n", job->failure.c_str());
        std::exit(1);
      }
      const auto game = Installed();
      return !Job(snapshot) && game && game->record && game->record->gog && game->record->gog->build_id == kBuildB;
    }, "the update");
  }
  std::optional<installs::UpdatePhase> Phase() {
    const auto state = installs::ReadUpdateState(Root().string());
    return state ? std::optional<installs::UpdatePhase>(state->phase) : std::nullopt;
  }
  // The game as A left it: its files, its record.
  void CheckA() {
    const auto root = Root();
    CHECK(Text(root / "data/level.dat") == kData && Text(root / "Music/theme.ogg") == kMusic);
    CHECK(Text(root / "bin/Game.exe") == kExe && !Exists(root / "data/new.dat"));
    const auto record = installs::ReadInstallRecord((root / installs::kInstallRecordName).string());
    CHECK(record && record->gog && record->gog->build_id == "1000000000000000101" && record->gog->meta_id == a_meta);
    CHECK(Text(root / ".playden-files").find("f\tMusic/theme.ogg\n") != std::string::npos);
    CHECK(!Exists(root / installs::kFilesChangedName));
  }
  // The game as B: files, file list, plan, record and launches from B's info file; the work folder gone.
  void CheckB() {
    const auto root = Root();
    CHECK(Text(root / "bin/Game.exe") == kExe && Text(root / "data/level.dat") == kDataB);
    CHECK(Text(root / "data/new.dat") == kNewFile && !Exists(root / "Music"));
    CHECK(Text(root / "goggame-1900000001.info") == InfoB() && Text(root / "save/defaults.cfg") == kDefaults);
    CHECK(!Exists(root / installs::kUpdateDirName) && Exists(root / installs::kFilesChangedName));
    const auto listed = Text(root / ".playden-files");
    CHECK(listed.find("f\tdata/new.dat\n") != std::string::npos && listed.find("Music") == std::string::npos);
    const auto header = gog::ReadPlanHeader((root / gog::kPlanPath).string());
    CHECK(header && header->build_id == kBuildB && header->meta_id == published.meta_id);
    CHECK(Exists(root / ".playden-gog/meta" / published.meta_id));
    const auto record = installs::ReadInstallRecord((root / installs::kInstallRecordName).string());
    CHECK(record && record->gog && record->gog->build_id == kBuildB && record->gog->meta_id == published.meta_id);
    CHECK((record->gog->products == std::vector<std::uint64_t>{kBase, kDlc}));
    CHECK(record->gog->language == "en-US" && record->language == "english");
    CHECK(record->install_bytes == kExe.size() + kDataB.size() + kNewFile.size() + kDefaults.size() +
                                       InfoB().size() + kScript.size());
    const auto info = gog::ParseInfo(InfoB());
    CHECK(info);
    const auto expected = gog::LaunchesFromInfo(*info);
    CHECK(!expected.empty() && expected[0].executable == "bin/SyntheticLauncherB.exe");
    CHECK(record->launches.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
      CHECK(record->launches[index].executable == expected[index].executable &&
            record->launches[index].working_directory == expected[index].working_directory);
    const auto game = Installed();
    CHECK(game && game->record && game->record->gog && game->record->gog->build_id == kBuildB);
    CHECK(game->install_bytes == record->install_bytes && !game->record->launches.empty());
    CHECK(game->record->launches[0].executable == "bin/SyntheticLauncherB.exe");
    // The script ran again on the new files: its INI key is there.
    CHECK(Text(root / "synthetic.cfg").find("path=" + WindowsApp(root.string()) + "\\data") != std::string::npos);
    CHECK(!service->Locked(kGame));
  }
  // The chunks the update needed (B's changed files), each fetched once, and nothing else.
  void CheckStagedOnce() const {
    std::map<std::string, int> wanted;
    for (const char* path : {"data\\level.dat", "data\\new.dat", "goggame-1900000001.info"})
      for (const auto& chunk : published.chunks.at(path)) wanted[chunk.compressed_md5] = 0;
    for (const auto& request : cdn.requests())
      if (request.kind == Kind::kChunk) {
        CHECK(wanted.count(request.hash));
        ++wanted[request.hash];
      }
    for (const auto& entry : wanted) CHECK(entry.second == 1);
  }
};

// The queue to the end: record, launches, file list, the script's INI key and the support copy; the snapshot lists it.
void InstallsThroughTheQueue() {
  World world;
  world.SignIn();
  world.Start();
  world.service->Install(kGame);
  world.WaitInstalled();
  const auto root = world.Root();
  world.CheckInstalledAt(root);
  CHECK(Text(root / "save/defaults.cfg") == kDefaults);
  const auto ini = Text(root / "synthetic.cfg");
  CHECK(ini.find("[system]") != std::string::npos);
  CHECK(ini.find("path=" + WindowsApp(root.string()) + "\\data") != std::string::npos);
  const auto game = world.Installed();
  CHECK(game && game->root == root.string() && game->volume_id == InstallService::kPrimaryVolumeId);
  CHECK(game->record && game->record->gog && !game->record->launches.empty());
  CHECK(game->install_bytes == game->record->install_bytes);
  CHECK(world.Logged("gog install game=gog-1900000001 step=plan result=ok language=english"));
  CHECK(world.Logged("gog install game=gog-1900000001 step=download result=ok files=6"));
  CHECK(world.Logged("gog install game=gog-1900000001 step=script product=1900000001 result=ok"));
  CHECK(world.Logged("gog install game=gog-1900000001 step=record result=ok"));
  world.CheckNoChunkTwice();
  world.CheckLogs();
  std::printf("installs through the queue ok\n");
}

// Pause after some chunks, then Resume: it finishes, and no chunk is fetched twice. Meanwhile Downloads names GOG's
// endpoint, never a host.
void PausesAndResumes() {
  World world;
  world.parallel = 2;
  world.SignIn();
  world.Hold(4);
  world.Start();
  world.service->Install(kGame);
  world.WaitHeld();
  {
    const auto snapshot = world.service->Snapshot();
    const auto* job = world.Job(snapshot);
    CHECK(job && job->state == InstallJobState::kRunning && job->server_name == "GOG · fastly");
    CHECK(job->done_bytes > 0 && job->done_bytes < job->total_bytes);
  }
  world.service->Pause(kGame);
  world.WaitState(InstallJobState::kPaused, "");
  world.WaitReleased();
  CHECK(Exists(world.Root() / ".playden-download/gog-journal"));
  world.Hold(-1);
  world.service->Resume(kGame);
  world.WaitInstalled();
  world.CheckInstalledAt(world.Root());
  world.CheckNoChunkTwice();
  world.CheckLogs();
  std::printf("pauses and resumes ok\n");
}

// Playden closes mid-download and starts again: the job comes back and carries on from the journal by itself. A
// paused job comes back paused and finishes on Resume. No chunk is fetched twice.
void RestartsMidDownload() {
  {
    World world;
    world.SignIn();
    world.Hold(5);
    world.Start();
    world.service->Install(kGame);
    world.WaitHeld();
    // Closed while a chunk waits: nothing in flight is cut short.
    world.service.reset();
    world.Hold(-1);
    world.Restart();
    world.WaitInstalled();
    world.CheckInstalledAt(world.Root());
    world.CheckNoChunkTwice();
  }
  {
    World world;
    world.parallel = 1;
    world.SignIn();
    world.Hold(3);
    world.Start();
    world.service->Install(kGame);
    world.WaitHeld();
    world.service->Pause(kGame);
    world.WaitState(InstallJobState::kPaused, "");
    world.WaitReleased();
    world.Hold(-1);
    world.Restart();
    world.WaitState(InstallJobState::kPaused, "Interrupted. Resume to continue.");
    world.service->Resume(kGame);
    world.WaitInstalled();
    world.CheckInstalledAt(world.Root());
    world.CheckNoChunkTwice();
    world.CheckLogs();
  }
  std::printf("restarts mid-download ok\n");
}

// Cancel mid-download deletes the folder.
void CancelsMidDownload() {
  World world;
  world.SignIn();
  world.Hold(3);
  world.Start();
  world.service->Install(kGame);
  world.WaitHeld();
  CHECK(Exists(world.Root() / ".playden-download"));
  world.service->Cancel(kGame);
  WaitFor([&] {
    const auto snapshot = world.service->Snapshot();
    return !world.Job(snapshot) && snapshot.uninstalling.empty() && !Exists(world.Root());
  }, "the canceled folder to go");
  CHECK(!world.Installed());
  std::printf("cancels mid-download ok\n");
}

// Signed out at start: the job waits for a sign-in, keeps its place and starts after RetryWaiting.
void WaitsForSignIn() {
  World world;
  world.Start();
  world.service->Install(kGame);
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForSignIn);
  CHECK(world.cdn.requests().empty());
  CHECK(world.Logged("gog install game=gog-1900000001 step=plan result=signed_out"));
  world.SignIn();
  world.service->RetryWaiting(Store::kGog);
  world.WaitInstalled();
  world.CheckInstalledAt(world.Root());
  world.CheckLogs();
  std::printf("waits for sign-in ok\n");
}

// GOG out of reach when the download would start (the token's refresh gets no answer): the job waits to reach GOG,
// keeps its session, and starts once it answers.
void WaitsToReachGog() {
  World world;
  world.SignIn();
  world.clock += 7200;  // the access token has expired: the next one needs a refresh
  world.auth.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
  world.Start();
  world.service->Install(kGame);
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForGog);
  CHECK(world.session->signed_in() && world.cdn.requests().empty());
  CHECK(world.Logged("gog install game=gog-1900000001 step=plan result=network"));
  world.auth.Script(Endpoint::kToken, Tokens(2));
  world.service->RetryWaiting(Store::kGog);
  world.WaitInstalled();
  world.CheckInstalledAt(world.Root());
  world.CheckLogs();
  std::printf("waits to reach gog ok\n");
}

// The builds fetch gets no answer while the access token is still good (Playden started offline within the token's
// hour): the job waits to reach GOG, not failed, and installs once GOG answers.
void WaitsWhenBuildsUnreachable() {
  World world;
  world.SignIn();
  world.builds_down = true;
  world.Start();
  world.service->Install(kGame);
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForGog);
  CHECK(world.Logged("gog install game=gog-1900000001 step=download result=network"));
  world.builds_down = false;
  world.service->RetryWaiting(Store::kGog);
  world.WaitInstalled();
  world.CheckInstalledAt(world.Root());
  world.CheckLogs();
  std::printf("waits when builds are out of reach ok\n");
}

// The CDN drops mid-download and every chunk fails its five tries: the job waits to reach GOG with its journal, and
// carries on from it once the CDN answers.
void WaitsWhenCdnDown() {
  World world;
  world.parallel = 1;
  world.SignIn();
  world.chunks_down = true;
  world.Start();
  world.service->Install(kGame);
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForGog);
  CHECK(world.session->signed_in());
  CHECK(Exists(world.Root() / ".playden-download/gog-journal"));
  CHECK(world.Logged("gog install game=gog-1900000001 step=download result=network"));
  world.chunks_down = false;
  world.service->RetryWaiting(Store::kGog);
  world.WaitInstalled();
  world.CheckInstalledAt(world.Root());
  world.CheckLogs();
  std::printf("waits when the cdn is down ok\n");
}

// GOG revokes the session mid-download: the job waits with the sign-in text, keeps what it downloaded, and carries on
// after the next sign-in.
void WaitsAfterRevokedMidDownload() {
  World world;
  world.parallel = 1;
  world.SignIn();
  {
    std::lock_guard<std::mutex> lock(world.hold_mutex);
    world.revoke_after = 3;
  }
  world.auth.Script(Endpoint::kToken, InvalidGrant());
  world.Start();
  world.service->Install(kGame);
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForSignIn);
  CHECK(!world.session->signed_in());
  CHECK(Exists(world.Root() / ".playden-download/gog-journal"));
  CHECK(!ReadFile((world.Root() / ".playden-download/gog-journal").string()).empty());
  {
    std::lock_guard<std::mutex> lock(world.hold_mutex);
    world.revoke_after = -1;
  }
  world.revoked = false;
  world.SignIn(2);
  world.service->RetryWaiting(Store::kGog);
  world.WaitInstalled();
  world.CheckInstalledAt(world.Root());
  world.CheckLogs();
  std::printf("waits after a revoked session ok\n");
}

// Uninstall deletes the whole folder, .playden-gog and the support files included.
void UninstallRemovesEverything() {
  World world;
  world.SignIn();
  world.Start();
  world.service->Install(kGame);
  world.WaitInstalled();
  CHECK(Exists(world.Root() / ".playden-gog/support"));
  world.service->Uninstall(kGame);
  WaitFor([&] {
    const auto snapshot = world.service->Snapshot();
    return !world.Installed() && snapshot.uninstalling.empty();
  }, "the uninstall");
  CHECK(!Exists(world.Root()));
  CHECK(world.Logged("gog install-removal result=ok"));
  std::printf("uninstall removes everything ok\n");
}

// A move to the USB drive: the record and its launches read back from there, .playden-gog travels, the old folder goes.
void MovesToUsb() {
  World world;
  world.SignIn();
  world.Start();
  world.service->Install(kGame);
  world.WaitInstalled();
  CHECK(world.service->Move(kGame, "usb0").empty());
  WaitFor([&] {
    const auto game = world.Installed();
    return game && game->volume_id == "usb0" && !world.Job(world.service->Snapshot()) && !Exists(world.Root());
  }, "the move");
  const auto moved = world.usb / "gog-1900000001";
  const auto game = world.Installed();
  CHECK(game->root == moved.string() && game->record && game->record->gog && !game->record->launches.empty());
  world.CheckInstalledAt(moved);
  CHECK(Exists(moved / ".playden-gog/files") && Exists(moved / ".playden-gog/meta/" / world.published.meta_id));
  CHECK(!Exists(world.Root()));
  std::printf("moves to usb ok\n");
}

// The drive has too little room for the download: the job fails with the free-space sentence.
void FreeSpaceShort() {
  World world;
  world.SignIn();
  world.free = 1024;
  world.Start();
  world.service->Install(kGame);
  world.WaitState(InstallJobState::kFailed, "Not enough free space.");
  std::printf("free space short ok\n");
}

// A game whose default build is gen 1, through the queue to the end: its record names generation 1, the builds item's
// ID and repository.json's MD5, and its launches come from its goggame-<id>.info; .playden-files lists every placed
// path. A move to the USB drive keeps it playable, and uninstall removes everything.
void Gen1Installs() {
  World world;
  GameV1 game;
  GameDepotV1 main;
  main.files.push_back({"bin\\Game.exe", kExe});
  main.files.push_back({"data\\level.dat", kData});
  main.files.push_back({"Music/theme.ogg", kMusic});
  main.files.push_back({"1900000001/app/defaults.cfg", kDefaults, true});  // <product>/app/ support: the game folder
  GameFileV1 logs{"logs"};
  logs.directory = true;
  main.files.push_back(logs);
  game.depots.push_back(main);
  GameDepotV1 english;
  english.languages = {"English"};
  english.files.push_back({"goggame-1900000001.info", ReadFile(g_fixtures + "/goggame-1900000001.info")});
  game.depots.push_back(english);
  const auto published = PublishV1(world.cdn, game);
  world.SignIn();
  world.Start();
  world.service->Install(kGame);
  world.WaitInstalled();

  const auto check = [&](const fs::path& root) {
    CHECK(Text(root / "bin/Game.exe") == kExe && Text(root / "data/level.dat") == kData &&
          Text(root / "Music/theme.ogg") == kMusic && Text(root / "defaults.cfg") == kDefaults);
    CHECK(fs::is_directory(root / "logs") && !Exists(root / ".playden-download"));
    const auto record = installs::ReadInstallRecord((root / installs::kInstallRecordName).string());
    CHECK(record && record->gog);
    CHECK(record->gog->generation == 1 && record->gog->build_id == "15751");
    CHECK(record->gog->meta_id == published.repository_md5);
    CHECK((record->gog->products == std::vector<std::uint64_t>{kBase, kDlc}));
    CHECK(record->gog->language == "en-US" && record->language == "english");
    CHECK(record->install_bytes == kExe.size() + kData.size() + kMusic.size() + kDefaults.size() +
                                       ReadFile(g_fixtures + "/goggame-1900000001.info").size());
    const auto info = gog::ParseInfo(ReadFile(g_fixtures + "/goggame-1900000001.info"));
    CHECK(info);
    const auto expected = gog::LaunchesFromInfo(*info);
    CHECK(!expected.empty() && record->launches.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
      CHECK(record->launches[index].executable == expected[index].executable &&
            record->launches[index].working_directory == expected[index].working_directory);
    const auto listed = Text(root / ".playden-files");
    std::vector<std::string> lines;
    for (std::size_t at = 0; at < listed.size();) {
      const auto end = listed.find('\n', at);
      lines.push_back(listed.substr(at, end - at));
      at = end == std::string::npos ? listed.size() : end + 1;
    }
    std::sort(lines.begin(), lines.end());
    CHECK((lines == std::vector<std::string>{"d\tlogs", "f\tMusic/theme.ogg", "f\tbin/Game.exe", "f\tdata/level.dat",
                                            "f\tdefaults.cfg", "f\tgoggame-1900000001.info"}));
  };
  check(world.Root());
  const auto installed = world.Installed();
  CHECK(installed && installed->record && installed->record->gog && installed->record->gog->generation == 1);
  CHECK(world.Logged("gog install game=gog-1900000001 step=download result=ok files=5"));
  CHECK(world.Logged("gog install game=gog-1900000001 step=record result=ok"));
  CHECK(world.cdn.Count(Kind::kChunk) == 0 && world.cdn.Count(Kind::kRange) == 5);
  for (const auto& request : world.cdn.requests())
    CHECK(request.kind != Kind::kSecureLink || (request.generation == 1 && request.timestamp == kTimestampV1));
  world.CheckLogs();

  CHECK(world.service->Move(kGame, "usb0").empty());
  WaitFor([&] {
    const auto game = world.Installed();
    return game && game->volume_id == "usb0" && !world.Job(world.service->Snapshot()) && !Exists(world.Root());
  }, "the move");
  const auto moved = world.usb / "gog-1900000001";
  check(moved);
  CHECK(Exists(moved / ".playden-gog/meta" / published.repository_md5));
  const auto game_after = world.Installed();
  CHECK(game_after && game_after->root == moved.string() && game_after->record && !game_after->record->launches.empty());

  world.service->Uninstall(kGame);
  WaitFor([&] {
    const auto snapshot = world.service->Snapshot();
    return !world.Installed() && snapshot.uninstalling.empty();
  }, "the uninstall");
  CHECK(!Exists(moved) && !Exists(world.Root()));
  std::printf("gen 1 installs ok\n");
}

// A game naming two __redist installers (XNA, DirectX) from the public dependency store: after the install their
// files and redist.list sit in .playden-gog, the record counts them, a move to the USB drive carries them, and
// uninstall removes them with the game.
void RedistInstallsMovesAndUninstalls() {
  World world;
  world.cdn.SetOpenLink(ReadFile(g_fixtures + "/dependencies-open-link.json"));
  const std::string msi = RandomBytes(48 * 1024, 61);
  const std::string setup = RandomBytes(16 * 1024, 62);
  const std::string cab = RandomBytes(24 * 1024, 63);
  StoreDependency xna;
  xna.id = "XNA";
  xna.exe_path = "__redist/XNA/xnafx31_redist.msi";
  xna.arguments_json = "\"/qn\"";
  xna.files.push_back({"DepotFile", "__redist/XNA/xnafx31_redist.msi", msi});
  StoreDependency directx;
  directx.id = "DirectX";
  directx.exe_path = "__redist/DirectX/DXSETUP.exe";
  directx.arguments_json = "\"/silent\"";
  directx.files.push_back({"DepotFile", "__redist\\DirectX\\DXSETUP.exe", setup});
  directx.files.push_back({"DepotFile", "__redist\\DirectX\\Feb2010_XAudio_x86.cab", cab});
  PublishDependencyStore(world.cdn, {xna, directx});
  auto game = SyntheticGame();
  game.dependencies = {"XNA", "DirectX"};
  world.published = Publish(world.cdn, game);
  world.SignIn();
  world.Start();
  world.service->Install(kGame);
  world.WaitInstalled();

  const std::string list = "gog-redist\tv1\n"
                           "P\tXNA\t.playden-gog/redist/XNA/xnafx31_redist.msi\t/qn\n"
                           "P\tDirectX\t.playden-gog/redist/DirectX/DXSETUP.exe\t/silent\n";
  const auto check = [&](const fs::path& root) {
    world.CheckInstalledAt(root, msi.size() + setup.size() + cab.size());
    CHECK(Text(root / ".playden-gog/redist/XNA/xnafx31_redist.msi") == msi);
    CHECK(Text(root / ".playden-gog/redist/DirectX/DXSETUP.exe") == setup);
    CHECK(Text(root / ".playden-gog/redist/DirectX/Feb2010_XAudio_x86.cab") == cab);
    CHECK(Text(root / ".playden-gog/redist.list") == list);
    const auto read = gog::ReadRedistList(root.string());
    CHECK(read.status == gog::RedistList::Status::kOk && read.entries.size() == 2 && read.entries[0].package);
    const auto listed = Text(root / ".playden-files");
    CHECK(listed.find("f\t.playden-gog/redist/XNA/xnafx31_redist.msi\n") != std::string::npos);
    CHECK(listed.find("f\t.playden-gog/redist/DirectX/Feb2010_XAudio_x86.cab\n") != std::string::npos);
  };
  check(world.Root());
  CHECK(world.Logged("gog plan dependencies game_folder=0 redist=2 missing=0"));
  CHECK(world.Logged("gog install game=gog-1900000001 step=download result=ok files=9"));
  world.CheckLogs();

  CHECK(world.service->Move(kGame, "usb0").empty());
  WaitFor([&] {
    const auto moved = world.Installed();
    return moved && moved->volume_id == "usb0" && !world.Job(world.service->Snapshot()) && !Exists(world.Root());
  }, "the move");
  const auto moved = world.usb / "gog-1900000001";
  check(moved);

  world.service->Uninstall(kGame);
  WaitFor([&] {
    const auto snapshot = world.service->Snapshot();
    return !world.Installed() && snapshot.uninstalling.empty();
  }, "the uninstall");
  CHECK(!Exists(moved) && !Exists(world.Root()));
  std::printf("redist installs, moves and uninstalls ok\n");
}

// The runner on its own: another store's game, and the server name before any chunk.
void RefusesOtherStores() {
  World world;
  world.StartRunner();
  const std::atomic<bool> cancel{false};
  const auto result = world.runner->Install(GameId::Steam(440), (world.internal / "440").string(), {}, cancel, {}, {});
  CHECK(!result.ok && !result.retry_later && result.failure == "This game can't be downloaded.");
  CHECK(world.runner->DownloadServerName() == "GOG");
  std::printf("refuses other stores ok\n");
}


// ---- Updates ------------------------------------------------------------------------------------------------------

void WriteUpdatePhase(const fs::path& root, installs::UpdatePhase phase) {
  installs::UpdateState state;
  state.phase = phase;
  const auto path = root / installs::kUpdateDirName / installs::kUpdateStateName;
  FILE* file = std::fopen(path.c_str(), "wb");
  const auto text = installs::FormatUpdateState(state);
  CHECK(file && std::fwrite(text.data(), 1, text.size(), file) == text.size() && std::fclose(file) == 0);
}

// A to B through the queue: while it stages the game is unlocked and A's files are as they were; once done the
// record has B's build, meta and launches, .playden-files is B's, only B's changed chunks were fetched, and install.log
// names no account, address or token.
void UpdatesThroughTheQueue() {
  World world;
  world.InstallA();
  world.PublishB();
  world.Hold(world.Served() + 2);
  CHECK(world.service->Update(kGame).empty());
  world.WaitHeld();
  {
    const auto snapshot = world.service->Snapshot();
    const auto* job = world.Job(snapshot);
    CHECK(job && job->kind == installs::InstallJobKind::kUpdate && job->state == InstallJobState::kRunning);
    CHECK(!job->locked && job->step == installs::InstallStep::kDownloading);
  }
  CHECK(!world.service->Locked(kGame) && world.Phase() == installs::UpdatePhase::kStaging);
  world.CheckA();
  world.Hold(-1);
  world.WaitUpdated();
  world.CheckB();
  world.CheckStagedOnce();
  CHECK(world.Logged("gog update game=gog-1900000001 step=plan result=ok language=english"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=stage result=ok files=6 changed=3 removed=1"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=record result=ok files=6"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=script product=1900000001 result=ok"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=apply result=ok replaced=3 deleted=1 skipped=0"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=finish result=ok"));
  world.CheckLogs();
  std::printf("updates through the queue ok\n");
}

// The game runs when everything is staged: the update waits for it with no file changed; once the gate lets it, it
// finishes.
void UpdateWaitsForTheGame() {
  World world;
  world.InstallA();
  world.PublishB();
  world.gate = installs::ApplyAnswer::kGameRunning;
  CHECK(world.service->Update(kGame).empty());
  world.WaitState(InstallJobState::kQueued, installs::InstallRunner::kWaitingForGame);
  world.CheckA();
  CHECK(world.Phase() == installs::UpdatePhase::kStaging && !world.service->Locked(kGame));
  CHECK(Exists(world.Root() / ".playden-update/record") && Exists(world.Root() / ".playden-update/staging/data/new.dat"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=apply result=game_running"));
  world.gate = installs::ApplyAnswer::kApply;
  world.service->RetryWaiting(Store::kGog);
  world.WaitUpdated();
  world.CheckB();
  world.CheckStagedOnce();
  world.CheckLogs();
  std::printf("update waits for the game ok\n");
}

// Everything staged while the game runs, then the player goes offline and signed out: once the game closes the update
// is put in place with no session, no GOG and no CDN, since nothing is left to download.
void StagedUpdateAppliesSignedOut() {
  World world;
  world.InstallA();
  world.PublishB();
  world.gate = installs::ApplyAnswer::kGameRunning;
  CHECK(world.service->Update(kGame).empty());
  world.WaitState(InstallJobState::kQueued, installs::InstallRunner::kWaitingForGame);
  CHECK(world.Phase() == installs::UpdatePhase::kStaging);
  CHECK(world.session->SignOut().ok);
  world.revoked = true;
  world.builds_down = true;
  world.chunks_down = true;
  world.auth.Script(Endpoint::kToken, Failure(HttpError::kConnectionFailed));
  world.cdn.ClearRequests();
  world.gate = installs::ApplyAnswer::kApply;
  world.service->RetryWaiting(Store::kGog);
  world.WaitUpdated();
  world.CheckB();
  CHECK(world.cdn.requests().empty() && !world.session->signed_in());
  CHECK(world.Logged("gog update game=gog-1900000001 step=stage result=ready"));
  world.CheckLogs();
  std::printf("staged update applies signed out ok\n");
}

// A staged update whose saved plan is damaged while it waits for the game is staged again, never put in place from
// the earlier record: that record goes before the new Stage, so a Stage that stops (the CDN down) leaves none.
void RestagedUpdateDropsItsRecord() {
  World world;
  world.InstallA();
  world.PublishB();
  world.gate = installs::ApplyAnswer::kGameRunning;
  CHECK(world.service->Update(kGame).empty());
  world.WaitState(InstallJobState::kQueued, installs::InstallRunner::kWaitingForGame);
  CHECK(Exists(world.Root() / ".playden-update/record"));
  {
    FILE* plan = std::fopen((world.Root() / ".playden-update/gog/files").c_str(), "wb");
    CHECK(plan && std::fclose(plan) == 0);
  }
  world.chunks_down = true;
  world.gate = installs::ApplyAnswer::kApply;
  world.service->RetryWaiting(Store::kGog);
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForGog);
  CHECK(!Exists(world.Root() / ".playden-update/record") && world.Phase() == installs::UpdatePhase::kStaging);
  CHECK(!world.service->Locked(kGame));
  world.CheckA();
  world.chunks_down = false;
  world.service->RetryWaiting(Store::kGog);
  world.WaitUpdated();
  world.CheckB();
  world.CheckLogs();
  std::printf("restaged update drops its record ok\n");
}

// Cancel while it stages: the game and its record stay A's, and the work folder goes.
void CancelsUpdateDuringStaging() {
  World world;
  world.InstallA();
  world.PublishB();
  world.Hold(world.Served() + 1);
  CHECK(world.service->Update(kGame).empty());
  world.WaitHeld();
  CHECK(Exists(world.Root() / ".playden-update/staged"));
  world.service->Cancel(kGame);
  WaitFor([&] { return !world.Job(world.service->Snapshot()) && !Exists(world.Root() / ".playden-update"); },
          "the canceled update to go");
  world.WaitReleased();
  world.CheckA();
  const auto game = world.Installed();
  CHECK(game && game->record && game->record->gog && game->record->gog->build_id == "1000000000000000101");
  CHECK(world.Logged("gog update-discard result=ok"));
  world.CheckLogs();
  std::printf("cancels an update during staging ok\n");
}

// Pause while it stages, then Resume: it finishes, no chunk fetched twice.
void PausesAndResumesUpdate() {
  World world;
  world.parallel = 1;
  world.InstallA();
  world.PublishB();
  world.Hold(world.Served() + 2);
  CHECK(world.service->Update(kGame).empty());
  world.WaitHeld();
  world.service->Pause(kGame);
  world.WaitState(InstallJobState::kPaused, "");
  world.WaitReleased();
  world.CheckA();
  world.Hold(-1);
  world.service->Resume(kGame);
  world.WaitUpdated();
  world.CheckB();
  world.CheckStagedOnce();
  std::printf("pauses and resumes an update ok\n");
}

// Playden closes while the update stages: a new service and runner carry on from the journal, no chunk fetched twice.
void RestartsMidStaging() {
  World world;
  world.InstallA();
  world.PublishB();
  world.Hold(world.Served() + 3);
  CHECK(world.service->Update(kGame).empty());
  world.WaitHeld();
  world.service.reset();
  world.Hold(-1);
  world.Restart();
  world.WaitUpdated();
  world.CheckB();
  world.CheckStagedOnce();
  world.CheckLogs();
  std::printf("restarts mid-staging ok\n");
}

// Playden stopped once the update was putting its files in place: at the next start the game is locked and the update
// finishes signed out, with GOG and its CDN out of reach.
void FinishesAfterRestartAtReplacing() {
  World world;
  world.InstallA();
  world.PublishB();
  world.gate = installs::ApplyAnswer::kGameRunning;
  CHECK(world.service->Update(kGame).empty());
  world.WaitState(InstallJobState::kQueued, installs::InstallRunner::kWaitingForGame);
  world.service.reset();
  WriteUpdatePhase(world.Root(), installs::UpdatePhase::kReplacing);
  CHECK(world.session->SignOut().ok);
  world.revoked = true;
  world.builds_down = true;
  world.chunks_down = true;
  world.auth.Script(Endpoint::kToken, Failure(HttpError::kConnectionFailed));
  world.cdn.ClearRequests();
  {
    // The game running: the runner asks the gate before anything else (signed out, it would otherwise wait for the
    // sign-in) and changes nothing.
    world.StartRunner();
    const std::atomic<bool> cancel{false};
    int asked = 0;
    const auto result = world.runner->Update(kGame, world.Root().string(), {}, cancel, {}, {}, [&] {
      ++asked;
      return false;
    });
    CHECK(!result.ok && result.retry_later && result.waiting == installs::InstallRunner::kWaitingForGame && asked == 1);
    CHECK(world.Phase() == installs::UpdatePhase::kReplacing);
    world.CheckA();
  }
  world.gate = installs::ApplyAnswer::kApply;
  world.block_scripts = true;
  world.Restart();
  WaitFor([&] { return world.scripts_blocked.load(); }, "the update's scripts");
  {
    // Its files are in place, the record isn't yet: the game can't run.
    const auto snapshot = world.service->Snapshot();
    const auto* job = world.Job(snapshot);
    CHECK(job && job->kind == installs::InstallJobKind::kUpdate && job->locked && world.service->Locked(kGame));
    const auto record = installs::ReadInstallRecord((world.Root() / installs::kInstallRecordName).string());
    CHECK(record && record->gog && record->gog->build_id == "1000000000000000101");
  }
  world.block_scripts = false;
  world.WaitUpdated();
  world.CheckB();
  CHECK(world.cdn.requests().empty() && !world.session->signed_in());
  world.CheckLogs();
  std::printf("finishes after a restart at replacing ok\n");
}

// Signed out when the update would stage: it waits for the GOG sign-in, and carries on after it.
void UpdateWaitsForSignIn() {
  World world;
  world.InstallA();
  world.PublishB();
  CHECK(world.session->SignOut().ok);
  CHECK(world.service->Update(kGame).empty());
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForSignIn);
  CHECK(world.cdn.requests().empty() && !Exists(world.Root() / ".playden-update"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=plan result=signed_out"));
  world.CheckA();
  world.SignIn(2);
  world.service->RetryWaiting(Store::kGog);
  world.WaitUpdated();
  world.CheckB();
  world.CheckLogs();
  std::printf("update waits for sign-in ok\n");
}

// Uninstalled while the update stages: both versions' files go with the folder. A move is refused meanwhile.
void UninstallsMidUpdate() {
  World world;
  world.InstallA();
  world.PublishB();
  world.Hold(world.Served() + 2);
  CHECK(world.service->Update(kGame).empty());
  world.WaitHeld();
  CHECK(Exists(world.Root() / ".playden-update/staging/data/level.dat"));
  CHECK(!world.service->Move(kGame, "usb0").empty());
  world.service->Uninstall(kGame);
  WaitFor([&] {
    const auto snapshot = world.service->Snapshot();
    return !world.Installed() && snapshot.uninstalling.empty() && !world.Job(snapshot);
  }, "the uninstall");
  world.WaitReleased();
  CHECK(!Exists(world.Root()) && !Exists(world.usb / "gog-1900000001"));
  std::printf("uninstalls mid-update ok\n");
}

// A move asked for while the update runs is refused; the update finishes and the game moves after it.
void MoveRefusedDuringUpdate() {
  World world;
  world.InstallA();
  world.PublishB();
  world.Hold(world.Served() + 1);
  CHECK(world.service->Update(kGame).empty());
  world.WaitHeld();
  CHECK(world.service->Move(kGame, "usb0") == "The game is already in the Queue.");
  world.Hold(-1);
  world.WaitUpdated();
  world.CheckB();
  CHECK(world.service->Move(kGame, "usb0").empty());
  WaitFor([&] {
    const auto game = world.Installed();
    return game && game->volume_id == "usb0" && !world.Job(world.service->Snapshot()) && !Exists(world.Root());
  }, "the move");
  const auto record = installs::ReadInstallRecord((world.usb / "gog-1900000001" / installs::kInstallRecordName).string());
  CHECK(record && record->gog && record->gog->build_id == kBuildB);
  std::printf("move refused during an update ok\n");
}

// The same build again: it finishes with nothing downloaded, no marker, and the record as it was.
void UpdatesToTheSameBuild() {
  World world;
  world.InstallA();
  const auto before = Text(world.Root() / installs::kInstallRecordName);
  world.cdn.ClearRequests();
  CHECK(world.service->Update(kGame).empty());
  WaitFor([&] {
    return !world.Job(world.service->Snapshot()) && world.Logged("gog update game=gog-1900000001 step=finish result=ok");
  }, "the update");
  CHECK(world.cdn.Count(Kind::kChunk) == 0 && world.cdn.Count(Kind::kSecureLink) == 0);
  CHECK(!Exists(world.Root() / installs::kFilesChangedName) && !Exists(world.Root() / installs::kUpdateDirName));
  CHECK(Text(world.Root() / installs::kInstallRecordName) == before);
  world.CheckInstalledAt(world.Root());
  CHECK(world.Logged("gog update game=gog-1900000001 step=apply result=ok replaced=0 deleted=0"));
  world.CheckLogs();
  std::printf("updates to the same build ok\n");
}

// ---- DLC turned off -----------------------------------------------------------------------------------------------

const std::string kDlcExtra = RandomBytes(20 * 1024, 26);
const std::string kDlcExtraB = RandomBytes(21 * 1024, 27);
const std::string kDlcInfo = "{\"gameId\":\"1900000002\",\"rootGameId\":\"1900000001\"}\n";
const std::string kDlcScript = "{\"actions\":[],\"productId\":\"1900000002\"}\n";

// The synthetic game (A, or with `b` build B) with a DLC depot of its own (a file and a support file) and the DLC's
// goggame-<id> files.
Game DlcGame(bool b) {
  auto game = b ? SyntheticGameB() : SyntheticGame();
  GameDepot dlc;
  dlc.product = kDlc;
  dlc.files.push_back({"DepotFile", "dlc\\extra.dat", b ? kDlcExtraB : kDlcExtra});
  dlc.files.push_back({"DepotFile", "save\\dlc.sav", "dlc save\n", {}, true});
  game.depots.push_back(dlc);
  GameDepot dlc_gog;
  dlc_gog.product = kDlc;
  dlc_gog.gog = true;
  dlc_gog.files.push_back({"DepotFile", "goggame-1900000002.info", kDlcInfo});
  dlc_gog.files.push_back({"DepotFile", "goggame-1900000002.script", kDlcScript});
  game.depots.push_back(dlc_gog);
  return game;
}

// The DLC's chunks were never asked for.
void CheckNoDlcChunk(World& world, const Published& published) {
  std::set<std::string> dlc;
  for (const char* path : {"dlc\\extra.dat", "save\\dlc.sav"})
    for (const auto& chunk : published.chunks.at(path)) dlc.insert(chunk.compressed_md5);
  for (const auto& request : world.cdn.requests())
    if (request.kind == Kind::kChunk) CHECK(!dlc.count(request.hash));
}

void CheckDlcOff(World& world, const std::string& build) {
  const auto root = world.Root();
  CHECK(!Exists(root / "dlc") && !Exists(root / ".playden-gog/support/1900000002"));
  CHECK(!Exists(root / "goggame-1900000002.info") && !Exists(root / "goggame-1900000002.script"));
  CHECK(Text(root / ".playden-files").find("dlc/") == std::string::npos);
  CHECK(Text(root / ".playden-files").find("1900000002") == std::string::npos);
  const auto text = Text(root / installs::kInstallRecordName);
  CHECK(text.find(",\"excluded\":[\"1900000002\"]},") != std::string::npos);
  const auto record = installs::ReadInstallRecord((root / installs::kInstallRecordName).string());
  CHECK(record && record->gog && record->gog->build_id == build);
  CHECK((record->gog->products == std::vector<std::uint64_t>{kBase}));
  CHECK((record->gog->excluded == std::vector<std::uint64_t>{kDlc}));
  const auto header = gog::ReadPlanHeader((root / gog::kPlanPath).string());
  CHECK(header && (header->products == std::vector<std::uint64_t>{kBase}));
  const auto game = world.Installed();
  CHECK(game && game->record && game->record->gog && game->record->gog->excluded == record->gog->excluded);
}

// The install sheet turned the DLC off: its files are never downloaded, and the record keeps it off.
void InstallsWithoutATurnedOffDlc() {
  World world;
  world.published = Publish(world.cdn, DlcGame(false));
  world.SignIn();
  world.Start();
  installs::InstallOptions options;
  options.excluded_dlc = {GameId::Gog(kDlc)};
  world.service->Install(kGame, std::nullopt, {}, options);
  world.WaitInstalled();
  const auto root = world.Root();
  CHECK(Text(root / "bin/Game.exe") == kExe && Text(root / "data/level.dat") == kData);
  CheckDlcOff(world, "1000000000000000101");
  CheckNoDlcChunk(world, world.published);
  CHECK(world.Logged("gog plan dlc excluded=1"));
  world.CheckLogs();
  std::printf("installs without a turned-off DLC ok\n");
}

// A newer build through a plain update (no options): the record's DLC stays off.
void UpdateKeepsTurnedOffDlcOff() {
  World world;
  world.published = Publish(world.cdn, DlcGame(false));
  world.SignIn();
  world.Start();
  installs::InstallOptions options;
  options.excluded_dlc = {GameId::Gog(kDlc)};
  world.service->Install(kGame, std::nullopt, {}, options);
  world.WaitInstalled();
  CheckDlcOff(world, "1000000000000000101");
  world.published = Publish(world.cdn, DlcGame(true));
  world.cdn.ClearRequests();
  CHECK(world.service->Update(kGame).empty());
  world.WaitUpdated();
  const auto root = world.Root();
  CHECK(Text(root / "data/level.dat") == kDataB && Text(root / "data/new.dat") == kNewFile);
  CheckDlcOff(world, kBuildB);
  CheckNoDlcChunk(world, world.published);
  CHECK(world.Logged("gog update game=gog-1900000001 step=stage result=ok files=6 changed=3 removed=1"));
  world.CheckLogs();
  std::printf("update keeps a turned-off DLC off ok\n");
}

// GogExcludedDlc: an install takes the options' GOG DLC, an update adds the record's, a DLC choice replaces them; the
// base game and other stores' IDs are dropped, the rest sorted once each.
void ExcludedDlcRules() {
  using stores::GogExcludedDlc;
  using Ids = std::vector<std::uint64_t>;
  installs::InstallOptions options;
  options.excluded_dlc = {GameId::Gog(7), GameId::Steam(440), GameId::Gog(kBase), GameId::Gog(5), GameId::Gog(7)};
  installs::InstalledRecord record;
  record.gog = installs::GogRecord{};
  record.gog->products = {kBase};
  record.gog->excluded = {9, 3, kBase};
  CHECK((GogExcludedDlc(options, false, record, kBase) == Ids{5, 7}));
  CHECK((GogExcludedDlc(options, true, record, kBase) == Ids{3, 5, 7, 9}));
  CHECK((GogExcludedDlc({}, true, record, kBase) == Ids{3, 9}));
  CHECK((GogExcludedDlc(options, true, std::nullopt, kBase) == Ids{5, 7}));
  installs::InstalledRecord steam_record;
  steam_record.excluded_dlc = {11};
  CHECK((GogExcludedDlc(options, true, steam_record, kBase) == Ids{5, 7}));
  options.dlc_choice = true;
  CHECK((GogExcludedDlc(options, true, record, kBase) == Ids{5, 7}));
  options.excluded_dlc.clear();
  CHECK(GogExcludedDlc(options, true, record, kBase).empty());
  std::printf("excluded DLC rules ok\n");
}

// ---- DLC changed (Manage DLC) -------------------------------------------------------------------------------------

const std::string kBuildA = "1000000000000000101";
const char* const kDlcPaths[] = {"dlc/extra.dat", ".playden-gog/support/1900000002/save/dlc.sav",
                                 "goggame-1900000002.info", "goggame-1900000002.script"};

// The DLC game (A) installed through the queue, with its DLC or with it turned off.
void InstallDlcGame(World& world, bool dlc_on) {
  world.published = Publish(world.cdn, DlcGame(false));
  world.SignIn();
  world.Start();
  installs::InstallOptions options;
  if (!dlc_on) options.excluded_dlc = {GameId::Gog(kDlc)};
  world.service->Install(kGame, std::nullopt, {}, options);
  world.WaitInstalled();
}

// Manage DLC's choice: `off` turns the DLC off, otherwise on.
installs::InstallOptions DlcChoice(bool off) {
  installs::InstallOptions options;
  if (off) options.excluded_dlc = {GameId::Gog(kDlc)};
  options.dlc_choice = true;
  return options;
}

// The DLC installed with the game (A's or B's): its files, support file and goggame-<id> files, listed, and the record
// and plan naming it, with nothing left out.
void CheckDlcOn(World& world, const std::string& build, bool b) {
  const auto root = world.Root();
  CHECK(Text(root / "dlc/extra.dat") == (b ? kDlcExtraB : kDlcExtra));
  CHECK(Text(root / ".playden-gog/support/1900000002/save/dlc.sav") == "dlc save\n");
  CHECK(Text(root / "goggame-1900000002.info") == kDlcInfo && Text(root / "goggame-1900000002.script") == kDlcScript);
  CHECK(Text(root / "bin/Game.exe") == kExe);
  const auto listed = Text(root / ".playden-files");
  for (const char* path : kDlcPaths) CHECK(listed.find(std::string("f\t") + path + "\n") != std::string::npos);
  CHECK(Text(root / installs::kInstallRecordName).find("\"excluded\"") == std::string::npos);
  const auto record = installs::ReadInstallRecord((root / installs::kInstallRecordName).string());
  CHECK(record && record->gog && record->gog->build_id == build);
  CHECK((record->gog->products == std::vector<std::uint64_t>{kBase, kDlc}) && record->gog->excluded.empty());
  const auto header = gog::ReadPlanHeader((root / gog::kPlanPath).string());
  CHECK(header && header->build_id == build && (header->products == std::vector<std::uint64_t>{kBase, kDlc}));
  const auto game = world.Installed();
  CHECK(game && game->record && game->record->gog && game->record->gog->products == record->gog->products);
}

// The chunks of the DLC's files in `published`.
std::set<std::string> DlcChunks(const Published& published) {
  std::set<std::string> chunks;
  for (const char* path : {"dlc\\extra.dat", "save\\dlc.sav", "goggame-1900000002.info", "goggame-1900000002.script"})
    for (const auto& chunk : published.chunks.at(path)) chunks.insert(chunk.compressed_md5);
  return chunks;
}

// The registry line's options: what a restart restores the job with.
std::string Registry(World& world) { return Text(world.internal / ".playden-games"); }

// Installed without the DLC, then turned on on the same build: only the DLC's files download, and the record names it.
void DlcChangeAddsTheDlc() {
  World world;
  InstallDlcGame(world, false);
  CheckDlcOff(world, kBuildA);
  world.cdn.ClearRequests();
  const auto before = world.Finishes();
  world.Hold(world.Served());
  CHECK(world.service->Update(kGame, DlcChoice(false)).empty());
  world.WaitHeld();
  {
    const auto snapshot = world.service->Snapshot();
    const auto* job = world.Job(snapshot);
    CHECK(job && job->kind == installs::InstallJobKind::kUpdate && job->dlc_change);
  }
  CHECK(Registry(world).find("\tdlcset") != std::string::npos);
  CHECK(Text(world.Root() / ".playden-update/dlc").empty() && Exists(world.Root() / ".playden-update/dlc"));
  world.Hold(-1);
  world.WaitFinished(before);
  CheckDlcOn(world, kBuildA, false);
  CHECK(Text(world.Root() / "data/level.dat") == kData && Text(world.Root() / "Music/theme.ogg") == kMusic);
  // The DLC's chunks, each once, and nothing else.
  const auto dlc = DlcChunks(world.published);
  std::map<std::string, int> fetched;
  for (const auto& request : world.cdn.requests())
    if (request.kind == Kind::kChunk) ++fetched[request.hash];
  CHECK(fetched.size() == dlc.size());
  for (const auto& entry : fetched) CHECK(dlc.count(entry.first) && entry.second == 1);
  CHECK(!Exists(world.Root() / installs::kUpdateDirName));
  CHECK(world.Logged("gog update game=gog-1900000001 step=stage result=ok files=10 changed=4 removed=0"));
  CHECK(world.Logged(" dlc_missing=0"));
  CHECK(!world.Logged("result=restaged"));
  world.CheckLogs();
  std::printf("a DLC change adds the DLC ok\n");
}

// Installed with the DLC, then turned off: nothing downloads, the DLC's files, support folder and goggame-<id> files
// go, and Downloads lists an update of 0 bytes.
void DlcChangeRemovesTheDlc() {
  World world;
  InstallDlcGame(world, true);
  CheckDlcOn(world, kBuildA, false);
  world.cdn.ClearRequests();
  const auto before = world.Finishes();
  CHECK(world.service->Update(kGame, DlcChoice(true)).empty());
  world.WaitFinished(before);
  CheckDlcOff(world, kBuildA);
  CHECK(Text(world.Root() / "bin/Game.exe") == kExe && Text(world.Root() / "data/level.dat") == kData);
  CHECK(world.cdn.Count(Kind::kChunk) == 0 && world.cdn.Count(Kind::kSecureLink) == 0);
  CHECK(world.Logged("gog update game=gog-1900000001 step=stage result=ok files=6 changed=0 removed=4"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=apply result=ok replaced=0 deleted=4"));
  const auto snapshot = world.service->Snapshot();
  const auto completed = std::find_if(snapshot.completed.begin(), snapshot.completed.end(),
                                      [](const installs::CompletedDownload& entry) { return entry.id == kGame; });
  CHECK(completed != snapshot.completed.end() && completed->updated && completed->update_bytes == 0);
  world.CheckLogs();
  std::printf("a DLC change removes the DLC ok\n");
}

// A newer build is out when the DLC is turned on: the change brings both.
void DlcChangeWithANewerBuild() {
  World world;
  InstallDlcGame(world, false);
  world.published = Publish(world.cdn, DlcGame(true));
  world.cdn.ClearRequests();
  CHECK(world.service->Update(kGame, DlcChoice(false)).empty());
  world.WaitUpdated();
  CheckDlcOn(world, kBuildB, true);
  CHECK(Text(world.Root() / "data/level.dat") == kDataB && Text(world.Root() / "data/new.dat") == kNewFile);
  CHECK(!Exists(world.Root() / "Music"));
  CHECK(world.Logged("gog update game=gog-1900000001 step=stage result=ok files=10 changed=7 removed=1"));
  world.CheckLogs();
  std::printf("a DLC change with a newer build ok\n");
}

// Playden stopped while a change that turns the DLC off was putting its files in place: at the next start it finishes
// signed out and offline, from the work folder's record.
void DlcChangeInterruptedAtReplacing() {
  World world;
  InstallDlcGame(world, true);
  world.gate = installs::ApplyAnswer::kGameRunning;
  CHECK(world.service->Update(kGame, DlcChoice(true)).empty());
  world.WaitState(InstallJobState::kQueued, installs::InstallRunner::kWaitingForGame);
  CHECK(world.Phase() == installs::UpdatePhase::kStaging && Exists(world.Root() / ".playden-update/record"));
  CHECK(Text(world.Root() / ".playden-update/dlc") == "1900000002\n");
  CheckDlcOn(world, kBuildA, false);
  world.service.reset();
  WriteUpdatePhase(world.Root(), installs::UpdatePhase::kReplacing);
  CHECK(world.session->SignOut().ok);
  world.revoked = true;
  world.builds_down = true;
  world.chunks_down = true;
  world.owned_down = true;
  world.auth.Script(Endpoint::kToken, Failure(HttpError::kConnectionFailed));
  world.cdn.ClearRequests();
  world.gate = installs::ApplyAnswer::kApply;
  const auto before = world.Finishes();
  world.Restart();
  world.WaitFinished(before);
  CheckDlcOff(world, kBuildA);
  CHECK(!Exists(world.Root() / installs::kUpdateDirName));
  CHECK(world.cdn.requests().empty() && !world.session->signed_in());
  world.CheckLogs();
  std::printf("a DLC change interrupted at replacing ok\n");
}

// Playden closes while a change (a newer build, the DLC turned off) stages: the job comes back with its choice and
// carries on from the journal, not staged again.
void DlcChangeRestartsMidStaging() {
  World world;
  InstallDlcGame(world, true);
  world.published = Publish(world.cdn, DlcGame(true));
  world.cdn.ClearRequests();
  world.Hold(world.Served() + 2);
  CHECK(world.service->Update(kGame, DlcChoice(true)).empty());
  world.WaitHeld();
  const auto registry = Registry(world);
  CHECK(registry.find("\tnodlc=gog-1900000002") != std::string::npos && registry.find("\tdlcset") != std::string::npos);
  world.service.reset();
  // Still held: the job comes back and waits at its next chunk.
  world.Restart();
  WaitFor([&] {
    const auto snapshot = world.service->Snapshot();
    const auto* job = world.Job(snapshot);
    std::lock_guard<std::mutex> lock(world.hold_mutex);
    return job && job->state == InstallJobState::kRunning && world.waiting > 0;
  }, "the restored change");
  {
    const auto snapshot = world.service->Snapshot();
    const auto* job = world.Job(snapshot);
    CHECK(job && job->kind == installs::InstallJobKind::kUpdate && job->dlc_change);
  }
  world.Hold(-1);
  world.WaitUpdated();
  CheckDlcOff(world, kBuildB);
  CHECK(Text(world.Root() / "data/level.dat") == kDataB && Text(world.Root() / "data/new.dat") == kNewFile);
  CheckNoDlcChunk(world, world.published);
  std::map<std::string, int> fetched;
  for (const auto& request : world.cdn.requests())
    if (request.kind == Kind::kChunk) CHECK(++fetched[request.hash] == 1);
  CHECK(!world.Logged("result=restaged"));
  CHECK(world.Logged("gog update plan reused"));
  world.CheckLogs();
  std::printf("a DLC change restarts mid-staging ok\n");
}

// A work folder staged with the DLC (a plain update, fully staged while the game ran) and a change that turns it off:
// it is staged again for the change. A work folder from a build before the sidecar, for a plain update of a record
// with nothing left out, is put in place as it is.
void StagingOfAnotherChoiceIsRestaged() {
  const auto stage_plain = [](World& world) {
    const std::atomic<bool> cancel{false};
    const auto result =
        world.runner->Update(kGame, world.Root().string(), {}, cancel, {}, {}, [] { return false; });
    CHECK(!result.ok && result.retry_later && result.waiting == installs::InstallRunner::kWaitingForGame);
    CHECK(Exists(world.Root() / ".playden-update/record") && world.Phase() == installs::UpdatePhase::kStaging);
  };
  {
    World world;
    InstallDlcGame(world, true);
    world.published = Publish(world.cdn, DlcGame(true));
    stage_plain(world);
    CHECK(Exists(world.Root() / ".playden-update/dlc") && Text(world.Root() / ".playden-update/dlc").empty());
    CHECK(Text(world.Root() / ".playden-update/staging/dlc/extra.dat") == kDlcExtraB);
    CHECK(world.service->Update(kGame, DlcChoice(true)).empty());
    world.WaitUpdated();
    CheckDlcOff(world, kBuildB);
    CHECK(world.Logged("gog update game=gog-1900000001 step=plan result=restaged"));
    CHECK(Text(world.Root() / "data/level.dat") == kDataB && Text(world.Root() / "data/new.dat") == kNewFile);
    world.CheckLogs();
  }
  {
    World world;
    InstallDlcGame(world, true);
    world.published = Publish(world.cdn, DlcGame(true));
    stage_plain(world);
    CHECK(::unlink((world.Root() / ".playden-update/dlc").c_str()) == 0);
    world.cdn.ClearRequests();
    CHECK(world.service->Update(kGame).empty());
    world.WaitUpdated();
    CHECK(!world.Logged("result=restaged"));
    CHECK(world.Logged("gog update game=gog-1900000001 step=stage result=ready"));
    CHECK(world.cdn.Count(Kind::kChunk) == 0);
    CheckDlcOn(world, kBuildB, true);
    world.CheckLogs();
  }
  std::printf("staging of another choice is restaged ok\n");
}

// GOG's owned list out of reach: the change waits to reach GOG and deletes nothing; once it answers, the change ends.
void DlcChangeWaitsWithoutOwnedList() {
  World world;
  InstallDlcGame(world, true);
  world.owned_down = true;
  const auto before = world.Finishes();
  CHECK(world.service->Update(kGame, DlcChoice(true)).empty());
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForGog);
  CHECK(world.Logged("gog plan owned status=503"));
  CheckDlcOn(world, kBuildA, false);
  CHECK(world.Phase() == installs::UpdatePhase::kStaging && !world.service->Locked(kGame));
  world.owned_down = false;
  world.service->RetryWaiting(Store::kGog);
  world.WaitFinished(before);
  CheckDlcOff(world, kBuildA);
  world.CheckLogs();
  std::printf("a DLC change waits without the owned list ok\n");
}

// Signed out: the change waits for the GOG sign-in, and ends after it.
void DlcChangeWaitsForSignIn() {
  World world;
  InstallDlcGame(world, true);
  CHECK(world.session->SignOut().ok);
  const auto before = world.Finishes();
  CHECK(world.service->Update(kGame, DlcChoice(true)).empty());
  world.WaitState(InstallJobState::kQueued, GogInstallRunner::kWaitingForSignIn);
  CheckDlcOn(world, kBuildA, false);
  CHECK(!Exists(world.Root() / installs::kUpdateDirName));
  world.SignIn(2);
  world.service->RetryWaiting(Store::kGog);
  world.WaitFinished(before);
  CheckDlcOff(world, kBuildA);
  world.CheckLogs();
  std::printf("a DLC change waits for sign-in ok\n");
}

// A DLC turned on that GOG no longer lists as owned isn't planned: the change ends with the base game alone, and the
// stage line counts it.
void DlcChangeSkipsAnUnownedDlc() {
  World world;
  InstallDlcGame(world, false);
  world.cdn.SetOwned("{\"owned\":[1900000001]}");
  world.cdn.ClearRequests();
  const auto before = world.Finishes();
  CHECK(world.service->Update(kGame, DlcChoice(false)).empty());
  world.WaitFinished(before);
  CHECK(world.Logged(" dlc_missing=1"));
  CHECK(world.cdn.Count(Kind::kChunk) == 0 && !Exists(world.Root() / "dlc"));
  const auto record = installs::ReadInstallRecord((world.Root() / installs::kInstallRecordName).string());
  CHECK(record && record->gog && (record->gog->products == std::vector<std::uint64_t>{kBase}));
  CHECK(record->gog->excluded.empty());
  world.CheckLogs();
  std::printf("a DLC change skips an unowned DLC ok\n");
}

// Installed before the DLC was bought: a plain update on the same build (what the page offers as a new DLC) brings it,
// and the record names it.
void PlainUpdateBringsABoughtDlc() {
  World world;
  world.cdn.SetOwned("{\"owned\":[1900000001]}");
  InstallDlcGame(world, true);
  auto record = installs::ReadInstallRecord((world.Root() / installs::kInstallRecordName).string());
  CHECK(record && record->gog && (record->gog->products == std::vector<std::uint64_t>{kBase}));
  CHECK(record->gog->excluded.empty() && !Exists(world.Root() / "dlc"));
  world.cdn.SetOwned("{\"owned\":[1900000001,1900000002]}");
  const auto before = world.Finishes();
  CHECK(world.service->Update(kGame).empty());
  world.WaitFinished(before);
  CheckDlcOn(world, kBuildA, false);
  world.CheckLogs();
  std::printf("a plain update brings a bought DLC ok\n");
}

// Uninstalled while a change stages: the game folder goes, the work folder and its sidecar with it.
void UninstallMidDlcChange() {
  World world;
  InstallDlcGame(world, false);
  world.cdn.ClearRequests();
  world.Hold(world.Served() + 1);
  CHECK(world.service->Update(kGame, DlcChoice(false)).empty());
  world.WaitHeld();
  CHECK(Exists(world.Root() / ".playden-update/dlc"));
  world.service->Uninstall(kGame);
  WaitFor([&] {
    const auto snapshot = world.service->Snapshot();
    return !world.Installed() && snapshot.uninstalling.empty() && !world.Job(snapshot);
  }, "the uninstall");
  world.WaitReleased();
  CHECK(!Exists(world.Root()));
  std::printf("uninstall mid DLC change ok\n");
}
}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  g_fixtures = argv[1];
  fs::remove_all(argv[2]);
  fs::create_directories(argv[2]);
  g_scratch = fs::canonical(argv[2]);
  RefusesOtherStores();
  InstallsThroughTheQueue();
  PausesAndResumes();
  RestartsMidDownload();
  CancelsMidDownload();
  WaitsForSignIn();
  WaitsAfterRevokedMidDownload();
  WaitsToReachGog();
  WaitsWhenBuildsUnreachable();
  WaitsWhenCdnDown();
  UninstallRemovesEverything();
  MovesToUsb();
  FreeSpaceShort();
  Gen1Installs();
  RedistInstallsMovesAndUninstalls();
  UpdatesThroughTheQueue();
  UpdateWaitsForTheGame();
  StagedUpdateAppliesSignedOut();
  RestagedUpdateDropsItsRecord();
  CancelsUpdateDuringStaging();
  PausesAndResumesUpdate();
  RestartsMidStaging();
  FinishesAfterRestartAtReplacing();
  UpdateWaitsForSignIn();
  UninstallsMidUpdate();
  MoveRefusedDuringUpdate();
  UpdatesToTheSameBuild();
  ExcludedDlcRules();
  InstallsWithoutATurnedOffDlc();
  UpdateKeepsTurnedOffDlcOff();
  DlcChangeAddsTheDlc();
  DlcChangeRemovesTheDlc();
  DlcChangeWithANewerBuild();
  DlcChangeInterruptedAtReplacing();
  DlcChangeRestartsMidStaging();
  StagingOfAnotherChoiceIsRestaged();
  DlcChangeWaitsWithoutOwnedList();
  DlcChangeWaitsForSignIn();
  DlcChangeSkipsAnUnownedDlc();
  PlainUpdateBringsABoughtDlc();
  UninstallMidDlcChange();
  fs::remove_all(g_scratch);
  std::printf("gog install runner ok\n");
  return 0;
}
