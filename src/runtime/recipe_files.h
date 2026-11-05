// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace playden::runtime {

// A file Playden ships for one game, placed in the game's folder when its files are prepared (GameRecipe::game_files),
// for a game that can't run on the console without it. Project Zomboid opens a handle on each of its 1704 media folders
// to watch them for changes, past what a title can hold open, and quits; its class path looks in the game's folder
// first, so a watcher that watches nothing can stand in for the game's.

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

}  // namespace playden::runtime
