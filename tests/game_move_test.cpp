// SPDX-License-Identifier: GPL-3.0-or-later
// Moving an installed game to another drive: the install service's move job with GameMover, on two
// folders standing for console storage and a USB drive.
#include "installs/game_move.h"
#include "installs/install_service.h"
#include "platform/managed_directory.h"
#include "steam/backend/depot_installer.h"
#include "stores/local/copy_plan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

using namespace playden;
using namespace playden::installs;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

const GameId kGame = GameId::Steam(730);

// Steam's runner as far as moves need it: removals.
class Stub final : public InstallRunner {
 public:
  Result Install(const GameId&, const std::string&, const InstallOptions&, const std::atomic<bool>&,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                 const std::function<void(InstallStep)>&) override {
    return {false, false, "No downloads here."};
  }
  std::string Remove(const std::string& root) override {
    std::string failure;
    steam::RemoveManagedInstall(root, &failure);
    return failure;
  }
};

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

struct Fixture {
  fs::path root, internal, usb;
  Stub stub;
  GameMoveOptions options;
  std::unique_ptr<GameMover> mover;
  std::unique_ptr<InstallService> service;
  std::mutex mutex;
  std::vector<std::string> log;
  std::vector<std::string> relocated;  // each relocation's `to`
  std::atomic<int> files{0};           // files copied into place
  std::atomic<ApplyAnswer> gate{ApplyAnswer::kApply};
  std::uint64_t game_bytes = 0;
  bool usb_available = true;

  Fixture() {
    char name[] = "/tmp/playden-game-move-XXXXXX";
    CHECK(mkdtemp(name));
    root = fs::canonical(name);  // /tmp is a link on macOS; ManagedDirectory never follows one
    internal = root / "data/playden-games";
    usb = root / "mnt/usb0/playden-games";
    fs::create_directories(internal);
    fs::create_directories(usb);
    MakeGame(internal / "730");
    Write(internal / ".playden-games", "730\tinternal\t" + std::to_string(game_bytes) + "\n");
    options.log = [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mutex);
      log.push_back(line);
    };
    options.buffer_bytes = 4096;
    options.after_file = [this](const std::string&) { ++files; };
    // As RuntimeHost::Relocate for a game whose title is stopped: the files move, then the title follows.
    options.relocate = [this](const GameRelocation& relocation) {
      GameRelocationResult result;
      result.message = relocation.move_files();
      if (!result.message.empty()) result.outcome = GameRelocationResult::Outcome::kFailed;
      std::lock_guard<std::mutex> lock(mutex);
      relocated.push_back(relocation.to);
      return result;
    };
  }
  ~Fixture() {
    service.reset();
    platform::SetOwnershipProbeForTesting(nullptr);
    fs::remove_all(root);
  }

  void MakeGame(const fs::path& folder) {
    std::string big(300000, '\0');
    for (std::size_t index = 0; index < big.size(); ++index) big[index] = static_cast<char>(index * 31 % 251);
    Write(folder / "game.exe", "MZ program");
    Write(folder / "data/level1.pak", big);
    Write(folder / "data/level2.pak", big.substr(1000));
    Write(folder / "data/deep/empty.txt", "");
    Write(folder / "readme.txt", "read me");
    Write(folder / ".playden-tree", "v1\n");
    Write(folder / ".playden-files", "f\tgame.exe\nd\tdata\nf\tdata/level1.pak\nf\tdata/level2.pak\nd\tdata/deep\n"
                                     "f\tdata/deep/empty.txt\nf\treadme.txt\n");
    game_bytes = 10 + big.size() + big.size() - 1000 + 7;
    Write(folder / kInstallRecordName,
          "{\"version\":2,\"app_id\":730,\"install_bytes\":" + std::to_string(game_bytes) + ",\"depots\":[]}\n");
  }

  std::vector<InstallVolume> Volumes() const {
    return {{InstallService::kPrimaryVolumeId, "Console storage", internal.string(), true},
            {"usb0", "USB Drive", usb.string(), usb_available}};
  }

  void Start(std::uint64_t free_bytes = 1ull << 40) {
    mover = std::make_unique<GameMover>(options);
    service = std::make_unique<InstallService>(InstallService::Runners{{Store::kSteam, &stub}}, Volumes());
    service->SetSpaceQuery([free_bytes](const std::string&) { return std::optional<std::uint64_t>(free_bytes); });
    service->SetRetryDelays(5ms, 20ms);
    service->SetApplyGate([this](GameId) { return gate.load(); });
    service->SetMoveRunner(mover.get());
  }
  void Restart(std::uint64_t free_bytes = 1ull << 40) {
    service.reset();
    Start(free_bytes);
  }

  InstallSnapshot Snapshot() { return service->Snapshot(); }
  std::optional<InstallJobView> Job() {
    for (const auto& job : Snapshot().jobs)
      if (job.id == kGame) return job;
    return std::nullopt;
  }
  std::optional<InstalledGame> Installed() {
    for (const auto& game : Snapshot().installed)
      if (game.id == kGame) return game;
    return std::nullopt;
  }
  template <typename Done>
  bool WaitFor(Done done, std::chrono::milliseconds limit = 5000ms) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
      if (done()) return true;
      std::this_thread::sleep_for(2ms);
    }
    return done();
  }
  bool Logged(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    return std::any_of(log.begin(), log.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
  }
  int Relocations() {
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<int>(relocated.size());
  }

  // The move ended with the game on the USB drive, whole, and nothing left behind.
  void CheckMoved() {
    CHECK(WaitFor([&] { return !Job(); }));
    const auto game = Installed();
    CHECK(game && game->volume_id == "usb0" && game->root == (usb / "730").string() && game->available);
    CHECK(game->install_bytes == game_bytes);
    const auto copy = usb / "730";
    CHECK(Read(copy / "game.exe") == "MZ program");
    CHECK(Read(copy / "data/level1.pak").size() == 300000);
    CHECK(Read(copy / "data/level2.pak").size() == 299000);
    CHECK(fs::exists(copy / "data/deep/empty.txt") && fs::file_size(copy / "data/deep/empty.txt") == 0);
    CHECK(Read(copy / kInstallRecordName).find("\"app_id\":730") != std::string::npos);
    CHECK(Read(copy / ".playden-files").find("f\tdata/level1.pak") != std::string::npos);
    CHECK(fs::exists(copy / ".playden-tree"));
    CHECK(!fs::exists(copy / ".playden-copy"));
    CHECK(!fs::exists(internal / "730"));
    CHECK(Read(internal / ".playden-games").find("730\tusb0\t") != std::string::npos);
    CHECK(Read(internal / ".playden-games").find("move=") == std::string::npos);
  }

  // Runs the mover by itself up to a stop, as Playden would until it quit, with the registry saying
  // what it said then.
  InstallRunner::Result CopyUntil(const std::atomic<bool>& cancel) {
    GameMover direct(options);
    return direct.Copy(kGame, (internal / "730").string(), (usb / "730").string(), "usb0", cancel, {}, {}, {});
  }
  void RegistryMoving() {
    Write(internal / ".playden-games", "730\tinternal\t" + std::to_string(game_bytes) + "\tmove=usb0\tfrom=internal\n");
  }
};

void MovesTheWholeGame() {
  Fixture f;
  std::atomic<bool> locked_while_copying{false};
  std::atomic<int> steps{0};
  f.options.after_file = [&](const std::string&) {
    ++f.files;
    if (f.service->Locked(kGame)) locked_while_copying = true;  // Play waits while the move runs
  };
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  const auto queued = f.Job();
  CHECK(!queued || (queued->kind == InstallJobKind::kMove && queued->from_volume_id == "internal"));
  f.CheckMoved();
  CHECK(locked_while_copying);
  CHECK(f.files == 8);  // six game files, then the file list and the record
  CHECK(f.Relocations() == 1);
  CHECK(!f.service->Locked(kGame));
  CHECK(f.Logged("move deleted the old folder"));
  (void)steps;
}

void HoldsTheEffectiveUidAroundFolderCalls() {
  Fixture f;
  std::atomic<int> holds{0}, outstanding{0};
  std::atomic<bool> held_at_relocation{false}, plan_written_held{false}, probed{false}, probed_unheld{false};
  const auto plan = f.usb / "730/.playden-copy/plan.json";
  f.options.hold_effective_uid = [&]() -> std::shared_ptr<void> {
    ++holds;
    ++outstanding;
    const bool had_plan = fs::exists(plan);
    return std::shared_ptr<void>(nullptr, [&, had_plan](void*) {
      if (!had_plan && fs::exists(plan)) plan_written_held = true;  // the plan appeared while held
      --outstanding;
    });
  };
  // The games folder's ownership probe writes a file there: held as well.
  static std::atomic<int>* held_now;
  static std::atomic<bool>* probe_seen;
  static std::atomic<bool>* probe_unheld;
  held_now = &outstanding;
  probe_seen = &probed;
  probe_unheld = &probed_unheld;
  platform::SetOwnershipProbeForTesting([](const fs::path&) {
    *probe_seen = true;
    if (*held_now == 0) *probe_unheld = true;
    return std::optional<bool>(true);
  });
  // The title is asked with nothing held: the runtime worker takes the same lock for its batch.
  auto follow = f.options.relocate;
  f.options.relocate = [&](const GameRelocation& relocation) {
    if (outstanding != 0) held_at_relocation = true;
    return follow(relocation);
  };
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  f.CheckMoved();
  CHECK(holds >= 8 * 3);  // each file's part made, read back and renamed
  CHECK(outstanding == 0 && !held_at_relocation);
  CHECK(plan_written_held && probed && !probed_unheld);
}

void RefusesWhatCantMove() {
  Fixture f;
  f.Start(f.game_bytes);  // no room for the margin
  CHECK(f.service->Move(kGame, "usb0") == "Not enough free space on USB Drive for this game.");
  CHECK(f.service->Move(kGame, "internal") == "The game is already on Console storage.");
  CHECK(f.service->Move(kGame, "nowhere") == "The storage is disconnected.");
  CHECK(f.service->Move(GameId::Steam(10), "usb0") == "The game is not installed.");
  CHECK(!f.Job());
  CHECK(!fs::exists(f.usb / "730"));
}

void SpaceIsCheckedAgainWhenItStarts() {
  Fixture f;
  f.options.space = [](const std::string&) { return std::optional<steam::VolumeSpace>(steam::VolumeSpace{1ull << 30, 1024}); };
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->state == InstallJobState::kFailed; }));
  CHECK(f.Job()->failure == "Not enough free space for the game. Make room, then Resume.");
  CHECK(!fs::exists(f.usb / "730"));  // nothing was made
  CHECK(f.Installed()->volume_id == "internal");
  f.service->Cancel(kGame);
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(fs::exists(f.internal / "730/data/level1.pak"));
}

void WaitsWhileThePlayerPlays() {
  Fixture f;
  f.gate = ApplyAnswer::kGameRunning;
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->failure == InstallRunner::kWaitingForGame; }));
  CHECK(!fs::exists(f.usb / "730"));  // nothing copied while the game runs
  // Uninstalling or updating a moving game is refused.
  f.service->Uninstall(kGame);
  CHECK(f.Installed() && f.Job() && f.Job()->kind == InstallJobKind::kMove);
  f.service->Update(kGame);
  CHECK(f.Job()->kind == InstallJobKind::kMove);
  CHECK(f.service->Move(kGame, "usb0") == "The game is already in the Queue.");
  f.gate = ApplyAnswer::kApply;
  f.CheckMoved();
}

void TheTitleBusyKeepsTheGameWhereItIs() {
  Fixture f;
  std::atomic<int> busy{2};
  auto follow = f.options.relocate;
  f.options.relocate = [&](const GameRelocation& relocation) {
    if (busy > 0) {
      --busy;
      // The title's own marker is there while it is asked, and nothing else changed.
      CHECK(fs::exists(fs::path(relocation.to) / kMoveSwitchName));
      return GameRelocationResult{GameRelocationResult::Outcome::kBusy, "Close the game first."};
    }
    return follow(relocation);
  };
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->failure == "Close the game first."; }));
  CHECK(f.Installed()->volume_id == "internal");
  f.CheckMoved();
  CHECK(f.files == 8);  // the waits copied nothing again
}

void CancelDeletesThePartialCopy() {
  Fixture f;
  std::atomic<bool> hold{true};
  f.options.after_file = [&](const std::string&) {
    ++f.files;
    while (hold) std::this_thread::sleep_for(1ms);
  };
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { return f.files >= 1; }));
  f.service->Cancel(kGame);
  hold = false;
  CHECK(f.WaitFor([&] { return !f.Job() && !fs::exists(f.usb / "730"); }));
  CHECK(f.Installed()->volume_id == "internal");
  CHECK(fs::exists(f.internal / "730/data/level1.pak"));
  CHECK(Read(f.internal / ".playden-games") == "730\tinternal\t" + std::to_string(f.game_bytes) + "\n");
}

void ResumesAfterAStopMidCopy() {
  Fixture f;
  std::atomic<bool> stop{false};
  f.options.after_file = [&](const std::string&) {
    if (++f.files == 2) stop = true;
  };
  CHECK(f.CopyUntil(stop).canceled);
  CHECK(fs::exists(f.usb / "730/.playden-copy/plan.json"));
  CHECK(!fs::exists(f.usb / "730" / kInstallRecordName));
  // While Playden was closed the game changed one copied file, added one and deleted one.
  Write(f.internal / "730/game.exe", "MZ patched");
  Write(f.internal / "730/settings.ini", "fullscreen=1");
  fs::remove(f.internal / "730/readme.txt");
  f.RegistryMoving();
  f.files = 0;
  f.options.after_file = [&](const std::string&) { ++f.files; };
  f.Start();
  const auto job = f.Job();
  CHECK(!job || (job->kind == InstallJobKind::kMove && !job->locked));
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(f.files < 9);  // what was copied stayed
  CHECK(Read(f.usb / "730/game.exe") == "MZ patched");
  CHECK(Read(f.usb / "730/settings.ini") == "fullscreen=1");
  CHECK(!fs::exists(f.usb / "730/readme.txt"));
  CHECK(f.Installed()->volume_id == "usb0");
  CHECK(!fs::exists(f.internal / "730"));
}

void AFileRewrittenAtItsSizeIsCopiedAgain() {
  Fixture f;
  // Every file copied, then stopped before the switch.
  f.options.checkpoint = [](MoveCheckpoint reached) { return reached != MoveCheckpoint::kCopied; };
  const std::atomic<bool> never{false};
  CHECK(f.CopyUntil(never).canceled);
  f.options.checkpoint = {};
  // While the move was paused the game rewrote a copied file at the same size.
  std::string rewritten(300000, 'x');
  Write(f.internal / "730/data/level1.pak", rewritten);
  struct timeval later[2] = {{1900000000, 0}, {1900000000, 0}};
  CHECK(::utimes((f.internal / "730/data/level1.pak").c_str(), later) == 0);
  // The resume saves the new plan and stops before it copies the file again.
  const std::atomic<bool> stopped{true};
  CHECK(f.CopyUntil(stopped).canceled);
  CHECK(!fs::exists(f.usb / "730/data/level1.pak"));  // the old copy went before the plan named the new file
  f.RegistryMoving();
  f.Start();
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(f.Installed()->volume_id == "usb0");
  CHECK(Read(f.usb / "730/data/level1.pak") == rewritten);
}

void AFileDeletedWhilePausedNeverComesBack() {
  Fixture f;
  // Every file copied, then stopped before the switch.
  f.options.checkpoint = [](MoveCheckpoint reached) { return reached != MoveCheckpoint::kCopied; };
  const std::atomic<bool> never{false};
  CHECK(f.CopyUntil(never).canceled);
  f.options.checkpoint = {};
  CHECK(fs::exists(f.usb / "730/readme.txt"));
  // While the move was paused the game deleted a file; the resume saves the new plan, then stops (here: no
  // free space) as a crash would.
  fs::remove(f.internal / "730/readme.txt");
  auto space = f.options.space;
  f.options.space = [](const std::string&) { return std::optional<steam::VolumeSpace>(steam::VolumeSpace{1ull << 30, 0}); };
  CHECK(!f.CopyUntil(never).failure.empty());
  CHECK(!fs::exists(f.usb / "730/readme.txt"));  // gone before the plan stopped naming it
  f.options.space = space;
  f.RegistryMoving();
  f.Start();
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(f.Installed()->volume_id == "usb0");
  CHECK(!fs::exists(f.usb / "730/readme.txt"));
}

void ResumesAfterEachStep() {
  for (const auto checkpoint : {MoveCheckpoint::kCopied, MoveCheckpoint::kSwitchMarked, MoveCheckpoint::kSwitched}) {
    Fixture f;
    f.options.checkpoint = [checkpoint](MoveCheckpoint reached) { return reached != checkpoint; };
    const std::atomic<bool> never{false};
    CHECK(f.CopyUntil(never).canceled);
    const bool marked = checkpoint != MoveCheckpoint::kCopied;
    CHECK(fs::exists(f.usb / "730" / kMoveSwitchName) == marked);
    CHECK(f.Relocations() == (checkpoint == MoveCheckpoint::kSwitched ? 1 : 0));
    f.RegistryMoving();
    f.options.checkpoint = {};
    f.files = 0;
    std::atomic<bool> hold{true};
    f.options.relocate = [&, follow = f.options.relocate](const GameRelocation& relocation) {
      while (hold) std::this_thread::sleep_for(1ms);
      return follow(relocation);
    };
    f.Start();
    // A move whose title may already name the copy must finish: it can't be canceled or paused.
    if (marked) {
      CHECK(f.Job() && f.Job()->locked);
      f.service->Cancel(kGame);
      f.service->Pause(kGame);
    }
    hold = false;
    f.CheckMoved();
    CHECK(f.files == 2);  // only the file list and the record, copied again
  }
}

// The checks count their files: every copied file before the switch, and again with the title stopped.
void ChecksCountTheirFiles() {
  Fixture f;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> counts;
  std::vector<InstallStep> steps;
  GameMover mover(f.options);
  const std::atomic<bool> never{false};
  const auto result = mover.Copy(kGame, (f.internal / "730").string(), (f.usb / "730").string(), "usb0", never, {},
                                 [&](InstallStep step) { steps.push_back(step); },
                                 [&](std::uint64_t done, std::uint64_t total) { counts.emplace_back(done, total); });
  CHECK(result.ok);
  CHECK((steps == std::vector<InstallStep>{InstallStep::kCopying, InstallStep::kVerifying, InstallStep::kSwitching}));
  // Six game files, checked once while verifying and once more at the switch.
  using Count = std::pair<std::uint64_t, std::uint64_t>;
  CHECK(counts.size() == 12 && counts[0] == Count(1, 6) && counts[5] == Count(6, 6) && counts[11] == counts[5]);
  CHECK(f.Logged("move rechecks game=730 files=6") && f.Logged("move rechecked game=730"));
}

// Stopped once the title followed the copy (Playden quit before the service knew): the move goes straight
// back to the switch, without walking or checking the files again, and finishes.
void ResumesAtTheSwitchWithoutCheckingAgain() {
  Fixture f;
  f.options.checkpoint = [](MoveCheckpoint reached) { return reached != MoveCheckpoint::kSwitched; };
  const std::atomic<bool> never{false};
  CHECK(f.CopyUntil(never).canceled);
  CHECK(f.Relocations() == 1);
  f.options.checkpoint = {};
  // As RuntimeHost::Relocate for a title that already names the copy: nothing to move.
  f.options.relocate = [&](const GameRelocation& relocation) {
    std::lock_guard<std::mutex> lock(f.mutex);
    f.relocated.push_back(relocation.to);
    return GameRelocationResult{};
  };
  // A file the game would have rewritten meanwhile is the title's business now: the copy is what it plays.
  Write(f.internal / "730/readme.txt", "changed!");
  std::vector<InstallStep> steps;
  std::size_t counted = 0;
  GameMover mover(f.options);
  f.files = 0;
  const auto result = mover.Copy(kGame, (f.internal / "730").string(), (f.usb / "730").string(), "usb0", never, {},
                                 [&](InstallStep step) { steps.push_back(step); },
                                 [&](std::uint64_t, std::uint64_t) { ++counted; });
  CHECK(result.ok && result.install_bytes == f.game_bytes);
  // Only the copy is checked, by size: no file is copied.
  CHECK((steps == std::vector<InstallStep>{InstallStep::kVerifying, InstallStep::kSwitching}) && counted == 6 && f.files == 0);
  CHECK(f.Relocations() == 2 && f.Logged("move resumes at the switch game=730"));
  CHECK(Read(f.usb / "730/readme.txt") == "read me");
  // A copy that lost a file since goes the whole way again: the file comes back before the old folder may go.
  fs::remove(f.usb / "730/data/level1.pak");
  f.files = 0;
  CHECK(mover.Copy(kGame, (f.internal / "730").string(), (f.usb / "730").string(), "usb0", never, {}, {}, {}).ok);
  CHECK(f.Logged("move copy changed at the switch game=730 path=data/level1.pak"));
  CHECK(fs::file_size(f.usb / "730/data/level1.pak") == 300000 && f.files >= 1);
  // Without the copy's record the copy isn't whole: the move doesn't finish.
  fs::remove(f.usb / "730" / kInstallRecordName);
  const auto refused = mover.Copy(kGame, (f.internal / "730").string(), (f.usb / "730").string(), "usb0", never, {}, {}, {});
  CHECK(!refused.ok && refused.failure.find("install record is missing") != std::string::npos);
}

void FinishesDeletingTheOldFolder() {
  Fixture f;
  // Stopped after the copy was listed and the old folder deleted, before its journal went.
  f.options.checkpoint = [](MoveCheckpoint reached) { return reached != MoveCheckpoint::kOldDeleted; };
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->state == InstallJobState::kFailed; }));
  CHECK(f.Installed()->volume_id == "usb0");
  CHECK(!fs::exists(f.internal / "730"));
  CHECK(fs::exists(f.usb / "730/.playden-copy"));
  CHECK(Read(f.internal / ".playden-games").find("730\tusb0\t" + std::to_string(f.game_bytes) + "\tmove=usb0\tfrom=internal\tpaused") !=
        std::string::npos);
  f.options.checkpoint = {};
  f.Restart();
  // Locked from the start; the worker may already have taken it, or finished it.
  const auto restored = f.Job();
  CHECK(!restored || (restored->state != InstallJobState::kPaused && restored->locked));
  f.CheckMoved();
  CHECK(f.Relocations() == 1);  // the title was not asked again
}

void OldFolderLeftAfterTheSwitchIsDeletedAtStart() {
  Fixture f;
  // Quit right after the registry named the copy: both folders are there.
  const std::atomic<bool> never{false};
  CHECK(f.CopyUntil(never).ok);
  Write(f.internal / ".playden-games",
        "730\tusb0\t" + std::to_string(f.game_bytes) + "\tmove=usb0\tfrom=internal\n");
  f.Start();
  f.CheckMoved();
}

void TheOldFolderStaysUntilTheRegistryIsSaved() {
  Fixture f;
  // The registry can't be saved: its temporary name is taken by a folder.
  fs::create_directories(f.internal / ".playden-games.tmp");
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->failure.find("couldn't save the move") != std::string::npos; }));
  CHECK(f.Job()->locked && f.Installed()->volume_id == "usb0");
  CHECK(fs::exists(f.internal / "730" / kInstallRecordName));  // still where the saved registry names it
  CHECK(Read(f.internal / ".playden-games").find("730\tinternal\t") != std::string::npos);
  // Once it saves, the move finishes by itself.
  fs::remove(f.internal / ".playden-games.tmp");
  f.CheckMoved();
  CHECK(f.Relocations() == 1);
}

void AMoveWhoseOldFolderWentIsAdoptedAtStart() {
  for (const bool journal_gone : {false, true}) {
    Fixture f;
    // The title follows the copy and the old folder is deleted, but the registry still names the old drive.
    const std::atomic<bool> never{false};
    CHECK(f.CopyUntil(never).ok);
    fs::remove_all(f.internal / "730");
    if (journal_gone) fs::remove_all(f.usb / "730/.playden-copy");
    f.RegistryMoving();
    f.Start();
    f.CheckMoved();
    CHECK(f.Relocations() == 1);  // the copy wasn't made again
  }
}

void UnownedDrivesBothWays() {
  Fixture f;
  // The USB drive records no owner or permissions (exFAT): its games folder is kUnownedTree.
  static std::string usb_root;
  usb_root = f.usb.string();
  platform::SetOwnershipProbeForTesting([](const fs::path& path) {
    return std::optional<bool>(path.string().rfind(usb_root, 0) != 0);
  });
  CHECK(GamesFolderPolicy(f.usb.string()) == platform::DirectoryPolicy::kUnownedTree);
  CHECK(GamesFolderPolicy(f.internal.string()) == platform::DirectoryPolicy::kApprovedTree);
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  f.CheckMoved();
  CHECK(f.Logged("policy=unowned"));
  CHECK(f.service->Move(kGame, "internal").empty());
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(f.Installed()->volume_id == "internal");
  CHECK(Read(f.internal / "730/data/level1.pak").size() == 300000);
  CHECK(!fs::exists(f.usb / "730"));
  CHECK(f.Logged("policy=owned"));
}

void RefusesAFolderInTheWay() {
  Fixture f;
  Write(f.usb / "730/mine.txt", "not Playden's");
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->state == InstallJobState::kFailed; }));
  CHECK(f.Job()->failure == "A folder on the destination is in the way of the move.");
  CHECK(Read(f.usb / "730/mine.txt") == "not Playden's");
  // Cancel discards only a copy of Playden's: the folder stays.
  f.service->Cancel(kGame);
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(Read(f.usb / "730/mine.txt") == "not Playden's");
  CHECK(f.Installed()->volume_id == "internal");
}

void CancelKeepsAnInstallInTheWay() {
  Fixture f;
  // A whole install of the game was left on the destination (an old folder a move kept).
  f.MakeGame(f.usb / "730");
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->state == InstallJobState::kFailed; }));
  CHECK(f.Job()->failure == "A folder on the destination is in the way of the move.");
  f.service->Cancel(kGame);
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(f.WaitFor([&] { return f.Logged("move kept a folder it didn't make"); }));
  CHECK(fs::exists(f.usb / "730" / kInstallRecordName) && fs::exists(f.usb / "730/data/level1.pak"));
  CHECK(f.Installed()->volume_id == "internal");
  // A copy of another folder isn't this move's either, once it holds a record.
  f.service.reset();
  stores::local::CopyPlan other;
  other.source = (f.root / "elsewhere/730").string();
  CHECK(stores::local::SaveCopyPlan(other, (f.usb / "730").string()).empty());
  CHECK(GameMover(f.options).Discard((f.internal / "730").string(), (f.usb / "730").string()) != "");
  CHECK(fs::exists(f.usb / "730" / kInstallRecordName));
  // Without a record it is a partial copy, whichever folder it was of: Cancel clears it.
  fs::remove(f.usb / "730" / kInstallRecordName);
  CHECK(GameMover(f.options).Discard((f.internal / "730").string(), (f.usb / "730").string()).empty());
  CHECK(!fs::exists(f.usb / "730"));
}

void CancelAfterTheTitleRefusedDeletesTheCopy() {
  Fixture f;
  // The files and the record were copied, then the title couldn't follow: the copy is still the move's.
  f.options.relocate = [](const GameRelocation& relocation) {
    const auto moved = relocation.move_files();
    CHECK(moved.empty());
    return GameRelocationResult{GameRelocationResult::Outcome::kFailed, "The title refused."};
  };
  f.Start();
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->state == InstallJobState::kFailed; }));
  CHECK(fs::exists(f.usb / "730" / kInstallRecordName) && !fs::exists(f.usb / "730" / kMoveSwitchName));
  f.service->Cancel(kGame);
  CHECK(f.WaitFor([&] { return !f.Job() && !fs::exists(f.usb / "730"); }));
  CHECK(f.Installed()->volume_id == "internal" && fs::exists(f.internal / "730/data/level1.pak"));
}

void NeverMovesOntoItsOwnFolder() {
  Fixture f;
  // A registry line naming no old drive, with the game already on its destination.
  f.MakeGame(f.usb / "730");
  fs::remove_all(f.internal / "730");
  Write(f.internal / ".playden-games", "730\tusb0\t" + std::to_string(f.game_bytes) + "\tmove=usb0\n");
  f.Start();
  CHECK(!f.Job());
  std::this_thread::sleep_for(50ms);
  CHECK(f.Installed() && f.Installed()->volume_id == "usb0");
  CHECK(fs::exists(f.usb / "730" / kInstallRecordName) && fs::exists(f.usb / "730/data/level1.pak"));
}

void CancelWhileTheGateIsAskedDropsTheMove() {
  Fixture f;
  f.Start();
  std::atomic<bool> asked{false}, hold{true};
  f.service->SetApplyGate([&](GameId) {
    asked = true;
    while (hold) std::this_thread::sleep_for(1ms);
    return ApplyAnswer::kGameRunning;
  });
  CHECK(f.service->Move(kGame, "usb0").empty());
  CHECK(f.WaitFor([&] { return asked.load(); }));
  f.service->Cancel(kGame);
  f.service->SetApplyGate([](GameId) { return ApplyAnswer::kApply; });
  hold = false;
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  std::this_thread::sleep_for(50ms);
  CHECK(!f.Job() && !fs::exists(f.usb / "730"));
  CHECK(f.Installed()->volume_id == "internal");
  CHECK(Read(f.internal / ".playden-games").find("move=") == std::string::npos);
}

void AMovedGamePlaysWithoutItsOldDrive() {
  Fixture f;
  // Moved from the USB drive and listed on console storage; Playden stopped before the old folder went,
  // and the USB drive is gone at the next start.
  Write(f.internal / ".playden-games", "730\tinternal\t" + std::to_string(f.game_bytes) + "\tmove=internal\tfrom=usb0\n");
  f.usb_available = false;
  f.Start();
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->state == InstallJobState::kFailed; }));
  CHECK(!f.Job()->locked && !f.service->Locked(kGame));
  CHECK(f.Job()->failure == "The game's old drive is not connected. Connect it, then Resume to delete the old folder.");
  f.service->Cancel(kGame);
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(f.Installed()->volume_id == "internal" && fs::exists(f.internal / "730/data/level1.pak"));
}

void AMoveToAnAbsentDriveCanBeCanceledOnceItsTitleIsChecked() {
  Fixture f;
  // Playden stopped part way through a move to the USB drive, which isn't there at the next start. The copy's
  // switch marker can't be read, so the move fails locked: the title might name the copy.
  {
    std::atomic<bool> cancel{false};
    f.options.checkpoint = [](MoveCheckpoint point) { return point != MoveCheckpoint::kCopied; };
    f.CopyUntil(cancel);
    f.options.checkpoint = nullptr;
  }
  f.RegistryMoving();
  f.usb_available = false;
  f.Start();
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->state == InstallJobState::kFailed; }));
  CHECK(f.Job()->locked && f.service->Locked(kGame));
  CHECK(f.Job()->failure == "The game's storage is not available.");
  f.service->Cancel(kGame);
  CHECK(f.Job() && f.Job()->kind == InstallJobKind::kMove);
  // A title that names the copy keeps it locked.
  f.service->ReleaseUnreachableMove(kGame, (f.usb / "730").string());
  CHECK(f.Job()->locked);
  // One that names the game's folder on console storage lets it go: Cancel drops the move.
  f.service->ReleaseUnreachableMove(kGame, (f.internal / "730").string());
  CHECK(!f.Job()->locked && !f.service->Locked(kGame));
  f.service->Cancel(kGame);
  CHECK(f.WaitFor([&] { return !f.Job(); }));
  CHECK(f.Installed()->volume_id == "internal" && fs::exists(f.internal / "730/data/level1.pak"));
  CHECK(Read(f.internal / ".playden-games").find("move=") == std::string::npos);
  // The copy on the drive that was away is left as it was; the next start doesn't bring the move back.
  f.usb_available = true;
  f.Restart();
  CHECK(!f.Job() && f.Installed()->volume_id == "internal" && fs::exists(f.usb / "730/data/level1.pak"));
}

void AMoveToAnAbsentDriveCarriesOnWhenItIsBack() {
  Fixture f;
  f.RegistryMoving();
  f.usb_available = false;
  f.Start();
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->state == InstallJobState::kFailed; }));
  CHECK(f.Job()->locked && f.Installed()->volume_id == "internal");
  f.service->Cancel(kGame);
  f.service->Pause(kGame);
  CHECK(f.Job() && f.Job()->kind == InstallJobKind::kMove);
  // With the drive back the move can be resumed, and finishes.
  f.usb_available = true;
  f.Restart();
  CHECK(f.Job() && !f.Job()->locked);
  f.service->Resume(kGame);
  f.CheckMoved();
}

void AMoveWhoseCopyIsMarkedStaysLocked() {
  Fixture f;
  // The switch marker on a connected destination: the title may name the copy, so the move only finishes.
  {
    std::atomic<bool> cancel{false};
    f.options.checkpoint = [](MoveCheckpoint point) { return point != MoveCheckpoint::kSwitchMarked; };
    f.CopyUntil(cancel);
    f.options.checkpoint = nullptr;
  }
  CHECK(fs::exists(f.usb / "730" / kMoveSwitchName));
  f.RegistryMoving();
  f.gate = ApplyAnswer::kGameRunning;
  f.Start();
  CHECK(f.WaitFor([&] { const auto job = f.Job(); return job && job->locked; }));
  f.service->Cancel(kGame);
  CHECK(f.Job() && f.Job()->kind == InstallJobKind::kMove);
}

void AMovedGameIsNotANewDownload() {
  Fixture f;
  // Installed long ago: Completed doesn't list it, and the copy's newer record doesn't make it list it.
  const auto record = (f.internal / "730" / kInstallRecordName).string();
  struct timeval old[2] = {{1600000000, 0}, {1600000000, 0}};
  CHECK(::utimes(record.c_str(), old) == 0);
  f.Start();
  CHECK(f.Snapshot().completed.empty());
  CHECK(f.service->Move(kGame, "usb0").empty());
  f.CheckMoved();
  CHECK(f.Snapshot().completed.empty());
  // It keeps the time it was installed (Home's Recently installed), also once Playden starts again.
  CHECK(f.Installed()->finished_at == 1600000000);
  CHECK(Read(f.internal / ".playden-games").find("\tfinished=1600000000") != std::string::npos);
  f.Restart();
  CHECK(f.Installed()->volume_id == "usb0" && f.Installed()->finished_at == 1600000000);
  CHECK(f.Snapshot().completed.empty());
}

// Installed yesterday: Completed lists it before and after the move, with the same time.
void AMovedGameKeepsItsInstallTime() {
  Fixture f;
  const auto record = (f.internal / "730" / kInstallRecordName).string();
  const auto yesterday = static_cast<time_t>(std::time(nullptr) - 24 * 60 * 60);
  struct timeval times[2] = {{yesterday, 0}, {yesterday, 0}};
  CHECK(::utimes(record.c_str(), times) == 0);
  f.Start();
  CHECK(f.Snapshot().completed.size() == 1 && f.Snapshot().completed[0].finished_at == yesterday);
  CHECK(f.service->Move(kGame, "usb0").empty());
  f.CheckMoved();
  CHECK(f.Installed()->finished_at == yesterday);
  CHECK(f.Snapshot().completed.size() == 1 && f.Snapshot().completed[0].finished_at == yesterday);
  f.Restart();
  CHECK(f.Installed()->finished_at == yesterday && f.Snapshot().completed.size() == 1);
}

}  // namespace

int main() {
  MovesTheWholeGame();
  HoldsTheEffectiveUidAroundFolderCalls();
  RefusesWhatCantMove();
  SpaceIsCheckedAgainWhenItStarts();
  WaitsWhileThePlayerPlays();
  TheTitleBusyKeepsTheGameWhereItIs();
  CancelDeletesThePartialCopy();
  ResumesAfterAStopMidCopy();
  AFileRewrittenAtItsSizeIsCopiedAgain();
  AFileDeletedWhilePausedNeverComesBack();
  ResumesAfterEachStep();
  ChecksCountTheirFiles();
  ResumesAtTheSwitchWithoutCheckingAgain();
  FinishesDeletingTheOldFolder();
  OldFolderLeftAfterTheSwitchIsDeletedAtStart();
  TheOldFolderStaysUntilTheRegistryIsSaved();
  AMoveWhoseOldFolderWentIsAdoptedAtStart();
  UnownedDrivesBothWays();
  RefusesAFolderInTheWay();
  CancelKeepsAnInstallInTheWay();
  CancelAfterTheTitleRefusedDeletesTheCopy();
  NeverMovesOntoItsOwnFolder();
  AMovedGameIsNotANewDownload();
  AMovedGameKeepsItsInstallTime();
  CancelWhileTheGateIsAskedDropsTheMove();
  AMovedGamePlaysWithoutItsOldDrive();
  AMoveToAnAbsentDriveCanBeCanceledOnceItsTitleIsChecked();
  AMoveToAnAbsentDriveCarriesOnWhenItIsBack();
  AMoveWhoseCopyIsMarkedStaysLocked();
  std::puts("game_move tests passed");
  return 0;
}
