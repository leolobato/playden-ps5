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

// Rules only the recipes repository's CI applies would go here; none so far. A recipe without an
// author is fine everywhere: it was shared without a name and shows as "from the community".
struct CommunityCheckOptions {};

// Every strict rule a parsed community recipe breaks, one sentence each; empty when it may be
// published and offered. On top of ParseCommunityRecipe:
// - identity: recipe_id community-<store>-<key>-<slug> for the game object (not the export's slugless form), at most
//   128 bytes; game.name 1-120 printable characters (IsGameName); made_on only when not game.store;
// - title optional (1-60 printable characters when set), author optional, contributors named once each; no text field holds
//   invalid UTF-8, control, bidirectional or surrogate characters (notes may hold new lines and tabs), and names and
//   titles have no space or invisible character at either end;
// - tested_kit and min_kit, when set, real kit versions (ParseKitVersion); tested_date YYYY-MM-DD when set;
// - locked empty: locks are for the recipes Playden ships;
// - program: every launch entry an .exe inside the game folder, starting in a folder inside it;
//   not a Windows or Wine system program, no folder or file name of only dots and spaces or ending in one;
//   the program rows (program, program_folder, arguments, extra_arguments) belong in the recipe's own fields, not defaults;
// - a settings-only recipe (community.format_version 2), which has no launch entries: its program rows are defaults
//   (program, program_folder, program_arguments, launch_option), checked as launch entries are; the arguments,
//   extra_arguments, dll_overrides, architecture and graphics rows are not defaults (graphics is the recipe's field);
// - library overrides: named libraries only, no wildcard;
// - environment: only the variables Playden itself sets that take no path (IsPlaydenEnvironmentVariable);
// - registry: Software\Wine\AppDefaults\<one of the recipe's own programs>\..., or a publisher key
//   (reported by CommunityRecipeFlags, not refused).
std::vector<std::string> CommunityRecipeRefusals(const CommunityRecipe& community,
                                                 const CommunityCheckOptions& options = {});

// What in the recipe changes how the game starts, one item each ("program: Darkwood.exe"): the
// program, arguments, library overrides, environment and registry values. A publisher key
// says it needs review. CI labels pull requests with it and the chooser lists it. A settings-only recipe changes the
// program only through its program rows: its launch entries, arguments and library overrides, once filled in, are
// Playden's own, so they are not listed.
std::vector<std::string> CommunityRecipeFlags(const CommunityRecipe& community);

// Why the kit a game's title runs with cannot run a community recipe, as a sentence for the
// player, or empty when it can: requirements the kit's template lacks (UnmetRecipeRequirements),
// or a min_kit newer than title_kit_version. An unknown version (a kit before 0.10) fits no
// min_kit. Pass the kit and version the game's title runs, which can differ from the
// installed kit's after a kit swap.
std::string CommunityRecipeFits(const CommunityRecipe& community, const VerifiedKitView& kit,
                                std::optional<KitVersion> title_kit_version);

}  // namespace playden::runtime
