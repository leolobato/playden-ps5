#include "runtime/steamworks.h"

#include "runtime/artifact_hash.h"
#include "steam/backend/content_language.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;

constexpr int kMaxDepth = 10;
constexpr std::size_t kMaxDllBytes = 64u * 1024u * 1024u;

std::string Lower(std::string text) {
  for (auto& value : text)
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
  return text;
}

// Whole regular file, not through a link, at most limit bytes.
std::optional<std::string> ReadFile(const fs::path& path, std::size_t limit) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return std::nullopt;
  struct stat info {};
  std::optional<std::string> out;
  if (::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && static_cast<std::size_t>(info.st_size) <= limit) {
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
      const auto count = ::read(file, bytes.data() + done, bytes.size() - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      done += static_cast<std::size_t>(count);
    }
    if (done == bytes.size()) out = std::move(bytes);
  }
  ::close(file);
  return out;
}

bool Exists(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0;
}

bool IsRegular(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// Writes bytes to path through a temporary file in the same folder, synced, then renamed.
bool WriteAtomic(const fs::path& path, std::string_view bytes, mode_t mode = 0644) {
  const auto temporary = path.parent_path() / (".playden-" + path.filename().string() + ".tmp");
  ::unlink(temporary.c_str());
  const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
  if (file < 0) return false;
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < bytes.size()) {
    const auto count = ::write(file, bytes.data() + done, bytes.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) ok = false;
    else done += static_cast<std::size_t>(count);
  }
  ok = ok && ::fchmod(file, mode) == 0 && ::fsync(file) == 0;
  ok = ::close(file) == 0 && ok;
  if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  const int folder = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (folder >= 0) {
    ::fsync(folder);
    ::close(folder);
  }
  return true;
}

bool EnsureFolder(const fs::path& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
  return ::mkdir(path.c_str(), 0755) == 0;
}

// "pe32" or "pe64" from the PE header in bytes.
std::string Architecture(std::string_view bytes) {
  if (bytes.size() < 64 || bytes[0] != 'M' || bytes[1] != 'Z') return {};
  const auto byte = [&](std::size_t at) { return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])); };
  const std::uint32_t pe = byte(0x3c) | byte(0x3d) << 8 | byte(0x3e) << 16 | byte(0x3f) << 24;
  if (pe > bytes.size() || bytes.size() - pe < 26 || bytes.compare(pe, 4, std::string_view("PE\0\0", 4)) != 0) return {};
  const auto machine = byte(pe + 4) | byte(pe + 5) << 8;
  const auto magic = byte(pe + 24) | byte(pe + 25) << 8;
  if (machine == 0x014c && magic == 0x010b) return "pe32";
  if (machine == 0x8664 && magic == 0x020b) return "pe64";
  return {};
}

void Walk(const fs::path& root, const std::string& relative, int depth, std::vector<std::string>* found) {
  std::error_code error;
  for (fs::directory_iterator it(root / relative, error), last; !error && it != last; it.increment(error)) {
    const auto name = it->path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    const auto status = it->symlink_status(error);
    if (error) { error.clear(); continue; }
    if (fs::is_symlink(status)) continue;
    const auto path = relative.empty() ? name : relative + "/" + name;
    if (fs::is_directory(status)) {
      if (depth < kMaxDepth) Walk(root, path, depth + 1, found);
    } else if (fs::is_regular_file(status)) {
      const auto lower = Lower(name);
      if (lower == "steam_api.dll" || lower == "steam_api64.dll") found->push_back(path);
    }
  }
}

std::string Join(const std::vector<std::uint32_t>& values, const char* separator) {
  std::string out;
  for (const auto value : values) out += (out.empty() ? "" : separator) + std::to_string(value);
  return out;
}

std::vector<std::uint32_t> Sorted(std::vector<std::uint32_t> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

// False when an action set this setup doesn't name may still be there: the emulator would still report a Steam Input
// controller.
bool RemoveStaleActionSets(const fs::path& controller, const std::vector<std::pair<std::string, std::string>>& sets) {
  std::error_code error;
  const auto folder = fs::symlink_status(controller, error);
  if (error) return error == std::errc::no_such_file_or_directory;
  if (!fs::is_directory(folder)) return true;
  std::vector<fs::path> stale;
  for (fs::directory_iterator it(controller, error), last; !error && it != last; it.increment(error)) {
    const auto name = it->path().filename().string();
    if (name.size() <= 4 || Lower(name.substr(name.size() - 4)) != ".txt") continue;
    std::error_code entry_error;
    const auto status = it->symlink_status(entry_error);
    if (entry_error) return false;
    if (!fs::is_regular_file(status)) continue;
    const auto set = name.substr(0, name.size() - 4);
    if (std::none_of(sets.begin(), sets.end(), [&](const auto& kept) { return kept.first == set; }))
      stale.push_back(it->path());
  }
  if (error) return false;
  for (const auto& path : stale)
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) return false;
  ::rmdir(controller.c_str());  // only when empty
  return true;
}

bool WriteSettings(const fs::path& folder, const SteamworksIdentity& identity, const std::string& original) {
  const auto settings = folder / "steam_settings";
  if (!EnsureFolder(settings)) return false;
  const auto app = std::to_string(identity.app_id);
  if (identity.write_game_appid_file && !Exists(folder / "steam_appid.txt") &&
      !WriteAtomic(folder / "steam_appid.txt", app))
    return false;
  if (!WriteAtomic(settings / "steam_appid.txt", app)) return false;
  if (identity.depots.empty()) ::unlink((settings / "depots.txt").c_str());
  else if (!WriteAtomic(settings / "depots.txt", Join(Sorted(identity.depots), "\n") + "\n")) return false;
  // Other apps the account owns: the emulator's BIsSubscribedApp answers yes for them, and they stay out of the
  // DLC list and the installed depots.
  if (identity.owned_apps.empty()) ::unlink((settings / "owned_app_ids.txt").c_str());
  else if (!WriteAtomic(settings / "owned_app_ids.txt", Join(Sorted(identity.owned_apps), "\n") + "\n")) return false;
  std::string account = identity.account_name;
  account.erase(std::remove_if(account.begin(), account.end(), [](char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == '=' || c == '[' || c == ']';
  }), account.end());
  if (account.empty()) account = "Playden";
  const std::string user = "[user::general]\naccount_name=" + account + "\naccount_steamid=" +
                           std::to_string(identity.steam_id) + "\nlanguage=" + identity.language +
                           "\n\n[user::saves]\nlocal_save_path=" + kSteamworksSavePath + "\n";
  std::string apps = "[app::dlcs]\nunlock_all=0\n";
  for (const auto dlc : Sorted(identity.owned_dlc)) apps += std::to_string(dlc) + "=dlc" + std::to_string(dlc) + "\n";
  const char* const offline = identity.network ? "0" : "1";
  const char* const no_networking = identity.network ? "0" : "1";
  const std::string main = std::string("[main::connectivity]\ndisable_lan_only=0\noffline=") + offline +
                           "\ndisable_networking=" + no_networking + "\n";
  static const char* const kLanguages =
      "arabic\nbulgarian\nschinese\ntchinese\nczech\ndanish\ndutch\nenglish\nfinnish\nfrench\ngerman\ngreek\n"
      "hungarian\nitalian\njapanese\nkoreana\nnorwegian\npolish\nportuguese\nbrazilian\nromanian\nrussian\n"
      "spanish\nlatam\nswedish\nthai\nturkish\nukrainian\nvietnamese";
  if (!WriteAtomic(settings / "configs.user.ini", user) || !WriteAtomic(settings / "configs.app.ini", apps) ||
      !WriteAtomic(settings / "configs.main.ini", main) || !WriteAtomic(settings / "supported_languages.txt", kLanguages))
    return false;
  const auto controller = settings / "controller";
  // Action sets an earlier setup wrote and this one doesn't: a game set up with a map now left out (Euro Truck
  // Simulator 2) gets its controller back only once the emulator has no action set left.
  if (!RemoveStaleActionSets(controller, identity.steam_input_sets)) return false;
  if (!identity.steam_input_sets.empty()) {
    if (!EnsureFolder(controller)) return false;
    for (const auto& [name, text] : identity.steam_input_sets)
      if (!WriteAtomic(controller / (name + ".txt"), text)) return false;
  }
  const auto interfaces = SteamInterfaces(original);
  if (interfaces.empty()) return true;  // keep a hand-supplied file, if any
  std::string text;
  for (const auto& line : interfaces) text += line + "\n";
  return WriteAtomic(settings / "steam_interfaces.txt", text) && WriteAtomic(folder / "steam_interfaces.txt", text);
}

struct Emulator {
  std::string bytes;
  std::string sha256;
};

// Playden's emulator or online bridge, the current build or a retired one.
bool IsPlaydenEmulator(const std::string& digest, const std::vector<std::string>& retired) {
  return digest == kSteamApi32Sha256 || digest == kSteamApi64Sha256 || digest == kSteamOnlineBridge64Sha256 ||
         digest == kSteamOnlineBridge32Sha256 ||
         std::find(retired.begin(), retired.end(), digest) != retired.end();
}

// Where Playden's online bridge keeps the emulator it forwards to, beside the game's DLL.
fs::path BridgeEmulatorPath(const fs::path& dll, bool pe64) {
  return dll.parent_path() / (pe64 ? kSteamOnlineEmulator64Name : kSteamOnlineEmulator32Name);
}

// Playden's file in the game's DLL slot: its emulator, or any build of the retired online bridge (its marker). A bridge
// is never kept as the game's original, even without the emulator copy it kept beside it; a game update's own DLL has
// no marker, so it is.
bool IsPlaydensDll(const std::string& digest, std::string_view bytes, const fs::path& dll, bool pe64,
                   const std::vector<std::string>& retired) {
  (void)dll;
  (void)pe64;
  return IsPlaydenEmulator(digest, retired) || bytes.find(kSteamOnlineBridgeMarker) != std::string_view::npos;
}

// Removes the emulator copy beside the game's DLL; a file there that is not Playden's emulator is left alone.
bool RemoveBridgeEmulator(const fs::path& dll, bool pe64, const std::vector<std::string>& retired) {
  const auto beside = BridgeEmulatorPath(dll, pe64);
  if (!Exists(beside)) return true;
  const auto bytes = IsRegular(beside) ? ReadFile(beside, kMaxDllBytes) : std::nullopt;
  const auto digest = bytes ? HashArtifactBytes(*bytes) : std::nullopt;
  if (!digest || !IsPlaydenEmulator(*digest, retired)) return true;
  return ::unlink(beside.c_str()) == 0 || errno == ENOENT;
}

std::optional<Emulator> LoadEmulator(const std::string& emulator_dir, bool pe64) {
  const auto bytes = ReadFile(fs::path(emulator_dir) / (pe64 ? "steam_api64.dll" : "steam_api.dll"), kMaxDllBytes);
  if (!bytes) return std::nullopt;
  const auto digest = HashArtifactBytes(*bytes);
  if (!digest || *digest != (pe64 ? kSteamApi64Sha256 : kSteamApi32Sha256)) return std::nullopt;
  return Emulator{*bytes, *digest};
}

// Moves the steam_api_copies out of found, comparing as the file system may: exFAT ignores case.
void DropSteamApiCopies(const std::vector<std::string>& copies, std::vector<std::string>* found) {
  found->erase(std::remove_if(found->begin(), found->end(), [&](const std::string& relative) {
    return std::any_of(copies.begin(), copies.end(), [&](const std::string& copy) { return Lower(copy) == Lower(relative); });
  }), found->end());
}

// Whether every folder from root down to relative's parent is a real folder, not a link.
bool RealFoldersTo(const fs::path& root, const std::string& relative) {
  auto folder = root;
  const auto parent = fs::path(relative).parent_path();
  for (const auto& part : parent) {
    folder /= part;
    struct stat info {};
    if (::lstat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
  }
  return true;
}

// The .orig of the one found DLL named like relative; empty when there is not exactly one.
fs::path OriginalFor(const fs::path& root, const std::vector<std::string>& found, const std::string& relative) {
  const auto name = Lower(fs::path(relative).filename().string());
  fs::path original;
  for (const auto& candidate : found) {
    if (Lower(fs::path(candidate).filename().string()) != name) continue;
    if (!original.empty()) return {};
    original = root / (candidate + ".orig");
  }
  return original;
}

}  // namespace

std::vector<std::string> RetiredSteamApiSha256() {
  return {"4635ee7b204af05b2e6fb2b05c5b884080218e18f3954e95f84d40e51a0e4ae7",   // owned_app_ids test build (7100368e), 32-bit
          "a87257fa603f14f172e14c995b55b1c7f69e0fefda0754cc66a3b76a15d1db63",   // owned_app_ids test build (7100368e), 64-bit
          "4007fdef3062f8fa194c64d2286ab4e3d72ca1be189657a3b1a4da8f04f33bbb",   // Steam Input callbacks (509fcb90), 32-bit
          "3c46a4b109a1bd03a2d0255d1cc0e6e72608d4f67d621227701af4d8ad3d5ba9",   // Steam Input callbacks (509fcb90), 64-bit
          "4cc9dfcab8b4df0db507fa91d0acec6e58693fd4edd78dc80d39bda8072d08ed",   // 2026-10-07, 32-bit
          "513430ac8b869b0eed258d9e5feba5176a35446b3b61a63d01ab1d87683cdd1b",   // 2026-10-07, 64-bit
          "cae4372bceda997d6c73cebc1c14eb27d39b74a7999d8f28384bdfd8d746a79c",   // release-2026_09_27, 32-bit
          "70d9826a402d3a78b0c07e9f7ea7c6832468a1113013f31ea570fccaa5445ea9"};  // release-2026_09_27, 64-bit
}

std::vector<std::string> FindSteamApi(const std::string& install_root) {
  std::vector<std::string> found;
  Walk(fs::path(install_root), "", 0, &found);
  std::sort(found.begin(), found.end());
  return found;
}

std::vector<std::string> SteamInterfaces(std::string_view bytes) {
  // Interface families accepted by gbe_fork's tools/generate_interfaces/generate_interfaces.cpp.
  static const char* const kPrefixes[] = {
      "STEAMAPPS_INTERFACE_VERSION", "SteamApps", "STEAMAPPLIST_INTERFACE_VERSION",
      "STEAMAPPTICKET_INTERFACE_VERSION", "SteamClient", "SteamController", "SteamFriends",
      "SteamGameServerStats", "SteamGameCoordinator", "SteamGameServer",
      "STEAMHTMLSURFACE_INTERFACE_VERSION_", "STEAMHTTP_INTERFACE_VERSION", "SteamInput",
      "STEAMINVENTORY_INTERFACE_V", "SteamMatchMakingServers", "SteamMatchMaking",
      "SteamMatchGameSearch", "SteamParties", "STEAMMUSIC_INTERFACE_VERSION",
      "STEAMMUSICREMOTE_INTERFACE_VERSION", "SteamNetworkingMessages", "SteamNetworkingSockets",
      "SteamNetworkingUtils", "SteamNetworking", "STEAMPARENTALSETTINGS_INTERFACE_VERSION",
      "STEAMREMOTEPLAY_INTERFACE_VERSION", "STEAMREMOTESTORAGE_INTERFACE_VERSION",
      "STEAMSCREENSHOTS_INTERFACE_VERSION", "STEAMTIMELINE_INTERFACE_V",
      "STEAMUGC_INTERFACE_VERSION", "SteamUser", "STEAMUSERSTATS_INTERFACE_VERSION",
      "SteamUtils", "STEAMVIDEO_INTERFACE_V", "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION",
      "SteamMasterServerUpdater"};
  std::set<std::string> found;
  const auto check = [&](std::string_view candidate) {
    if (candidate.size() < 10) return;
    bool versioned = candidate == "STEAMCONTROLLER_INTERFACE_VERSION";
    for (const char* prefix : kPrefixes) {
      const std::string_view start(prefix);
      if (versioned || candidate.size() <= start.size() || candidate.compare(0, start.size(), start) != 0) continue;
      const auto version = candidate.substr(start.size());
      versioned = std::all_of(version.begin(), version.end(), [](char c) { return c >= '0' && c <= '9'; });
    }
    if (versioned) found.emplace(candidate);
  };
  std::size_t start = 0;
  for (std::size_t at = 0; at <= bytes.size(); ++at) {
    const bool printable = at < bytes.size() && bytes[at] >= 0x20 && bytes[at] <= 0x7e;
    if (printable) continue;
    check(bytes.substr(start, at - start));
    start = at + 1;
  }
  // New SDKs keep the legacy SteamClient() export at v017 beside newer versions.
  if (found.count("SteamClient017"))
    for (auto it = found.begin(); it != found.end();)
      it = it->rfind("SteamClient", 0) == 0 && *it != "SteamClient017" ? found.erase(it) : std::next(it);
  return {found.begin(), found.end()};
}

SteamworksResult EmulateSteamworks(const std::string& install_root, const std::string& emulator_dir,
                                   const SteamworksIdentity& identity, const std::vector<std::string>& retired) {
  SteamworksResult result;
  const fs::path root(install_root);
  result.found = FindSteamApi(install_root);
  DropSteamApiCopies(identity.steam_api_copies, &result.found);
  const auto fail = [&](std::string why) {
    result.message = std::move(why);
    return result;
  };
  for (const auto& relative : result.found) {
    const auto path = root / relative;
    const auto orig = fs::path(path.string() + ".orig");
    const bool pe64 = Lower(path.filename().string()) == "steam_api64.dll";
    const auto emulator = LoadEmulator(emulator_dir, pe64);
    if (!emulator) return fail("Playden's Steam emulator files are missing or damaged; reinstall Playden.");
    const Emulator& installed = *emulator;
    const auto current = ReadFile(path, kMaxDllBytes);
    if (!current) return fail("Playden could not read the game's " + relative + ".");
    const auto digest = HashArtifactBytes(*current);
    if (!digest) return fail("Playden could not check the game's " + relative + ".");
    std::string original;
    if (IsPlaydensDll(*digest, *current, path, pe64, retired)) {
      // Playden's own (an earlier build after a Playden update, or the online bridge): the game's DLL is the .orig.
      const auto kept = IsRegular(orig) ? ReadFile(orig, kMaxDllBytes) : std::nullopt;
      if (!kept) return fail("The game's original " + relative + " is missing; verify the download.");
      // An earlier Playden kept its own emulator as the original. The game's interface versions
      // are lost with it, and the emulator would hand the game the wrong ones.
      const auto kept_digest = HashArtifactBytes(*kept);
      if (!kept_digest || IsPlaydenEmulator(*kept_digest, retired))
        return fail("An earlier Playden lost the game's own " + relative + "; reinstall the game to get it back.");
      if (*digest != installed.sha256) {
        if (!WriteAtomic(path, installed.bytes)) return fail("Playden could not set up Steam features for this game.");
        result.changed.push_back(relative);
      }
      // A retired bridge's emulator copy goes once the bridge has left.
      if (!RemoveBridgeEmulator(path, pe64, retired)) return fail("Playden could not remove the game's Steam online files.");
      original = *kept;
    } else {
      if (Architecture(*current) != (pe64 ? "pe64" : "pe32")) continue;  // not a Steam API library this name promises
      // A first swap, or a newer original from a game update: keep it before replacing it.
      const auto kept = IsRegular(orig) ? ReadFile(orig, kMaxDllBytes) : std::nullopt;
      if ((!kept || *kept != *current) && !WriteAtomic(orig, *current))
        return fail("Playden could not keep a copy of the game's " + relative + ".");
      if (!WriteAtomic(path, installed.bytes)) return fail("Playden could not set up Steam features for this game.");
      result.changed.push_back(relative);
      original = *current;
    }
    if (!WriteSettings(path.parent_path(), identity, original))
      return fail("Playden could not write the game's Steam settings.");
  }
  for (const auto& relative : identity.steam_api_copies) {
    const auto source = OriginalFor(root, result.found, relative);
    const auto original = !source.empty() && IsRegular(source) ? ReadFile(source, kMaxDllBytes) : std::nullopt;
    // A game whose folders changed in an update may no longer ask for this. It starts, or stops as it would have.
    if (!original || !RealFoldersTo(root, relative)) {
      result.skipped.push_back(relative);
      continue;
    }
    const auto path = root / relative;
    const auto current = IsRegular(path) ? ReadFile(path, kMaxDllBytes) : std::nullopt;
    if (current && *current == *original) continue;
    if (!WriteAtomic(path, *original)) return fail("Playden could not put a copy of the game's Steam library at " + relative + ".");
    result.changed.push_back(relative);
  }
  result.ok = true;
  return result;
}

namespace {
// A program's first bytes (its PE header), without following links.
std::optional<std::string> ReadHead(const fs::path& path, std::size_t size) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return std::nullopt;
  std::string bytes(size, '\0');
  const auto count = ::pread(file, bytes.data(), size, 0);
  ::close(file);
  if (count <= 0) return std::nullopt;
  bytes.resize(static_cast<std::size_t>(count));
  return bytes;
}

// The steam.dll in a program's folder, whatever its case; empty when there is none.
fs::path LegacySteamIn(const fs::path& folder) {
  std::error_code error;
  for (fs::directory_iterator it(folder, error), last; !error && it != last; it.increment(error))
    if (Lower(it->path().filename().string()) == "steam.dll") return it->path();
  return {};
}
}  // namespace

SteamworksResult ProvideLegacySteam(const std::string& install_root, const std::string& emulator_dir,
                                    const std::vector<std::string>& programs) {
  SteamworksResult result;
  const fs::path root(install_root);
  std::optional<std::string> stub;
  for (const auto& relative : std::set<std::string>(programs.begin(), programs.end())) {
    const auto program = root / relative;
    const auto header = ReadHead(program, 4096);
    if (!header || Architecture(*header) != "pe32") continue;
    const auto folder = program.parent_path();
    const auto present = LegacySteamIn(folder);
    const auto folder_relative = fs::path(relative).parent_path() / "steam.dll";
    if (!present.empty()) {
      result.found.push_back(fs::path(relative).parent_path().empty() ? present.filename().string()
                                                                      : (fs::path(relative).parent_path() / present.filename()).string());
      continue;  // the game's own (or ours from before): left alone
    }
    if (!stub) {
      stub = ReadFile(fs::path(emulator_dir) / "steam.dll", kMaxDllBytes);
      const auto digest = stub ? HashArtifactBytes(*stub) : std::nullopt;
      if (!digest || *digest != kLegacySteamSha256) {
        result.message = "Playden's runtime kit is missing its steam.dll; reinstall Playden.";
        return result;
      }
    }
    if (!WriteAtomic(folder / "steam.dll", *stub)) {
      result.message = "Playden could not add steam.dll beside the game's " + relative + ".";
      return result;
    }
    result.found.push_back(folder_relative.string());
    result.changed.push_back(folder_relative.string());
  }
  result.ok = true;
  return result;
}

SteamworksResult RemoveLegacySteam(const std::string& install_root, const std::vector<std::string>& programs) {
  SteamworksResult result;
  const fs::path root(install_root);
  for (const auto& relative : std::set<std::string>(programs.begin(), programs.end())) {
    const auto present = LegacySteamIn((root / relative).parent_path());
    if (present.empty()) continue;
    const auto bytes = ReadFile(present, kMaxDllBytes);
    const auto digest = bytes ? HashArtifactBytes(*bytes) : std::nullopt;
    const auto shown = (fs::path(relative).parent_path() / present.filename()).string();
    result.found.push_back(shown);
    if (!digest || *digest != kLegacySteamSha256) continue;  // the game's own
    if (::unlink(present.c_str()) != 0) {
      result.message = "Playden could not remove its steam.dll beside the game's " + relative + ".";
      return result;
    }
    result.changed.push_back(shown);
  }
  result.ok = true;
  return result;
}

bool WriteSteamAppIdBeside(const std::string& install_root, const std::vector<std::string>& found,
                           const std::vector<std::string>& start_folders, std::uint32_t app_id) {
  if (!app_id) return false;
  bool ok = true;
  const fs::path root(install_root);
  std::vector<fs::path> files;
  for (const auto& library : found) files.push_back(fs::path(library).parent_path() / "steam_appid.txt");
  for (const auto& folder : start_folders) files.push_back(fs::path(folder) / "steam_appid.txt");
  for (const auto& relative : files) {
    if (relative.is_absolute() || std::any_of(relative.begin(), relative.end(), [](const fs::path& part) {
          return part == "..";
        }))
      continue;
    // Only through real folders: a link inside the game folder could point a write anywhere.
    if (!RealFoldersTo(root, relative.generic_string())) continue;
    if (!Exists(root / relative) && !WriteAtomic(root / relative, std::to_string(app_id))) ok = false;
  }
  return ok;
}

SteamworksResult RestoreSteamworks(const std::string& install_root, const std::vector<std::string>& retired,
                                   const std::vector<std::string>& steam_api_copies) {
  SteamworksResult result;
  const fs::path root(install_root);
  result.found = FindSteamApi(install_root);
  DropSteamApiCopies(steam_api_copies, &result.found);
  for (const auto& relative : steam_api_copies) {
    const auto path = root / relative;
    const auto source = OriginalFor(root, result.found, relative);
    if (source.empty() || !IsRegular(source) || !IsRegular(path) || !RealFoldersTo(root, relative)) continue;
    const auto copy = ReadFile(path, kMaxDllBytes);
    if (!copy || copy != ReadFile(source, kMaxDllBytes)) continue;  // not what Playden put there
    if (::unlink(path.c_str()) != 0) {
      result.message = "Playden could not remove its copy of the game's Steam library at " + relative + ".";
      return result;
    }
    result.changed.push_back(relative);
  }
  for (const auto& relative : result.found) {
    const auto path = root / relative;
    const bool pe64 = Lower(path.filename().string()) == "steam_api64.dll";
    const auto current = ReadFile(path, kMaxDllBytes);
    const auto digest = current ? HashArtifactBytes(*current) : std::nullopt;
    if (!digest || !IsPlaydensDll(*digest, *current, path, pe64, retired)) continue;  // already the game's own
    const auto kept = ReadFile(fs::path(path.string() + ".orig"), kMaxDllBytes);
    const auto kept_digest = kept ? HashArtifactBytes(*kept) : std::nullopt;
    if (kept_digest && IsPlaydenEmulator(*kept_digest, retired)) {
      result.message = "An earlier Playden lost the game's own " + relative + "; reinstall the game to get it back.";
      return result;
    }
    if (!kept || !WriteAtomic(path, *kept)) {
      result.message = "Playden could not put back the game's own " + relative + "; verify the download.";
      return result;
    }
    if (!RemoveBridgeEmulator(path, pe64, retired)) {
      result.message = "Playden could not remove the game's Steam online files.";
      return result;
    }
    result.changed.push_back(relative);
  }
  result.ok = true;
  return result;
}

std::string SerializeSteamworksFacts(const std::vector<std::uint32_t>& depots, const std::vector<std::uint32_t>& owned_dlc,
                                     const std::string& language, const std::vector<std::uint32_t>& excluded_dlc,
                                     const std::vector<std::uint32_t>& owned_apps, std::uint64_t owned_apps_account) {
  std::string text = "depots=" + Join(Sorted(depots), ",") + "\ndlc=" + Join(Sorted(owned_dlc), ",") + "\n";
  // The language the depots were installed for. Older files have no line and read as English,
  // which is what they were installed in.
  if (!language.empty() && language != std::string(steam::kDefaultContentLanguage))
    text += "language=" + language + "\n";
  if (!excluded_dlc.empty()) text += "nodlc=" + Join(Sorted(excluded_dlc), ",") + "\n";
  if (!owned_apps.empty())
    text += "apps=" + Join(Sorted(owned_apps), ",") + "\napps_account=" + std::to_string(owned_apps_account) + "\n";
  return text;
}

bool WriteSteamworksFacts(const std::string& install_root, const std::vector<std::uint32_t>& depots,
                          const std::vector<std::uint32_t>& owned_dlc, const std::string& language,
                          const std::vector<std::uint32_t>& excluded_dlc,
                          const std::vector<std::uint32_t>& owned_apps, std::uint64_t owned_apps_account) {
  return WriteAtomic(fs::path(install_root) / kSteamworksFactsFile,
                     SerializeSteamworksFacts(depots, owned_dlc, language, excluded_dlc, owned_apps, owned_apps_account));
}

bool ReadSteamworksFacts(const std::string& install_root, SteamworksIdentity* identity) {
  const auto text = ReadFile(fs::path(install_root) / kSteamworksFactsFile, 64 * 1024);
  if (!text) return false;
  std::vector<std::uint32_t> depots, dlc, excluded, apps;
  std::string language;
  std::uint64_t apps_account = 0;
  std::size_t start = 0;
  while (start < text->size()) {
    auto end = text->find('\n', start);
    if (end == std::string::npos) end = text->size();
    const auto line = text->substr(start, end - start);
    start = end + 1;
    const auto equals = line.find('=');
    if (equals == std::string::npos) continue;
    if (line.compare(0, equals, "language") == 0) {
      const auto value = line.substr(equals + 1);
      // Only a language Steam names; anything else leaves the game on English rather than
      // handing the emulator a value it cannot use.
      if (steam::IsContentLanguage(value)) language = value;
      continue;
    }
    if (line.compare(0, equals, "apps_account") == 0) {
      const auto value = line.substr(equals + 1);
      if (value.empty() || value.size() > 20 || !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
      errno = 0;
      const auto parsed = std::strtoull(value.c_str(), nullptr, 10);
      if (errno == ERANGE) return false;
      apps_account = parsed;
      continue;
    }
    auto* target = line.compare(0, equals, "depots") == 0 ? &depots
                   : line.compare(0, equals, "dlc") == 0  ? &dlc
                   : line.compare(0, equals, "nodlc") == 0 ? &excluded
                   : line.compare(0, equals, "apps") == 0  ? &apps
                                                            : nullptr;
    if (!target) continue;
    std::size_t at = equals + 1;
    while (at < line.size()) {
      auto comma = line.find(',', at);
      if (comma == std::string::npos) comma = line.size();
      const auto item = line.substr(at, comma - at);
      if (item.empty() || item.size() > 10 || !std::all_of(item.begin(), item.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
      unsigned long long value = 0;
      for (const char digit : item) value = value * 10 + static_cast<unsigned>(digit - '0');
      if (value > 0xffffffffull) return false;
      target->push_back(static_cast<std::uint32_t>(value));
      at = comma + 1;
    }
  }
  identity->depots = std::move(depots);
  identity->owned_dlc = std::move(dlc);
  identity->excluded_dlc = std::move(excluded);
  identity->owned_apps = std::move(apps);
  identity->owned_apps_account = apps_account;
  if (!language.empty()) identity->language = language;
  return true;
}

}  // namespace playden::runtime
