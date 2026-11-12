// settings_only_recipe_test.cpp - Settings-only community recipes (community.format_version 2): what the file may
// carry, and that format 1 keeps its meaning.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/community_rules.h"
#include "runtime/game_install.h"
#include "runtime/game_settings.h"
#include "runtime/recipe_choice.h"

#include <cstdio>
#include <string>
#include <vector>

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


// Playden's recipe for the copy: what a settings-only recipe takes its program from.
GameRecipe Own() {
  GameRecipe own;
  own.id = "steam-274520-auto";
  own.version = 1;
  own.app_id = 274520;
  own.name = "Darkwood";
  own.launch_entries = {{"game", "Darkwood", "Darkwood.exe", ""}, {"dx11", "Darkwood (DX11)", "bin/Darkwood11.exe", ""}};
  own.architecture = "pe64";
  own.graphics = "dxvk";
  own.arguments = "-force-d3d11 {resolution}";
  own.resolution_arguments = "-screen-width {width} -screen-height {height}";
  own.dll_overrides = "d3d11,dxgi=n";
  own.runtime = "wine";
  own.prefix = "default";
  own.detected = "unity";
  return own;
}

CommunityRecipe Read(const std::string& defaults, const std::string& graphics = {}) {
  CommunityRecipe community;
  std::string reason;
  const auto fields = (graphics.empty() ? std::string() : "  \"graphics\": \"" + graphics + "\",\n") +
                      "  \"defaults\": {" + defaults + "},\n";
  if (!ParseCommunityRecipe(File(2, fields), &community, &reason)) std::fprintf(stderr, "unread: %s\n", reason.c_str());
  community.recipe.revision = std::string(64, 'a');
  BindCommunityRecipe(&community, playden::GameId::Steam(274520), 274520);
  return community;
}

void Apply() {
  const auto own = Own();
  std::vector<std::string> asked;
  // The copy has bin/Old.exe (32-bit) and Playden's programs; nothing else.
  const auto copy = [&](const std::string& program) -> std::string {
    asked.push_back(program);
    if (program == "bin/Old.exe") return "pe32";
    if (program == "Darkwood.exe" || program == "bin/Darkwood11.exe") return "pe64";
    return {};
  };
  KitCapabilities kit;
  kit.vkd3d_proton = true;
  EffectiveSettings settings;
  SettingsError error;

  // No program rows: Playden's program and every program-shaped field, the recipe's settings on top.
  auto community = Read("\"controller_preset\": \"mouse\", \"d3d12\": \"vkd3d-proton\"");
  CHECK(FillSettingsOnlyRecipe(&community.recipe, own, copy).empty() && asked.empty());
  const auto& recipe = community.recipe;
  CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[0].executable == "Darkwood.exe" &&
        recipe.architecture == "pe64" && recipe.graphics == "dxvk" && recipe.arguments == own.arguments &&
        recipe.resolution_arguments == own.resolution_arguments && recipe.dll_overrides == own.dll_overrides &&
        recipe.runtime == "wine" && recipe.prefix == "default" && recipe.detected == "unity");
  CHECK(recipe.id == "community-steam-274520-mouse" && recipe.defaults.at("controller_preset") == "mouse");
  CHECK(BuildEffectiveSettings(recipe, nullptr, {}, kit, &settings, &error));
  CHECK(settings.launch && settings.launch->executable == "Darkwood.exe" && settings.graphics == "dxvk" &&
        settings.values.at("controller_preset").value == "mouse" && settings.values.at("d3d12").value == "vkd3d-proton");
  // Graphics the file sets wins over Playden's.
  community = Read("\"controller_preset\": \"mouse\"", "opengl");
  FillSettingsOnlyRecipe(&community.recipe, own, copy);
  CHECK(community.recipe.graphics == "opengl" && BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) &&
        settings.graphics == "opengl");

  // A program the copy has: it starts, built as the copy says, with the recipe's arguments for it.
  community = Read("\"program\": \"bin\\\\Old.exe\", \"program_arguments\": \"-windowed\"");
  CHECK(FillSettingsOnlyRecipe(&community.recipe, own, copy).empty());
  CHECK(asked.size() == 1 && asked[0] == "bin/Old.exe" && community.recipe.architecture == "pe32");
  CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error));
  CHECK(settings.launch && settings.launch->executable == "bin/Old.exe" && settings.architecture == "pe32" &&
        settings.launch->arguments == "-windowed");
  // One it hasn't: Playden's program, the program rows dropped, the missing program returned.
  community = Read("\"program\": \"bin/Gone.exe\", \"program_arguments\": \"-windowed\", \"program_folder\": \"bin\","
                   " \"controller_preset\": \"mouse\"");
  CHECK(FillSettingsOnlyRecipe(&community.recipe, own, copy) == "bin/Gone.exe");
  CHECK(!community.recipe.defaults.count("program") && !community.recipe.defaults.count("program_arguments") &&
        !community.recipe.defaults.count("program_folder") && community.recipe.defaults.at("controller_preset") == "mouse");
  CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) && settings.launch &&
        settings.launch->executable == "Darkwood.exe" && settings.architecture == "pe64");

  // A launch option of Playden's recipe starts; one it hasn't is dropped, so the settings still build.
  community = Read("\"launch_option\": \"dx11\"");
  CHECK(FillSettingsOnlyRecipe(&community.recipe, own, copy).empty());
  CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) && settings.launch &&
        settings.launch->executable == "bin/Darkwood11.exe");
  community = Read("\"launch_option\": \"dx11\", \"program_arguments\": \"-dx11\", \"program_folder\": \".\"");
  FillSettingsOnlyRecipe(&community.recipe, own, copy);
  CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) && settings.launch &&
        settings.launch->executable == "bin/Darkwood11.exe" && settings.launch->arguments == "-dx11" &&
        settings.launch->in_install_root && settings.values.at("program_arguments").value == "-dx11");
  community = Read("\"launch_option\": \"vulkan\", \"program_arguments\": \"-vulkan\"");
  CHECK(FillSettingsOnlyRecipe(&community.recipe, own, copy) == "vulkan" && !community.recipe.defaults.count("launch_option") &&
        !community.recipe.defaults.count("program_arguments"));
  CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) && settings.launch &&
        settings.launch->executable == "Darkwood.exe" && settings.launch->arguments.empty());
  // Arguments and a folder alone go with Playden's own program.
  community = Read("\"program_arguments\": \"-nolog\", \"program_folder\": \"bin\"");
  CHECK(FillSettingsOnlyRecipe(&community.recipe, own, copy).empty());
  CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) && settings.launch &&
        settings.launch->executable == "Darkwood.exe" && settings.launch->arguments == "-nolog" &&
        settings.launch->working_directory == "bin");
  // Playden's recipe itself is not changed.
  CHECK(own.launch_entries[0].arguments.empty() && own.launch_entries.size() == 2);

  // A generated recipe leaves its DXVK overrides to Graphics and asks Unity for Direct3D 11: filled from one, the
  // recipe gets both completed as the generated recipe would, here with vkd3d-proton asking for Direct3D 12.
  {
    auto generated = own;
    generated.dll_overrides.clear();
    CHECK(IsGeneratedRecipeId(generated.id));
    community = Read("\"d3d12\": \"vkd3d-proton\"");
    FillSettingsOnlyRecipe(&community.recipe, generated, copy);
    CHECK(UsesGeneratedLaunch(community.recipe));
    CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error));
    CHECK(Has(settings.dll_overrides, "d3d8,d3d9,d3d10core,d3d11,dxgi=n") && Has(settings.arguments, "-force-d3d12") &&
          !Has(settings.arguments, "-force-d3d11"));
    // Playden's shipped recipe names its overrides itself: nothing is added.
    auto shipped = generated;
    shipped.id = "steam-274520-darkwood-v1";
    community = Read("\"controller_preset\": \"mouse\"");
    FillSettingsOnlyRecipe(&community.recipe, shipped, copy);
    CHECK(!UsesGeneratedLaunch(community.recipe));
    CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) &&
          !Has(settings.dll_overrides, "d3d8,d3d9") && Has(settings.arguments, "-force-d3d11"));
  }

  // Playden's own settings for the copy stay under the recipe's: a generated recipe's Steam emulator, a shipped
  // recipe's lock. The recipe's defaults win.
  {
    auto with_defaults = own;
    with_defaults.defaults = {{"steam_features", "emulate"}, {"controller_preset", "gamepad"}, {"launch_option", "dx11"}};
    with_defaults.locked = {"graphics"};
    community = Read("\"controller_preset\": \"mouse\"");
    FillSettingsOnlyRecipe(&community.recipe, with_defaults, copy);
    CHECK(community.recipe.defaults.at("steam_features") == "emulate" && community.recipe.defaults.at("controller_preset") == "mouse" &&
          community.recipe.locked.count("graphics"));
    CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) &&
          settings.values.at("steam_features").value == "emulate" && settings.launch &&
          settings.launch->executable == "bin/Darkwood11.exe");
    // A recipe naming its program keeps Playden's launch option out.
    community = Read("\"program\": \"bin/Old.exe\"");
    FillSettingsOnlyRecipe(&community.recipe, with_defaults, copy);
    CHECK(!community.recipe.defaults.count("launch_option") && community.recipe.defaults.at("steam_features") == "emulate");
    CHECK(BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error) && settings.launch &&
          settings.launch->executable == "bin/Old.exe");
  }

  // Identity: the game, id and revision, with no program of its own.
  community = Read("\"controller_preset\": \"mouse\"");
  FillSettingsOnlyRecipe(&community.recipe, own, copy);
  RuntimeInstall record;
  record.runtime_number = 274520;
  record.game = playden::GameId::Steam(274520);
  record.recipe_id = community.recipe.id;
  record.recipe_version = RecipeVersionTag(community.recipe);
  CHECK(community.executable.empty() && RecipeMatchesInstall(community.recipe, record));
  auto other = community;
  other.recipe.revision = std::string(64, 'b');
  CHECK(!RecipeMatchesInstall(other.recipe, record));

  // Selection: the filled recipe applies; without a program Playden can find, Playden's recipe with the reason.
  VerifiedKitView view;
  RecipeCandidates candidates;
  candidates.shipped = &own;
  candidates.kit = &view;
  const CommunityRecipeChoice wanted{community.recipe.id, community.recipe.revision};
  candidates.wanted = &wanted;
  candidates.community = &community;
  candidates.community_state = CommunityObjectState::kLoaded;
  auto selection = SelectRecipe(candidates);
  CHECK(selection.community && selection.recipe == &community.recipe && selection.fallback_reason.empty());
  auto unfound = Read("\"controller_preset\": \"mouse\"");
  CHECK(FillSettingsOnlyRecipe(&unfound.recipe, GameRecipe{}, copy).empty() && unfound.recipe.launch_entries.empty());
  candidates.community = &unfound;
  candidates.shipped = nullptr;
  selection = SelectRecipe(candidates);
  CHECK(!selection.community && selection.recipe == nullptr &&
        selection.fallback_reason == "Playden found no program to start in this copy.");
  // Unfilled, it doesn't apply either.
  auto unfilled = Read("\"controller_preset\": \"mouse\"");
  candidates.community = &unfilled;
  CHECK(SelectRecipe(candidates).fallback_reason == kCommunityRecipeNoProgram);
}

// A share from a game based on a settings-only recipe whose program the player kept.
void Share() {
  const auto own = Own();
  const auto copy = [](const std::string& program) { return program == "Bin/Old.exe" ? std::string("pe32") : std::string(); };
  KitCapabilities kit;
  auto base = Read("\"controller_preset\": \"mouse\", \"program\": \"Bin/Old.exe\", \"resolution\": \"1920x1080\"");
  auto filled = base;
  FillSettingsOnlyRecipe(&filled.recipe, own, copy);
  EffectiveSettings before, after;
  SettingsError error;
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {}, kit, &before, &error));
  // The player changed the resolution and graphics, and kept the recipe's program.
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {{"resolution", "1280x720"}, {"unlock.graphics", "on"},
                                                        {"graphics", "opengl"}, {"extra_arguments", "-log"}}, kit,
                               &after, &error));
  CHECK(IsProgramRow("program") && IsProgramRow("arguments") && IsProgramRow("dll_overrides") && !IsProgramRow("graphics") &&
        !IsProgramRow("resolution"));
  const auto shared = BuildSettingsOnlyRecipe(base, before, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(IsSettingsOnlyRecipe(shared) && shared.executable.empty() && shared.recipe.launch_entries.empty());
  CHECK(shared.recipe.defaults.at("resolution") == "1280x720" && shared.recipe.defaults.at("controller_preset") == "mouse" &&
        shared.recipe.defaults.at("program") == "Bin/Old.exe" && shared.recipe.graphics == "opengl");
  // What Playden filled in for this copy stays out: own's program, arguments, overrides; and Extra arguments, the
  // player's own row, which no community recipe carries.
  CHECK(!shared.recipe.defaults.count("extra_arguments"));
  CHECK(!shared.recipe.defaults.count("launch_option") && !shared.recipe.defaults.count("arguments") &&
        !shared.recipe.defaults.count("architecture") && !shared.recipe.defaults.count("dll_overrides"));
  auto named = shared;
  named.recipe.id = "community-steam-274520-shared";
  named.title = "Shared";
  named.tested_kit = "0.19";
  const auto text = SerializeCommunityRecipe(named);
  for (const char* field : {"launch_entries", "executable", "architecture", "runtime", "prefix", "dll_overrides",
                            "\"arguments\"", "resolution_arguments", "detected"})
    CHECK(!Has(text, field));
  CHECK(Has(text, "\"format_version\": 2") && Has(text, "\"graphics\": \"opengl\""));
  CommunityRecipe reread;
  std::string reason;
  CHECK(ParseCommunityRecipe(text, &reread, &reason) && IsSettingsOnlyRecipe(reread) &&
        reread.recipe.defaults == named.recipe.defaults && SerializeCommunityRecipe(reread) == text);
  CHECK(CommunityRecipeRefusals(reread).empty());
  // Nothing changed: the share is the base's defaults, with no other row (an older Playden refuses a recipe naming a
  // setting it doesn't know, so a new row must stay out until the player changes it).
  const auto untouched = BuildSettingsOnlyRecipe(base, before, before, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(untouched.recipe.defaults == base.recipe.defaults);
  // Graphics kept: the field stays as the base has it (none here).
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {{"resolution", "1280x720"}}, kit, &after, &error));
  const auto kept = BuildSettingsOnlyRecipe(base, before, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(kept.recipe.graphics.empty() && !Has(SerializeCommunityRecipe(kept), "\"graphics\""));
  // Online is shared; the sign-in and invites stay each player's own; Offline emulation and its Steam network value
  // are shared.
  SetOnlineSteamEnabled(true);
  kit.helper_process = true;  // Online runs Steam beside the game
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {}, kit, &before, &error));
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {{"unlock.steam_features", "on"}, {"steam_features", "online"},
                                                        {"steam_sign_in", "kept"}, {"game_invites", "off"}}, kit, &after,
                               &error));
  CHECK(ChosenValue(after.values.at("steam_features")) == "online" && ChosenValue(after.values.at("steam_sign_in")) == "kept");
  CHECK(ChosenValue(after.values.at("game_invites")) == "off");
  const auto online = BuildSettingsOnlyRecipe(base, before, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(online.recipe.defaults.at("steam_features") == "online" && !online.recipe.defaults.count("steam_sign_in") &&
        !online.recipe.defaults.count("game_invites"));
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {{"unlock.steam_features", "on"}, {"steam_features", "emulate"},
                                                        {"steam_network", "offline"}}, kit, &after, &error));
  const auto offline = BuildSettingsOnlyRecipe(base, before, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(offline.recipe.defaults.count("steam_network") && offline.recipe.defaults.at("steam_network") == "offline");
  SetOnlineSteamEnabled(false);
  // The performance overlay is the player's own: a recipe fixes the game.
  kit.dxvk_config = true;
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {}, kit, &before, &error));
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {{"performance_overlay", "on"}, {"overlay_detail", "full"}}, kit,
                               &after, &error));
  CHECK(after.values.at("performance_overlay").value == "on" && after.values.at("overlay_detail").available);
  const auto overlay = BuildSettingsOnlyRecipe(base, before, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(!overlay.recipe.defaults.count("performance_overlay") && !overlay.recipe.defaults.count("overlay_detail"));
  // Nor from a base shared before the overlay became the player's own.
  auto old_base = base;
  old_base.recipe.defaults["performance_overlay"] = "on";
  old_base.recipe.defaults["overlay_detail"] = "full";
  const auto from_old = BuildSettingsOnlyRecipe(old_base, before, before, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(!from_old.recipe.defaults.count("performance_overlay") && !from_old.recipe.defaults.count("overlay_detail"));
  // Steam library folder: a game this Playden installed shows in Steam's folder by its own default
  // ("default.steam_library_path"), which is no change: it stays out of the share, also against a base built without
  // it. The player's choice goes in.
  kit.steam_library_path = true;
  const std::map<std::string, std::string> installed_here = {{"default.steam_library_path", "steam"}};
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, installed_here, kit, &before, &error));
  auto changed = installed_here;
  changed["resolution"] = "1280x720";
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, changed, kit, &after, &error));
  CHECK(ChosenValue(after.values.at("steam_library_path")) == "steam");
  auto library = BuildSettingsOnlyRecipe(base, before, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(!library.recipe.defaults.count("steam_library_path") && library.recipe.defaults.at("resolution") == "1280x720");
  EffectiveSettings plain;
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {}, kit, &plain, &error));
  library = BuildSettingsOnlyRecipe(base, plain, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(!library.recipe.defaults.count("steam_library_path"));
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, {{"steam_library_path", "steam"}}, kit, &after, &error));
  library = BuildSettingsOnlyRecipe(base, plain, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(library.recipe.defaults.at("steam_library_path") == "steam");
  {
    // A recipe that sets the row reads back and passes the community rules.
    auto shared_row = library;
    shared_row.recipe.id = "community-steam-274520-shared";
    shared_row.title = "Shared";
    CommunityRecipe reread_row;
    std::string why;
    CHECK(ParseCommunityRecipe(SerializeCommunityRecipe(shared_row), &reread_row, &why) &&
          reread_row.recipe.defaults.at("steam_library_path") == "steam" && CommunityRecipeRefusals(reread_row).empty());
  }
  changed["steam_library_path"] = "playden";
  CHECK(BuildEffectiveSettings(filled.recipe, nullptr, changed, kit, &after, &error));
  library = BuildSettingsOnlyRecipe(base, before, after, playden::GameRef::Steam(274520), "Darkwood");
  CHECK(library.recipe.defaults.at("steam_library_path") == "playden");
}

}  // namespace

int main() {
  Parse();
  Apply();
  Share();
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
