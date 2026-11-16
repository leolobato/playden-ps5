#include "runtime/settings_storage.h"
#include "runtime/game_settings.h"
#include "steam_id.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
  namespace fs = std::filesystem;
  using namespace playden::runtime;
  char pattern[] = "/tmp/playden-settings-storage-XXXXXX";
  CHECK(mkdtemp(pattern));
  const auto root = fs::canonical(pattern);
  const auto state = root / "public-state";
  fs::create_directory(state);
  CHECK(chmod(state.c_str(), 0755) == 0);
  const auto legacy = state / "game-settings.json";
  const GameSettingsRecord original{"", {}, 4};
  const auto text = std::string("{\"version\":1,\"games\":{\"220\":") + SerializeGameSettingsRecord(original) + "}}\n";
  std::ofstream(legacy) << text;
  CHECK(chmod(legacy.c_str(), 0644) == 0);
  std::string reason;
  auto prepared = PrepareGameSettingsStorage(state, &reason);
  CHECK(prepared && *prepared == state / "game-settings-private/game-settings.json");
  struct stat info{};
  // Open to FTP (#868): folder 0777, settings 0666.
  CHECK(lstat(prepared->parent_path().c_str(), &info) == 0 && (info.st_mode & 07777) == 0777 && info.st_uid == geteuid());
  CHECK(lstat(prepared->c_str(), &info) == 0 && (info.st_mode & 07777) == 0666);
  CHECK(lstat(state.c_str(), &info) == 0 && (info.st_mode & 07777) == 0755);
  std::map<playden::GameId, GameSettingsRecord> loaded;
  CHECK(LoadGameSettingsFile(prepared->string(), &loaded, &reason) && loaded.at(SteamId(220)) == original);
  const GameSettingsRecord updated{"", {}, 5};
  CHECK(SaveGameSettingsFile(prepared->string(), {{SteamId(220), updated}}, &reason));
  std::ofstream(legacy) << "malformed older source";
  CHECK(PrepareGameSettingsStorage(state, &reason) == prepared);
  CHECK(LoadGameSettingsFile(prepared->string(), &loaded, &reason) && loaded.at(SteamId(220)) == updated);
  std::ifstream preserved(legacy); std::string bytes; std::getline(preserved, bytes);
  CHECK(bytes == "malformed older source");
  std::ofstream(*prepared) << "malformed private record";
  CHECK(!PrepareGameSettingsStorage(state, &reason));
  const auto fresh = root / "fresh-state";
  CHECK(PrepareGameSettingsStorage(fresh, &reason));
  const auto broken = root / "broken-state";
  fs::create_directory(broken);
  std::ofstream(broken / "game-settings.json") << "malformed";
  CHECK(!PrepareGameSettingsStorage(broken, &reason));
  CHECK(!fs::exists(broken / "game-settings-private/game-settings.json"));
  const auto unsafe = root / "unsafe-state";
  fs::create_directory(unsafe);
  fs::create_directory(unsafe / "game-settings-private");
  // The app user's folder at any mode is Playden's own (#868); a link in its place is not.
  CHECK(chmod((unsafe / "game-settings-private").c_str(), 0755) == 0);
  CHECK(PrepareGameSettingsStorage(unsafe, &reason));
  fs::remove_all(unsafe / "game-settings-private");
  fs::create_directory_symlink(state / "game-settings-private", unsafe / "game-settings-private");
  CHECK(!PrepareGameSettingsStorage(unsafe, &reason));
  CHECK(!PrepareGameSettingsStorage("relative-state", &reason));
  fs::create_directory_symlink(state, root / "alias");
  CHECK(!PrepareGameSettingsStorage(root / "alias", &reason));
  fs::remove_all(root);
  std::puts("runtime settings storage tests passed");
}
