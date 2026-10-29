#include "runtime/layout_migration.h"
#include "platform/storage.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace fs = std::filesystem;
using namespace playden::runtime;

namespace {
fs::path g_base;

struct Fixture {
  LayoutPaths paths;
  LayoutOps ops;
  std::vector<std::string> lines;
  int root_calls = 0;
};

void Write(const fs::path& file, const std::string& text, mode_t mode = 0644) {
  std::ofstream(file) << text;
  CHECK(chmod(file.c_str(), mode) == 0);
}

std::string Read(const fs::path& file) {
  std::ifstream in(file);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

unsigned Mode(const fs::path& path) {
  struct stat info{};
  CHECK(lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}

std::unique_ptr<Fixture> Make(const std::string& name) {
  auto fixture = std::make_unique<Fixture>();
  const auto data = g_base / name / "data";
  auto& paths = fixture->paths;
  paths.boundary = data;
  paths.app_root = data / "homebrew" / "FAKE77031";
  paths.user_root = data / "playden" / "user";
  paths.log_root = data / "playden" / "logs";
  paths.games_root = data / "playden-games";
  paths.state_root = data / "playden" / "runtime-state";
  fs::create_directories(paths.app_root / "playden-data" / "game-settings-private");
  fs::create_directories(paths.app_root / "logs");
  fs::create_directories(paths.app_root / "games" / "220");
  CHECK(chmod((paths.app_root / "playden-data" / "game-settings-private").c_str(), 0700) == 0);
  Write(paths.app_root / "playden-data" / "session.v1", "credentials", 0600);
  Write(paths.app_root / "playden-data" / "game-settings-private" / "game-settings.json", "{}", 0600);
  Write(paths.app_root / "playden-data" / "library.json", "[]");
  Write(paths.app_root / "logs" / "native-startup.log", "pre-grant\n");
  Write(paths.app_root / "logs" / "runtime.log", "old run\n");
  Write(paths.app_root / "games" / "220" / "game.exe", "MZ");
  fs::create_directories(paths.app_root / "games" / ".playden-games");
  auto* raw = fixture.get();
  fixture->ops.log = [raw](const std::string& line) { raw->lines.push_back(line); };
  fixture->ops.as_root = [raw](const std::function<bool()>& work) { ++raw->root_calls; return work(); };
  return fixture;
}

bool Moved(const LayoutPaths& paths) {
  return fs::exists(paths.user_root / "session.v1") && fs::exists(paths.user_root / "library.json") &&
         fs::exists(paths.user_root / "game-settings-private" / "game-settings.json") &&
         fs::exists(paths.log_root / "native-startup.log") && fs::exists(paths.log_root / "runtime.log") &&
         fs::exists(paths.games_root / "220" / "game.exe") && fs::exists(paths.games_root / ".playden-games");
}

// A valid install record; `journal` adds a pending operation.
void WriteInstalls(const LayoutPaths& paths, bool journal, const std::string& install_path = {}) {
  const std::string record =
      "{\"schema_version\":1,\"install_id\":\"i220\",\"title_id\":\"FAKE78001\",\"app_id\":220,"
      "\"data_root\":\"/data/playden/games/i220\",\"default_profile_id\":\"default\",\"runtime_bundle_id\":\"kit\","
      "\"recipe_id\":\"generic\",\"recipe_version\":\"1\",\"settings_revision\":1,\"exit_policy\":\"exit-title\","
      "\"launch_entry_id\":\"main\",\"launch_path\":\"game.exe\",\"launch_arguments\":\"\","
      "\"launch_working_directory\":\"\",\"install_path\":\"" +
      (install_path.empty() ? (paths.app_root / "games" / "220").string() : install_path) + "\","
      "\"volume_id\":\"internal\",\"state\":\"preparing\",\"artifacts\":[],\"last_failure\":null,\"journal\":" +
      std::string(journal ? "{\"operation_id\":\"op1\",\"operation\":\"prepare\",\"step\":\"bottle\"}" : "null") + "}";
  const std::string text = "{\"schema_version\":1,\"reservations\":[{\"install_id\":\"i220\",\"title_id\":\"FAKE78001\"}],"
                           "\"installs\":[" + record + "]}";
  fs::create_directories(paths.state_root);
  CHECK(chmod(paths.state_root.c_str(), 0700) == 0);
  CHECK(playden::Storage(paths.state_root.string()).WriteDurable("installs.json", playden::Bytes(text.begin(), text.end())).ok());
}

void MovesUserDataLogsAndGames() {
  auto f = Make("moves");
  const auto result = MigrateLayout(f->paths, f->ops);
  CHECK(result.user_moved && result.logs_moved && result.games_moved && result.notice.empty());
  CHECK(result.user_root == f->paths.user_root && result.log_root == f->paths.log_root &&
        result.games_root == f->paths.games_root);
  CHECK(Moved(f->paths));
  CHECK(Mode(f->paths.user_root) == 01777 && Mode(f->paths.log_root) == 01777);
  CHECK(Mode(f->paths.user_root / "session.v1") == 0600);
  CHECK(Mode(f->paths.user_root / "game-settings-private") == 0700);
  CHECK(Read(f->paths.user_root / "session.v1") == "credentials");
  CHECK(f->root_calls == 1);  // only the games rename
  const auto journal = LoadLayoutJournal(f->paths.state_root);
  CHECK(journal && journal->step == "games-moved");
  CHECK(journal->from == (f->paths.app_root / "games").string() && journal->to == f->paths.games_root.string());
  CHECK(Mode(f->paths.state_root / kLayoutJournal) == 0600);
  CHECK(Read(f->paths.app_root / "games-MOVED.txt").find(f->paths.games_root.string()) != std::string::npos);
  // Later starts change nothing until the runtime host marks the move done.
  const auto again = MigrateLayout(f->paths, f->ops);
  CHECK(again.games_moved && again.user_root == f->paths.user_root && f->root_calls == 1);
  CHECK(MarkLayoutDone(f->paths.state_root));
  const auto done = LoadLayoutJournal(f->paths.state_root);
  CHECK(done && done->step == "done" && done->from == journal->from && done->to == journal->to);
  CHECK(MigrateLayout(f->paths, f->ops).games_moved);
}

void KeepsStubFolders() {
  auto f = Make("stubs");
  MigrateLayout(f->paths, f->ops);
  for (const auto* stub : {"playden-data", "logs"}) {
    const auto folder = f->paths.app_root / stub;
    CHECK(fs::is_directory(folder));
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(folder)) names.push_back(entry.path().filename().string());
    CHECK(names.size() == 1 && names[0] == "MOVED.txt");
  }
  CHECK(Read(f->paths.app_root / "playden-data" / "MOVED.txt").find(f->paths.user_root.string()) != std::string::npos);
  CHECK(Read(f->paths.app_root / "logs" / "MOVED.txt").find(f->paths.log_root.string()) != std::string::npos);
  CHECK(!fs::exists(f->paths.app_root / "games"));
  // The next start's pre-grant lines land in the stub again; the move is not redone.
  Write(f->paths.app_root / "logs" / "native-startup.log", "next start\n");
  MigrateLayout(f->paths, f->ops);
  CHECK(Read(f->paths.app_root / "logs" / "native-startup.log") == "next start\n");
}

void ResumesAfterEachStep() {
  // Interrupted inside the user step: half the entries already moved.
  {
    auto f = Make("resume-user");
    fs::create_directories(f->paths.user_root);
    fs::rename(f->paths.app_root / "playden-data" / "library.json", f->paths.user_root / "library.json");
    fs::create_directories(f->paths.state_root);
    CHECK(chmod(f->paths.state_root.c_str(), 0700) == 0);
    const std::string text = "{\"step\":\"user\",\"from\":\"a\",\"to\":\"b\",\"postponed\":0}";
    CHECK(playden::Storage(f->paths.state_root.string()).WriteDurable(kLayoutJournal, playden::Bytes(text.begin(), text.end())).ok());
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(result.games_moved && result.notice.empty() && Moved(f->paths));
  }
  // Interrupted inside the logs step, after the next start wrote its pre-grant lines.
  {
    auto f = Make("resume-logs");
    auto ops = f->ops;
    int renames = 0;
    // Fail the second log entry once: the first stays moved for forward retry.
    ops.rename = [&](const fs::path& from, const fs::path& to) -> std::error_code {
      if (from.parent_path().filename() == "logs" && ++renames == 2) return {EIO, std::generic_category()};
      return ::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : std::error_code{errno, std::generic_category()};
    };
    const auto held = MigrateLayout(f->paths, ops);
    CHECK(held.user_moved && !held.logs_moved && !held.games_moved && !held.notice.empty());
    CHECK(held.log_root == f->paths.app_root / "logs" && held.games_root == f->paths.app_root / "games");
    CHECK(!fs::exists(f->paths.app_root / "logs" / "native-startup.log") && fs::exists(f->paths.log_root / "native-startup.log"));
    CHECK(LoadLayoutJournal(f->paths.state_root)->step == "logs");
    // A crash left one log moved; the next start writes the same name again.
    Write(f->paths.app_root / "logs" / "native-startup.log", "next start\n");
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(result.logs_moved && result.games_moved && result.notice.empty());
    CHECK(Read(f->paths.log_root / "native-startup.log") == "pre-grant\n");
    CHECK(Read(f->paths.log_root / "native-startup.log.moved") == "next start\n");
  }
  // Interrupted after the games rename, before games-moved was written.
  {
    auto f = Make("resume-games");
    CHECK(MigrateLayout(f->paths, f->ops).games_moved);
    const std::string text = "{\"step\":\"games\",\"from\":\"a\",\"to\":\"b\",\"postponed\":0}";
    CHECK(playden::Storage(f->paths.state_root.string()).WriteDurable(kLayoutJournal, playden::Bytes(text.begin(), text.end())).ok());
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(result.games_moved && result.games_root == f->paths.games_root);
    const auto journal = LoadLayoutJournal(f->paths.state_root);
    CHECK(journal->step == "games-moved" && journal->from == (f->paths.app_root / "games").string());
  }
}

void WriteJournal(const LayoutPaths& paths, const std::string& file, const std::string& text) {
  fs::create_directories(paths.state_root);
  CHECK(chmod(paths.state_root.c_str(), 0700) == 0);
  CHECK(playden::Storage(paths.state_root.string()).WriteDurable(file, playden::Bytes(text.begin(), text.end())).ok());
}

// Every interrupted logs step can leave one more copy of a name: each keeps its own.
void LogsStepInterruptedTwice() {
  auto f = Make("logs-twice");
  fs::create_directories(f->paths.user_root);
  fs::create_directories(f->paths.log_root);
  for (const auto& name : {"session.v1", "library.json", "game-settings-private"})
    fs::rename(f->paths.app_root / "playden-data" / name, f->paths.user_root / name);
  Write(f->paths.log_root / "native-startup.log", "first\n");
  Write(f->paths.log_root / "native-startup.log.moved", "second\n");
  Write(f->paths.app_root / "logs" / "native-startup.log", "third\n");
  WriteJournal(f->paths, kLayoutJournal, "{\"step\":\"logs\",\"from\":\"a\",\"to\":\"b\",\"postponed\":0}");
  const auto result = MigrateLayout(f->paths, f->ops);
  CHECK(result.logs_moved && result.games_moved && result.notice.empty() && Moved(f->paths));
  CHECK(Read(f->paths.log_root / "native-startup.log") == "first\n");
  CHECK(Read(f->paths.log_root / "native-startup.log.moved") == "second\n");
  CHECK(Read(f->paths.log_root / "native-startup.log.moved.2") == "third\n");
}

// A fresh install has no games to move: the shared games folder is still made, 0777.
void FreshInstallMakesSharedGamesFolder() {
  auto f = Make("fresh-games");
  fs::remove_all(f->paths.app_root / "games");
  const auto result = MigrateLayout(f->paths, f->ops);
  CHECK(result.games_moved && result.notice.empty());
  CHECK(fs::is_directory(f->paths.games_root) && Mode(f->paths.games_root) == 0777);
  // Gone later (made by hand, removed by hand): the next start makes it again.
  fs::remove(f->paths.games_root);
  CHECK(MigrateLayout(f->paths, f->ops).games_moved && Mode(f->paths.games_root) == 0777);
  // One that came with the games keeps its mode.
  auto moved = Make("moved-games");
  CHECK(chmod((moved->paths.app_root / "games").c_str(), 0755) == 0);
  CHECK(MigrateLayout(moved->paths, moved->ops).games_moved && Mode(moved->paths.games_root) == 0755);
}

// A launch session still pending: the game may run or be suspended from its old folder.
void PostponesGamesWhileLaunchPending() {
  auto f = Make("launch");
  WriteJournal(f->paths, "launch-pending.v1", "session");
  const auto result = MigrateLayout(f->paths, f->ops);
  CHECK(result.user_moved && result.logs_moved && !result.games_moved && result.notice.empty());
  CHECK(fs::exists(f->paths.app_root / "games" / "220") && !fs::exists(f->paths.games_root));
  CHECK(LoadLayoutJournal(f->paths.state_root)->postponed == 1);
  bool logged = false;
  for (const auto& line : f->lines) logged = logged || line.find("pending launch title=unreadable running=unknown") != std::string::npos;
  CHECK(logged);
  CHECK(playden::Storage(f->paths.state_root.string()).RemoveDurable("launch-pending.v1").ok());
  const auto moved = MigrateLayout(f->paths, f->ops);
  CHECK(moved.games_moved && Moved(f->paths) && LoadLayoutJournal(f->paths.state_root)->postponed == 0);
}

// A pending session (StorageLaunchSessionJournal's format) whose game the console says has
// ended no longer holds the move: that happens whenever Playden started the last game.
// A game started or suspended from Home (no pending launch) also holds the move back while it runs.
void RunningGameFromHomeHoldsTheMove() {
  for (const bool running : {true, false}) {
    auto f = Make(std::string("home-") + (running ? "running" : "stopped"));
    WriteInstalls(f->paths, false);
    std::vector<std::string> asked;
    f->ops.title_running = [&asked, running](const std::string& title) {
      asked.push_back(title);
      return std::optional<bool>(running);
    };
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(result.games_moved == !running && Moved(f->paths) == !running);
    CHECK(asked == std::vector<std::string>{"FAKE78001"});
  }
}

void PendingLaunchOfStoppedGameMoves() {
  const auto hex = [](const std::string& text) {
    std::string out;
    for (const unsigned char c : text) { char pair[3]; std::snprintf(pair, sizeof(pair), "%02x", c); out += pair; }
    return out;
  };
  const std::string session = "PLAYDEN-LAUNCH-V1\n" + hex("00000000-0000-4000-8000-000000000001") + "\n" +
                              hex("steam-220") + "\n" + hex("FAKE78001") + "\n" + hex("wine-wow64") +
                              "\n1\n\n2\n1760000000\n1760000001\n0\n0\n\n";
  // Running (or suspended), unknown, or no query: the move waits.
  for (const auto answer : {std::optional<bool>(true), std::optional<bool>(), std::optional<bool>(false)}) {
    for (const bool query : {true, false}) {
      if (!query && answer != std::optional<bool>(true)) continue;
      auto f = Make(std::string("launch-title-") + (answer ? (*answer ? "running" : "stopped") : "unknown") +
                    (query ? "" : "-noquery"));
      WriteJournal(f->paths, "launch-pending.v1", session);
      std::vector<std::string> asked;
      if (query)
        f->ops.title_running = [&asked, answer](const std::string& title) { asked.push_back(title); return answer; };
      const auto result = MigrateLayout(f->paths, f->ops);
      const bool moves = query && answer == std::optional<bool>(false);
      CHECK(result.games_moved == moves && Moved(f->paths) == moves);
      CHECK(query ? asked == std::vector<std::string>{"FAKE78001"} : asked.empty());
      if (!moves) {
        CHECK(LoadLayoutJournal(f->paths.state_root)->postponed == 1);
        // Told once the move kept waiting, in words that say what to do.
        for (int start = 2; start <= kLayoutPostponeNoticeAfter; ++start) {
          const auto later = MigrateLayout(f->paths, f->ops);
          CHECK(later.notice.empty() == (start < kLayoutPostponeNoticeAfter));
          if (!later.notice.empty())
            CHECK(later.notice.find("the last game you started from Playden may still be running or suspended. Close it") !=
                  std::string::npos);
        }
      }
    }
  }
}

void PostponesGamesWhileJournalPending() {
  // A pending journal of a game on another drive doesn't hold the move back.
  {
    auto usb = Make("pending-usb");
    WriteInstalls(usb->paths, true, "/mnt/usb0/playden/games/220");
    CHECK(MigrateLayout(usb->paths, usb->ops).games_moved && Moved(usb->paths));
  }
  // Nor does one of a record whose folder is gone (the game was installed again elsewhere).
  {
    auto stale = Make("pending-stale");
    WriteInstalls(stale->paths, true, (stale->paths.app_root / "games" / "999").string());
    CHECK(MigrateLayout(stale->paths, stale->ops).games_moved && Moved(stale->paths));
  }
  auto f = Make("pending");
  WriteInstalls(f->paths, true);
  for (int start = 1; start <= kLayoutPostponeNoticeAfter; ++start) {
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(result.user_moved && result.logs_moved && !result.games_moved);
    CHECK(result.games_root == f->paths.app_root / "games" && fs::exists(f->paths.app_root / "games" / "220"));
    CHECK(!fs::exists(f->paths.games_root) && f->root_calls == 0);
    CHECK(LoadLayoutJournal(f->paths.state_root)->postponed == start);
    CHECK(result.notice.empty() == (start < kLayoutPostponeNoticeAfter));
  }
  bool logged = false;
  for (const auto& line : f->lines) logged = logged || line.find("pending prepare journal") != std::string::npos;
  CHECK(logged);
  // A settings publication still in flight in the game's control folder also holds it back:
  // its receipt is pending, or missing.
  WriteInstalls(f->paths, false);
  const auto control = f->paths.boundary / "playden" / "games" / "i220" / ".playden-control";
  fs::create_directories(control);
  Write(control / "settings-operation.v1", "x", 0600);
  CHECK(!MigrateLayout(f->paths, f->ops).games_moved);
  Write(control / "publication-state.v1", "{\"schema_version\":1,\"phase\":\"pending\"}", 0600);
  CHECK(!MigrateLayout(f->paths, f->ops).games_moved);
  // Once committed, the operation stays in the folder but is done: the games move.
  Write(control / "publication-state.v1", "{\"schema_version\":1,\"phase\":\"committed\"}", 0600);
  const auto result = MigrateLayout(f->paths, f->ops);
  CHECK(result.games_moved && result.notice.empty() && Moved(f->paths));
  CHECK(LoadLayoutJournal(f->paths.state_root)->postponed == 0);
}

// A Ready game keeps its last, committed publication's operation in its control folder; that
// doesn't hold the games back.
void MovesReadyGameWithCommittedPublication() {
  auto f = Make("committed");
  WriteInstalls(f->paths, false);
  const auto control = f->paths.boundary / "playden" / "games" / "i220" / ".playden-control";
  fs::create_directories(control);
  Write(control / "settings-operation.v1", "x", 0600);
  Write(control / "publication-state.v1",
        "{\"schema_version\":1,\"phase\":\"committed\",\"transaction_id\":\"t\",\"settings_revision\":1}\n", 0600);
  const auto result = MigrateLayout(f->paths, f->ops);
  CHECK(result.games_moved && result.notice.empty() && Moved(f->paths));
  CHECK(LoadLayoutJournal(f->paths.state_root)->step == "games-moved");
}

void RefusesToMergeIntoNonEmptyTarget() {
  {
    auto f = Make("merge-user");
    fs::create_directories(f->paths.user_root);
    Write(f->paths.user_root / "someone-elses.txt", "x");
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(!result.user_moved && !result.logs_moved && !result.games_moved && !result.notice.empty());
    CHECK(result.user_root == f->paths.app_root / "playden-data");
    CHECK(fs::exists(f->paths.app_root / "playden-data" / "session.v1") && !fs::exists(f->paths.user_root / "session.v1"));
    CHECK(!LoadLayoutJournal(f->paths.state_root));  // nothing started, so the next start checks again
    bool both = false;
    for (const auto& line : f->lines)
      both = both || (line.find(f->paths.user_root.string()) != std::string::npos &&
                      line.find((f->paths.app_root / "playden-data").string()) != std::string::npos);
    CHECK(both);
    CHECK(!MigrateLayout(f->paths, f->ops).user_moved);
  }
  {
    auto f = Make("merge-games");
    fs::create_directories(f->paths.games_root / "other");
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(result.user_moved && result.logs_moved && !result.games_moved && !result.notice.empty());
    CHECK(result.games_root == f->paths.app_root / "games" && fs::exists(f->paths.app_root / "games" / "220"));
    CHECK(!fs::exists(f->paths.games_root / "220"));
  }
}

void AbortsOnCrossDevice() {
  auto f = Make("exdev");
  f->ops.rename = [](const fs::path& from, const fs::path& to) -> std::error_code {
    if (from.filename() == "games") return {EXDEV, std::generic_category()};
    return ::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : std::error_code{errno, std::generic_category()};
  };
  const auto result = MigrateLayout(f->paths, f->ops);
  CHECK(result.user_moved && result.logs_moved && !result.games_moved);
  CHECK(result.notice.find("another drive") != std::string::npos);
  CHECK(fs::exists(f->paths.app_root / "games" / "220" / "game.exe") && !fs::exists(f->paths.games_root));
  CHECK(LoadLayoutJournal(f->paths.state_root)->step == "games");
}

void RevertsTheMove() {
  auto f = Make("revert");
  // Never moved: nothing to do.
  CHECK(RevertLayout(f->paths, f->ops).complete);
  CHECK(MigrateLayout(f->paths, f->ops).games_moved);
  Write(f->paths.log_root / "runtime-0.6.log", "new\n");
  Write(f->paths.app_root / "logs" / "native-startup.log", "next start\n");
  const auto calls = f->root_calls;
  const auto reverted = RevertLayout(f->paths, f->ops);
  CHECK(reverted.complete && reverted.error.empty());
  CHECK(f->root_calls == calls + 1);
  const auto app = f->paths.app_root;
  CHECK(Read(app / "playden-data" / "session.v1") == "credentials" && Mode(app / "playden-data" / "session.v1") == 0600);
  CHECK(Mode(app / "playden-data" / "game-settings-private") == 0700);
  CHECK(fs::exists(app / "games" / "220" / "game.exe") && !fs::exists(f->paths.games_root));
  CHECK(fs::exists(app / "logs" / "runtime-0.6.log"));
  CHECK(Read(app / "logs" / "native-startup.log") == "next start\n");
  CHECK(Read(app / "logs" / "native-startup.log.moved") == "pre-grant\n");
  CHECK(!fs::exists(app / "playden-data" / "MOVED.txt") && !fs::exists(app / "logs" / "MOVED.txt") &&
        !fs::exists(app / "games-MOVED.txt"));
  CHECK(!LoadLayoutJournal(f->paths.state_root));
  // A later 0.6 start moves everything again.
  const auto again = MigrateLayout(f->paths, f->ops);
  CHECK(again.games_moved && again.notice.empty() && Moved(f->paths));
}

void RevertKeepsConflictsAndJournal() {
  auto f = Make("revert-conflict");
  CHECK(MigrateLayout(f->paths, f->ops).games_moved);
  Write(f->paths.app_root / "playden-data" / "session.v1", "written by 0.4\n", 0600);
  const auto reverted = RevertLayout(f->paths, f->ops);
  CHECK(!reverted.complete && reverted.error.find("session.v1") != std::string::npos);
  CHECK(Read(f->paths.user_root / "session.v1") == "credentials");
  CHECK(LoadLayoutJournal(f->paths.state_root)->step == "revert-user");
  CHECK(!MigrateLayout(f->paths, f->ops).runtime_ready);
  // A 0.6 start after the failed revert merges both places back.
  fs::remove(f->paths.app_root / "playden-data" / "session.v1");
  const auto resumed = MigrateLayout(f->paths, f->ops);
  CHECK(resumed.games_moved && resumed.notice.empty() && Moved(f->paths));
}
void StartupDefersGamesWithoutActivityChecks() {
  auto f = Make("deferred-games");
  WriteInstalls(f->paths, false);
  f->ops.defer_games = true;
  f->ops.title_running = [](const std::string&) -> std::optional<bool> { CHECK(false); return {}; };
  const auto result = MigrateLayout(f->paths, f->ops);
  CHECK(result.runtime_ready && result.user_moved && result.logs_moved && !result.games_moved);
  CHECK(fs::exists(f->paths.app_root / "games/220") && !fs::exists(f->paths.games_root));
  CHECK(LoadLayoutJournal(f->paths.state_root)->step == "games");
}

// runtime-state deleted or replaced by hand after the move: the record is written again from what
// the folders show, and startup goes on instead of holding on its own moved data.
void RebuildsALostMoveRecord() {
  bool rebuilt = false;
  {
    auto f = Make("lost-record");
    CHECK(MigrateLayout(f->paths, f->ops).games_moved && MarkLayoutDone(f->paths.state_root));
    fs::remove_all(f->paths.state_root);
    Write(f->paths.app_root / "logs" / "native-startup.log", "pre-grant\n");  // every start writes one
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(result.runtime_ready && result.user_moved && result.logs_moved && result.games_moved && result.notice.empty());
    CHECK(result.user_root == f->paths.user_root && Read(f->paths.user_root / "session.v1") == "credentials");
    const auto journal = LoadLayoutJournal(f->paths.state_root);
    CHECK(journal && journal->step == "games-moved" && journal->to == f->paths.games_root.string());
    for (const auto& line : f->lines) rebuilt = rebuilt || line.find("move record rebuilt") != std::string::npos;
    CHECK(rebuilt);
    // It resumed at the logs step: the app folder's new log moved too, beside the one already there.
    CHECK(!fs::exists(f->paths.app_root / "logs" / "native-startup.log"));
    CHECK(Read(f->paths.log_root / "native-startup.log.moved") == "pre-grant\n");
  }
  {
    // Lost while the games still waited in the app folder: their move goes on later.
    auto f = Make("lost-record-deferred");
    f->ops.defer_games = true;
    CHECK(MigrateLayout(f->paths, f->ops).user_moved);
    fs::remove_all(f->paths.state_root);
    const auto result = MigrateLayout(f->paths, f->ops);
    CHECK(result.runtime_ready && result.user_moved && !result.games_moved);
    CHECK(LoadLayoutJournal(f->paths.state_root)->step == "games" && fs::exists(f->paths.app_root / "games/220"));
  }
}

void PartialUserMoveBlocksServicesAndOnlyMovesForward() {
  auto f = Make("partial-user");
  const auto old = f->paths.app_root / "playden-data";
  unsigned attempts = 0;
  f->ops.rename = [&](const fs::path& from, const fs::path& to) -> std::error_code {
    if (from.parent_path() == old && ++attempts == 2) return {EIO, std::generic_category()};
    return ::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : std::error_code{errno, std::generic_category()};
  };
  const auto first = MigrateLayout(f->paths, f->ops);
  CHECK(!first.runtime_ready && !first.user_moved);
  CHECK(fs::exists(f->paths.user_root / "game-settings-private") && !fs::exists(old / "game-settings-private"));
  CHECK(Read(f->paths.state_root / kLayoutJournal).find("game-settings-private") != std::string::npos);
  f->ops.rename = [](const fs::path&, const fs::path&) { return std::error_code(EIO, std::generic_category()); };
  const auto retry = MigrateLayout(f->paths, f->ops);
  CHECK(!retry.runtime_ready && !fs::exists(old / "game-settings-private"));
  f->ops.rename = {};
  CHECK(MigrateLayout(f->paths, f->ops).runtime_ready && Moved(f->paths));
}

void RenameParentsSyncBeforeJournalAdvances() {
  auto f = Make("sync-order");
  std::vector<fs::path> pending;
  std::string journal_at_rename;
  unsigned renames = 0, syncs = 0;
  f->ops.rename = [&](const fs::path& from, const fs::path& to) -> std::error_code {
    CHECK(pending.empty());
    journal_at_rename = Read(f->paths.state_root / kLayoutJournal);
    const int result = ::rename(from.c_str(), to.c_str());
    if (!result) { pending = {from.parent_path(), to.parent_path()}; ++renames; }
    return result == 0 ? std::error_code{} : std::error_code{errno, std::generic_category()};
  };
  f->ops.sync = [&](const fs::path& path) {
    if (!pending.empty()) {
      CHECK(Read(f->paths.state_root / kLayoutJournal) == journal_at_rename);
      CHECK(path == pending.front());
      pending.erase(pending.begin());
      ++syncs;
    }
    return true;
  };
  CHECK(MigrateLayout(f->paths, f->ops).games_moved);
  CHECK(renames == 6 && syncs == 2 * renames && pending.empty());
  CHECK(RevertLayout(f->paths, f->ops).complete);
  CHECK(renames == 12 && syncs == 2 * renames && pending.empty());
}

void RevertRetriesTheLastRenameSync() {
  auto f = Make("revert-last-sync");
  CHECK(MigrateLayout(f->paths, f->ops).games_moved);
  bool failed = false;
  f->ops.sync = [&](const fs::path& path) {
    if (path == f->paths.log_root && fs::is_empty(path)) { failed = true; return false; }
    return true;
  };
  CHECK(!RevertLayout(f->paths, f->ops).complete && failed);
  CHECK(LoadLayoutJournal(f->paths.state_root)->step == "revert-logs");
  bool retried = false;
  f->ops.sync = [&](const fs::path& path) { if (path == f->paths.log_root) retried = true; return true; };
  CHECK(RevertLayout(f->paths, f->ops).complete && retried);
}

void FailedDirectorySyncKeepsUserStepBlocked() {
  auto f = Make("user-sync");
  const auto old = f->paths.app_root / "playden-data";
  bool renamed = false;
  f->ops.rename = [&](const fs::path& from, const fs::path& to) -> std::error_code {
    const int result = ::rename(from.c_str(), to.c_str());
    renamed = result == 0 && from.parent_path() == old;
    return result == 0 ? std::error_code{} : std::error_code{errno, std::generic_category()};
  };
  f->ops.sync = [&](const fs::path& path) { return !renamed || path != old; };
  const auto failed = MigrateLayout(f->paths, f->ops);
  CHECK(!failed.runtime_ready && !failed.user_moved);
  CHECK(LoadLayoutJournal(f->paths.state_root)->step == "user");
  f->ops.sync = {};
  f->ops.rename = {};
  CHECK(MigrateLayout(f->paths, f->ops).runtime_ready && Moved(f->paths));
}

}  // namespace

int main() {
  char pattern[] = "/tmp/playden-layout-XXXXXX";
  CHECK(mkdtemp(pattern));
  g_base = fs::canonical(pattern);
  ::umask(022);
  StartupDefersGamesWithoutActivityChecks();
  PartialUserMoveBlocksServicesAndOnlyMovesForward();
  FailedDirectorySyncKeepsUserStepBlocked();
  RenameParentsSyncBeforeJournalAdvances();
  RevertRetriesTheLastRenameSync();
  MovesUserDataLogsAndGames();
  KeepsStubFolders();
  ResumesAfterEachStep();
  PostponesGamesWhileJournalPending();
  LogsStepInterruptedTwice();
  FreshInstallMakesSharedGamesFolder();
  PostponesGamesWhileLaunchPending();
  PendingLaunchOfStoppedGameMoves();
  RunningGameFromHomeHoldsTheMove();
  MovesReadyGameWithCommittedPublication();
  RefusesToMergeIntoNonEmptyTarget();
  RebuildsALostMoveRecord();
  AbortsOnCrossDevice();
  RevertsTheMove();
  RevertKeepsConflictsAndJournal();
  fs::remove_all(g_base);
  std::puts("layout migration tests passed");
}
