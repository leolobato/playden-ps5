// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace playden::runtime {

// A file Playden ships for one game, placed in the game's folder when its files are prepared (GameRecipe::game_files),
// for a game that can't run on the console without it. When the game no longer needs it, a later recipe retires it
// (GameRecipe::retired_game_files) and Playden removes its own copy.

struct RecipeFilesResult {
  bool ok = true;
  std::string message;               // on failure: a sentence for the player
  std::vector<std::string> changed;  // files (relative) that were written
  std::vector<std::string> skipped;  // "<file>: <why>" for files left alone
};

// Copies each of paths from source_dir to the same relative path in game_root, making its missing folders. A file
// already there stays, whatever it holds: an update that brings its own file must not lose it. Without its source, or
// with a link or a file in the way of a folder, a file is skipped: the game then starts, or stops, as it would have.
// Links are never followed. Idempotent; the game must be stopped.
RecipeFilesResult ApplyRecipeFiles(const std::string& game_root, const std::string& source_dir,
                                   const std::vector<std::string>& paths);

// Removes each file an earlier recipe placed (GameRecipe::retired_game_files) from game_root, when it is a regular file
// whose SHA-256 is the one given: Playden's own copy, which the game never had. A file with other bytes is the game's,
// or the player's, and stays (skipped). A missing file needs nothing. Links are never followed. Idempotent; the game
// must be stopped. `changed` lists the files removed.
RecipeFilesResult RetireRecipeFiles(const std::string& game_root,
                                    const std::vector<std::pair<std::string, std::string>>& retired);

}  // namespace playden::runtime
