// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "installs/install_record.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace playden::runtime {

// A Steam library where Valve's client (Steam features = Online, runtime/steam_host.h) finds a game installed.
//
// The client answers ISteamApps (BIsAppInstalled, GetAppInstallDir, GetInstalledDepots) from the appmanifest files in
// the libraries its libraryfolders.vdf lists. Without them it says the game isn't installed and names a folder that
// doesn't exist. A title that reads [steam] shows the game at C:\Program Files (x86)\Steam\steamapps\common\<installdir>
// (prospero-win src/pw_steam_view.h), so that is the library: the shared client lists it once (each bottle has its own
// C:), and each game's bottle holds its own appmanifest_<appid>.acf there.

inline constexpr const char* kSteamLibraryWindowsPath = "C:\\Program Files (x86)\\Steam";
inline constexpr const char* kSteamLibraryAppsFolder = "drive_c/Program Files (x86)/Steam/steamapps";

// libraryfolders.vdf: library "0" is the client's own folder (client_windows_path, its SteamPath), "1" the bottle's
// kSteamLibraryWindowsPath.
std::string SteamLibraryFoldersText(const std::string& client_windows_path);

struct SteamAppManifest {
  std::uint32_t app_id = 0;
  std::string name;
  std::string install_dir;  // the folder in steamapps\common
  std::int64_t last_updated = 0;
  installs::InstalledRecord record;  // the install's size, branch, language and depots
};
// appmanifest_<appid>.acf (Steam's AppState), installed and up to date: StateFlags 4, the record's depots with their
// manifests, its size, branch (betakey, when not public) and language.
std::string SteamAppManifestText(const SteamAppManifest& manifest);
std::string SteamAppManifestName(std::uint32_t app_id);

// Writes libraryfolders.vdf in the client's steamapps and config folders when it doesn't list the bottle's library
// yet (the client may rewrite the file with what it learns; a listing that has the library is left alone). Files are
// 0666, like the client's own: the game's title, which runs as another user, writes there. False when it can't.
bool WriteSteamLibraryFolders(const std::filesystem::path& client_folder, const std::string& client_windows_path);
// Writes the game's appmanifest in the bottle's steamapps (prefix: the bottle), creating the folders, when the text
// differs. False when it can't.
bool WriteSteamAppManifest(const std::filesystem::path& prefix, std::uint32_t app_id, const std::string& text);

}  // namespace playden::runtime
