// recipe_exports_test.cpp - Exported recipes: their own folder beside the logs, a file name with the game's name, one
// export per game, and the move of older versions' exports out of the logs folder.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/recipe_exports.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace playden;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                                 \
    }                                                                             \
  } while (0)

const GameId kHalfLife = GameId::Steam(220);
const GameId kNightSky = GameId::Local("2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f");
const GameId kWitcher = GameId::Gog(1207658924);

void Put(const fs::path& path, const std::string& text) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return;
  std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
}

std::string Get(const fs::path& path) {
  std::string text;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return text;
  char buffer[4096];
  for (std::size_t count; (count = std::fread(buffer, 1, sizeof buffer, file)) > 0;) text.append(buffer, count);
  std::fclose(file);
  return text;
}

std::vector<std::string> Names(const fs::path& folder) {
  std::vector<std::string> names;
  std::error_code error;
  for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
    names.push_back(it->path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

mode_t Mode(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 ? info.st_mode & 07777 : 0;
}

void FileNames() {
  CHECK(ExportedRecipeFileName(kHalfLife, "Half-Life 2") == "recipe-half-life-2-steam-220.json");
  CHECK(ExportedRecipeFileName(kNightSky, "Night Sky") == "recipe-night-sky-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json");
  // Marks go, punctuation and spaces are one "-", and nothing a file system could refuse is left.
  CHECK(ExportedRecipeFileName(kHalfLife, "Pokémon: Café / \"Edition\" <2>?*|\\") ==
        "recipe-pokemon-cafe-edition-2-steam-220.json");
  CHECK(ExportedRecipeFileName(kWitcher, "The Witcher: Enhanced Edition") == "recipe-the-witcher-enhanced-edition-gog-1207658924.json");
  CHECK(ExportedRecipeFileName(kHalfLife, "  ..Game.. ") == "recipe-game-steam-220.json");
  // Nothing NameKey keeps: the install ID alone, not a made-up "game".
  CHECK(ExportedRecipeFileName(kNightSky, "ゼルダ") == "recipe-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json");
  CHECK(ExportedRecipeFileName(kHalfLife, "") == "recipe-steam-220.json");
  CHECK(ExportedRecipeFileName(kHalfLife, "!!!") == "recipe-steam-220.json");
  // Bounded: a long name gives at most 48 bytes of it, without a "-" left at the cut.
  const auto long_name = ExportedRecipeFileName(kNightSky, std::string(300, 'x') + " y");
  CHECK(long_name == "recipe-" + std::string(48, 'x') + "-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json");
  const auto cut = ExportedRecipeFileName(kHalfLife, std::string(47, 'a') + " bbbb");
  CHECK(cut == "recipe-" + std::string(47, 'a') + "-steam-220.json");
  CHECK(long_name.size() < 120);
}

void Matching() {
  CHECK(IsExportedRecipeOf("recipe-half-life-2-steam-220.json", kHalfLife));
  CHECK(IsExportedRecipeOf("recipe-hl2-steam-220.json", kHalfLife));  // before a rename
  CHECK(IsExportedRecipeOf("recipe-steam-220.json", kHalfLife));
  CHECK(IsExportedRecipeOf("recipe-220.json", kHalfLife));  // older versions, in the logs folder
  CHECK(IsExportedRecipeOf("recipe-the-witcher-gog-1207658924.json", kWitcher));
  CHECK(IsExportedRecipeOf("recipe-gog-1207658924.json", kWitcher));
  CHECK(!IsExportedRecipeOf("recipe-the-witcher-steam-1207658924.json", kWitcher));
  CHECK(!IsExportedRecipeOf("recipe-1207658924.json", kWitcher));
  CHECK(IsExportedRecipeOf("recipe-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json", kNightSky));
  CHECK(IsExportedRecipeOf("recipe-night-sky-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json", kNightSky));
  // Another game's, even when its ID ends in this one's digits or its name holds them.
  CHECK(!IsExportedRecipeOf("recipe-x-steam-1220.json", kHalfLife));
  CHECK(!IsExportedRecipeOf("recipe-1220.json", kHalfLife));
  CHECK(!IsExportedRecipeOf("recipe-steam-220-steam-440.json", kHalfLife));
  CHECK(!IsExportedRecipeOf("recipe-half-life-2-steam-220.json", GameId::Steam(20)));
  CHECK(!IsExportedRecipeOf("recipe-Half-steam-220.json", kHalfLife));
  CHECK(!IsExportedRecipeOf("recipe--steam-220.json", kHalfLife));
  CHECK(!IsExportedRecipeOf("recipe-half-life-2-steam-220.json.tmp", kHalfLife));
  CHECK(!IsExportedRecipeOf("runtime.log", kHalfLife));

  CHECK(IsLogsFolderRecipe("recipe-526870.json"));
  CHECK(IsLogsFolderRecipe("recipe-526870.json.tmp"));
  CHECK(IsLogsFolderRecipe("recipe-gog-1207658924.json"));
  CHECK(IsLogsFolderRecipe("recipe-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json"));
  for (const char* other : {"recipe-.json", "recipe-abc.json", "recipe-0526870.json", "recipe-local-nope.json",
                            "recipe-registry-steam-220.json", "runtime.log", "title-FAKE78001.log", "xrecipe-1.json",
                            "recipe-half-life-2-steam-220.json"})
    CHECK(!IsLogsFolderRecipe(other));
}

void Writing(const fs::path& root) {
  const auto folder = root / kExportedRecipesFolder;
  // Made when missing, open to FTP; the file is world-readable.
  const auto written = WriteExportedRecipe(folder, kHalfLife, "HL2", "{\"v\":1}\n");
  CHECK(written && *written == folder / "recipe-hl2-steam-220.json");
  CHECK(Mode(folder) == 0777);
  CHECK(Mode(folder / "recipe-hl2-steam-220.json") == 0644);
  CHECK(Get(folder / "recipe-hl2-steam-220.json") == "{\"v\":1}\n");
  // Another game's export, and one older versions wrote, stay apart.
  Put(folder / "recipe-night-sky-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json", "night");
  Put(folder / "recipe-220.json", "old");
  Put(folder / "notes.txt", "mine");
  CHECK(FindExportedRecipe(folder, kNightSky) == std::string("recipe-night-sky-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json"));
  // Renamed: the new export replaces both earlier ones of the game, and nothing else.
  const auto again = WriteExportedRecipe(folder, kHalfLife, "Half-Life 2", "{\"v\":2}\n");
  CHECK(again && again->filename() == "recipe-half-life-2-steam-220.json");
  CHECK((Names(folder) == std::vector<std::string>{"notes.txt", "recipe-half-life-2-steam-220.json",
                                                   "recipe-night-sky-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json"}));
  CHECK(FindExportedRecipe(folder, kHalfLife) == std::string("recipe-half-life-2-steam-220.json"));
  CHECK(ReadExportedRecipe(folder, "recipe-half-life-2-steam-220.json") == std::string("{\"v\":2}\n"));
  CHECK(!FindExportedRecipe(folder, GameId::Steam(440)));
  CHECK(!FindExportedRecipe(root / "missing", kHalfLife));
  CHECK(!ReadExportedRecipe(folder, "recipe-missing-steam-1.json"));
  // Many other files never hide a recipe from the lookup.
  fs::create_directories(root / "crowded");
  for (int index = 0; index < 4200; ++index) Put(root / "crowded" / ("note-" + std::to_string(index) + ".txt"), "");
  Put(root / "crowded" / "recipe-zz-steam-220.json", "z");
  CHECK(FindExportedRecipe(root / "crowded", kHalfLife) == std::string("recipe-zz-steam-220.json"));
  fs::remove_all(root / "crowded");
  // Too large to be a recipe: not read.
  Put(folder / "recipe-big-steam-440.json", std::string(kMaxExportedRecipeBytes + 1, ' '));
  CHECK(FindExportedRecipe(folder, GameId::Steam(440)) == std::string("recipe-big-steam-440.json"));
  CHECK(!ReadExportedRecipe(folder, "recipe-big-steam-440.json"));
  // A link in the folder's place is refused, for writes and reads.
  const auto linked = root / "linked";
  fs::create_symlink(folder, linked);
  CHECK(!WriteExportedRecipe(linked, kHalfLife, "Half-Life 2", "x"));
  CHECK(!FindExportedRecipe(linked, kHalfLife));
  // A relative folder is refused.
  CHECK(!WriteExportedRecipe("exported-recipes", kHalfLife, "Half-Life 2", "x"));
}

void MovingOutOfTheLogs(const fs::path& root) {
  const auto logs = root / "logs";
  const auto folder = root / kExportedRecipesFolder;
  fs::create_directories(logs);
  Put(logs / "runtime.log", "started\n");
  Put(logs / "recipe-526870.json", "steam");
  Put(logs / "recipe-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json", "local");
  Put(logs / "recipe-440.json.tmp", "half");
  Put(logs / "recipe-registry-steam-220.json", "not an export");
  // Nothing to move: the folder isn't made.
  CHECK(MoveLogsFolderRecipes(root / "empty-logs", folder).empty());
  CHECK(!fs::exists(folder));
  const auto lines = MoveLogsFolderRecipes(logs, folder);
  CHECK(lines.size() == 3);
  CHECK((Names(logs) == std::vector<std::string>{"recipe-registry-steam-220.json", "runtime.log"}));
  CHECK((Names(folder) == std::vector<std::string>{"recipe-526870.json", "recipe-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json"}));
  CHECK(Get(folder / "recipe-526870.json") == "steam");
  CHECK(Mode(folder) == 0777);
  // Found as the game's export, and replaced by its next one.
  CHECK(FindExportedRecipe(folder, GameId::Steam(526870)) == std::string("recipe-526870.json"));
  CHECK(FindExportedRecipe(folder, kNightSky) == std::string("recipe-local-2bb7b7fa-e2f3-4b55-a118-7d33aaba5a8f.json"));
  // A second start with the same name in both places keeps the folder's and leaves the logs one.
  Put(logs / "recipe-526870.json", "again");
  const auto second = MoveLogsFolderRecipes(logs, folder);
  CHECK(second.size() == 1 && second[0].find("left recipe-526870.json") != std::string::npos);
  CHECK(Get(folder / "recipe-526870.json") == "steam" && Get(logs / "recipe-526870.json") == "again");
  CHECK(MoveLogsFolderRecipes(root / "missing", folder).empty());
}

}  // namespace

int main() {
  const auto root = fs::canonical(fs::temp_directory_path()) / ("playden-recipe-exports-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "write");
  fs::create_directories(root / "move");
  FileNames();
  Matching();
  Writing(root / "write");
  MovingOutOfTheLogs(root / "move");
  fs::remove_all(root);
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("recipe_exports: ok");
  return 0;
}
