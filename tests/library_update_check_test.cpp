// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/update_check.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "steam_id.h"

using namespace playden;

namespace {

void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

void Write(Storage& storage, const std::string& path, const std::string& text) {
  Check(storage.WriteAtomic(path, Bytes(text.begin(), text.end())).ok(), "write fixture");
}

}  // namespace

int main() {
  char root[] = "/tmp/playden-update-check-XXXXXX";
  Check(mkdtemp(root), "temporary root");
  Storage storage(root);
  UpdateCheckStore store(storage);

  // Nothing saved yet: an empty check, not an error.
  auto missing = store.Load(76561198000000001ull);
  Check(missing.ok() && !missing.value(), "an account without a file has no check");

  // A round trip keeps the time, each game's kind and target, and the sizes measured for it.
  SavedUpdateCheck saved;
  saved.checked_at = 1'800'000'000;
  saved.games[SteamId(1091500)] = {true, false, 2'100'000'000, 2'600'000'000, 77, true};
  saved.games[SteamId(548430)] = {false, true, 0, 0, 78, true};  // measured: nothing to download
  saved.games[SteamId(220)] = {true, true, 0, 0, 79, false};
  saved.games[SteamId(440)] = {false, false, 50};  // nothing to download: not saved
  Check(store.Save(76561198000000001ull, saved).ok(), "save a check");
  auto loaded = store.Load(76561198000000001ull);
  Check(loaded.ok() && loaded.value(), "load the check back");
  auto expected = saved;
  expected.games.erase(SteamId(440));
  Check(*loaded.value() == expected, "time, kinds and sizes survive the round trip");
  struct stat info{};
  Check(stat((std::string(root) + "/updates-76561198000000001.v2").c_str(), &info) == 0 && (info.st_mode & 0777) == 0666,
        "the file is open to FTP (#868)");
  // Sizes not measured are not kept.
  saved.games[SteamId(220)].download_bytes = 5;
  Check(store.Save(76561198000000001ull, saved).ok() && *store.Load(76561198000000001ull).value() == expected,
        "an unmeasured size is dropped");

  // Accounts don't see each other's checks.
  Check(store.Load(76561198000000002ull).ok() && !store.Load(76561198000000002ull).value(),
        "another account has none");
  SavedUpdateCheck other;
  other.checked_at = 1'700'000'000;
  other.games[SteamId(620)] = {true, false, 7, 9, 1, true};
  Check(store.Save(76561198000000002ull, other).ok(), "save the other account's check");
  Check(*store.Load(76561198000000001ull).value() == expected && *store.Load(76561198000000002ull).value() == other,
        "each account reads its own");

  // A never-finished check with no games still loads.
  Check(store.Save(76561198000000003ull, {}).ok(), "save an empty check");
  Check(store.Load(76561198000000003ull).value()->checked_at == 0, "an empty check loads");

  // Damaged files fail the load.
  const char* damaged[] = {
      "", "version=1\nchecked_at=1\n", "version=2\n", "version=2\nchecked_at=-1\n", "version=2\nchecked_at=01\n",
      "version=2\nchecked_at=1", "version=2\nchecked_at=1\n440 0 5 5 1\n", "version=2\nchecked_at=1\n440 4 5 5 1\n",
      "version=2\nchecked_at=1\n440 8 5 5 1\n", "version=2\nchecked_at=1\n440 1 5\n", "version=2\nchecked_at=1\nabc 1 5 5 1\n",
      "version=2\nchecked_at=1\n440 1 5 5 1\n440 2 6 6 1\n", "version=2\nchecked_at=1\n440 1 99999999999999999999999 5 1\n"};
  for (const char* text : damaged) {
    Write(storage, "updates-76561198000000004.v2", text);
    const auto result = store.Load(76561198000000004ull);
    Check(!result.ok() && result.error() == StorageError::kInvalidData, "a damaged file is refused");
  }
  Write(storage, "updates-76561198000000004.v2",
        "version=2\nchecked_at=5\n440 5 5 6 7\nlocal-0123abcd-0000-4000-8000-000000000000 2 0 0 0\n");
  const auto mixed = store.Load(76561198000000004ull);
  Check(mixed.ok() && mixed.value()->games.size() == 2, "game keys of any store parse");

  // Without a version 2 file, version 1's flags are read; its sizes were Steam's whole-depot estimates.
  Write(storage, "updates-76561198000000005.v1", "version=1\nchecked_at=5\n440 3 5\n");
  const auto old = store.Load(76561198000000005ull);
  Check(old.ok() && old.value()->checked_at == 5 && old.value()->games.at(SteamId(440)) == SavedGameUpdate{true, true},
        "a version 1 file keeps its flags only");
  Write(storage, "updates-76561198000000005.v1", "version=1\nchecked_at=5\n440 4 5\n");
  Check(!store.Load(76561198000000005ull).ok(), "a damaged version 1 file is refused");

  Check(!store.Save(0, saved).ok() && !store.Load(0).ok(), "account 0 is refused");

  // GOG's store: its own file name, the account key, and GOG's game keys; Steam's files stay as they were.
  {
    char gog_root[] = "/tmp/playden-update-check-gog-XXXXXX";
    Check(mkdtemp(gog_root), "temporary GOG root");
    Storage gog_storage(gog_root);
    UpdateCheckStore gog(gog_storage, "updates-gog-");
    UpdateCheckStore steam(gog_storage);
    constexpr std::uint64_t kKey = 0x8000000000000abcull;
    SavedUpdateCheck of_gog;
    of_gog.checked_at = 1'800'000'500;
    of_gog.games[GameId::Gog(1207658924)] = {true, false, 300, 400, 5, true};
    of_gog.games[GameId::Gog(2000000001)] = {true, false, 0, 0, 0, false};
    Check(gog.Save(kKey, of_gog).ok(), "save GOG's check");
    const std::string gog_path = std::string(gog_root) + "/updates-gog-" + std::to_string(kKey) + ".v2";
    Check(stat(gog_path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0666, "GOG's file is updates-gog-<key>.v2, open to FTP");
    Check(stat((std::string(gog_root) + "/updates-" + std::to_string(kKey) + ".v2").c_str(), &info) != 0,
          "no Steam file for the same number");
    Check(steam.Load(kKey).ok() && !steam.Load(kKey).value(), "Steam's store doesn't read GOG's file");
    const auto back = gog.Load(kKey);
    Check(back.ok() && back.value() && *back.value() == of_gog, "gog-<id> keys survive the round trip");
    // The default prefix is Steam's, unchanged; the same number names two files.
    Check(steam.Save(kKey, saved).ok() && *steam.Load(kKey).value() == expected && *gog.Load(kKey).value() == of_gog,
          "Steam's and GOG's checks of the same number are separate files");
    Check(stat((std::string(gog_root) + "/updates-" + std::to_string(kKey) + ".v2").c_str(), &info) == 0,
          "the default prefix writes updates-<n>.v2");
    // A load keeps every store's valid key: the model takes its own store's.
    Write(gog_storage, "updates-gog-7.v2", "version=2\nchecked_at=9\ngog-1207658924 1 0 0 0\n440 1 0 0 0\n");
    const auto both = gog.Load(7);
    Check(both.ok() && both.value()->games.size() == 2 && both.value()->games.count(GameId::Gog(1207658924)) &&
              both.value()->games.count(SteamId(440)),
          "keys of both stores load");
    std::filesystem::remove_all(gog_root);
  }
  std::filesystem::remove_all(root);
  std::puts("library_update_check_test passed");
  return 0;
}
