// settings_only_recipe_test.cpp - Settings-only community recipes (community.format_version 2): what the file may
// carry, and that format 1 keeps its meaning.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/game_settings.h"

#include <cstdio>
#include <string>

using namespace playden::runtime;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A community recipe for Darkwood; fields is what goes between the name and the community block.
std::string File(int format, const std::string& fields) {
  return "{\n  \"recipe_id\": \"community-steam-274520-mouse\",\n  \"recipe_version\": 1,\n"
         "  \"game\": {\"store\": \"steam\", \"key\": \"274520\", \"name\": \"Darkwood\"},\n"
         "  \"name\": \"Darkwood\",\n" + fields +
         "  \"community\": {\"format_version\": " + std::to_string(format) +
         ", \"title\": \"Mouse preset\", \"status\": \"works\", \"author\": \"Ana\"}\n}\n";
}

const std::string kProgram =
    "  \"launch_entries\": [{\"id\": \"game\", \"label\": \"Darkwood\", \"executable\": \"Darkwood.exe\"}],\n"
    "  \"architecture\": \"pe64\", \"graphics\": \"dxvk\", \"executable\": \"Darkwood.exe\",\n";
const std::string kSettings = "  \"defaults\": {\"controller_preset\": \"mouse\"},\n";

void Parse() {
  CommunityRecipe community;
  std::string reason;
  // Format 2 without a program reads: no launch entries, no executable, no architecture or graphics of its own.
  CHECK(ParseCommunityRecipe(File(2, kSettings), &community, &reason));
  CHECK(community.format_version == 2 && IsSettingsOnlyRecipe(community) && community.executable.empty() &&
        community.recipe.launch_entries.empty() && community.recipe.architecture.empty() &&
        community.recipe.graphics.empty() && community.recipe.defaults.at("controller_preset") == "mouse");
  CHECK(community.game == playden::GameRef::Steam(274520) && community.title == "Mouse preset");
  // Its program rows are defaults: a program, a launch option (checked once Playden fills its entries), arguments.
  CHECK(ParseCommunityRecipe(File(2, "  \"defaults\": {\"program\": \"bin/Game.exe\", \"launch_option\": \"dx11\","
                                     " \"program_arguments\": \"-windowed\", \"program_folder\": \"bin\"},\n"),
                             &community, &reason));
  CHECK(community.recipe.defaults.at("program") == "bin/Game.exe" && community.recipe.defaults.at("launch_option") == "dx11");
  CHECK(!ParseCommunityRecipe(File(2, "  \"defaults\": {\"program\": \"/etc/Game.exe\"},\n"), &community, &reason) &&
        Has(reason, "default program"));
  // Graphics may be set: a default for the graphics row would not apply, as the recipe's field wins.
  CHECK(ParseCommunityRecipe(File(2, "  \"graphics\": \"opengl\",\n" + kSettings), &community, &reason) &&
        community.recipe.graphics == "opengl");
  CHECK(!ParseCommunityRecipe(File(2, "  \"graphics\": \"glide\",\n" + kSettings), &community, &reason) &&
        Has(reason, "graphics"));

  // What Playden fills in, and what belongs to a recipe with its own program, is refused in format 2.
  for (const char* field : {"\"launch_entries\": []", "\"architecture\": \"pe64\"", "\"runtime\": \"wine\"",
                            "\"prefix\": \"default\"", "\"detected\": \"unity\"", "\"dll_overrides\": \"d3d9=n\"",
                            "\"arguments\": \"-x\"", "\"resolution_arguments\": \"-w {width}\"",
                            "\"executable\": \"Darkwood.exe\"", "\"registry\": {}", "\"ini\": {}", "\"game_files\": []",
                            "\"steam_api_copies\": []"}) {
    const std::string text = File(2, std::string("  ") + field + ",\n" + kSettings);
    const std::string name = std::string(field).substr(1, std::string(field).find('"', 1) - 1);
    CHECK(!ParseCommunityRecipe(text, &community, &reason) && Has(reason, "settings-only") && Has(reason, name));
  }
  CHECK(!ParseCommunityRecipe(File(2, kProgram + kSettings), &community, &reason) && Has(reason, "launch_entries"));

  // Format 1 keeps its meaning: a program and its executable, or the file is refused.
  CHECK(ParseCommunityRecipe(File(1, kProgram + kSettings), &community, &reason));
  CHECK(community.format_version == 1 && !IsSettingsOnlyRecipe(community) && community.executable == "Darkwood.exe");
  CHECK(!ParseCommunityRecipe(File(1, kSettings), &community, &reason) && Has(reason, "launch_entries"));
  CHECK(!ParseCommunityRecipe(File(1, "  \"architecture\": \"pe64\", \"graphics\": \"dxvk\",\n" + kSettings),
                              &community, &reason) &&
        Has(reason, "launch_entries"));
  // An executable without launch entries is refused in either format.
  CHECK(!ParseCommunityRecipe(File(1, "  \"executable\": \"Darkwood.exe\", \"architecture\": \"pe64\", \"graphics\": "
                                      "\"dxvk\",\n" + kSettings),
                              &community, &reason));
  CHECK(!ParseCommunityRecipe(File(2, "  \"executable\": \"Darkwood.exe\",\n" + kSettings), &community, &reason));
  // Shipped and generated recipes still need launch entries.
  GameRecipe shipped;
  CHECK(!ParseGameRecipe("{\"recipe_id\": \"steam-274520\", \"recipe_version\": 1, \"app_id\": 274520, "
                         "\"architecture\": \"pe64\", \"graphics\": \"dxvk\"}",
                         &shipped, &reason) &&
        reason == "recipe needs launch_entries");
  // ParseGameRecipe alone reads no settings-only file: only the community parser knows its version.
  CHECK(!ParseGameRecipe(File(2, kSettings), &shipped, &reason));

  // The version decides before anything else: an unknown one is refused for that alone, with the versions read.
  CHECK(!ParseCommunityRecipe(File(3, kSettings), &community, &reason) &&
        reason == "community recipe needs community.format_version 1 to 2");
  CHECK(!ParseCommunityRecipe(File(0, kProgram + kSettings), &community, &reason) &&
        reason == "community recipe needs community.format_version 1 to 2");
  // A build that reads format 1 only refuses a format 2 file, and still reads format 1.
  CHECK(!ParseCommunityRecipe(File(2, kSettings), &community, &reason, 1) &&
        reason == "community recipe needs community.format_version 1");
  CHECK(ParseCommunityRecipe(File(1, kProgram + kSettings), &community, &reason, 1));

  // Shares from Playden stay format 1: the export has its program.
  CHECK(ParseCommunityRecipe(File(1, kProgram + kSettings), &community, &reason));
  CHECK(Has(SerializeCommunityRecipe(community), "\"format_version\": 1"));
}

}  // namespace

int main() {
  Parse();
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
