#pragma once
// recipe_choice.h - Which recipe a game runs with: the player's wanted community recipe against the
// recipes Playden ships and generates, the one an installed game has applied, what a switch drops,
// and the registry values a recipe left in the bottle.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/community_rules.h"
#include "runtime/game_install.h"
#include "runtime/game_settings.h"
#include "runtime/kit_version.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Whether a recipe is the record's game's: the same game (Steam: the app ID) and runtime number. A community recipe
// is the game's once it is bound to it (BindCommunityRecipe), after its game matched the game's GameRef. A
// record copy that doesn't carry its game (read back from a journal) relies on the number, which the
// repository keeps to one game.
bool RecipeForGame(const GameRecipe& recipe, const RuntimeInstall& record);
// Whether a recipe is the one an install was prepared with: its game, id and RecipeVersionTag. Every
// place that resolves an install record to a recipe compares this, so two revisions of one community
// recipe are never confused.
bool RecipeMatchesInstall(const GameRecipe& recipe, const RuntimeInstall& record);

// Whether the cache gave the wanted community recipe's object.
enum class CommunityObjectState { kLoaded, kMissing, kUnreadable };

inline constexpr const char* kCommunityRecipeRemoved = "This recipe was removed.";
inline constexpr const char* kCommunityRecipeUnreadable = "The recipe can't be read.";
// A settings-only recipe on a copy where Playden finds no program of its own (FillSettingsOnlyRecipe).
inline constexpr const char* kCommunityRecipeNoProgram = "Playden found no program to start in this copy.";
inline constexpr const char* kCommunityRecipeNeedsRuntimeUpdate =
    " Update the game's runtime kit first: Play, or Maintenance › Update runtime kit.";

// The recipes a game could run with.
struct RecipeCandidates {
  const GameRecipe* shipped = nullptr;    // Playden's own recipe for the game
  const GameRecipe* generated = nullptr;  // from Steam's launch options or the game's files
  const CommunityRecipeChoice* wanted = nullptr;  // the player's choice, when any
  // The wanted recipe's object at its revision, when the cache has it (its recipe.revision set).
  const CommunityRecipe* community = nullptr;
  CommunityObjectState community_state = CommunityObjectState::kMissing;
  const VerifiedKitView* kit = nullptr;         // the installed kit; null: none admitted
  std::optional<KitVersion> kit_version;        // its version (kits before 0.10 have none)
  // The version the game's title runs: the kit's when the title runs its runtime, otherwise unknown
  // (a title made from an earlier kit keeps that runtime until Update), which fits no min_kit.
  std::optional<KitVersion> title_kit_version;
};

struct RecipeSelection {
  const GameRecipe* recipe = nullptr;  // null: no recipe at all (the caller generates one)
  bool community = false;              // recipe is the wanted community recipe
  // Why the wanted community recipe was not selected ("The recipe needs runtime kit version 0.13 or newer.",
  // kCommunityRecipeRemoved, kCommunityRecipeUnreadable); empty when it was, or none was wanted.
  std::string fallback_reason;
};

// Precedence: the wanted community recipe when its object loaded and CommunityRecipeFits is empty
// for the title's kit, else the shipped recipe, else the generated one. A settings-only recipe needs the program
// Playden filled in (FillSettingsOnlyRecipe): without one it falls back with kCommunityRecipeNoProgram. Pure; the
// settings sheet and preparation share it.
RecipeSelection SelectRecipe(const RecipeCandidates& candidates);

// The program a recipe starts as it selects it: its "program" default (a game-relative path), else the launch entry its
// "launch_option" default names, else its default entry (the first that isn't the player's own program). '/'
// separated, relative to the game folder; empty when it names none.
std::string SelectedProgram(const GameRecipe& recipe);

// Another game's community recipe (found by searching by name) as it applies to this copy. architecture_in_copy is asked
// for the program the recipe selects (SelectedProgram) and answers its architecture in this copy, empty when the copy
// hasn't it.
// - Not in the copy: the game's own program starts, Playden's choice: the launch entries and architecture of `own`, and
//   none of the recipe's defaults or locks for the program rows (launch option, program, folder, arguments,
//   architecture). Everything else stays the recipe's. Returns that program. Nothing changes when own has no program.
// - In the copy, built for another architecture than the recipe says: the copy's architecture, without the recipe's
//   architecture default or lock. Returns empty.
// Pure.
std::string UseOwnProgram(GameRecipe* recipe, const GameRecipe& own,
                          const std::function<std::string(const std::string& program)>& architecture_in_copy);

// A settings-only community recipe (community.format_version 2) as it applies to this copy, which it is always filled
// for, its own game's or another's. From `own` (Playden's shipped recipe for the copy, else the generated one): the
// launch entries, architecture, runtime, prefix, detected, dll_overrides, arguments and resolution_arguments, and the
// graphics when the recipe sets none. The recipe's defaults keep applying, its program rows too:
// - A "program" default the copy has starts: architecture_in_copy answers its architecture, which the recipe takes,
//   and it is listed as the player's own program (kProgramEntryId) with the recipe's folder and arguments. One the copy
//   hasn't: the program rows are dropped as UseOwnProgram drops them, and own's program starts.
// - Without one, a "launch_option" default names one of own's entries; one own hasn't is dropped with the program rows.
// - The "program_folder" and "program_arguments" defaults go on the entry that starts (BuildEffectiveSettings takes a
//   recipe's from the entry).
// Returns the program (or launch option) dropped, empty when none was. own without launch entries leaves the recipe
// without any, which SelectRecipe refuses (kCommunityRecipeNoProgram). Pure.
std::string FillSettingsOnlyRecipe(GameRecipe* recipe, const GameRecipe& own,
                                   const std::function<std::string(const std::string& program)>& architecture_in_copy);

// A player override that stops applying when the game's base (recipe and preset) changes.
struct DroppedOverride {
  std::string setting_id;
  std::string value;
  std::string reason;  // one line
};
// The overrides that apply with the old base and not with the new one: a launch option the new
// recipe doesn't have, a value its rows refuse, a row it locks. Pure.
std::vector<DroppedOverride> DroppedOverrides(const GameRecipe& from, const SettingsPreset* from_preset,
                                              const GameRecipe& to, const SettingsPreset* to_preset,
                                              const std::map<std::string, std::string>& overrides,
                                              const KitCapabilities& kit);

// A registry value a recipe wrote in the bottle (the identity, not the data).
struct TrackedRegistryValue {
  std::string key;
  std::string name;
  bool default_value = false;
};
inline bool operator==(const TrackedRegistryValue& left, const TrackedRegistryValue& right) {
  return left.key == right.key && left.name == right.name && left.default_value == right.default_value;
}
// What the recipe registry step left in a game's bottle: the recipe (id, and a community recipe's full
// revision) and the values it wrote. Kept per install in Playden's private runtime state; the step
// removes what an earlier recipe wrote that the current one doesn't set, so the step can run again
// after an interruption. Values not listed (the game's own) are never touched.
struct RecipeRegistryState {
  std::string recipe_id;
  std::string revision;
  std::vector<TrackedRegistryValue> values;
};
std::string SerializeRecipeRegistryState(const RecipeRegistryState& state);
bool ParseRecipeRegistryState(std::string_view json, RecipeRegistryState* state, std::string* reason);
// The values to apply now for `recipe` after `previous` (null: nothing tracked): removals (empty data)
// for tracked values the recipe doesn't set, then the recipe's own values, which are written even when
// the game changed them. *next is the state to save once they are applied.
std::vector<RegistryValue> RecipeRegistryTransition(const RecipeRegistryState* previous, const GameRecipe& recipe,
                                                    RecipeRegistryState* next);
// What to save before the bottle is touched: `next` plus whatever `previous` tracked that it doesn't list,
// so an interruption (or a failed final write) leaves a superset tracked and the next transition removes
// every value that is no longer wanted.
RecipeRegistryState RecipeRegistryInterim(const RecipeRegistryState* previous, const RecipeRegistryState& next);

}  // namespace playden::runtime
