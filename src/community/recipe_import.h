#pragma once
// recipe_import.h - A recipe file a player brings in themselves (web access), checked and made into a recipe Playden
// keeps in its community recipe store as its own.
//
// An imported recipe is always local: its id becomes community-<store>-<key>-imported-<16 hex of the file's sha256>, so
// it never passes for a published recipe. Sharing it makes a new recipe, and nobody votes on it.
// Only a recipe the recipe service would take is imported (no INI values, no field it refuses), so the player can share
// it later; and it must pass the strict rules the console applies to every community recipe (CommunityRecipeRefusals).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/game_settings.h"

#include <optional>
#include <string>
#include <string_view>

namespace playden::community {

// The largest file imported: the recipe service's own limit.
inline constexpr std::size_t kMaxImportedRecipeBytes = 16 << 10;
inline constexpr const char* kImportedRecipeSlugPrefix = "imported-";
inline constexpr const char* kImportedRecipeTitle = "Imported recipe";

struct ImportedRecipe {
  std::string bytes;               // the file Playden keeps: its sha256 is the revision
  runtime::CommunityRecipe recipe;  // as those bytes parse
};

// The recipe the file holds, ready to keep; nothing with a sentence for the player in *error. Pure.
// The file may be an "Export as recipe" file (no slug, no title), a recipe from the recipes repository, or a shared
// one. It keeps its title (Imported recipe when it has none), author and what it sets; it loses its locks.
std::optional<ImportedRecipe> PrepareImportedRecipe(std::string_view file, std::string* error);

// Whether a recipe id is one PrepareImportedRecipe made.
bool IsImportedRecipeId(std::string_view id);

}  // namespace playden::community
