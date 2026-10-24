#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Steamworks emulation for games without a Steam client on the console: the game's
// steam_api.dll / steam_api64.dll is replaced by gbe_fork's (shipped unmodified in
// <runtime_dir>/steamworks, pinned below), the original is kept as <dll>.orig, and
// steam_settings/ beside it tells the emulator who plays and what they own. Every call is
// idempotent and must run while the game is stopped. Links are never followed.
inline constexpr const char* kSteamApi32Sha256 = "cae4372bceda997d6c73cebc1c14eb27d39b74a7999d8f28384bdfd8d746a79c";
inline constexpr const char* kSteamApi64Sha256 = "70d9826a402d3a78b0c07e9f7ea7c6832468a1113013f31ea570fccaa5445ea9";

// Playden's online bridge (tools/steam-online, 64-bit and 32-bit): with SteamworksIdentity::online it becomes the
// game's steam_api64.dll / steam_api.dll, the emulator moves beside it as steam_api64_gbe.dll / steam_api_gbe.dll, and
// steam_settings/playden_online.txt gives it the account's refresh token. It forwards every call to the emulator and
// signs in to Steam for real tickets, lobbies, friends and presence (docs/online-play.md).
inline constexpr const char* kSteamOnlineBridge64Sha256 = "85b605792dace855617bed277da7815e466d0d1e1fd8ff24e0c17ba2b5c2fb8e";
inline constexpr const char* kSteamOnlineBridge32Sha256 = "db49b29dbe63838d1069a3a43e6cf5fd5a5baabcfd19fe10ae8900608a9e2c6c";
inline constexpr const char* kSteamOnlineEmulator64Name = "steam_api64_gbe.dll";
inline constexpr const char* kSteamOnlineEmulator32Name = "steam_api_gbe.dll";
inline constexpr const char* kSteamOnlineSettingsName = "playden_online.txt";

// The emulator builds earlier Playden versions shipped (32-bit, 64-bit). A game they set up
// still has one as its DLL: it is Playden's, replaced like the current one and never kept as
// the game's original. Add the outgoing pins here whenever the two above change.
std::vector<std::string> RetiredSteamApiSha256();

// A minimal steam.dll for games from before Steamworks (built from tools/steam-legacy, no imports):
// every call says Steam runs and the game is owned.
inline constexpr const char* kLegacySteamSha256 = "f813075716892ab19ee9b0c06000c328bd5f15e713692384a30ddbcfc9065e57";

// Where the emulator keeps ISteamRemoteStorage files: <local_save_path>/<app id>/remote, which
// is where Playden's Cloud mapping looks (steam/backend/cloud_mapping.h kEmulatorUserData).
inline constexpr const char* kSteamworksSavePath = "C:\\Program Files (x86)\\Steam\\userdata\\0";

// What the download recorded beside the game (SteamworksFactsFile): installed depots and the
// DLC this account owns.
inline constexpr const char* kSteamworksFactsFile = ".playden-steam";  // "depots=1,2\ndlc=3\n"

struct SteamworksIdentity {
  std::uint32_t app_id = 0;
  std::string account_name;  // empty: "Playden"
  std::uint64_t steam_id = 0;
  std::string language = "english";
  std::vector<std::uint32_t> owned_dlc;
  std::vector<std::uint32_t> depots;  // empty: no depots.txt
  // A steam_appid.txt beside a DLL that has none (Steam games). Off for a game whose app ID came from its
  // own steam_appid.txt or Game settings: the emulator reads steam_settings/steam_appid.txt, and a file
  // Playden left in the game's folder would later pass for the game's own.
  bool write_game_appid_file = true;
  // "Steam features" = Online: the game gets the online bridge, which signs in with this refresh token.
  // Without a token the game gets the plain emulator.
  bool online = false;
  std::string refresh_token;
};

struct SteamworksResult {
  bool ok = false;
  std::string message;               // on failure: a sentence for the player
  std::vector<std::string> changed;  // DLL paths (relative) replaced or restored by this call
  std::vector<std::string> found;    // every steam_api DLL found (relative)
};

// steam_api.dll / steam_api64.dll below install_root (any case, at most 10 folders deep, no
// links, no hidden folders), relative with '/' separators, sorted.
std::vector<std::string> FindSteamApi(const std::string& install_root);

// Swaps every found DLL for the emulator and writes its steam_settings/. A DLL that already is
// the emulator, or an earlier build of it (retired), needs its .orig, which must be the game's
// own; a DLL that is neither (a newer original from an update) becomes the new .orig. No
// steam_api DLL: ok with nothing found.
SteamworksResult EmulateSteamworks(const std::string& install_root, const std::string& emulator_dir,
                                   const SteamworksIdentity& identity,
                                   const std::vector<std::string>& retired = RetiredSteamApiSha256());

// Puts each .orig back over a swapped DLL (the emulator, the online bridge or a retired build); .orig and
// steam_settings/ stay, except the bridge's emulator copy and its token file, which are removed.
SteamworksResult RestoreSteamworks(const std::string& install_root,
                                   const std::vector<std::string>& retired = RetiredSteamApiSha256());

// Games from before Steamworks (GUN) load steam.dll from beside their program and quit with "Failed
// to find Steam." without it. For a game with no steam_api DLL, puts the minimal steam.dll
// (<emulator_dir>/steam.dll, pinned above) next to each 32-bit program (relative, '/' separators)
// whose folder has no steam.dll of its own. A game that doesn't load it is unaffected.
SteamworksResult ProvideLegacySteam(const std::string& install_root, const std::string& emulator_dir,
                                    const std::vector<std::string>& programs);

// Removes a steam.dll Playden put there (same bytes as the minimal one) from each program's folder.
SteamworksResult RemoveLegacySteam(const std::string& install_root, const std::vector<std::string>& programs);

// Interface versions a steam_api DLL asks for, as gbe_fork's generate_interfaces finds them.
std::vector<std::string> SteamInterfaces(std::string_view dll_bytes);

// The facts file the download writes, and its reader (missing or unreadable: false).
std::string SerializeSteamworksFacts(const std::vector<std::uint32_t>& depots, const std::vector<std::uint32_t>& owned_dlc);
bool WriteSteamworksFacts(const std::string& install_root, const std::vector<std::uint32_t>& depots,
                          const std::vector<std::uint32_t>& owned_dlc);
bool ReadSteamworksFacts(const std::string& install_root, SteamworksIdentity* identity);

}  // namespace playden::runtime
