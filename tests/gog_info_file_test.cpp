// SPDX-License-Identifier: GPL-3.0-or-later
// goggame-<id>.info: which play task is the default, which others are offered, working folders, arguments, refused
// paths, and finding the file ignoring case. Synthetic files only (fixtures/gog/README.md).
#include "gog/info_file.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace playden;
using playden::gog::Info;
using playden::gog::LaunchesFromInfo;
using playden::gog::ParseInfo;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

std::string ReadFile(const fs::path& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file);
  std::string text;
  char buffer[4096];
  for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, read);
  std::fclose(file);
  return text;
}

void WriteFile(const fs::path& path, const std::string& text) {
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file);
  CHECK(std::fwrite(text.data(), 1, text.size(), file) == text.size());
  std::fclose(file);
}

std::vector<std::string> Programs(const std::vector<stores::LaunchOption>& launches) {
  std::vector<std::string> programs;
  for (const auto& launch : launches) programs.push_back(launch.executable);
  return programs;
}

// One info file with the given play tasks (each a JSON object's text).
std::string WithTasks(const std::vector<std::string>& tasks) {
  std::string text = "{\"gameId\":\"7\",\"rootGameId\":\"7\",\"buildId\":\"9\",\"name\":\"Seven\",\"playTasks\":[";
  for (std::size_t index = 0; index < tasks.size(); ++index) text += (index ? "," : "") + tasks[index];
  return text + "]}";
}

void LauncherPrimary(const fs::path& fixtures) {
  const auto info = ParseInfo(ReadFile(fixtures / "goggame-1900000001.info"));
  CHECK(info && info->game_id == "1900000001" && info->root_game_id == "1900000001" &&
        info->build_id == "50000000000000002" && info->name == "Synthetic Game" && info->tasks.size() == 6);
  CHECK(info->tasks[0].primary && info->tasks[0].category == "launcher" && !info->tasks[0].working_dir);
  CHECK(info->tasks[2].hidden && info->tasks[5].type == "URLTask");
  const auto launches = LaunchesFromInfo(*info);
  // The launcher is the default; the hidden game comes right after it, then the other game and the tool. The manual
  // (a document) and the URLTask are not offered.
  CHECK((Programs(launches) ==
         std::vector<std::string>{"SyntheticLauncher.exe", "syntheticHR.exe", "synthetic.exe", "synthetic_config.exe"}));
  CHECK(launches[0].description == "Synthetic Game" && launches[1].description == "SyntheticHR" &&
        launches[3].description == "Config");
  for (const auto& launch : launches) CHECK(launch.working_directory.empty() && launch.arguments.empty());
}

void GamePrimaryKeepsSpelling(const fs::path& fixtures) {
  // The Jazz Jackrabbit 2 shape: a `game` primary named in lower case while the file on disk is "Jazz2.exe". The
  // spelling stays as written; the runtime finds the file ignoring case.
  const auto info = ParseInfo(ReadFile(fixtures / "goggame-1900000002.info"));
  CHECK(info && info->tasks.size() == 3);
  const auto launches = LaunchesFromInfo(*info);
  CHECK((Programs(launches) == std::vector<std::string>{"jazz2.exe", "jcs.exe"}));
  CHECK(launches[0].working_directory.empty() && launches[0].description == "Synthetic Platformer");
}

void TaskChoice() {
  // No isPrimary: the first FileTask is the default, even after a URLTask.
  {
    const auto info = ParseInfo(WithTasks({R"({"type":"URLTask","category":"document","link":"https://x.invalid"})",
                                           R"({"type":"FileTask","category":"game","path":"first.exe"})",
                                           R"({"type":"FileTask","category":"game","path":"second.exe"})"}));
    CHECK(info);
    CHECK((Programs(LaunchesFromInfo(*info)) == std::vector<std::string>{"first.exe", "second.exe"}));
  }
  // A primary later in the file comes first; the others keep file order. A hidden game is offered only after a
  // launcher, so here it is left out.
  {
    const auto info = ParseInfo(WithTasks({R"({"type":"FileTask","category":"tool","path":"tool.exe"})",
                                           R"({"type":"FileTask","category":"game","path":"hidden.exe","isHidden":true})",
                                           R"({"type":"FileTask","category":"game","path":"game.exe","isPrimary":true})"}));
    CHECK(info);
    CHECK((Programs(LaunchesFromInfo(*info)) == std::vector<std::string>{"game.exe", "tool.exe"}));
  }
  // Only URLTasks: nothing to start.
  {
    const auto info = ParseInfo(WithTasks({R"({"type":"URLTask","link":"https://x.invalid","isPrimary":true})"}));
    CHECK(info && LaunchesFromInfo(*info).empty());
  }
  // A refused primary is left out before the default is chosen: the first usable FileTask is then the default.
  {
    const auto info = ParseInfo(WithTasks({R"({"type":"FileTask","category":"game","path":"..\\up.exe","isPrimary":true})",
                                           R"({"type":"FileTask","category":"game","path":"game.exe"})"}));
    CHECK(info);
    CHECK((Programs(LaunchesFromInfo(*info)) == std::vector<std::string>{"game.exe"}));
  }
}

void WorkingFoldersAndArguments() {
  const auto info = ParseInfo(WithTasks({
      R"({"type":"FileTask","category":"game","path":"bin\\game.exe","isPrimary":true})",
      R"({"type":"FileTask","category":"game","path":"bin\\game.exe","workingDir":"","name":"Empty"})",
      R"({"type":"FileTask","category":"game","path":"bin\\game.exe","workingDir":"bin","name":"Bin"})",
      R"({"type":"FileTask","category":"game","path":"dosbox\\dosbox.exe","arguments":"-conf \"..\\game.conf\" -noconsole","workingDir":"DOSBOX"})",
      R"({"type":"FileTask","category":"game","path":"..\\escape.exe"})",
      R"({"type":"FileTask","category":"game","path":"C:\\Windows\\notepad.exe"})",
      R"({"type":"FileTask","category":"game","path":"ok.exe","workingDir":"..\\x"})",
      R"({"type":"FileTask","category":"game","path":"ok.exe","arguments":"-a\n-b"})",
      R"({"type":"FileTask","category":"game","path":""})",
  }));
  CHECK(info);
  const auto launches = LaunchesFromInfo(*info);
  CHECK(launches.size() == 4);
  // A nested program with workingDir absent or empty starts in the install root.
  CHECK(launches[0].executable == "bin/game.exe" && launches[0].working_directory.empty());
  CHECK(launches[1].executable == "bin/game.exe" && launches[1].working_directory.empty() &&
        launches[1].description == "Empty");
  CHECK(launches[2].working_directory == "bin" && launches[2].description == "Bin");
  // Arguments are kept as written; the runtime splits them.
  CHECK(launches[3].executable == "dosbox/dosbox.exe" && launches[3].working_directory == "DOSBOX" &&
        launches[3].arguments == "-conf \"..\\game.conf\" -noconsole");
}

void Limits() {
  // At most 32 options, and labels cut to 120 bytes without splitting a character.
  std::vector<std::string> tasks;
  for (int index = 0; index < 40; ++index)
    tasks.push_back(R"({"type":"FileTask","category":"game","path":"g)" + std::to_string(index) + R"(.exe"})");
  std::string name;
  for (int index = 0; index < 59; ++index) name += "a";
  for (int index = 0; index < 40; ++index) name += "\xC3\xA9";  // é: 2 bytes each, the 120-byte cut falls inside one
  tasks[0] = R"({"type":"FileTask","category":"game","path":"g0.exe","name":")" + name + R"(\tx"})";
  const auto info = ParseInfo(WithTasks(tasks));
  CHECK(info);
  const auto launches = LaunchesFromInfo(*info);
  CHECK(launches.size() == 32 && launches[31].executable == "g31.exe");
  CHECK(launches[0].description.size() == 119 && launches[0].description == name.substr(0, 119));
  const auto short_name = ParseInfo(WithTasks({R"({"type":"FileTask","path":"a.exe","name":"Tab\there"})"}));
  CHECK(short_name && LaunchesFromInfo(*short_name)[0].description == "Tab here");
}

void Parsing() {
  // A byte-order mark is accepted; malformed JSON and non-objects give nothing.
  CHECK(ParseInfo("\xEF\xBB\xBF" + WithTasks({R"({"type":"FileTask","path":"a.exe"})"})));
  CHECK(!ParseInfo(R"({"playTasks":[)"));
  CHECK(!ParseInfo("[]"));
  CHECK(!ParseInfo(""));
  // A file over 1 MiB is refused.
  CHECK(!ParseInfo(WithTasks({R"({"type":"FileTask","path":"a.exe","name":")" + std::string(1024 * 1024, 'n') + "\"}"})));
  // No playTasks: no launches. A task that isn't an object is skipped.
  const auto none = ParseInfo(R"({"gameId":"7"})");
  CHECK(none && none->tasks.empty() && LaunchesFromInfo(*none).empty());
  const auto mixed = ParseInfo(WithTasks({"7", "\"x\"", R"({"type":"FileTask","path":"a.exe"})"}));
  CHECK(mixed && mixed->tasks.size() == 1);
  // Numeric IDs read as text too.
  const auto numeric = ParseInfo(R"({"gameId":1900000003,"buildId":5})");
  CHECK(numeric && numeric->game_id == "1900000003" && numeric->build_id == "5");
}

void FindsTheFile(const fs::path& scratch, const fs::path& fixtures) {
  std::error_code error;
  fs::remove_all(scratch, error);
  fs::create_directories(scratch / "game");
  const auto root = (scratch / "game").string();
  CHECK(!gog::FindInfoFile(root, 7));
  WriteFile(scratch / "game" / "GOGGAME-7.INFO", ReadFile(fixtures / "goggame-1900000002.info"));
  WriteFile(scratch / "game" / "goggame-8.info.bak", "x");
  const auto found = gog::FindInfoFile(root, 7);
  CHECK(found && *found == root + "/GOGGAME-7.INFO");
  CHECK(!gog::FindInfoFile(root, 8));
  const auto read = gog::ReadInfo(*found);
  CHECK(read && LaunchesFromInfo(*read)[0].executable == "jazz2.exe");
  // A link or a folder is not the info file.
  fs::create_directory(scratch / "game" / "goggame-9.info");
  CHECK(!gog::FindInfoFile(root, 9));
  fs::create_symlink(scratch / "game" / "GOGGAME-7.INFO", scratch / "game" / "goggame-10.info");
  CHECK(!gog::FindInfoFile(root, 10));
  CHECK(!gog::ReadInfo(root + "/goggame-10.info"));
  CHECK(!gog::FindInfoFile("relative", 7));
  fs::remove_all(scratch, error);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <fixtures/gog> <scratch folder>\n", argv[0]);
    return 2;
  }
  const fs::path fixtures = argv[1];
  LauncherPrimary(fixtures);
  GamePrimaryKeepsSpelling(fixtures);
  TaskChoice();
  WorkingFoldersAndArguments();
  Limits();
  Parsing();
  FindsTheFile(argv[2], fixtures);
  return 0;
}
