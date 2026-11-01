// Compile-time interception is limited to the production settings translation
// unit. This file calls the real syscalls while injecting specific I/O failures.
#undef fsync
#undef openat
#undef rename
#undef renameat
#undef unlink
#undef unlinkat
#include "runtime/game_settings.h"
#include "steam_id.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
namespace {
bool fail_file_sync = false, fail_directory_sync = false, fail_remove_config = false;
bool reject_relative_operations = false;
int config_renames_before_failure = -1;
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path);
  CHECK(input.good());
  return {std::istreambuf_iterator<char>(input), {}};
}
mode_t Mode(const std::filesystem::path& path) {
  struct stat status {};
  CHECK(::lstat(path.c_str(), &status) == 0);
  return status.st_mode & 0777;
}
extern "C" int playden_test_openat(int fd, const char* name, int flags, ...) {
  if (reject_relative_operations) {
    errno = EINVAL;
    return -1;
  }
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list arguments;
    va_start(arguments, flags);
    mode = static_cast<mode_t>(va_arg(arguments, int));
    va_end(arguments);
  }
  return ::openat(fd, name, flags, mode);
}
}
extern "C" int playden_test_fsync(int fd) {
  struct stat status {};
  if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && fail_file_sync) {
    fail_file_sync = false;
    errno = EIO;
    return -1;
  }
  if (S_ISDIR(status.st_mode) && fail_directory_sync) {
    fail_directory_sync = false;
    errno = EIO;
    return -1;
  }
  return ::fsync(fd);
}
extern "C" int playden_test_renameat(int from_dir, const char* from, int to_dir, const char* to) {
  if (std::string(to) == "dxvk.conf" && config_renames_before_failure >= 0 &&
      config_renames_before_failure-- == 0) {
    errno = EIO;
    return -1;
  }
  return ::renameat(from_dir, from, to_dir, to);
}
extern "C" int playden_test_rename(const char* from, const char* to) {
  const auto name = std::filesystem::path(to).filename().string();
  if (name == "dxvk.conf" && config_renames_before_failure >= 0 &&
      config_renames_before_failure-- == 0) {
    errno = EIO;
    return -1;
  }
  return ::rename(from, to);
}
extern "C" int playden_test_unlinkat(int fd, const char* name, int flags) {
  if (std::string(name) == "dxvk.conf" && fail_remove_config) {
    fail_remove_config = false;
    errno = EIO;
    return -1;
  }
  return ::unlinkat(fd, name, flags);
}
extern "C" int playden_test_unlink(const char* path) {
  if (std::filesystem::path(path).filename() == "dxvk.conf" && fail_remove_config) {
    fail_remove_config = false;
    errno = EIO;
    return -1;
  }
  return ::unlink(path);
}
int main(int argc, char** argv) {
  using namespace playden::runtime;
  namespace fs = std::filesystem;
  const fs::path source = argc > 1 ? argv[1] : ".";
  const auto root = fs::canonical(fs::temp_directory_path()) / ("playden-settings-io-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "game"); fs::create_directories(root / "prefix");
  fs::permissions(root, fs::perms::owner_all);
  fs::permissions(root / "prefix", fs::perms::owner_all);
  const auto records = root / "settings.json";
  std::string reason;
  reject_relative_operations = true;
  CHECK(SaveGameSettingsFile(records.string(), {{SteamId(220), {"", {}, 1}}}, &reason));
  CHECK(Mode(records) == 0600);
  const auto original = Read(records);
  fail_file_sync = true;
  CHECK(!SaveGameSettingsFile(records.string(), {{SteamId(220), {"", {}, 2}}}, &reason));
  CHECK(Read(records) == original);  // no rename before the data durability barrier
  fail_directory_sync = true;
  CHECK(!SaveGameSettingsFile(records.string(), {{SteamId(220), {"", {}, 2}}}, &reason));
  CHECK(!fail_directory_sync);  // a directory barrier was attempted after rename
  CHECK(SaveGameSettingsFile(records.string(), {{SteamId(220), {"", {}, 2}}}, &reason));
  CHECK(::chmod(records.c_str(), 0644) == 0);  // admitted legacy owner file
  CHECK(SaveGameSettingsFile(records.string(), {{SteamId(220), {"", {}, 3}}}, &reason));
  CHECK(Mode(records) == 0600);  // every replacement migrates to private mode
  std::map<playden::GameId, GameSettingsRecord> loaded;
  CHECK(::chmod(records.c_str(), 0660) == 0);
  CHECK(!LoadGameSettingsFile(records.string(), &loaded, &reason));
  CHECK(::chmod(records.c_str(), 0600) == 0);
  {
    // A game's Steam app ID (another store's game) is saved only when set, even as the record's only
    // value; a malformed one is dropped and the rest of the record loads.
    const auto imported = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
    GameSettingsRecord only_app;
    only_app.steam_app_id = 99700;
    const auto apps = root / "apps.json";
    CHECK(SaveGameSettingsFile(apps.string(), {{SteamId(220), {"", {}, 3}}, {imported, only_app}}, &reason));
    const auto text = Read(apps);
    CHECK(text.find("\"220\":{\"preset\":\"\",\"revision\":3,\"overrides\":{}}") != std::string::npos);
    CHECK(text.find("\"overrides\":{},\"steam_app_id\":\"99700\"}") != std::string::npos);
    std::map<playden::GameId, GameSettingsRecord> apps_loaded;
    CHECK(LoadGameSettingsFile(apps.string(), &apps_loaded, &reason));
    CHECK(apps_loaded.at(imported) == only_app && apps_loaded.at(SteamId(220)).steam_app_id == 0);
    GameSettingsRecord parsed;
    for (const char* value : {"\"0123\"", "\"0\"", "\"4294967296\"", "\"12a\"", "99700"}) {
      CHECK(ParseGameSettingsRecord(std::string("{\"preset\":\"p\",\"revision\":2,\"steam_app_id\":") + value + "}",
                                    &parsed, &reason));
      CHECK(parsed.steam_app_id == 0 && parsed.preset_id == "p" && parsed.revision == 2);
    }
    CHECK(ParseGameSettingsRecord("{\"steam_app_id\":\" 4294967295\\n\"}", &parsed, &reason) &&
          parsed.steam_app_id == 4294967295u);
    CHECK(!ParseSteamAppId("+5") && !ParseSteamAppId("") && ParseSteamAppId("\t7\r\n") == 7u);
    // The Steam program chosen at Install is saved only when set, as a game-relative path.
    GameSettingsRecord only_launch;
    only_launch.steam_launch = "Win64/RSDragonwilds.exe";
    const auto launches = root / "launches.json";
    CHECK(SaveGameSettingsFile(launches.string(), {{SteamId(1374490), only_launch}}, &reason));
    CHECK(Read(launches).find("\"steam_launch\":\"Win64/RSDragonwilds.exe\"") != std::string::npos);
    std::map<playden::GameId, GameSettingsRecord> launches_loaded;
    CHECK(LoadGameSettingsFile(launches.string(), &launches_loaded, &reason) &&
          launches_loaded.at(SteamId(1374490)) == only_launch);
    CHECK(ParseGameSettingsRecord("{\"steam_launch\":\"Bin\\\\Game.exe\"}", &parsed, &reason) &&
          parsed.steam_launch == "Bin/Game.exe");
    CHECK(ParseGameSettingsRecord("{\"steam_launch\":\"../x.exe\"}", &parsed, &reason) && parsed.steam_launch.empty());
    GameSettingsRecord only_kit;
    only_kit.kit = "0.17";
    const auto kits = root / "kits.json";
    CHECK(SaveGameSettingsFile(kits.string(), {{SteamId(220), only_kit}}, &reason));
    CHECK(Read(kits).find("\"kit\":\"0.17\"") != std::string::npos);
    std::map<playden::GameId, GameSettingsRecord> kits_loaded;
    CHECK(LoadGameSettingsFile(kits.string(), &kits_loaded, &reason) && kits_loaded.at(SteamId(220)) == only_kit);
    only_kit.kit = "latest";
    CHECK(SaveGameSettingsFile(kits.string(), {{SteamId(220), only_kit}}, &reason));
    CHECK(LoadGameSettingsFile(kits.string(), &kits_loaded, &reason) && kits_loaded.at(SteamId(220)).kit == "latest");
    CHECK(ParseGameSettingsRecord("{\"kit\":\"not-a-version\"}", &parsed, &reason) && parsed.kit.empty());
    CHECK(ParseGameSettingsRecord("{}", &parsed, &reason) && parsed.kit.empty());
    CHECK(SerializeGameSettingsRecord({}).find("\"kit\"") == std::string::npos);
    const auto omitted = root / "omitted-kit.json";
    CHECK(SaveGameSettingsFile(omitted.string(), {{SteamId(220), GameSettingsRecord{}}}, &reason));
    CHECK(Read(omitted).find("220") == std::string::npos);
  }

  const auto public_private_root = root / "public-settings";
  fs::create_directory(public_private_root);
  CHECK(!SaveGameSettingsFile((public_private_root / "settings.json").string(),
                              {{SteamId(220), {"", {}, 4}}}, &reason));
  CHECK(!SaveGameSettingsFile("relative-settings.json", {{SteamId(220), {"", {}, 4}}}, &reason));
  const auto linked_settings = root / "linked-settings.json";
  CHECK(::symlink(records.c_str(), linked_settings.c_str()) == 0);
  CHECK(!SaveGameSettingsFile(linked_settings.string(), {{SteamId(220), {"", {}, 4}}}, &reason));

  const auto legacy_root = root / "legacy-public";
  const auto migrated_root = root / "migrated-private";
  fs::create_directory(legacy_root);
  fs::create_directory(migrated_root);
  fs::permissions(migrated_root, fs::perms::owner_all);
  const auto legacy = legacy_root / "game-settings.json";
  const auto migrated = migrated_root / "game-settings.json";
  CHECK(MigrateLegacyGameSettingsFile(
      (legacy_root / "missing.json").string(), migrated.string(), &reason));
  CHECK(!fs::exists(migrated));
  { std::ofstream(legacy) << original; }
  CHECK(::chmod(legacy.c_str(), 0644) == 0);
  CHECK(MigrateLegacyGameSettingsFile(legacy.string(), migrated.string(), &reason));
  CHECK(Read(legacy) == original);
  CHECK(Mode(legacy) == 0644);
  CHECK(Mode(migrated) == 0600);
  CHECK(!MigrateLegacyGameSettingsFile(legacy.string(), migrated.string(), &reason));
  CHECK(reason.find("already exist") != std::string::npos);
  const auto malformed = legacy_root / "malformed.json";
  { std::ofstream(malformed) << "{"; }
  CHECK(!MigrateLegacyGameSettingsFile(
      malformed.string(), (migrated_root / "other.json").string(), &reason));
  CHECK(!fs::exists(migrated_root / "other.json"));
  const auto unsafe_mode = legacy_root / "unsafe-mode.json";
  { std::ofstream(unsafe_mode) << original; }
  CHECK(::chmod(unsafe_mode.c_str(), 0660) == 0);
  CHECK(!MigrateLegacyGameSettingsFile(
      unsafe_mode.string(), (migrated_root / "unsafe.json").string(), &reason));

  GameRecipe recipe;
  CHECK(ParseGameRecipe(Read(source / "runtime/recipes/steam-220-hl2-v1.json"), &recipe, &reason));
  SettingsLaunchInput input;
  input.recipe = &recipe;
  input.target = {"playden-steam-220", (root / "game").string()};
  input.kit = KitCapabilitiesFromManifest({kKitDxvkConfig}, true);
  input.record.overrides = {{"frame_limit", "30"}, {"windows_version", "win7"}};
  input.runtime_dir = (source / "runtime").string();
  SettingsLaunchPlan plan;
  CHECK(BuildSettingsLaunchPlan(input, &plan, &reason));
  std::ofstream(root / "prefix/user.reg") << "malformed registry";
  const std::string old_config = "# Generated by Playden from this game's settings. old\n";
  std::ofstream(root / "game/dxvk.conf") << old_config;
  config_renames_before_failure = 1; // publish new config, then refuse rollback's rename
  CHECK(!ApplySettingsFiles(plan, (root / "game").string(), (root / "prefix").string(), 1, &reason));
  CHECK(reason.find("rollback failed") != std::string::npos);
  CHECK(reason.find("reconciliation required") != std::string::npos);
  CHECK(Read(root / "game/dxvk.conf") == plan.dxvk_config);

  fs::remove(root / "game/dxvk.conf");
  config_renames_before_failure = -1;
  fail_remove_config = true;
  CHECK(!ApplySettingsFiles(plan, (root / "game").string(), (root / "prefix").string(), 1, &reason));
  CHECK(reason.find("rollback failed") != std::string::npos);
  CHECK(reason.find("reconciliation required") != std::string::npos);
  CHECK(Read(root / "game/dxvk.conf") == plan.dxvk_config);
  CHECK(Read(root / "prefix/user.reg") == "malformed registry");
  fs::remove_all(root);
  std::puts("settings I/O: durability and failed restore/remove diagnostics pass");
}
