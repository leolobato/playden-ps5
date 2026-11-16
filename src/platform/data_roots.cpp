// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/data_roots.h"

namespace playden::platform::data_roots {
namespace fs = std::filesystem;

const std::vector<fs::path>& DriveMounts() {
  static const std::vector<fs::path> mounts{"/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2",
                                            "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"};
  return mounts;
}

const std::vector<DataRoot>& DataRoots() {
  const fs::path data = kPlaydenRoot;
  static const std::vector<DataRoot> roots{
      {data, "Playden's own data (everything below)", RootOwner::kRoot},
      {data / kUser, "Sign-in, preferences, game settings, shared recipes, paired browsers"},
      {data / kLogs, "Logs"},
      {data / kState, "Installed games, their titles, pending removals"},
      {data / kArt, "Game artwork cache"},
      {data / kGames, "Bottles: each game's Windows folder and saves; <id>.kept are kept saves"},
      {data / kStaging, "Titles and bottles being built"},
      {data / kSteamClient, "Valve's Steam client, for games with Steam features = Online"},
      {data / kExportedRecipes, "Recipes exported as files"},
      {data / kRedist, "Microsoft runtimes Playden downloads for games"},
      {data / kTitleRemover, "Requests to the title remover"},
      {data / kKits, "The runtime kit", RootOwner::kRoot},
      {data / kTitleRule, "The list of game titles Playden may register", RootOwner::kRoot},
      {data / kSelfUpdate, "Playden updates being installed", RootOwner::kRoot},
      {kGamesFilesRoot, "Game files on internal storage"},
      {kImportRoot, "Games you copy in to import (yours: kept unless empty)", RootOwner::kRoot},
      {fs::path(kHomebrewRoot) / "FAKE78xxx", "One PS5 title per game"},
      {fs::path("/mnt/<drive>") / kDriveFolder, "Game files and imports on a drive", RootOwner::kRoot, true},
  };
  return roots;
}

}  // namespace playden::platform::data_roots
