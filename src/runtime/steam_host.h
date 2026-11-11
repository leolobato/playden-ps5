// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace playden::runtime {

// The Steam host (docs/online-play.md): for a game set to Steam features = Online, Valve's Steam client runs in a second
// process beside the game, started by the game's title (the profile's [helper] section, kit capability helper-process).
// Playden's own program for it is runtime/steam-host/steam.exe, built from tools/steam-host.

// Valve's client: Playden installs the current build of Valve's client manifest steam_client_win64 and says which
// version it is (runtime.log, the host's log, the client folder's marker, test reports of Online games). The host calls
// the client by position, as in builds 1788652215 and 1792618285: a build that moved a call shows up as a failed
// sign-in (the game starts without Steam) or a crash of the host.
// Playden's steam.exe (tools/steam-host/build.sh prints it).
inline constexpr const char* kSteamHostSha256 = "f7a42328af962e44e2a31a4d5e112b32eac70908b1269555ea9ee7d02182d167";

// Where the client's files and the host live on the console's internal storage, one copy for every game, and the same
// folder as a game's Wine sees it (drive Z: is the console's root).
inline constexpr const char* kSteamClientFolder = "/data/playden/steam-client/client";
inline constexpr const char* kSteamHostProgram = "Z:\\data\\playden\\steam-client\\client\\steam.exe";
// The file the host writes when the game may start, below the bottle's drive_c, and how long the title waits for it.
inline constexpr const char* kSteamHostReady = "playden/steam-host.ready";
inline constexpr int kSteamHostWaitSeconds = 60;
// Beside the ready file, made by Playden for a game whose Logging is Verbose: the title shows its own steps and each line
// the host adds as notifications while it waits (prospero-win's [helper]).
inline constexpr const char* kSteamHostNotify = "steam-host.ready.notify";

}  // namespace playden::runtime
