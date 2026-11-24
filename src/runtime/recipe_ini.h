// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "runtime/game_settings.h"

namespace playden::runtime {

// Some games start their settings launcher, and exit, until it has written a value to an INI file in the game
// folder: Batman: Arkham Asylum's ShippingPC-BmGame.exe starts BmLauncher.exe while BmGame/Config/UserEngine.ini has
// no [Engine.Engine] Language. The launcher can't start the game on the console (no second process), so a recipe
// writes the value instead (GameRecipe::ini).

// Puts value under [section] in the INI text when its key is missing or empty there; a value the player or the game
// wrote stays. Every other byte is kept: a key missing from the section goes after the section's last line, a
// section missing from the file goes at its end, and new lines use the file's own line ending. Sections and keys
// match ignoring case, as Windows' INI calls do. Returns whether the text changed.
bool SetIniValueIfEmpty(std::string* text, const std::string& section, const std::string& key, const std::string& value);

struct RecipeIniResult {
  bool ok = true;
  std::string message;               // on failure: a sentence for the player
  std::vector<std::string> changed;  // files (relative) that were written
  std::vector<std::string> skipped;  // "<file>: <why>" for files left alone (missing, not a plain text INI)
};

// Applies a recipe's INI values to the game folder at game_root. File names match ignoring case, as on Windows.
// A missing file is skipped, not created: the game makes its own from its defaults, and a file with only the
// recipe's lines could hide them. Links are never followed. A file is rewritten only when a value changes,
// through a synced temporary file renamed over it, with the original's permissions. Idempotent; the game must be
// stopped.
RecipeIniResult ApplyRecipeIniValues(const std::string& game_root, const std::vector<IniValue>& values);

// Puts value after the '=' of the first line of a flat key=value file (no sections) whose key matches, ignoring case
// and the spaces around it, when that line holds another value (compared ignoring case: hex digits). *previous gets
// the value it held, and stays empty when no line has the key: such a file is left as it is. Every other byte is kept.
// Returns whether the text changed.
bool ReplaceKeyValue(std::string* text, const std::string& key, const std::string& value,
                     std::optional<std::string>* previous);

struct GameFileValueResult {
  bool ok = true;
  std::string message;   // on failure: why the file could not be written
  bool found = false;    // the file and its key are there
  bool changed = false;  // the file was rewritten
  std::string previous;  // the value the key held
};

// Applies ReplaceKeyValue to a plain text file of the game folder at game_root, found as ApplyRecipeIniValues finds
// one (ignoring case, never through a link) and written the same way. A missing file or key is left for the game.
GameFileValueResult ReplaceGameFileValue(const std::string& game_root, const std::string& relative,
                                         const std::string& key, const std::string& value);

}  // namespace playden::runtime
