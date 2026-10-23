#include "runtime/steamworks.h"

#include "runtime/artifact_hash.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
  CHECK(Read(plugin / "steam_settings/configs.main.ini").find("offline=1\n") != std::string::npos);
  CHECK(Read(plugin / "steam_settings/depots.txt") == "2062431\n");
  CHECK(Read(plugin / "steam_settings/steam_interfaces.txt") ==
        "STEAMAPPS_INTERFACE_VERSION008\nSTEAMCONTROLLER_INTERFACE_VERSION\nSteamClient017\nSteamUser021\n");
  CHECK(Read(plugin / "steam_interfaces.txt") == Read(plugin / "steam_settings/steam_interfaces.txt"));
  CHECK(!fs::exists(root / ".hidden/steam_api.dll.orig"));

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

  {  // Online: the bridge is the 64-bit DLL, the emulator sits beside it, and the token file is private.
    const auto game = root / "online";
    const auto own64 = GameDll(true, "online64");
    const auto own32 = GameDll(false, "online32");
    Write(game / "Win64/steam_api64.dll", own64);
    Write(game / "Win32/steam_api.dll", own32);
    const auto bridge = Read(fs::path(emulator) / "steam_api64_online.dll");
    const auto bridge32 = Read(fs::path(emulator) / "steam_api_online.dll");
    CHECK(!bridge.empty() && *HashArtifactBytes(bridge) == kSteamOnlineBridge64Sha256);
    CHECK(!bridge32.empty() && *HashArtifactBytes(bridge32) == kSteamOnlineBridge32Sha256);
    SteamworksIdentity online = identity;
    online.account_name = "player";
    online.online = true;
    online.refresh_token = "eyJ.refresh.token";
    auto swapped = EmulateSteamworks(game.string(), emulator, online);
    CHECK(swapped.ok && swapped.changed.size() == 2);
    CHECK(Read(game / "Win64/steam_api64.dll") == bridge && Read(game / "Win64/steam_api64_gbe.dll") == emulator64);
    CHECK(Read(game / "Win64/steam_api64.dll.orig") == own64);
    const auto token_file = game / "Win64/steam_settings/playden_online.txt";
    CHECK(Read(token_file) == "app_id=2062430\nsteam_id=76561198000000000\naccount_name=player\n"
                              "refresh_token=eyJ.refresh.token\n");
    CHECK((fs::status(token_file).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write));
    CHECK(Read(game / "Win64/steam_settings/configs.main.ini").find("offline=0\n") != std::string::npos);
    // 32-bit DLLs get the 32-bit bridge.
    CHECK(Read(game / "Win32/steam_api.dll") == bridge32 && Read(game / "Win32/steam_api_gbe.dll") == emulator32);
    CHECK(fs::exists(game / "Win32/steam_settings/playden_online.txt"));
    // Again: nothing changes hands.
    CHECK(EmulateSteamworks(game.string(), emulator, online).changed.empty());
    // A token that would break a line is refused.
    SteamworksIdentity broken_token = online;
    broken_token.refresh_token = "a\nb";
    CHECK(!EmulateSteamworks(game.string(), emulator, broken_token).ok);
    // Back to offline emulation: the bridge, its emulator copy and the token go.
    swapped = EmulateSteamworks(game.string(), emulator, identity);
    CHECK(swapped.ok && (swapped.changed == std::vector<std::string>{"Win32/steam_api.dll", "Win64/steam_api64.dll"}));
    CHECK(Read(game / "Win64/steam_api64.dll") == emulator64 && !fs::exists(game / "Win64/steam_api64_gbe.dll"));
    CHECK(Read(game / "Win32/steam_api.dll") == emulator32 && !fs::exists(game / "Win32/steam_api_gbe.dll"));
    CHECK(!fs::exists(token_file) && Read(game / "Win64/steam_api64.dll.orig") == own64);
    // Online again, then the game's own files: everything of Playden's goes.
    CHECK(EmulateSteamworks(game.string(), emulator, online).ok);
    auto restored = RestoreSteamworks(game.string());
    CHECK(restored.ok && restored.changed.size() == 2);
    CHECK(Read(game / "Win64/steam_api64.dll") == own64 && !fs::exists(game / "Win64/steam_api64_gbe.dll"));
    CHECK(Read(game / "Win32/steam_api.dll") == own32 && !fs::exists(game / "Win32/steam_api_gbe.dll"));
    CHECK(!fs::exists(token_file) && !fs::exists(game / "Win32/steam_settings/playden_online.txt"));
    // A damaged bridge is never installed.
    const auto damaged = root / "online-emulator";
    Write(damaged / "steam_api64.dll", emulator64);
    Write(damaged / "steam_api.dll", emulator32);
    Write(damaged / "steam_api64_online.dll", bridge.substr(1));
    Write(damaged / "steam_api_online.dll", bridge32);
    swapped = EmulateSteamworks(game.string(), damaged.string(), online);
    CHECK(!swapped.ok && swapped.message.find("online files") != std::string::npos);
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
    CHECK(RetiredSteamApiSha256().size() == 2);
  }

  fs::remove_all(root);
  fs::remove_all(broken);
  std::puts("steamworks tests passed");
}
