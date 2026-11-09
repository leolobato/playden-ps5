// homebrew_gog_icon_test.cpp - The icon a GOG copy of a game names in its goggame-*.info.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/gog_icon.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
using playden::homebrew::GogIconFile;

namespace {

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file);
  std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
}

// The .info GOG writes for Fallout (shortened): the launcher is primary and has no icon; the game names one.
const char* kFallout = R"json({
  "gameId": "1440148836", "rootGameId": "1440148836", "name": "Fallout",
  "playTasks": [
    {"category": "launcher", "isPrimary": true, "name": "Fallout", "path": "FalloutLauncher.exe", "type": "FileTask"},
    {"category": "game", "icon": "goggame-1440148836.ico", "name": "Fallout (Classic)", "path": "falloutw.exe", "type": "FileTask"},
    {"category": "game", "isHidden": true, "name": "FalloutHR", "path": "falloutwHR.exe", "type": "FileTask"},
    {"category": "tool", "name": "Hi-Res Config", "path": "f1_res_Config.exe", "type": "FileTask"},
    {"category": "document", "name": "Manual", "path": "manual.pdf", "type": "FileTask"},
    {"category": "document", "link": "http://www.gog.com/support/fallout", "name": "Support", "type": "URLTask"}
  ],
  "version": 1
})json";

}  // namespace

int main() {
  // The real path: the system's temporary folder is behind a link on macOS, and links are refused.
  const fs::path root = fs::canonical(fs::temp_directory_path()) / ("playden-gog-icon-" + std::to_string(::getpid()));
  fs::remove_all(root);
  int folders = 0;
  const auto folder = [&]() {
    const auto path = root / ("game-" + std::to_string(++folders));
    fs::create_directories(path);
    return path;
  };

  {  // Fallout: the program's task names the icon; the launcher's task has none, so the game task's is used.
    const auto game = folder();
    Write(game / "goggame-1440148836.info", kFallout);
    Write(game / "goggame-1440148836.ico", "ico");
    Write(game / "fallout.ico", "other");
    CHECK(GogIconFile(game.string(), "falloutw.exe") == "goggame-1440148836.ico");
    CHECK(GogIconFile(game.string(), "FALLOUTW.EXE") == "goggame-1440148836.ico");
    CHECK(GogIconFile(game.string(), "FalloutLauncher.exe") == "goggame-1440148836.ico");
    CHECK(GogIconFile(game.string(), "f1_res_Config.exe") == "goggame-1440148836.ico");
    CHECK(GogIconFile(game.string(), "") == "goggame-1440148836.ico");
    // Not a link to the game's folder, and not a relative path.
    fs::create_directory_symlink(game, root / "linked");
    CHECK(GogIconFile((root / "linked").string(), "falloutw.exe").empty());
    CHECK(GogIconFile("relative", "falloutw.exe").empty());
  }
  {  // The program's own task wins over the first game task; '\' and "./" read as the same path.
    const auto game = folder();
    Write(game / "goggame-1.info", R"({"gameId":"1","rootGameId":"1","playTasks":[
      {"category":"game","icon":"first.ico","path":"first.exe","type":"FileTask"},
      {"category":"tool","icon":"Tools.ico","path":"bin\\Tool.exe","type":"FileTask"}]})");
    Write(game / "first.ico", "a");
    Write(game / "Tools.ico", "b");
    CHECK(GogIconFile(game.string(), "bin/tool.exe") == "Tools.ico");
    CHECK(GogIconFile(game.string(), "./BIN\\Tool.exe") == "Tools.ico");
    CHECK(GogIconFile(game.string(), "other.exe") == "first.ico");
  }
  {  // An icon that isn't a plain .ico file in the folder is passed over: the folder's only goggame-*.ico is used.
    const auto game = folder();
    Write(game / "goggame-2.info", R"({"playTasks":[
      {"category":"game","icon":"sub/game.ico","path":"game.exe"},
      {"category":"game","icon":"../game.ico","path":"game.exe"},
      {"category":"game","icon":"..","path":"game.exe"},
      {"category":"game","icon":"game.png","path":"game.exe"},
      {"category":"game","icon":"missing.ico","path":"game.exe"},
      {"category":"game","icon":"linked.ico","path":"game.exe"},
      {"category":"game","icon":"folder.ico","path":"game.exe"}]})");
    Write(game / "sub/game.ico", "a");
    Write(root / "game.ico", "outside");
    Write(game / "game.png", "png");
    Write(game / "real.ico", "real");
    fs::create_symlink(game / "real.ico", game / "linked.ico");
    fs::create_directories(game / "folder.ico");
    CHECK(GogIconFile(game.string(), "game.exe").empty());
    Write(game / "goggame-2.ico", "only");
    CHECK(GogIconFile(game.string(), "game.exe") == "goggame-2.ico");
    // Two goggame-*.ico and no usable task: neither is guessed.
    Write(game / "goggame-3.ico", "second");
    CHECK(GogIconFile(game.string(), "game.exe").empty());
  }
  {  // A task without an icon, malformed JSON, or no .info at all: the only goggame-*.ico, else nothing.
    const auto game = folder();
    Write(game / "goggame-4.info", R"({"playTasks":[{"category":"game","path":"game.exe"}]})");
    CHECK(GogIconFile(game.string(), "game.exe").empty());
    Write(game / "goggame-4.ico", "only");
    CHECK(GogIconFile(game.string(), "game.exe") == "goggame-4.ico");
    const auto broken = folder();
    Write(broken / "goggame-5.info", R"({"playTasks":[{"category":"game","icon":"goggame-5.ico")");
    Write(broken / "goggame-5.ico", "only");
    Write(broken / "named.ico", "named");
    CHECK(GogIconFile(broken.string(), "game.exe") == "goggame-5.ico");
    Write(broken / "goggame-5.info", "[1, 2, 3]");
    CHECK(GogIconFile(broken.string(), "game.exe") == "goggame-5.ico");
    Write(broken / "goggame-5.info", R"({"playTasks":{"icon":"named.ico"}})");
    CHECK(GogIconFile(broken.string(), "game.exe") == "goggame-5.ico");
    const auto bare = folder();
    Write(bare / "game.ico", "not GOG's");
    CHECK(GogIconFile(bare.string(), "game.exe").empty());
    CHECK(GogIconFile((root / "missing").string(), "game.exe").empty());
  }
  {  // Several .info files (a DLC beside the game): the base game's tasks come first, whatever the names' order.
    const auto game = folder();
    Write(game / "goggame-100.info", R"({"gameId":"100","rootGameId":"900","playTasks":[
      {"category":"game","icon":"dlc.ico","path":"dlc.exe"}]})");
    Write(game / "goggame-900.info", R"({"gameId":"900","rootGameId":"900","playTasks":[
      {"category":"game","icon":"base.ico","path":"base.exe"}]})");
    Write(game / "dlc.ico", "a");
    Write(game / "base.ico", "b");
    CHECK(GogIconFile(game.string(), "launcher.exe") == "base.ico");
    CHECK(GogIconFile(game.string(), "dlc.exe") == "dlc.ico");
    // A FIFO named like an .info doesn't block, and a huge one isn't read.
    CHECK(::mkfifo((game / "goggame-000.info").c_str(), 0600) == 0);
    CHECK(GogIconFile(game.string(), "launcher.exe") == "base.ico");
    const auto huge = folder();
    Write(huge / "goggame-6.info", std::string(R"({"playTasks":[{"category":"game","icon":"a.ico","path":"a.exe"}]})") +
                                       std::string(300 * 1024, ' '));
    Write(huge / "a.ico", "a");
    CHECK(GogIconFile(huge.string(), "a.exe").empty());
  }

  fs::remove_all(root);
  std::puts("gog icon tests passed");
}
