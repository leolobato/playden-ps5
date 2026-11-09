#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playden::runtime {

// Steamworks emulation for games without a Steam client on the console: the game's
// steam_api.dll / steam_api64.dll is replaced by gbe_fork's (shipped in <runtime_dir>/steamworks,
// built as runtime/steamworks/README.md says, pinned below), the original is kept as <dll>.orig, and
// steam_settings/ beside it tells the emulator who plays and what they own. Every call is
// idempotent and must run while the game is stopped. Links are never followed.
inline constexpr const char* kSteamApi32Sha256 = "4007fdef3062f8fa194c64d2286ab4e3d72ca1be189657a3b1a4da8f04f33bbb";
inline constexpr const char* kSteamApi64Sha256 = "3c46a4b109a1bd03a2d0255d1cc0e6e72608d4f67d621227701af4d8ad3d5ba9";

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
inline constexpr const char* kSteamworksFactsFile = ".playden-steam";  // "depots=1,2\ndlc=3\nnodlc=4\n"

struct SteamworksIdentity {
  std::uint32_t app_id = 0;
  std::string account_name;  // empty: "Playden"
  std::uint64_t steam_id = 0;
  std::string language = "english";
  std::vector<std::uint32_t> owned_dlc;
  std::vector<std::uint32_t> excluded_dlc;  // turned off at install: never added to owned_dlc
  std::vector<std::uint32_t> depots;  // empty: no depots.txt
  // A steam_appid.txt beside a DLL that has none (Steam games). Off for a game whose app ID came from its
  // own steam_appid.txt or Game settings: the emulator reads steam_settings/steam_appid.txt, and a file
  // Playden left in the game's folder would later pass for the game's own.
  bool write_game_appid_file = true;
  // Relative paths that get a copy of the game's own DLL (the .orig) while the emulator stays where the game loads it
  // from, for a game that hashes the file it finds at one of these paths (GameRecipe::steam_api_copies).
  std::vector<std::string> steam_api_copies;
  // The game's Steam Input action sets (SteamInputActionSets), as steam_settings/controller/<name>.txt. With them the
  // emulator answers Steam Input with the controller it reads through XInput; with none it reports no controller.
  std::vector<std::pair<std::string, std::string>> steam_input_sets;  // action set name, file text
};

struct SteamworksResult {
  bool ok = false;
  std::string message;               // on failure: a sentence for the player
  std::vector<std::string> changed;  // DLL paths (relative) replaced or restored by this call
  std::vector<std::string> found;    // every steam_api DLL found (relative), without steam_api_copies
  std::vector<std::string> skipped;  // steam_api_copies with no folder to go in or no single original to copy
};

// steam_api.dll / steam_api64.dll below install_root (any case, at most 10 folders deep, no
// links, no hidden folders), relative with '/' separators, sorted.
std::vector<std::string> FindSteamApi(const std::string& install_root);

// Swaps every found DLL for the emulator and writes its steam_settings/. A DLL that already is
// the emulator, or an earlier build of it (retired), needs its .orig, which must be the game's
// own; a DLL that is neither (a newer original from an update) becomes the new .orig. No
// steam_api DLL: ok with nothing found. Then each of identity.steam_api_copies gets the .orig of the one found DLL with
// its file name; these paths are not swapped.
SteamworksResult EmulateSteamworks(const std::string& install_root, const std::string& emulator_dir,
                                   const SteamworksIdentity& identity,
                                   const std::vector<std::string>& retired = RetiredSteamApiSha256());

// Puts each .orig back over a swapped DLL (the emulator or a retired build); .orig and
// steam_settings/ stay. A file at one of steam_api_copies that holds a kept .orig is removed.
SteamworksResult RestoreSteamworks(const std::string& install_root,
                                   const std::vector<std::string>& retired = RetiredSteamApiSha256(),
                                   const std::vector<std::string>& steam_api_copies = {});

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

// The facts file the download writes, and its reader (missing or unreadable: false). `language`
// is the content language the depots were installed for; English, and files written before
// Playden could choose one, leave the line out. `excluded_dlc`: DLC the player turned off at install, which each
// prepare leaves out when it adds the account's owned DLC.
std::string SerializeSteamworksFacts(const std::vector<std::uint32_t>& depots, const std::vector<std::uint32_t>& owned_dlc,
                                     const std::string& language = {}, const std::vector<std::uint32_t>& excluded_dlc = {});
bool WriteSteamworksFacts(const std::string& install_root, const std::vector<std::uint32_t>& depots,
                          const std::vector<std::uint32_t>& owned_dlc, const std::string& language = {},
                          const std::vector<std::uint32_t>& excluded_dlc = {});
bool ReadSteamworksFacts(const std::string& install_root, SteamworksIdentity* identity);

}  // namespace playden::runtime
