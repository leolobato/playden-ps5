// SPDX-License-Identifier: GPL-3.0-or-later
// A real web server over made-up games in a folder, for tests/test_web_files_smoke.py and for trying the page in a
// browser: web_files_fixture_server <empty folder> [port]. Prints "port=<n>", then each pairing code as "code=<digits>".
#include "web/service.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace playden::web;
namespace fs = std::filesystem;

namespace {
void Put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  fs::create_directories(argv[1]);
  const auto base = fs::canonical(argv[1]);
  const std::uint16_t wanted = argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : 0;

  const auto bottle = [&](const std::string& id) { return base / "games" / id / "library/prefixes/game"; };
  const auto gta = bottle("steam-12210");
  Put(gta / "drive_c/users/prospero/Documents/Rockstar Games/GTA IV/Profiles/7A3C91F0/SGTA400", std::string(512 * 1024, 's'));
  Put(gta / "drive_c/users/prospero/Documents/Rockstar Games/GTA IV/Profiles/7A3C91F0/cfg.set", "config");
  Put(gta / "drive_c/users/prospero/AppData/Roaming/settings.ini", "[video]\nvsync=1\n");
  fs::create_directories(gta / "drive_c/ProgramData");
  const auto gta_folder = base / "usb/playden-games/steam-12210";
  Put(gta_folder / "GTAIV.exe", "exe");
  Put(gta_folder / "autoexec.cfg", "fps_max 60\n");
  Put(gta_folder / "steam_settings/configs.user.ini", "[user::general]\naccount_name=Player\n");
  Put(gta_folder / ".playden-install.json", "{}");
  const auto wolverine = bottle("local-3fa9c1");
  fs::create_directories(wolverine / "drive_c/users/prospero/Documents");
  const auto wolverine_folder = base / "usb/playden/import/Wolverine";
  Put(wolverine_folder / "Binaries/Wolverine.exe", "exe");
  Put(wolverine_folder / "Support/DirectX/DXSETUP.exe", "dx");
  const auto darkwood = bottle("steam-274520");
  Put(darkwood / "drive_c/users/prospero/AppData/LocalLow/Acid Wizard Studio/Darkwood/save.dat", "kept");

  FileCatalog catalog;
  catalog.known = true;
  {
    FileGame game;
    game.id = "steam-12210";
    game.name = "Grand Theft Auto IV";
    game.kind = "steam";
    game.app = 12210;
    game.bottle = gta.string();
    game.game_folder = gta_folder.string();
    game.game_folder_shown = "/mnt/usb0/playden-games/steam-12210";
    game.drive = "usb";
    game.windows_user_bytes = 18'000'000;
    game.last_played = 1793700000;
    catalog.games.push_back(game);
    game = {};
    game.id = "local-3fa9c1";
    game.name = "X-Men Origins - Wolverine";
    game.kind = "imported";
    game.bottle = wolverine.string();
    game.game_folder = wolverine_folder.string();
    game.game_folder_shown = "/mnt/usb0/playden/import/Wolverine";
    game.drive = "usb";
    game.physx_needed = "2.8.1";
    catalog.games.push_back(game);
    game = {};
    game.id = "steam-1091500";
    game.name = "Cyberpunk 2077";
    game.kind = "steam";
    game.app = 1091500;
    game.bottle = bottle("steam-1091500").string();
    game.drive = "usb";
    game.game_folder = (base / "absent").string();
    game.game_folder_shown = "/mnt/usb0/absent";
    game.drive_absent = true;
    game.running = true;
    game.cloud = true;
    catalog.games.push_back(game);
    game = {};
    game.id = "steam-274520";
    game.name = "Darkwood";
    game.kind = "steam";
    game.app = 274520;
    game.bottle = darkwood.string();
    game.installed = false;
    game.cloud = true;
    catalog.games.push_back(game);
  }
  fs::create_directories(bottle("steam-1091500") / "drive_c/users/prospero");

  ServiceConfig config;
  config.paths = {(base / "logs").string(), (base / "homebrew").string(), (base / "kits").string(), (base / "app").string()};
  fs::create_directories(base / "logs");
  config.server.first_port = config.server.last_port = wanted;
  config.server.loopback_only = true;
  // Import recipe: any file is used by the game named, or by GTA IV from the games list; a file with "bad" is refused.
  config.files.import_recipe = [](const std::string& game, std::string file) -> RecipeImportAnswer {
    if (file.find("bad") != std::string::npos) return {422, "This file isn't a Playden recipe."};
    return {200, "The game now uses this recipe.", game.empty() ? std::string("steam-12210") : game};
  };
  Service service(std::move(config));
  service.SetFileCatalog(catalog);
  service.SetEnabled(true);
  std::string shown;
  bool reported = false;
  while (true) {
    const auto snapshot = service.Snapshot();
    if (!reported && snapshot.state == ServiceSnapshot::State::kOn) {
      std::printf("port=%u\n", snapshot.port);
      std::fflush(stdout);
      reported = true;
    }
    if (snapshot.state == ServiceSnapshot::State::kFailed) {
      std::fprintf(stderr, "%s\n", snapshot.error.c_str());
      return 1;
    }
    if (snapshot.pairing_code != shown) {
      shown = snapshot.pairing_code;
      if (!shown.empty()) {
        std::printf("code=%s\n", shown.c_str());
        std::fflush(stdout);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
