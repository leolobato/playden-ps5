#include "runtime/steamworks.h"

#include "runtime/artifact_hash.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using namespace playden::runtime;
namespace fs = std::filesystem;

namespace {

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

// A game's own steam_api DLL: a PE header and the interface names the game was built with.
std::string GameDll(bool pe64, const std::string& tag) {
  std::string bytes(0x200, '\0');
  bytes[0] = 'M'; bytes[1] = 'Z'; bytes[0x3c] = 0x40;
  bytes[0x40] = 'P'; bytes[0x41] = 'E';
  bytes[0x44] = pe64 ? 0x64 : 0x4c; bytes[0x45] = pe64 ? static_cast<char>(0x86) : 0x01;
  bytes[0x40 + 24] = 0x0b; bytes[0x40 + 25] = pe64 ? 0x02 : 0x01;
  static const char kNames[] = "\0SteamUser021\0SteamClient017\0SteamClient020\0STEAMAPPS_INTERFACE_VERSION008\0"
                               "SteamUtils\0SteamFriendsX15\0STEAMCONTROLLER_INTERFACE_VERSION\0";
  return bytes + std::string(kNames, sizeof(kNames) - 1) + tag;
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc > 1);
  const std::string emulator = argv[1];
  const auto emulator64 = Read(fs::path(emulator) / "steam_api64.dll");
  const auto emulator32 = Read(fs::path(emulator) / "steam_api.dll");
  CHECK(!emulator64.empty() && !emulator32.empty());
  const auto root = fs::temp_directory_path() / ("playden-steamworks-" + std::to_string(::getpid()));
  fs::remove_all(root);

  // Interface names, as gbe_fork's generator finds them.
  const auto interfaces = SteamInterfaces(GameDll(true, ""));
  CHECK((interfaces == std::vector<std::string>{"STEAMAPPS_INTERFACE_VERSION008", "STEAMCONTROLLER_INTERFACE_VERSION",
                                                 "SteamClient017", "SteamUser021"}));

  // Discovery: any case, nested, no links, no hidden folders, nothing else.
  const auto own64 = GameDll(true, "own64");
  const auto own32 = GameDll(false, "own32");
  Write(root / "Game_Data/Plugins/x86_64/Steam_API64.dll", own64);
  Write(root / "bin/steam_api.dll", own32);
  Write(root / ".hidden/steam_api.dll", own32);
  Write(root / "steam_api64.dll.orig.bak", own64);
  fs::create_symlink(root / "bin/steam_api.dll", root / "steam_api.dll");
  CHECK((FindSteamApi(root.string()) ==
         std::vector<std::string>{"Game_Data/Plugins/x86_64/Steam_API64.dll", "bin/steam_api.dll"}));

  // Swap: the emulator goes in, the original is kept, steam_settings/ says who plays.
  SteamworksIdentity identity;
  identity.app_id = 2062430;
  identity.account_name = "player=one";
  identity.steam_id = 76561198000000000ull;
  identity.owned_dlc = {3001, 3000, 3001};
  identity.depots = {2062431};
  auto result = EmulateSteamworks(root.string(), emulator, identity);
  CHECK(result.ok && result.changed.size() == 2);
  const auto plugin = root / "Game_Data/Plugins/x86_64";
  CHECK(Read(plugin / "Steam_API64.dll") == emulator64 && Read(plugin / "Steam_API64.dll.orig") == own64);
  CHECK(Read(root / "bin/steam_api.dll") == emulator32 && Read(root / "bin/steam_api.dll.orig") == own32);
  CHECK(Read(plugin / "steam_appid.txt") == "2062430" && Read(plugin / "steam_settings/steam_appid.txt") == "2062430");
  const auto user = Read(plugin / "steam_settings/configs.user.ini");
  CHECK(user.find("account_name=playerone\n") != std::string::npos);
  CHECK(user.find("account_steamid=76561198000000000\n") != std::string::npos);
  CHECK(user.find("local_save_path=C:\\Program Files (x86)\\Steam\\userdata\\0\n") != std::string::npos);
  CHECK(Read(plugin / "steam_settings/configs.app.ini") == "[app::dlcs]\nunlock_all=0\n3000=dlc3000\n3001=dlc3001\n");
  CHECK(Read(plugin / "steam_settings/configs.main.ini") ==
        "[main::connectivity]\ndisable_lan_only=0\noffline=0\ndisable_networking=0\n");
  // Steam network = Offline: Steam offline, the emulator's networking off; back online again.
  identity.network = false;
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(Read(plugin / "steam_settings/configs.main.ini") ==
        "[main::connectivity]\ndisable_lan_only=0\noffline=1\ndisable_networking=1\n");
  identity.network = true;
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(Read(plugin / "steam_settings/configs.main.ini").find("offline=0\ndisable_networking=0\n") != std::string::npos);
  CHECK(Read(plugin / "steam_settings/depots.txt") == "2062431\n");
  // No other owned apps: no owned_app_ids.txt.
  CHECK(!fs::exists(plugin / "steam_settings/owned_app_ids.txt"));
  // Another owned app the game asks about (Black Mesa checks Source SDK Base 2006, 215): the emulator reads
  // owned_app_ids.txt, and the DLC list and the depots stay as they are.
  identity.owned_apps = {243730, 215};
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(Read(plugin / "steam_settings/owned_app_ids.txt") == "215\n243730\n");
  CHECK(Read(root / "bin/steam_settings/owned_app_ids.txt") == "215\n243730\n");
  CHECK(Read(plugin / "steam_settings/depots.txt") == "2062431\n");
  CHECK(Read(plugin / "steam_settings/configs.app.ini") == "[app::dlcs]\nunlock_all=0\n3000=dlc3000\n3001=dlc3001\n");
  // An app the account no longer owns leaves the file with it.
  identity.owned_apps.clear();
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(!fs::exists(plugin / "steam_settings/owned_app_ids.txt"));
  CHECK(Read(plugin / "steam_settings/steam_interfaces.txt") ==
        "STEAMAPPS_INTERFACE_VERSION008\nSTEAMCONTROLLER_INTERFACE_VERSION\nSteamClient017\nSteamUser021\n");
  CHECK(Read(plugin / "steam_interfaces.txt") == Read(plugin / "steam_settings/steam_interfaces.txt"));
  CHECK(!fs::exists(root / ".hidden/steam_api.dll.orig"));
  // No Steam Input map: no controller folder, so the emulator reports no Steam Input controller.
  CHECK(!fs::exists(plugin / "steam_settings/controller"));
  // With the game's Steam Input map, each action set gets its file.
  identity.steam_input_sets = {{"Menu", "select=A\n"}, {"OnFoot", "Move=LJOY=joystick_move\n"}};
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(Read(plugin / "steam_settings/controller/Menu.txt") == "select=A\n");
  CHECK(Read(plugin / "steam_settings/controller/OnFoot.txt") == "Move=LJOY=joystick_move\n");
  CHECK(Read(root / "bin/steam_settings/controller/Menu.txt") == "select=A\n");
  // A later setup with fewer action sets removes the others; one without a map removes them all, so the emulator
  // reports no Steam Input controller again. Other files in the folder stay.
  identity.steam_input_sets = {{"Menu", "select=A\n"}};
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(fs::exists(plugin / "steam_settings/controller/Menu.txt"));
  CHECK(!fs::exists(plugin / "steam_settings/controller/OnFoot.txt"));
  identity.steam_input_sets.clear();
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(!fs::exists(plugin / "steam_settings/controller"));
  CHECK(!fs::exists(root / "bin/steam_settings/controller"));
  Write(plugin / "steam_settings/controller/Menu.txt", "select=A\n");
  Write(plugin / "steam_settings/controller/readme.md", "mine");
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(!fs::exists(plugin / "steam_settings/controller/Menu.txt"));
  CHECK(Read(plugin / "steam_settings/controller/readme.md") == "mine");
  // A stale action set that can't be removed fails the setup: the emulator would still report a Steam Input controller.
  Write(plugin / "steam_settings/controller/Menu.txt", "select=A\n");
  CHECK(chmod((plugin / "steam_settings/controller").c_str(), 0555) == 0);
  CHECK(!EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(chmod((plugin / "steam_settings/controller").c_str(), 0755) == 0);
  CHECK(EmulateSteamworks(root.string(), emulator, identity).ok);
  CHECK(!fs::exists(plugin / "steam_settings/controller/Menu.txt"));
  fs::remove_all(plugin / "steam_settings/controller");

  // Another store's game: no steam_appid.txt of Playden's beside the DLL, only the emulator's own; a
  // game's own file stays as it is.
  {
    const auto other = root.string() + "-other";
    Write(fs::path(other) / "Bin/steam_api64.dll", own64);
    Write(fs::path(other) / "Lib/steam_api.dll", own32);
    Write(fs::path(other) / "Lib/steam_appid.txt", "99700\n");
    SteamworksIdentity other_identity;
    other_identity.app_id = 4000;
    other_identity.write_game_appid_file = false;
    const auto other_result = EmulateSteamworks(other, emulator, other_identity);
    CHECK(other_result.ok && other_result.changed.size() == 2);
    CHECK(!fs::exists(fs::path(other) / "Bin/steam_appid.txt") && Read(fs::path(other) / "Bin/steam_settings/steam_appid.txt") == "4000");
    CHECK(Read(fs::path(other) / "Lib/steam_appid.txt") == "99700\n" &&
          Read(fs::path(other) / "Lib/steam_settings/steam_appid.txt") == "4000");
    fs::remove_all(other);
  }

  // Again: nothing changes hands; the settings follow a new account.
  identity.account_name = "";
  result = EmulateSteamworks(root.string(), emulator, identity);
  CHECK(result.ok && result.changed.empty() && Read(plugin / "Steam_API64.dll.orig") == own64);
  CHECK(Read(plugin / "steam_settings/configs.user.ini").find("account_name=Playden\n") != std::string::npos);

  // A game update brings a newer original: it becomes the kept copy.
  const auto updated = GameDll(true, "updated");
  Write(plugin / "Steam_API64.dll", updated);
  result = EmulateSteamworks(root.string(), emulator, identity);
  CHECK(result.ok && result.changed.size() == 1 && Read(plugin / "Steam_API64.dll.orig") == updated);

  // Back to the game's own files, and the emulator once more.
  result = RestoreSteamworks(root.string());
  CHECK(result.ok && result.changed.size() == 2);
  CHECK(Read(plugin / "Steam_API64.dll") == updated && Read(root / "bin/steam_api.dll") == own32);
  CHECK(RestoreSteamworks(root.string()).changed.empty());
  CHECK(EmulateSteamworks(root.string(), emulator, identity).changed.size() == 2);

  // The emulator without its kept original cannot be undone: refused.
  fs::remove(root / "bin/steam_api.dll.orig");
  result = EmulateSteamworks(root.string(), emulator, identity);
  CHECK(!result.ok && result.message.find("verify the download") != std::string::npos);
  Write(root / "bin/steam_api.dll", own32);

  // A damaged emulator copy is never installed.
  const auto broken = root.string() + "-emulator";
  Write(fs::path(broken) / "steam_api64.dll", emulator64.substr(1));
  Write(fs::path(broken) / "steam_api.dll", emulator32);
  CHECK(RestoreSteamworks(root.string()).ok);
  result = EmulateSteamworks(root.string(), broken, identity);
  CHECK(!result.ok && result.message.find("damaged") != std::string::npos);
  CHECK(Read(plugin / "Steam_API64.dll") == updated);

  // The facts the download leaves beside the game.
  Write(root / kSteamworksFactsFile, SerializeSteamworksFacts({20, 10}, {3001, 3000}));
  SteamworksIdentity facts;
  CHECK(ReadSteamworksFacts(root.string(), &facts));
  CHECK((facts.depots == std::vector<std::uint32_t>{10, 20}) && (facts.owned_dlc == std::vector<std::uint32_t>{3000, 3001}));
  CHECK(facts.excluded_dlc.empty() && facts.owned_apps.empty() && facts.save_folders.empty());
  // Owned Source SDK Base apps are kept too, for a start before the next license read.
  Write(root / kSteamworksFactsFile, SerializeSteamworksFacts({10}, {}, {}, {}, {243730, 215}, 76561198000000001ull));
  CHECK(Read(root / kSteamworksFactsFile) == "depots=10\ndlc=\napps=215,243730\napps_account=76561198000000001\n");
  SteamworksIdentity sdk;
  CHECK(ReadSteamworksFacts(root.string(), &sdk));
  CHECK((sdk.owned_apps == std::vector<std::uint32_t>{215, 243730}) && (sdk.depots == std::vector<std::uint32_t>{10}));
  CHECK(sdk.owned_apps_account == 76561198000000001ull);
  // An account that isn't a number makes the file unreadable, like any other bad value.
  Write(root / kSteamworksFactsFile, "depots=10\napps=215\napps_account=x\n");
  SteamworksIdentity bad_account;
  CHECK(!ReadSteamworksFacts(root.string(), &bad_account));
  Write(root / kSteamworksFactsFile, SerializeSteamworksFacts({10}, {3000}, {}, {4001, 4000}));
  SteamworksIdentity turned_off;
  CHECK(ReadSteamworksFacts(root.string(), &turned_off));
  CHECK((turned_off.excluded_dlc == std::vector<std::uint32_t>{4000, 4001}) && (turned_off.owned_dlc == std::vector<std::uint32_t>{3000}));
  // The game's Cloud save folders in the Windows user folder (#714), one line each. A folder that is not a plain path
  // below that folder is left out, on writing and on reading, and the rest of the file still reads.
  Write(root / kSteamworksFactsFile,
        SerializeSteamworksFacts({10}, {}, {}, {}, {}, 0,
                                 {"AppData/LocalLow/Eternity/Leaf it Alone/Saves", "../Public", "Documents/My Games/X"}));
  CHECK(Read(root / kSteamworksFactsFile) ==
        "depots=10\ndlc=\nsave=AppData/LocalLow/Eternity/Leaf it Alone/Saves\nsave=Documents/My Games/X\n");
  SteamworksIdentity saves;
  CHECK(ReadSteamworksFacts(root.string(), &saves));
  CHECK((saves.save_folders == std::vector<std::string>{"AppData/LocalLow/Eternity/Leaf it Alone/Saves", "Documents/My Games/X"}));
  Write(root / kSteamworksFactsFile,
        "depots=10\ndlc=3000\nsave=../../../../etc\nsave=/data/x\nsave=AppData//X\nsave=AppData/./X\nsave=C:/X\n"
        "save=AppData\\X\nsave=A/%TEMP%\nsave=\nsave=AppData/Roaming/Game\n");
  SteamworksIdentity unsafe;
  CHECK(ReadSteamworksFacts(root.string(), &unsafe));
  CHECK((unsafe.save_folders == std::vector<std::string>{"AppData/Roaming/Game"}) &&
        (unsafe.owned_dlc == std::vector<std::uint32_t>{3000}));
  CHECK(IsProfileSaveFolder("Saved Games/Game") && IsProfileSaveFolder("a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p") &&
        !IsProfileSaveFolder("a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q") &&
        IsProfileSaveFolder(std::string(kMaxProfileSaveFolderBytes, 'a')) &&
        !IsProfileSaveFolder(std::string(kMaxProfileSaveFolderBytes + 1, 'a')));
  // At most kMaxProfileSaveFolders, written or read.
  std::vector<std::string> many;
  std::string many_lines = "depots=10\n";
  for (std::size_t index = 0; index <= kMaxProfileSaveFolders; ++index) {
    many.push_back("Game/" + std::to_string(index));
    many_lines += "save=Game/" + std::to_string(index) + "\n";
  }
  CHECK(SerializeSteamworksFacts({10}, {}, {}, {}, {}, 0, many).find("save=Game/" + std::to_string(kMaxProfileSaveFolders)) ==
        std::string::npos);
  Write(root / kSteamworksFactsFile, many_lines);
  SteamworksIdentity capped;
  CHECK(ReadSteamworksFacts(root.string(), &capped) && capped.save_folders.size() == kMaxProfileSaveFolders);
  // A file from before Playden could choose a language reads as English, which is what it got.
  CHECK(facts.language == "english");

  // Each game keeps the language it was downloaded in, whatever Settings says now, so the
  // emulator tells the game the language its files are actually in.
  Write(root / kSteamworksFactsFile, SerializeSteamworksFacts({20, 10}, {3001, 3000}, "russian"));
  SteamworksIdentity localised;
  CHECK(ReadSteamworksFacts(root.string(), &localised));
  CHECK(localised.language == "russian" && (localised.depots == std::vector<std::uint32_t>{10, 20}));
  // English is the default, so it is not written out; the reader answers with it either way.
  CHECK(SerializeSteamworksFacts({10}, {}, "english").find("language=") == std::string::npos);
  // A language Steam does not have leaves the game on English rather than reaching the emulator.
  Write(root / kSteamworksFactsFile, "depots=10\ndlc=\nlanguage=klingon\n");
  SteamworksIdentity unknown;
  CHECK(ReadSteamworksFacts(root.string(), &unknown));
  CHECK(unknown.language == "english");

  Write(root / kSteamworksFactsFile, "depots=1,x\n");
  CHECK(!ReadSteamworksFacts(root.string(), &facts));
  // A game without steam_api: nothing to do.
  CHECK(EmulateSteamworks((root / "Game_Data/Plugins").string() + "-none", emulator, identity).ok);

  {  // A game from before Steamworks gets the minimal steam.dll beside its 32-bit program (GUN).
    const auto legacy = root / "legacy";
    const auto stub = Read(fs::path(emulator) / "steam.dll");
    CHECK(!stub.empty());
    Write(legacy / "Gun.exe", GameDll(false, "gun"));
    Write(legacy / "x64/Tool.exe", GameDll(true, "tool"));
    Write(legacy / "own/Old.exe", GameDll(false, "old"));
    Write(legacy / "own/Steam.dll", "the game's own steam.dll");
    const std::vector<std::string> programs = {"Gun.exe", "x64/Tool.exe", "own/Old.exe"};
    auto added = ProvideLegacySteam(legacy.string(), emulator, programs);
    CHECK(added.ok && (added.changed == std::vector<std::string>{"steam.dll"}));
    CHECK(Read(legacy / "steam.dll") == stub && !fs::exists(legacy / "x64/steam.dll"));
    CHECK(Read(legacy / "own/Steam.dll") == "the game's own steam.dll");
    // Again: nothing changes. The game's own mode takes only Playden's away.
    CHECK(ProvideLegacySteam(legacy.string(), emulator, programs).changed.empty());
    auto removed = RemoveLegacySteam(legacy.string(), programs);
    CHECK(removed.ok && (removed.changed == std::vector<std::string>{"steam.dll"}));
    CHECK(!fs::exists(legacy / "steam.dll") && fs::exists(legacy / "own/Steam.dll"));
    // A runtime without the pinned steam.dll refuses rather than adding something else.
    const auto fake = root / "fake-emulator";
    Write(fake / "steam.dll", "not the stub");
    CHECK(!ProvideLegacySteam(legacy.string(), fake.string(), programs).ok && !fs::exists(legacy / "steam.dll"));
  }

  {  // A Playden update brings a new emulator build; games set up by the earlier one keep their original.
    const auto game = root / "updated-playden";
    const auto own = GameDll(false, "own-after-update");
    const std::string earlier = "an earlier Playden's emulator build";
    const std::vector<std::string> retired = {*HashArtifactBytes(earlier)};
    Write(game / "steam_api.dll", earlier);
    Write(game / "steam_api.dll.orig", own);
    auto swapped = EmulateSteamworks(game.string(), emulator, identity, retired);
    CHECK(swapped.ok && (swapped.changed == std::vector<std::string>{"steam_api.dll"}));
    CHECK(Read(game / "steam_api.dll") == emulator32 && Read(game / "steam_api.dll.orig") == own);
    CHECK(Read(game / "steam_settings/steam_interfaces.txt") ==
          "STEAMAPPS_INTERFACE_VERSION008\nSTEAMCONTROLLER_INTERFACE_VERSION\nSteamClient017\nSteamUser021\n");
    // The game's own mode puts it back from an earlier build too.
    Write(game / "steam_api.dll", earlier);
    auto restored = RestoreSteamworks(game.string(), retired);
    CHECK(restored.ok && Read(game / "steam_api.dll") == own);
    // An earlier Playden kept its emulator as the original: the game's own is lost, and nothing is
    // swapped or written over it.
    Write(game / "steam_api.dll", emulator32);
    Write(game / "steam_api.dll.orig", earlier);
    swapped = EmulateSteamworks(game.string(), emulator, identity, retired);
    CHECK(!swapped.ok && swapped.message.find("reinstall the game") != std::string::npos);
    CHECK(Read(game / "steam_api.dll.orig") == earlier);
    restored = RestoreSteamworks(game.string(), retired);
    CHECK(!restored.ok && restored.message.find("reinstall the game") != std::string::npos);
    CHECK(Read(game / "steam_api.dll") == emulator32);
    // The pins Playden ships cover both architectures of every earlier build.
    CHECK(RetiredSteamApiSha256().size() == 8);
  }

  {  // A game that hashes its library at one path and loads it from another (Clair Obscur: Expedition 33).
    const auto game = root / "hashes-its-library";
    const std::string engine = "Engine/Binaries/ThirdParty/Steamworks/Steamv157/Win64/steam_api64.dll";
    const std::string copy = "Sandfall/Binaries/Win64/steam_api64.dll";
    const auto own = GameDll(true, "own");
    Write(game / engine, own);
    Write(game / "Sandfall/Binaries/Win64/Game.exe", "program");
    auto hashed = identity;
    hashed.steam_api_copies = {copy};
    auto swapped = EmulateSteamworks(game.string(), emulator, hashed);
    CHECK(swapped.ok && swapped.skipped.empty() && (swapped.found == std::vector<std::string>{engine}));
    CHECK((swapped.changed == std::vector<std::string>{engine, copy}));
    CHECK(Read(game / engine) == emulator64 && Read(game / (engine + ".orig")) == own && Read(game / copy) == own);
    CHECK(!fs::exists(game / "Sandfall/Binaries/Win64/steam_settings"));
    // Again: the copy isn't found as a library to swap, and nothing changes.
    swapped = EmulateSteamworks(game.string(), emulator, hashed);
    CHECK(swapped.ok && swapped.changed.empty() && swapped.found.size() == 1 && Read(game / copy) == own);
    // A game update brings a newer original: the copy follows it.
    const auto newer = GameDll(true, "newer");
    Write(game / engine, newer);
    swapped = EmulateSteamworks(game.string(), emulator, hashed);
    CHECK(swapped.ok && Read(game / engine) == emulator64 && Read(game / copy) == newer);
    // A link at the path is replaced, not followed.
    fs::remove(game / copy);
    Write(game / "elsewhere.dll", "elsewhere");
    fs::create_symlink(game / "elsewhere.dll", game / copy);
    swapped = EmulateSteamworks(game.string(), emulator, hashed);
    CHECK(swapped.ok && !fs::is_symlink(game / copy) && Read(game / copy) == newer && Read(game / "elsewhere.dll") == "elsewhere");
    // The game's own mode removes the copy and leaves a file Playden didn't put there.
    auto restored = RestoreSteamworks(game.string(), RetiredSteamApiSha256(), {copy});
    CHECK(restored.ok && (restored.changed == std::vector<std::string>{copy, engine}));
    CHECK(!fs::exists(game / copy) && Read(game / engine) == newer);
    Write(game / copy, "the player's file");
    restored = RestoreSteamworks(game.string(), RetiredSteamApiSha256(), {copy});
    CHECK(restored.ok && restored.changed.empty() && Read(game / copy) == "the player's file");
    fs::remove(game / copy);
    // Without the folder, nothing is made: the game's folders changed in an update.
    auto moved = hashed;
    moved.steam_api_copies = {"Moved/Binaries/Win64/steam_api64.dll"};
    swapped = EmulateSteamworks(game.string(), emulator, moved);
    CHECK(swapped.ok && (swapped.skipped == moved.steam_api_copies) && !fs::exists(game / "Moved"));
    // A linked folder on the way is not followed.
    fs::create_directories(game / "real-folder");
    fs::create_symlink(game / "real-folder", game / "Linked");
    moved.steam_api_copies = {"Linked/steam_api64.dll"};
    swapped = EmulateSteamworks(game.string(), emulator, moved);
    CHECK(swapped.ok && swapped.skipped.size() == 1 && !fs::exists(game / "real-folder/steam_api64.dll"));
    // Two libraries of that name: which original it wants isn't known, so nothing is copied.
    Write(game / "Other/steam_api64.dll", GameDll(true, "other"));
    swapped = EmulateSteamworks(game.string(), emulator, hashed);
    CHECK(swapped.ok && (swapped.skipped == hashed.steam_api_copies) && !fs::exists(game / copy));
  }

  {  // The Steam host's mode: steam_appid.txt beside each of the game's own libraries and in each program's folder
     // (the working folder the library reads it from), a game's own file kept, and no write through a link.
    const auto game = root / "steam-host";
    Write(game / "bin/steam_api64.dll", GameDll(true, "host"));
    Write(game / "steam_api.dll", GameDll(false, "host32"));
    Write(game / "steam_appid.txt", "999");
    Write(game / "Game/Binaries/Win64/Game-Win64-Shipping.exe", "MZ");
    fs::create_directories(root / "elsewhere");
    fs::create_directory_symlink(root / "elsewhere", game / "linked");
    Write(root / "elsewhere/steam_api64.dll", GameDll(true, "linked"));
    Write(root / "elsewhere/Game.exe", "MZ");
    fs::create_directories(root / "outside");
    CHECK(WriteSteamAppIdBeside(game.string(), {"bin/steam_api64.dll", "steam_api.dll", "linked/steam_api64.dll"},
                                {"Game/Binaries/Win64", "linked", "", "../outside", "/data"}, 548430));
    CHECK(Read(game / "bin/steam_appid.txt") == "548430" && Read(game / "steam_appid.txt") == "999");
    CHECK(Read(game / "Game/Binaries/Win64/steam_appid.txt") == "548430");
    CHECK(!fs::exists(root / "elsewhere/steam_appid.txt") && !fs::exists(root / "outside/steam_appid.txt"));
    CHECK(!WriteSteamAppIdBeside(game.string(), {"steam_api.dll"}, {}, 0));
  }

  {  // The retired online bridge, any build of it, is Playden's: never kept as the game's original, and its emulator copy
     // goes when the game gets its own DLL back.
    const auto game = root / "bridged";
    const auto own = GameDll(true, "own-bridged");
    const std::string bridge = "a playden-online-bridge build";
    Write(game / "steam_api64.dll", bridge);
    Write(game / "steam_api64_gbe.dll", emulator64);
    Write(game / "steam_api64.dll.orig", own);
    auto swapped = EmulateSteamworks(game.string(), emulator, identity);
    CHECK(swapped.ok && (swapped.changed == std::vector<std::string>{"steam_api64.dll"}));
    CHECK(Read(game / "steam_api64.dll") == emulator64 && Read(game / "steam_api64.dll.orig") == own);
    CHECK(!fs::exists(game / "steam_api64_gbe.dll"));
    Write(game / "steam_api64.dll", bridge);
    Write(game / "steam_api64_gbe.dll", emulator64);
    auto restored = RestoreSteamworks(game.string());
    CHECK(restored.ok && Read(game / "steam_api64.dll") == own && !fs::exists(game / "steam_api64_gbe.dll"));
    // A game update replaces the bridge with the game's new DLL while the emulator copy stays: the new DLL has no
    // marker, so it is kept as the game's original.
    const auto updated = GameDll(true, "own-after-game-update");
    Write(game / "steam_api64.dll", updated);
    Write(game / "steam_api64_gbe.dll", emulator64);
    swapped = EmulateSteamworks(game.string(), emulator, identity);
    CHECK(swapped.ok && Read(game / "steam_api64.dll.orig") == updated && Read(game / "steam_api64.dll") == emulator64);
    // A bridge whose emulator copy is gone (or another build of the emulator) is still Playden's: the saved original
    // stays the game's.
    Write(game / "steam_api64.dll", bridge);
    fs::remove(game / "steam_api64_gbe.dll");
    Write(game / "steam_api64.dll.orig", own);
    restored = RestoreSteamworks(game.string());
    CHECK(restored.ok && Read(game / "steam_api64.dll") == own && Read(game / "steam_api64.dll.orig") == own);
    Write(game / "steam_api64.dll", bridge);
    swapped = EmulateSteamworks(game.string(), emulator, identity);
    CHECK(swapped.ok && Read(game / "steam_api64.dll") == emulator64 && Read(game / "steam_api64.dll.orig") == own);
    // Without the marker, an unknown DLL is the game's, whatever sits beside it.
    const auto other = root / "not-bridged";
    Write(other / "steam_api64.dll", own);
    Write(other / "steam_api64_gbe.dll", "something else");
    CHECK(EmulateSteamworks(other.string(), emulator, identity).ok && Read(other / "steam_api64.dll.orig") == own);
    CHECK(fs::exists(other / "steam_api64_gbe.dll"));
  }

  fs::remove_all(root);
  fs::remove_all(broken);
  std::puts("steamworks tests passed");
}
