#pragma once
// community_rules.h - The strict rules for community recipes (docs/community-recipes.md), which the
// recipes repository's CI (community_recipe_check) and the console both apply, and whether a kit fits one.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/game_settings.h"
#include "runtime/kit_version.h"
#include "runtime/package_installer.h"

#include <optional>
#include <string>
#include <vector>

namespace playden::runtime {

inline constexpr std::size_t kCommunityTitleMaxCharacters = 60;
inline constexpr std::size_t kCommunityNameMaxCharacters = 64;
inline constexpr std::size_t kCommunityMaxContributors = 32;

struct CommunityCheckOptions {
  // The recipes repository credits every file: author must be set. The console reads recipes
  // without one.
  bool require_author = false;
};

// Every strict rule a parsed community recipe breaks, one sentence each; empty when it may be
// published and offered. On top of ParseCommunityRecipe:
// - identity: recipe_id community-<app id>-<slug> for app_id (not the export's slugless form), at most 64 bytes;
// - title 1-60 printable characters, author set when options ask, contributors named once each; no text field holds
//   invalid UTF-8, control, bidirectional or surrogate characters (notes may hold new lines and tabs), and names and
//   titles have no space or invisible character at either end;
// - tested_kit and min_kit, when set, real kit versions (ParseKitVersion); tested_date YYYY-MM-DD when set;
// - locked empty: locks are for the recipes Playden ships;
// - program: every launch entry an .exe inside the game folder, starting in a folder inside it;
//   not a Windows or Wine system program, no folder or file name of only dots and spaces or ending in one;
//   the program rows (program, program_folder, arguments, extra_arguments) belong in the recipe's own fields, not defaults;
// - library overrides: named libraries only, no wildcard;
// - environment: only the variables Playden itself sets that take no path (IsPlaydenEnvironmentVariable);
// - registry: Software\Wine\AppDefaults\<one of the recipe's own programs>\..., or a publisher key
//   (reported by CommunityRecipeFlags, not refused).
std::vector<std::string> CommunityRecipeRefusals(const CommunityRecipe& community,
                                                 const CommunityCheckOptions& options = {});

// What in the recipe changes how the game starts, one item each ("program: Darkwood.exe"): the
// program, arguments, library overrides, environment and registry values. A publisher key
// says it needs review. CI labels pull requests with it and the chooser lists it.
std::vector<std::string> CommunityRecipeFlags(const CommunityRecipe& community);

// Why the kit a game's title runs with cannot run a community recipe, as a sentence for the
// player, or empty when it can: requirements the kit's template lacks (UnmetRecipeRequirements),
// or a min_kit newer than title_kit_version. An unknown version (a kit before 0.10) fits no
// min_kit. Pass the kit and version the game's title runs, which can differ from the
// installed kit's after a kit swap.
std::string CommunityRecipeFits(const CommunityRecipe& community, const VerifiedKitView& kit,
                                std::optional<KitVersion> title_kit_version);

}  // namespace playden::runtime
