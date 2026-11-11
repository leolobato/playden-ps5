// community_rules.cpp - The strict rules for community recipes and whether a kit fits one.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/community_rules.h"

#include "runtime/prospero_profile.h"
#include "steam/backend/app_info.h"

#include <set>

namespace playden::runtime {
namespace {

std::string Lower(std::string text) {
  for (auto& c : text) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  return text;
}

// What a text field may hold.
struct TextRules {
  bool multiline = false;  // \n and \t allowed (notes)
  bool trimmed = false;    // no space, NBSP or other Unicode space at either end (names, titles)
};

bool IsBidiControl(std::uint32_t point) {
  return point == 0x200e || point == 0x200f || (point >= 0x202a && point <= 0x202e) || (point >= 0x2066 && point <= 0x2069);
}

// A space or an invisible character that can hide at the ends of a name.
bool IsBlank(std::uint32_t point) {
  return point == 0x20 || point == 0xa0 || point == 0x1680 || point == 0x180e || (point >= 0x2000 && point <= 0x200d) ||
         point == 0x202f || point == 0x205f || point == 0x2060 || point == 0x3000 || point == 0xfeff;
}

// Valid UTF-8 without control, bidirectional or surrogate characters (and, when asked, surrounding
// spaces): the number of characters, or nothing.
std::optional<std::size_t> PrintableCharacters(std::string_view text, TextRules rules = {}) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < text.size(); ++count) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    std::uint32_t point = 0;
    if (lead < 0x80) length = 1, point = lead;
    else if ((lead & 0xe0) == 0xc0) length = 2, point = lead & 0x1f;
    else if ((lead & 0xf0) == 0xe0) length = 3, point = lead & 0x0f;
    else if ((lead & 0xf8) == 0xf0) length = 4, point = lead & 0x07;
    else return std::nullopt;
    if (index + length > text.size()) return std::nullopt;
    for (std::size_t next = 1; next < length; ++next) {
      const auto byte = static_cast<unsigned char>(text[index + next]);
      if ((byte & 0xc0) != 0x80) return std::nullopt;
      point = (point << 6) | (byte & 0x3f);
    }
    static constexpr std::uint32_t kShortest[] = {0, 0, 0x80, 0x800, 0x10000};
    if (point < kShortest[length] || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return std::nullopt;
    const bool allowed_control = rules.multiline && (point == '\n' || point == '\t');
    if (!allowed_control && (point < 0x20 || (point >= 0x7f && point <= 0x9f))) return std::nullopt;
    if (point == 0x2028 || point == 0x2029 || IsBidiControl(point)) return std::nullopt;
    if (rules.trimmed && IsBlank(point) && (index == 0 || index + length == text.size())) return std::nullopt;
    index += length;
  }
  return count;
}

bool PlainText(std::string_view text, bool multiline = false) {
  TextRules rules;
  rules.multiline = multiline;
  return PrintableCharacters(text, rules).has_value();
}

bool IsDate(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
  for (std::size_t index = 0; index < text.size(); ++index)
    if (index != 4 && index != 7 && (text[index] < '0' || text[index] > '9')) return false;
  const int month = (text[5] - '0') * 10 + (text[6] - '0');
  const int day = (text[8] - '0') * 10 + (text[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

std::string Basename(std::string path) {
  for (auto& c : path) c = c == '\\' ? '/' : c;
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// NAME=value entries of an Environment variables value, trimmed as the profile reads them.
std::vector<std::pair<std::string, std::string>> EnvironmentEntries(const std::string& value) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (std::size_t start = 0; start <= value.size();) {
    const auto end = std::min(value.find(';', start), value.size());
    std::string entry = value.substr(start, end - start);
    start = end + 1;
    const auto equals = entry.find('=');
    if (equals == std::string::npos) continue;
    std::string name = entry.substr(0, equals), item = entry.substr(equals + 1);
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!item.empty() && item.front() == ' ') item.erase(0, 1);
    while (!item.empty() && item.back() == ' ') item.pop_back();
    entries.emplace_back(std::move(name), std::move(item));
  }
  return entries;
}

// The library names of an override list that are not plain names (a wildcard, an empty name).
std::vector<std::string> UnnamedLibraries(const std::string& list) {
  std::vector<std::string> refused;
  for (const auto& entry : SplitDllOverrides(list)) {
    const auto names = entry.substr(0, entry.find('='));
    std::size_t start = 0;
    while (start <= names.size()) {
      const auto end = std::min(names.find(',', start), names.size());
      const auto name = names.substr(start, end - start);
      start = end + 1;
      if (name.empty() ||
          name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
        refused.push_back(name.empty() ? entry : name);
    }
  }
  return refused;
}

const std::string* Default(const GameRecipe& recipe, const char* id) {
  const auto found = recipe.defaults.find(id);
  return found == recipe.defaults.end() || found->second.empty() ? nullptr : &found->second;
}

// Windows and Wine programs a launch entry may not name: a registry key for one changes the system's behaviour, not a game's.
bool IsSystemProgram(const std::string& basename) {
  static const std::set<std::string> kPrograms = {
      "msiexec", "explorer", "rundll32", "regedit", "regsvr32", "cmd", "conhost", "wineboot", "winecfg",
      "winedbg", "services", "svchost", "start", "control", "notepad", "iexplore", "plugplay", "winemenubuilder",
      "wineconsole", "taskmgr", "reg", "powershell", "cscript", "wscript", "mshta", "steam", "steamwebhelper"};
  auto name = Lower(basename);
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
  return kPrograms.count(name) != 0;
}

// A path component Windows reads as another name: only dots and spaces ("...", ".. "), or ending in a dot or a space.
bool HasAliasedComponent(const std::string& relative) {
  for (std::size_t start = 0; start <= relative.size();) {
    const auto end = std::min(relative.find_first_of("/\\", start), relative.size());
    const auto part = relative.substr(start, end - start);
    start = end + 1;
    if (part.empty()) continue;
    if (part.back() == '.' || part.back() == ' ' || part.find_first_not_of(". ") == std::string::npos) return true;
  }
  return false;
}

constexpr std::string_view kAppDefaults = "software\\wine\\appdefaults\\";

// The lowercase program a Software\Wine\AppDefaults\<program>\... key names; empty for other keys.
std::string AppDefaultsProgram(const std::string& key) {
  const auto folded = Lower(key);
  if (folded.rfind(kAppDefaults, 0) != 0) return {};
  const auto rest = folded.substr(kAppDefaults.size());
  return rest.substr(0, rest.find('\\'));
}

std::string RegistryText(const RegistryValue& value) {
  return value.key + "\\" + value.name + " = " + value.data;
}

}  // namespace

std::vector<std::string> CommunityRecipeRefusals(const CommunityRecipe& community, const CommunityCheckOptions& options) {
  std::vector<std::string> refusals;
  const auto refuse = [&](std::string why) { refusals.push_back(std::move(why)); };
  const auto& recipe = community.recipe;

  // Identity: made from the game object, never split.
  const auto export_id = CommunityRecipeIdFor(community.game, {});
  if (recipe.id.size() > kCommunityRecipeIdMaxBytes)
    refuse("recipe_id is longer than " + std::to_string(kCommunityRecipeIdMaxBytes) + " bytes.");
  else if (recipe.id == export_id)
    refuse("recipe_id has no slug: name it " + export_id + "-<slug>, with lowercase letters, digits and single hyphens.");
  else if (!CommunityRecipeSlug(recipe.id, community.game))
    refuse("recipe_id must be " + export_id +
           "-<slug>: a slug of 1-32 lowercase letters, digits and single hyphens, no hyphen first or last.");
  if (!IsStoreKey(community.game.store, community.game.key))
    refuse("game names no store and key: \"game\": {\"store\", \"key\", \"name\"}.");
  if (!IsGameName(community.game_name)) refuse("game.name must be 1-120 printable characters.");
  if (!community.made_on.empty() && community.made_on == community.game.store)
    refuse("made_on is only for a copy from another store; leave it out when it is " + community.made_on + ".");

  // Who and what.
  const auto title = PrintableCharacters(community.title, {false, true});
  // The title is optional: without one, Playden calls the recipe "Recipe from <author>".
  if (!community.title.empty() && (!title || *title > kCommunityTitleMaxCharacters))
    refuse("community.title must be 1-60 printable characters.");
  const auto name_ok = [](const std::string& name) {
    const auto characters = PrintableCharacters(name, {false, true});
    return characters && *characters >= 1 && *characters <= kCommunityNameMaxCharacters;
  };
  if (!community.author.empty() && !name_ok(community.author))
    refuse("community.author must be 1-64 printable characters.");
  std::set<std::string> contributors;
  if (community.contributors.size() > kCommunityMaxContributors) refuse("community.contributors lists more than 32 names.");
  for (const auto& name : community.contributors) {
    if (!name_ok(name)) refuse("Each of community.contributors must be 1-64 printable characters.");
    else if (!contributors.insert(name).second || name == community.author)
      refuse("community.contributors names " + name + " twice (or names the author).");
  }
  if (!community.tested_kit.empty() && !ParseKitVersion(community.tested_kit))
    refuse("community.tested_kit is not a kit version like 0.12: " + community.tested_kit + ".");
  if (!community.min_kit.empty() && !ParseKitVersion(community.min_kit))
    refuse("community.min_kit is not a kit version like 0.12: " + community.min_kit + ".");
  if (!community.tested_date.empty() && !IsDate(community.tested_date))
    refuse("community.tested_date must be YYYY-MM-DD.");
  const std::pair<const char*, const std::string*> texts[] = {
      {"community.tested_firmware", &community.tested_firmware}, {"community.tested_date", &community.tested_date},
      {"community.status", &community.status}, {"the recipe's name", &recipe.name}};
  for (const auto& [where, text] : texts)
    if (!PlainText(*text)) refuse(std::string(where) + " must be valid UTF-8 without control or bidirectional characters.");
  if (!PlainText(community.notes, true))
    refuse("community.notes must be valid UTF-8 without control (other than new lines and tabs) or bidirectional characters.");
  if (!recipe.locked.empty())
    refuse("locked must be empty: a community recipe can't stop the player from changing a row.");

  // Program: inside the game folder.
  std::set<std::string> programs;
  for (const auto& entry : recipe.launch_entries) {
    const auto relative = steam::RelativeGamePath(entry.executable);
    if (!relative) {
      refuse("Launch entry " + entry.id + " starts " + entry.executable + ", which is not inside the game folder.");
      continue;
    }
    if (Lower(*relative).size() < 4 || Lower(*relative).substr(relative->size() - 4) != ".exe")
      refuse("Launch entry " + entry.id + " starts " + entry.executable + ", which is not a Windows program (.exe).");
    if (IsSystemProgram(Basename(*relative)))
      refuse("Launch entry " + entry.id + " starts " + entry.executable + ", a Windows or Wine system program: name the game's own program.");
    if (HasAliasedComponent(*relative))
      refuse("Launch entry " + entry.id + " starts " + entry.executable +
             ", with a folder or file name of only dots and spaces or ending in a dot or space.");
    if (const auto folder = LaunchWorkingFolder(entry)) {
      if (HasAliasedComponent(*folder))
        refuse("Launch entry " + entry.id + " starts in " + (entry.working_directory.empty() ? *folder : entry.working_directory) +
               ", with a folder name of only dots and spaces or ending in a dot or space.");
    } else {
      refuse("Launch entry " + entry.id + " starts in " + entry.working_directory + ", which is not inside the game folder.");
    }
    programs.insert(Lower(Basename(*relative)));
  }
  if (!IsSettingsOnlyRecipe(community)) {
    for (const char* id : {"program", "program_folder", "program_arguments", "arguments", "extra_arguments", "dll_overrides"})
      if (recipe.defaults.count(id))
        refuse(std::string("defaults sets ") + id + ": put the program, its folder, arguments and library overrides in "
               "the recipe's launch_entries, arguments and dll_overrides (the player's own arguments row is not shared).");
  } else {
    // Settings-only: the program rows are its only way to name a program; the rest is Playden's for the copy.
    for (const char* id : {"arguments", "extra_arguments", "dll_overrides", "architecture"})
      if (recipe.defaults.count(id))
        refuse(std::string("defaults sets ") + id + ": a settings-only recipe takes the program's arguments, library "
               "overrides and architecture from Playden's recipe for the copy; name a program with defaults program "
               "and its arguments with program_arguments, or share a recipe with its own program (format_version 1).");
    if (recipe.defaults.count("graphics"))
      refuse("defaults sets graphics: the Graphics row comes from the recipe's graphics field, so set \"graphics\" "
             "instead.");
    if (const auto* program = Default(recipe, "program")) {
      const auto relative = steam::RelativeGamePath(*program);
      if (!relative) {
        refuse("defaults program " + *program + " is not inside the game folder.");
      } else {
        if (IsSystemProgram(Basename(*relative)))
          refuse("defaults program " + *program + " is a Windows or Wine system program: name the game's own program.");
        if (HasAliasedComponent(*relative))
          refuse("defaults program " + *program +
                 " has a folder or file name of only dots and spaces or ending in a dot or space.");
      }
    }
    if (const auto* folder = Default(recipe, "program_folder"); folder && !folder->empty()) {
      const auto relative = steam::RelativeGamePath(*folder, true);
      if (!relative) refuse("defaults program_folder " + *folder + " is not inside the game folder.");
      else if (HasAliasedComponent(*relative))
        refuse("defaults program_folder " + *folder +
               " has a folder name of only dots and spaces or ending in a dot or space.");
    }
  }

  // Online and the sign-in hand the player's Steam account to the game: never from someone else's recipe.
  if (const auto* steam = Default(recipe, "steam_features"); steam && *steam == "online")
    refuse("defaults sets steam_features to online: each player turns on Online Steam features for themselves.");
  if (Default(recipe, "steam_sign_in"))
    refuse("defaults sets steam_sign_in: each player chooses when a game gets their Steam sign-in.");
  if (Default(recipe, "game_invites"))
    refuse("defaults sets game_invites: each player chooses what a game does with their Steam invites.");

  // Library overrides: named libraries only.
  for (const auto& [where, list] : {std::pair<const char*, const std::string*>{"dll_overrides", &recipe.dll_overrides},
                                    {"defaults library_overrides", Default(recipe, "library_overrides")},
                                    {"defaults registry_dll_overrides", Default(recipe, "registry_dll_overrides")}}) {
    if (!list) continue;
    for (const auto& name : UnnamedLibraries(*list))
      refuse(std::string(where) + " names " + name + ": name each library, without a wildcard.");
  }

  // Environment: only the variables Playden itself sets.
  if (const auto* environment = Default(recipe, "environment"))
    for (const auto& [name, value] : EnvironmentEntries(*environment))
      if (!IsPlaydenEnvironmentVariable(name))
        refuse("Environment variable " + name + " is not one Playden sets; a community recipe may set only those.");
      // A share carries the Address space setting instead: on a game whose title still runs an older runtime the
      // setting waits for its update, while the raw variable would make every setting of the game refused.
      else if (name == "WINE_LARGE_ADDRESS_AWARE")
        refuse("Environment variable WINE_LARGE_ADDRESS_AWARE: share the Address space setting instead.");
      else if (name == "PW_NSIPROXY_DISABLE")
        refuse("Environment variable PW_NSIPROXY_DISABLE: share the Network adapters setting instead.");

  // Registry: the recipe's own programs; publisher keys are flagged for review instead.
  for (const auto& value : recipe.registry) {
    const auto program = AppDefaultsProgram(value.key);
    if (program.empty()) continue;
    if (!programs.count(program))
      refuse("Registry key " + value.key + " is for " + program + ", which is not one of the recipe's programs.");
  }
  return refusals;
}

std::vector<std::string> CommunityRecipeFlags(const CommunityRecipe& community) {
  std::vector<std::string> flags;
  const auto& recipe = community.recipe;
  const bool settings_only = IsSettingsOnlyRecipe(community);
  if (settings_only) {
    // Its program rows; what Playden fills in is Playden's own.
    const auto* program = Default(recipe, "program");
    const auto* folder = Default(recipe, "program_folder");
    const auto* arguments = Default(recipe, "program_arguments");
    const std::string name = program ? *program : "Playden's program";
    if (program) flags.push_back("program: " + *program + (folder && !folder->empty() ? " (starts in " + *folder + ")" : ""));
    else if (folder && !folder->empty()) flags.push_back("Playden's program starts in " + *folder);
    if (arguments && !arguments->empty()) flags.push_back("arguments of " + name + ": " + *arguments);
    if (const auto* option = Default(recipe, "launch_option"); option && !program)
      flags.push_back("starts Playden's launch option " + *option);
  }
  for (const auto& entry : settings_only ? std::vector<LaunchEntry>{} : recipe.launch_entries) {
    std::string item = "program: " + entry.executable;
    if (!entry.working_directory.empty()) item += " (starts in " + entry.working_directory + ")";
    else if (entry.in_install_root) item += " (starts in the game folder)";
    flags.push_back(std::move(item));
    if (!entry.arguments.empty()) flags.push_back("arguments of " + entry.executable + ": " + entry.arguments);
  }
  if (!settings_only) {
    if (const auto* option = Default(recipe, "launch_option"))
      if (recipe.launch_entries.empty() || *option != recipe.launch_entries.front().id)
        flags.push_back("starts the program " + *option + ", not the first one");
    if (!recipe.arguments.empty()) flags.push_back("arguments: " + recipe.arguments);
    if (!recipe.resolution_arguments.empty() && recipe.arguments.find("{resolution}") != std::string::npos)
      flags.push_back("resolution arguments: " + recipe.resolution_arguments);
  }
  if (const auto* extra = Default(recipe, "extra_arguments")) flags.push_back("extra arguments: " + *extra);
  if (!settings_only && !recipe.dll_overrides.empty()) flags.push_back("library overrides: " + recipe.dll_overrides);
  if (const auto* added = Default(recipe, "library_overrides")) flags.push_back("added library overrides: " + *added);
  if (const auto* registry = Default(recipe, "registry_dll_overrides"))
    flags.push_back("library load order in the registry: " + *registry);
  if (const auto* environment = Default(recipe, "environment")) flags.push_back("environment: " + *environment);
  for (const auto& value : settings_only ? std::vector<RegistryValue>{} : recipe.registry)
    flags.push_back(AppDefaultsProgram(value.key).empty() ? "publisher registry key, needs review: " + RegistryText(value)
                                                          : "registry: " + RegistryText(value));
  return flags;
}

std::string CommunityRecipeFeature(const CommunityRecipe& community) {
  const auto& recipe = community.recipe;
  if (Default(recipe, "steam_network") || Default(recipe, "steam_sign_in") || Default(recipe, "game_invites"))
    return "online-steam";
  if (const auto* steam = Default(recipe, "steam_features"); steam && *steam == "online") return "online-steam";
  return {};
}

std::string CommunityRecipeFits(const CommunityRecipe& community, const VerifiedKitView& kit,
                                std::optional<KitVersion> title_kit_version) {
  if (!community.min_kit.empty()) {
    const auto needed = ParseKitVersion(community.min_kit);
    if (!needed) return "The recipe asks for a runtime kit version this Playden doesn't know (" + community.min_kit + ").";
    if (!title_kit_version || *title_kit_version < *needed) return "The recipe needs runtime kit version " + FormatKitVersion(*needed) + " or newer.";
  }
  const auto unmet = UnmetRecipeRequirements(community.recipe.requirements, kit);
  if (!unmet.empty()) return "The runtime kit lacks what this recipe needs (" + unmet + ").";
  return {};
}

}  // namespace playden::runtime
