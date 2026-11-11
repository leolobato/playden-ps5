// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime/redist_install.h"
#include "runtime/steam_host.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Valve's Steam client files for the Steam host (runtime/steam_host.h), downloaded from Valve and never shipped with
// Playden. One copy per console, in kSteamClientFolder, with Playden's steam.exe in place of Valve's (the host loads
// Valve's steamclient64.dll itself and never runs Valve's steam.exe) and without Valve's crash reporter
// (SteamClientLeftOut). Playden takes Valve's current client: the manifest says which version and package that is.

struct SteamClientPackage {
  std::string file;    // the package's name on Valve's client CDN, as the client manifest lists it
  std::string sha256;  // lower case, of the zip (the manifest's sha2)
  std::uint64_t bytes = 0;
};
struct SteamClientManifest {
  std::string version;         // the manifest's version: digits
  SteamClientPackage package;  // bins_win64: the client's 64-bit and 32-bit libraries
};
inline constexpr const char* kSteamClientCdn = "https://client-update.akamai.steamstatic.com/";
inline constexpr const char* kSteamClientManifest = "steam_client_win64";
// Valve's client manifest (its VDF text): the version and the bins_win64 package. Nullopt when either is missing or
// malformed (a version that isn't digits, a hash that isn't 64 hex digits, a file name with a path in it).
std::optional<SteamClientManifest> ParseSteamClientManifest(std::string_view text);
// Valve's files Playden leaves out of the client folder: its crash reporter (crashhandler*.dll, which uploads minidumps
// of the process that holds the session, and steamerrorreporter*.exe). The client loads crashhandler by name when it
// is there and does without otherwise.
bool SteamClientLeftOut(std::string_view relative);

struct SteamClientInstallResult {
  std::string result;  // "installed", "already" or "failed"
  std::string detail;  // why it failed
  std::size_t files = 0;
  bool download_failed = false;  // the package could not be downloaded, or wasn't the one the manifest names
  std::string version;           // installed or already there
  std::string client_dll_sha256;  // "installed": the unpacked steamclient64.dll's SHA-256, for Playden's own record
};

// The client version installed in folder with this Playden's steam.exe (its marker says so, steamclient64.dll is
// there and steam.exe has host_sha256: the folder is open to everyone); nullopt when none is.
std::optional<std::string> SteamClientInstalled(const std::string& folder, const std::string& host_sha256 = kSteamHostSha256);

// Puts manifest's client files and host_exe (Playden's runtime/steam-host/steam.exe) in folder, unless that version is
// there already (replace: put it in again anyway). The package is kept in <parent of folder>/downloads while it
// downloads (resumed by a later call), checked against the manifest's SHA-256 and removed once unpacked. Everything is
// unpacked into a folder beside it first, then renamed into place, so folder is either whole or absent; other folders
// beside it (earlier clients, named by version) go. With fetch empty, only a finished install counts. Folders are
// 0777 and files 0666 or 0755: the client writes its own state there, and the game's title, which runs as another
// user, reads them.
SteamClientInstallResult InstallSteamClient(const std::string& folder, const std::string& host_exe, const RangeFetch& fetch,
                                            const SteamClientManifest& manifest, bool replace = false,
                                            const std::string& host_sha256 = kSteamHostSha256);

// Removes the client's saved sign-in from folder (config/config.vdf, config/loginusers.vdf), so the next start signs in
// with the session Playden hands over. True when none is left.
bool RemoveSteamClientSignIn(const std::string& folder);

}  // namespace playden::runtime
