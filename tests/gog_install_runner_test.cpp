// SPDX-License-Identifier: GPL-3.0-or-later
// GOG's install runner (stores/gog_install) in a real install service, on two folders standing for console storage
// and a USB drive: a synthetic gen 2 game from the stub CDN (gog_cdn_stub.h, gog_synthetic_game.h) with its
// goggame-<id>.info and .script, a GOG session on the scripted auth stub (gog_stub_transport.h). Install to the end,
// pause and resume, a restart, cancel, signed out and revoked sessions, uninstall, a move to the USB drive, free space,
// a synthetic gen 1 game (ranges of a main.bin) to the end, moved and uninstalled, and a game with __redist installers
// installed, moved and uninstalled. Built twice: with the host's walks, and with the console's walk by path.
// Usage: gog_install_runner_test <fixtures/gog> <scratch folder>.

#include "gog/dependencies.h"
#include "gog/info_file.h"
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
  fs::remove_all(g_scratch);
  std::printf("gog install runner ok\n");
  return 0;
}
