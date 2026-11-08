// recipe_choice.cpp - Which recipe a game runs with, what a switch drops, and the registry values a
// recipe left in the bottle.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/recipe_choice.h"

#include "homebrew/json.hpp"
#include "steam/backend/app_info.h"
#include "steam/backend/json.h"

#include <algorithm>

namespace playden::runtime {

bool RecipeForGame(const GameRecipe& recipe, const RuntimeInstall& record) {
  if (recipe.app_id != record.runtime_number) return false;
  return record.game == GameId{} || RecipeGame(recipe) == record.game;
}

bool RecipeMatchesInstall(const GameRecipe& recipe, const RuntimeInstall& record) {
  // The record holds a longer community recipe id as its alias (InstalledRecipeId).
  return RecipeForGame(recipe, record) && InstalledRecipeId(recipe.id) == record.recipe_id &&
         RecipeVersionTag(recipe) == record.recipe_version;
}

RecipeSelection SelectRecipe(const RecipeCandidates& candidates) {
  RecipeSelection selection;
  if (candidates.wanted && !candidates.wanted->id.empty()) {
    std::string reason;
    const auto* community = candidates.community;
    // The object must be the chosen recipe at the chosen revision; anything else reads as unreadable.
    const bool loaded = candidates.community_state == CommunityObjectState::kLoaded && community &&
                        community->recipe.id == candidates.wanted->id &&
                        community->recipe.revision == candidates.wanted->revision;
    if (!loaded) {
      reason = candidates.community_state == CommunityObjectState::kMissing ? kCommunityRecipeRemoved
                                                                            : kCommunityRecipeUnreadable;
    } else if (!candidates.kit) {
      reason = "No verified runtime kit is installed.";
    } else if (IsSettingsOnlyRecipe(*community) && community->recipe.launch_entries.empty()) {
      reason = kCommunityRecipeNoProgram;
    } else {
      reason = CommunityRecipeFits(*community, *candidates.kit, candidates.title_kit_version);
      if (reason.empty()) {
        selection.recipe = &community->recipe;
        selection.community = true;
        return selection;
      }
      // The installed kit would run it: only the game's title still runs an older runtime.
      if (!candidates.title_kit_version && candidates.kit_version && !community->min_kit.empty())
        if (const auto needed = ParseKitVersion(community->min_kit); needed && *candidates.kit_version >= *needed)
          reason += kCommunityRecipeNeedsRuntimeUpdate;
    }
    selection.fallback_reason = std::move(reason);
  }
  selection.recipe = candidates.shipped ? candidates.shipped : candidates.generated;
  return selection;
}

std::string SelectedProgram(const GameRecipe& recipe) {
  std::string program;
  if (const auto chosen = recipe.defaults.find("program"); chosen != recipe.defaults.end() && !chosen->second.empty()) {
    program = steam::RelativeGamePath(chosen->second).value_or(chosen->second);
  } else {
    const LaunchEntry* entry = nullptr;
    if (const auto option = recipe.defaults.find("launch_option"); option != recipe.defaults.end())
      for (const auto& candidate : recipe.launch_entries)
        if (candidate.id == option->second) entry = &candidate;
    // As BuildEffectiveSettings starts without a choice (DefaultLaunchEntry).
    if (!entry)
      for (const auto& candidate : recipe.launch_entries)
        if (candidate.id != kProgramEntryId) { entry = &candidate; break; }
    if (!entry && !recipe.launch_entries.empty()) entry = &recipe.launch_entries.front();
    if (entry) program = entry->executable;
  }
  std::replace(program.begin(), program.end(), '\\', '/');
  return program;
}

namespace {
// The rows that choose the program, which another copy's program can't keep.
constexpr const char* kProgramRows[] = {"launch_option", "program", "program_folder", "program_arguments", "architecture"};
}  // namespace

std::string UseOwnProgram(GameRecipe* recipe, const GameRecipe& own,
                          const std::function<std::string(const std::string& program)>& architecture_in_copy) {
  if (!recipe) return {};
  const auto program = SelectedProgram(*recipe);
  if (program.empty()) return {};
  if (const auto architecture = architecture_in_copy(program); !architecture.empty()) {
    if (architecture != recipe->architecture) {
      recipe->architecture = architecture;
      recipe->defaults.erase("architecture");
      recipe->locked.erase("architecture");
    }
    return {};
  }
  if (own.launch_entries.empty()) return {};
  recipe->launch_entries = own.launch_entries;
  recipe->architecture = own.architecture;
  for (const char* id : kProgramRows) {
    recipe->defaults.erase(id);
    recipe->locked.erase(id);
  }
  return program;
}

std::string FillSettingsOnlyRecipe(GameRecipe* recipe, const GameRecipe& own,
                                   const std::function<std::string(const std::string& program)>& architecture_in_copy) {
  if (!recipe) return {};
  recipe->launch_entries = own.launch_entries;
  recipe->architecture = own.architecture;
  recipe->runtime = own.runtime;
  recipe->prefix = own.prefix;
  recipe->detected = own.detected;
  recipe->dll_overrides = own.dll_overrides;
  recipe->arguments = own.arguments;
  recipe->resolution_arguments = own.resolution_arguments;
  if (recipe->graphics.empty()) recipe->graphics = own.graphics;
  auto& defaults = recipe->defaults;
  // The copy's program decides the architecture.
  defaults.erase("architecture");
  recipe->locked.erase("architecture");
  if (recipe->launch_entries.empty()) return {};
  const auto drop_program_rows = [&] {
    for (const char* id : kProgramRows) {
      defaults.erase(id);
      recipe->locked.erase(id);
    }
  };
  // The recipe's folder and arguments go with the program it starts: BuildEffectiveSettings takes a recipe's from
  // the launch entry, and its rows only from the player.
  const auto carry_rows = [&](LaunchEntry* entry) {
    if (const auto folder = defaults.find("program_folder"); folder != defaults.end()) {
      const auto relative = steam::RelativeGamePath(folder->second, true);
      entry->working_directory = !relative || *relative == "." ? std::string() : *relative;
      entry->in_install_root = relative && *relative == "." && !folder->second.empty();
      defaults.erase(folder);
    }
    if (const auto arguments = defaults.find("program_arguments"); arguments != defaults.end()) {
      entry->arguments = arguments->second;
      defaults.erase(arguments);
    }
  };
  LaunchEntry* chosen_entry = nullptr;
  if (const auto option = defaults.find("launch_option"); option != defaults.end()) {
    for (auto& entry : recipe->launch_entries)
      if (entry.id == option->second) chosen_entry = &entry;
    if (!chosen_entry) {
      // A program default starts instead of any launch option; without one, own's starts as Playden has it, as the
      // folder and arguments were for that launch option's program.
      const bool program = defaults.count("program") && !defaults.at("program").empty();
      const auto dropped = option->second;
      defaults.erase(option);
      recipe->locked.erase("launch_option");
      if (!program) {
        drop_program_rows();
        return dropped;
      }
    }
  }
  if (const auto chosen = defaults.find("program"); chosen != defaults.end() && !chosen->second.empty()) {
    const auto program = SelectedProgram(*recipe);
    const auto architecture = architecture_in_copy(program);
    if (architecture.empty()) {
      drop_program_rows();
      return program;
    }
    recipe->architecture = architecture;
    // Listed as the player's own program, which BuildEffectiveSettings starts for the "program" row.
    LaunchEntry entry;
    entry.id = kProgramEntryId;
    entry.executable = program;
    const auto slash = program.rfind('/');
    entry.label = slash == std::string::npos ? program : program.substr(slash + 1);
    carry_rows(&entry);
    recipe->launch_entries.push_back(std::move(entry));
    return {};
  }
  if (chosen_entry) {
    // A launch option of own's: its program in this copy, when the copy knows it.
    if (const auto architecture = architecture_in_copy(SelectedProgram(*recipe)); !architecture.empty())
      recipe->architecture = architecture;
  } else {
    // Own's default program (DefaultLaunchEntry).
    for (auto& entry : recipe->launch_entries)
      if (entry.id != kProgramEntryId) { chosen_entry = &entry; break; }
    if (!chosen_entry) chosen_entry = &recipe->launch_entries.front();
  }
  carry_rows(chosen_entry);
  return {};
}

std::vector<DroppedOverride> DroppedOverrides(const GameRecipe& from, const SettingsPreset* from_preset,
                                              const GameRecipe& to, const SettingsPreset* to_preset,
                                              const std::map<std::string, std::string>& overrides,
                                              const KitCapabilities& kit) {
  std::vector<DroppedOverride> dropped;
  EffectiveSettings before, after;
  SettingsError ignored;
  const bool had_before = BuildEffectiveSettings(from, from_preset, overrides, kit, &before, &ignored);
  const bool has_after = BuildEffectiveSettings(to, to_preset, overrides, kit, &after, &ignored);
  // Whether the player's value is what a row applies with a base.
  const auto applies = [&](const EffectiveSettings& settings, bool built, const std::string& id, const std::string& value) {
    if (!built) return false;
    const auto found = settings.values.find(id);
    if (found == settings.values.end()) return false;
    const auto& row = found->second;
    return row.available && (!row.locked || row.unlocked) && ChosenValue(row) == value;
  };
  for (const auto& [id, value] : overrides) {
    if (IsUnlockKey(id) || !FindSetting(id)) continue;
    // A value that didn't apply with the old base either is no loss.
    if (had_before && !applies(before, true, id, value)) continue;
    std::string why;
    DroppedOverride item{id, value, {}};
    if (!ValidateSettingValue(to, id, value, &why)) {
      item.reason = id == "launch_option" ? "The recipe has no launch option \"" + value + "\"."
                                          : "The recipe's row doesn't take \"" + value + "\": " + why + ".";
    } else if (has_after) {
      const auto found = after.values.find(id);
      if (found == after.values.end()) continue;
      const auto& row = found->second;
      if (row.locked && !row.unlocked) item.reason = "Locked by the recipe.";
      else if (!row.available) item.reason = row.reason.empty() ? "Not available with this recipe." : row.reason;
      else if (ChosenValue(row) != value) item.reason = row.reason.empty() ? "Not applied with this recipe." : row.reason;
    }
    if (!item.reason.empty()) dropped.push_back(std::move(item));
  }
  return dropped;
}

namespace {
namespace json = homebrew::json;
}  // namespace

std::string SerializeRecipeRegistryState(const RecipeRegistryState& state) {
  std::string text = "{\"recipe_id\":\"" + json::escape(state.recipe_id) + "\",\"revision\":\"" +
                     json::escape(state.revision) + "\",\"values\":[";
  bool first = true;
  for (const auto& value : state.values) {
    if (!first) text += ",";
    first = false;
    text += "{\"key\":\"" + json::escape(value.key) + "\",\"name\":\"" + json::escape(value.name) + "\"";
    if (value.default_value) text += ",\"default\":true";
    text += "}";
  }
  return text + "]}\n";
}

bool ParseRecipeRegistryState(std::string_view text, RecipeRegistryState* state, std::string* reason) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  const auto root = steam::json::Parse(text);
  if (!root || !root->is_object()) return fail("recipe registry state is not a JSON object");
  RecipeRegistryState out;
  const auto* id = root->Find("recipe_id");
  const auto* revision = root->Find("revision");
  const auto* values = root->Find("values");
  if (!id || !id->is_string() || !revision || !revision->is_string() || !values || !values->is_array())
    return fail("recipe registry state needs recipe_id, revision and values");
  out.recipe_id = id->text();
  out.revision = revision->text();
  for (const auto& entry : values->array()) {
    const auto* key = entry.Find("key");
    const auto* name = entry.Find("name");
    const auto* default_value = entry.Find("default");
    if (!entry.is_object() || !key || !key->is_string() || !name || !name->is_string() ||
        (default_value && !default_value->is_bool()))
      return fail("recipe registry state has a malformed value");
    out.values.push_back({key->text(), name->text(), default_value && default_value->boolean()});
  }
  *state = std::move(out);
  return true;
}

namespace {

bool FoldedEqual(const std::string& left, const std::string& right) {
  return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return lower(a) == lower(b);
  });
}

}  // namespace

std::vector<RegistryValue> RecipeRegistryTransition(const RecipeRegistryState* previous, const GameRecipe& recipe,
                                                    RecipeRegistryState* next) {
  std::vector<RegistryValue> values;
  RecipeRegistryState after;
  after.recipe_id = recipe.id;
  if (IsCommunityRecipeId(recipe.id)) after.revision = recipe.revision;
  // Wine folds case in key and value names (as ApplyRegistryValues does): a value in both is an assignment,
  // never a removal plus an assignment, which would be refused as a duplicate.
  const auto same = [](const TrackedRegistryValue& tracked, const RegistryValue& value) {
    return tracked.default_value == value.default_value && FoldedEqual(tracked.key, value.key) &&
           (tracked.default_value || FoldedEqual(tracked.name, value.name));
  };
  if (previous)
    for (const auto& tracked : previous->values) {
      const bool kept = std::any_of(recipe.registry.begin(), recipe.registry.end(),
                                    [&](const RegistryValue& value) { return same(tracked, value); });
      if (!kept) values.push_back({tracked.key, tracked.name, {}, tracked.default_value});
    }
  for (const auto& value : recipe.registry) {
    values.push_back(value);
    // A value the recipe removes itself (empty data) is not one it owns afterwards.
    if (!value.data.empty()) after.values.push_back({value.key, value.name, value.default_value});
  }
  if (next) *next = std::move(after);
  return values;
}

RecipeRegistryState RecipeRegistryInterim(const RecipeRegistryState* previous, const RecipeRegistryState& next) {
  RecipeRegistryState interim = next;
  if (!previous) return interim;
  for (const auto& tracked : previous->values) {
    const bool listed = std::any_of(interim.values.begin(), interim.values.end(), [&](const TrackedRegistryValue& value) {
      return value.default_value == tracked.default_value && FoldedEqual(value.key, tracked.key) &&
             (value.default_value || FoldedEqual(value.name, tracked.name));
    });
    if (!listed) interim.values.push_back(tracked);
  }
  return interim;
}

}  // namespace playden::runtime
