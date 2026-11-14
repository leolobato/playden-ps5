// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "library/store.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden {

// Recipes the player exports from Game settings (Export as file) have their own folder beside the logs:
// /data/playden/exported-recipes on the console, exported-recipes/ in the data folder on a Mac. They are never in the
// logs folder, so a log report or the web page's Logs tab never holds one.
inline constexpr const char* kExportedRecipesFolder = "exported-recipes";
// Larger than any recipe the recipe service takes (16 KB).
inline constexpr std::size_t kMaxExportedRecipeBytes = 64 * 1024;

// "recipe-half-life-2-steam-220.json", "recipe-night-sky-local-<uuid>.json": the game's name as a NameKey (a-z, 0-9
// and "-", at most 48 bytes, safe on /data and exFAT), then its install ID, which keeps the name unique. A name with
// nothing NameKey keeps (a title in Japanese, say) gives "recipe-<install id>.json".
std::string ExportedRecipeFileName(const GameId& id, std::string_view game_name);
// Whether file is an export of the game: a name ExportedRecipeFileName gives for any game name, or the name older
// versions wrote into the logs folder (recipe-<ToKey>.json: "recipe-220.json", "recipe-local-<uuid>.json").
bool IsExportedRecipeOf(std::string_view file, const GameId& id);
// Whether a file in the logs folder is an export older versions wrote there, or its leftover temporary file.
bool IsLogsFolderRecipe(std::string_view file);

// Writes text as the game's export in folder (made 0777 when missing, the file 0644: FTP reads and deletes them), and
// then removes the game's earlier exports, a name from before a rename included. The file's path, or nothing.
std::optional<std::filesystem::path> WriteExportedRecipe(const std::filesystem::path& folder, const GameId& id,
                                                         std::string_view game_name, std::string_view text);
// The game's newest export in folder: its file name, or nothing.
std::optional<std::string> FindExportedRecipe(const std::filesystem::path& folder, const GameId& id);
// Reads the export named (one FindExportedRecipe gave), up to kMaxExportedRecipeBytes.
std::optional<std::string> ReadExportedRecipe(const std::filesystem::path& folder, const std::string& file);
// Moves the exports older versions wrote into the logs folder to folder, keeping their names. A name the folder
// already has is left in the logs folder. One line for the log per file moved or left.
std::vector<std::string> MoveLogsFolderRecipes(const std::filesystem::path& logs, const std::filesystem::path& folder);

}  // namespace playden
