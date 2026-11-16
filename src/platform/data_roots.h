// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Every place Playden writes to on the console, in one list (#868). Uninstall Playden, the startup
// self-heal and the README's "Where Playden keeps data" table use it; a test keeps the README in
// step. Paths are console paths.
//
// Modes: everything Playden makes is open so the console owner can delete it over FTP (LAN tools
// run as root, which has no permission bypass on /data): folders 0777, files 0666. Only the files
// named by IsSecretFile stay 0600 (still deletable: their folders are 0777). The bottles of live
// games stay owner-only until their title's runtime kit has publication-open-modes.
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace playden::platform::data_roots {

inline constexpr const char* kData = "/data";
inline constexpr const char* kPlaydenRoot = "/data/playden";
inline constexpr const char* kGamesFilesRoot = "/data/playden-games";
inline constexpr const char* kImportRoot = "/data/playden-import";
inline constexpr const char* kHomebrewRoot = "/data/homebrew";
inline constexpr const char* kAppTitleId = "FAKE77031";
// The folder Playden keeps on each drive, under its mount point (/mnt/ext0/playden).
inline constexpr const char* kDriveFolder = "playden";
// Drive mount points Playden looks at; a USB drive's number can change between boots.
const std::vector<std::filesystem::path>& DriveMounts();

// Folders under kPlaydenRoot.
inline constexpr const char* kUser = "user";
inline constexpr const char* kLogs = "logs";
inline constexpr const char* kState = "runtime-state";
inline constexpr const char* kRequests = "requests";  // under runtime-state
inline constexpr const char* kTrash = "trash";        // under runtime-state
inline constexpr const char* kArt = "art";
inline constexpr const char* kGames = "games";  // bottles: games/<install id>, kept ones games/<id>.kept
inline constexpr const char* kKits = "kits";
inline constexpr const char* kTitleRule = "title-rule";
inline constexpr const char* kSelfUpdate = "self-update";
inline constexpr const char* kStaging = "staging";
inline constexpr const char* kTitleRemover = "title-remover";
inline constexpr const char* kSteamClient = "steam-client";
inline constexpr const char* kExportedRecipes = "exported-recipes";
inline constexpr const char* kRedist = "redist";  // Microsoft redistributables Playden downloads for games
// Folders under user/.
inline constexpr const char* kGameSettingsPrivate = "game-settings-private";
inline constexpr const char* kCommunityRecipes = "community-recipes";
inline constexpr const char* kWebAccess = "web-access";
inline constexpr const char* kGog = "gog";
// The uninstall's journal, under kPlaydenRoot: while it exists, startup finishes the removal and
// recreates nothing.
inline constexpr const char* kUninstallJournal = "uninstall.json";

// Who owns a root, and whether a cleanup tool running as root can delete inside it.
enum class RootOwner { kRoot, kApp };

struct DataRoot {
  std::filesystem::path path;  // console path
  std::string holds;           // what it holds, in player words (the README table)
  RootOwner owner = RootOwner::kApp;
  bool per_drive = false;      // one on each drive, under DriveMounts()
};

// Every root, outermost first. The README table lists exactly these paths.
const std::vector<DataRoot>& DataRoots();

// Secrets: a sign-in or a pairing token. Owner-only (0600) files, still deletable over FTP. `name`
// is a file name; `relative` is its path under the root that holds it. The launcher sign-ins in a
// bottle are runtime::PrivateProfilePaths().
// Inline: low-level stores and their native-policy tests use it without the data_roots library.
inline const std::vector<std::string>& SecretFileNames() {
  static const std::vector<std::string> names{
      "session.v1",       // Steam sign-in (user/)
      "steam-device-id",  // Steam machine id (user/)
      "browsers.v1",      // paired browsers (user/web-access/)
      "sharing-id",       // community recipes install secret (user/community-recipes/)
      "account-token",    // community recipes account (user/community-recipes/)
      "session",          // GOG sign-in (user/gog/)
  };
  return names;
}

inline bool IsSecretFile(std::string_view name) {
  const auto& names = SecretFileNames();
  return std::find(names.begin(), names.end(), name) != names.end();
}


// The mode a file Playden writes gets: 0600 for a secret, else 0666.
inline unsigned FileModeFor(std::string_view name) { return IsSecretFile(name) ? 0600 : 0666; }

}  // namespace playden::platform::data_roots
