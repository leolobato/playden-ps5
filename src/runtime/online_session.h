#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace playden::runtime {

// The handoff of the account's Steam session to a game set to Steam features = Online. Playden writes it at Play into
// the game's private bottle (C:\playden\online-session.txt, mode 0600), and the Steam host beside the game
// (tools/steam-host) reads it once as it starts and deletes it. It never goes into the game's folder, which may be on a drive
// without permissions. Leftovers (a Play that did not start the game) are removed the next time Playden looks.
// With Steam sign-in = Kept for the Home screen, the file says keep=1: the host leaves it, so a start from the PS5
// Home screen signs in too, and Playden removes it only when the game leaves that mode or the account signs out.
struct OnlineSession {
  std::uint32_t app_id = 0;
  std::uint64_t steam_id = 0;
  std::string account_name;
  std::string refresh_token;
  // Playden's own device fields (steam/backend/cm_client MakeDeviceIdentity), and a login ID that is not Playden's
  // (BridgeLoginId). The Steam host does not use them: Valve's client has its own.
  std::string machine_name;
  std::vector<std::uint8_t> machine_id;
  std::uint32_t login_id = 0;
  std::uint32_t cell_id = 0;
  bool keep = false;  // the host leaves the file for the next start
  // Game invites: ask, join, show or off: what the Steam host does with a friend's invite while the game runs. Empty
  // writes no key, which the host takes as off.
  std::string invites;
  // The SHA-256 of the steamclient64.dll Playden installed (64 lower-case hex digits), from Playden's own record: the
  // client folder is open to everyone, so the host loads the DLL only when it has this hash. Empty writes no key.
  std::string client_sha256;
  // How the account shows to Steam friends while the game runs (Friends tab › Appear invisible): persona=invisible, or
  // persona=online. A file kept for the Home screen keeps the choice it was written with until it is written again.
  bool invisible = false;
  // Game settings › Network adapters is Off (PW_NSIPROXY_DISABLE): adapters=off, and the host doesn't make the
  // console's network adapters listable. Otherwise no key, and it does: Valve's client needs them for peer-to-peer.
  bool adapters_off = false;
  // Settings › Online play › Steam sign-in notice: notice=1, and the host shows "Online on Steam as <name>" once the
  // game may start. Off writes no key.
  bool notice = false;
};

// What one Play from Playden asks for once, beside the handoff (C:\playden\online-launch.txt, 0600): join a friend's
// lobby, and invite friends to the first lobby the game opens or joins. The Steam host reads it and deletes it whatever
// keep says, passes the lobby to the game and sends the invites (docs/online-play.md); Playden writes it only at a Play
// that hands over the session, and removes a leftover wherever it removes a handoff, and whenever a Play does not start
// or ends.
struct OnlineLaunch {
  std::uint64_t connect_lobby = 0;
  std::vector<std::uint64_t> invites;  // Steam IDs; the first kOnlineLaunchMaxInvites different ones are written
};

inline constexpr const char* kOnlineSessionFolder = "drive_c/playden";
inline constexpr const char* kOnlineSessionFile = "online-session.txt";
inline constexpr const char* kOnlineLaunchFile = "online-launch.txt";
inline constexpr std::size_t kOnlineLaunchMaxInvites = 16;

// A login ID that is never Playden's own, so neither session replaces the other, and stable for a device.
std::uint32_t BridgeLoginId(std::uint32_t playden_login_id);

// The file's text; empty when a value is missing or would break a line.
std::string RenderOnlineSession(const OnlineSession& session);

// Writes the handoff into the bottle at prefix (its drive_c must exist). False when it could not be written.
bool WriteOnlineSession(const std::filesystem::path& prefix, const OnlineSession& session);

// Whether the bottle at prefix holds a handoff.
bool OnlineSessionExists(const std::filesystem::path& prefix);

// Puts a kept handoff's folder back to 0700 and the file to 0600 (a recursive chmod over FTP opens them up). False when
// they could not be checked or tightened.
bool TightenOnlineSession(const std::filesystem::path& prefix);

// Removes a handoff the host did not take, and a one-launch file with it. True when neither is left; removed
// (optional) says whether a handoff was there.
bool RemoveOnlineSession(const std::filesystem::path& prefix, bool* removed = nullptr);

// The one-launch file's text: written=<Unix seconds>, then connect_lobby=<id> and invite=<steamid> lines (0 IDs and
// repeats left out, at most kOnlineLaunchMaxInvites). Empty when there is nothing to ask. A reader ignores a file
// without written or one written more than two minutes before it reads it (by the same console clock): one left over
// from a start that failed early, which a later start from the Home screen must not act on.
std::string RenderOnlineLaunch(const OnlineLaunch& launch, std::int64_t written_unix_seconds);
// The same, written now (the system clock).
std::string RenderOnlineLaunch(const OnlineLaunch& launch);

// Writes the one-launch file beside the handoff. False when there was nothing to write or it could not be written.
bool WriteOnlineLaunch(const std::filesystem::path& prefix, const OnlineLaunch& launch);

// Removes a one-launch file the host did not take. True when none is left; removed (optional) says whether one was
// there.
bool RemoveOnlineLaunch(const std::filesystem::path& prefix, bool* removed = nullptr);

// The notify file beside the host's ready file (runtime/steam_host.h kSteamHostNotify), in the bottle's C:\playden:
// there (empty, 0600) when on, so the title shows the start's steps as notifications (Logging = Verbose); removed
// otherwise. True when it is as asked.
bool SetHostNotify(const std::filesystem::path& prefix, bool on);

// Removes the sign-in Valve's client saved in the bottle at prefix: its machine-user store,
// C:\users\prospero\AppData\Local\Steam\local.vdf, which holds the saved token (the client folder keeps the account
// list, runtime/steam_client_install.h RemoveSteamClientSignIn). True when none is left; removed (optional) says
// whether one was there.
bool RemoveSteamClientBottleSignIn(const std::filesystem::path& prefix, bool* removed = nullptr);

// Removes all that signs the bottle's game in to Steam: Playden's handoff and one-launch file (RemoveOnlineSession) and
// the token Valve's client saved (RemoveSteamClientBottleSignIn), each even when the other could not go. True when none
// is left; removed (optional) says whether the handoff or the token was there.
bool RemoveOnlineSignIn(const std::filesystem::path& prefix, bool* removed = nullptr);

}  // namespace playden::runtime
