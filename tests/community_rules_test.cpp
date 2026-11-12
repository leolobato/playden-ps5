// community_rules_test.cpp - Kit versions, community recipe ids and the strict community rules.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/community_rules.h"
#include "runtime/game_settings.h"
#include "runtime/kit_version.h"
#include "runtime/prospero_profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

std::string Read(const std::string& path) {
  std::string text;
  if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, count);
    std::fclose(file);
  }
  return text;
}

bool Mentions(const std::vector<std::string>& items, const std::string& text) {
  return std::any_of(items.begin(), items.end(),
                     [&](const std::string& item) { return item.find(text) != std::string::npos; });
}

void KitVersions() {
  CHECK(ParseKitVersion("0.9") && ParseKitVersion("0.12") && *ParseKitVersion("0.9") < *ParseKitVersion("0.12"));
  CHECK(*ParseKitVersion("0.12") < *ParseKitVersion("1.0") && *ParseKitVersion("1.0") > *ParseKitVersion("0.99"));
  CHECK(*ParseKitVersion("0.10") > *ParseKitVersion("0.9"));  // numbers, not text
  // A dev version counts as the release it leads to.
  CHECK(ParseKitVersion("0.12-dev.20261022.1") && *ParseKitVersion("0.12-dev.20261022.1") == *ParseKitVersion("0.12"));
  CHECK(*ParseKitVersion("0.12-dev.20261019.3") < *ParseKitVersion("0.13"));
  CHECK(FormatKitVersion(*ParseKitVersion("0.12-dev.20261022.1")) == "0.12");
  for (const char* garbage : {"", "kit-h", "r9", "hl2-fw1360-dev1", "0", "0.", ".12", "0.12.1", " 0.12", "0.12 ", "v0.12",
                              "00.12", "0.012", "-0.12", "+0.12", "0.12-dev", "0.12-dev.2026102.1", "0.12-dev.20261022",
                              "0.12-dev.20261022.0", "0.12-dev.20261022.x", "0.12-rc1", "12345.0", "0.1a"})
    CHECK(!ParseKitVersion(garbage));
  // The version a kit manifest's runtime names.
  CHECK(KitVersionTextFromArtifactRevision("f729534-kit-0.12") == std::optional<std::string>("0.12"));
  CHECK(KitVersionTextFromArtifactRevision("f729534-kit-0.12-dev.20261019.1") ==
        std::optional<std::string>("0.12-dev.20261019.1"));
  CHECK(KitVersionFromArtifactRevision("f729534-kit-0.12-dev.20261019.1") == ParseKitVersion("0.12"));
  for (const char* older : {"bceed2d-kit-h-r1", "70bfc92-kit-h-r5", "kit-0.12", "-kit-0.12", "f729534", "F729534-kit-0.12",
                            "f729534-kit-0.12x", ""})
    CHECK(!KitVersionFromArtifactRevision(older));
}

void Ids() {
  CHECK(IsValidCommunitySlug("120hz-left-stick-pointer") && IsValidCommunitySlug("a") && IsValidCommunitySlug("9"));
  CHECK(IsValidCommunitySlug(std::string(32, 'a')) && !IsValidCommunitySlug(std::string(33, 'a')));
  for (const char* bad : {"", "-a", "a-", "a--b", "A", "a_b", "a b", "a.b", "é", "a/b", "mouse-Preset"})
    CHECK(!IsValidCommunitySlug(bad));
  const auto darkwood = playden::GameRef::Steam(274520);
  const playden::GameRef tmnt{"imported", "tmnt-mutant-melee"};
  CHECK(CommunityRecipeIdFor(darkwood, "mouse") == "community-steam-274520-mouse");
  CHECK(CommunityRecipeIdFor(darkwood, "") == "community-steam-274520");
  CHECK(CommunityRecipeIdFor(tmnt, "arrow-keys") == "community-imported-tmnt-mutant-melee-arrow-keys");
  CHECK(CommunityRecipeIdFor({"epic", std::string(32, 'f')}, "a") == "community-epic-" + std::string(32, 'f') + "-a");
  CHECK(CommunityRecipeIdFor({"gog", "1207658924"}, "a") == "community-gog-1207658924-a");
  // Every Steam game's id fits the installed-game record's 64 bytes; the longest imported one doesn't.
  const auto longest_steam = CommunityRecipeIdFor(playden::GameRef::Steam(4294967295u), std::string(32, 'z'));
  CHECK(longest_steam.size() == 59 && longest_steam.size() <= kInstalledRecipeIdMaxBytes && IsCommunityRecipeId(longest_steam));
  const auto longest_imported = CommunityRecipeIdFor({"imported", std::string(48, 'a')}, std::string(32, 'z'));
  CHECK(longest_imported.size() == 100 && longest_imported.size() > kInstalledRecipeIdMaxBytes &&
        IsCommunityRecipeId(longest_imported));
  // A record and the title hold it as its alias: community-h-<first 24 hex of its SHA-256>; an id that fits stays.
  CHECK(InstalledRecipeId(longest_imported) == "community-h-f924d923ba7c01f94c0a2b65");
  CHECK(InstalledRecipeId(longest_steam) == longest_steam && InstalledRecipeId("steam-220-auto") == "steam-220-auto");
  CHECK(IsCommunityRecipeAlias(InstalledRecipeId(longest_imported)) && !IsCommunityRecipeId(InstalledRecipeId(longest_imported)));
  CHECK(IsInstalledCommunityRecipeId(InstalledRecipeId(longest_imported)) && IsInstalledCommunityRecipeId(longest_steam) &&
        !IsInstalledCommunityRecipeId(longest_imported));
  CHECK(!IsCommunityRecipeAlias("community-h-f924d923ba7c01f94c0a2b6") && !IsCommunityRecipeAlias("community-h-F924D923BA7C01F94C0A2B65") &&
        !IsCommunityRecipeAlias("community-h-f924d923ba7c01f94c0a2b65\n"));
  // The slug comes only from comparing with an id made from the game; nothing splits an id.
  CHECK(CommunityRecipeSlug("community-steam-274520-120hz-left-stick-pointer", darkwood) == "120hz-left-stick-pointer");
  CHECK(CommunityRecipeSlug("community-imported-tmnt-mutant-melee-arrow-keys", tmnt) == "arrow-keys");
  // A name key with hyphens: the same id is another game's slug, so only the game decides.
  CHECK(CommunityRecipeSlug("community-imported-tmnt-mutant-melee-arrow-keys", playden::GameRef{"imported", "tmnt"}) ==
        "mutant-melee-arrow-keys");
  CHECK(!CommunityRecipeSlug("community-steam-274520-120hz-left-stick-pointer", playden::GameRef::Steam(27452)));
  CHECK(!CommunityRecipeSlug("community-steam-274520", darkwood));  // the export's form has no slug
  CHECK(!CommunityRecipeSlug("community-steam-274520-" + std::string(33, 'a'), darkwood));
  CHECK(!CommunityRecipeSlug("community-steam-274520-a\n", darkwood));
  CHECK(!CommunityRecipeSlug("community-steam-274520-a", playden::GameRef{}));
  CHECK(IsCommunityRecipeId("community-steam-274520"));  // but the export's form is a community recipe id
  for (const char* good : {"community-steam-274520-a", "community-epic-a-b", "community-gog-1-a", "community-imported-a",
                           "community-imported-tmnt-mutant-melee-arrow-keys"})
    CHECK(IsCommunityRecipeId(good));
  for (const char* bad : {"community-", "community-steam", "community-steam-", "community-local-a", "community-274520-a",
                          "community-steam-274520-", "community-steam-274520--a", "community-steam-274520-A",
                          "community-steam-274520-a\n", "community-Steam-1-a", "community-steam-274520-a-",
                          "steam-274520-auto", "community274520"})
    CHECK(!IsCommunityRecipeId(bad));
  CHECK(IsCommunityRecipeId("community-imported-" + std::string(109, 'a')));   // 128 bytes
  CHECK(!IsCommunityRecipeId("community-imported-" + std::string(110, 'a')));  // 129 bytes
}

CommunityRecipe Load(const std::string& path) {
  CommunityRecipe community;
  std::string reason;
  if (!ParseCommunityRecipe(Read(path), &community, &reason)) std::fprintf(stderr, "%s: %s\n", path.c_str(), reason.c_str());
  return community;
}

void Rules(const std::string& fixtures) {
  CommunityCheckOptions ci;
  const auto good = Load(fixtures + "/recipes/steam/274520/120hz-left-stick-pointer.json");
  CHECK(good.recipe.id == "community-steam-274520-120hz-left-stick-pointer" && good.title == "120 Hz, pointer on left stick");
  CHECK(good.game == playden::GameRef::Steam(274520) && good.game_name == "Darkwood" && good.made_on.empty());
  // Not bound to a game yet: the game it is applied to binds it.
  CHECK(good.recipe.app_id == 0 && good.recipe.game == playden::GameId{});
  CHECK(good.tested_kit == "0.12" && good.min_kit.empty() && good.contributors.empty());
  CHECK(CommunityRecipeRefusals(good, ci).empty());
  const auto flags = CommunityRecipeFlags(good);
  CHECK(Mentions(flags, "program: Darkwood.exe") && Mentions(flags, "arguments: -force-d3d11 {resolution}") &&
        Mentions(flags, "library overrides: d3d8,d3d9,d3d10core,d3d11,dxgi=n") && !Mentions(flags, "registry"));

  // Every rule in its own sentence.
  const auto refused = [&](const char* file, const char* sentence) {
    const auto community = Load(fixtures + "/hostile/" + file);
    const auto refusals = CommunityRecipeRefusals(community, ci);
    if (!Mentions(refusals, sentence)) {
      std::fprintf(stderr, "%s: expected a refusal with \"%s\", got:\n", file, sentence);
      for (const auto& refusal : refusals) std::fprintf(stderr, "  %s\n", refusal.c_str());
      ++failures;
    }
  };
  refused("program-outside-folder.json", "not inside the game folder");
  refused("program-absolute.json", "not inside the game folder");
  refused("program-drive-letter.json", "not inside the game folder");
  refused("working-folder-outside.json", "starts in ../.., which is not inside the game folder");
  refused("wildcard-override.json", "without a wildcard");
  refused("unknown-environment.json", "DXVK_CONFIG_FILE is not one Playden sets");
  refused("registry-other-program.json", "explorer.exe, which is not one of the recipe's programs");
  refused("locked.json", "locked must be empty");
  refused("export-no-slug-no-title.json", "has no slug");
  // The title is optional; the export is still refused for its missing slug.
  CHECK(!Mentions(CommunityRecipeRefusals(Load(fixtures + "/hostile/export-no-slug-no-title.json"), ci), "community.title"));
  refused("kit-not-a-version.json", "tested_kit is not a kit version");
  refused("kit-not-a-version.json", "min_kit is not a kit version");
  refused("env-vkd3d-log-file.json", "VKD3D_LOG_FILE is not one Playden sets");
  refused("env-vkd3d-shader-cache-path.json", "VKD3D_SHADER_CACHE_PATH is not one Playden sets");
  refused("env-large-address-aware.json", "share the Address space setting instead");
  refused("env-nsiproxy-disable.json", "share the Network adapters setting instead");
  refused("program-system-msiexec.json", "msiexec.exe, a Windows or Wine system program");
  refused("program-dots-and-spaces.json", "only dots and spaces");
  refused("program-folder-trailing-dot.json", "ending in a dot or space");
  refused("working-folder-dots.json", "starts in ..., with a folder name of only dots and spaces");
  refused("text-title-bidi.json", "1-60 printable");
  refused("text-title-nbsp.json", "1-60 printable");
  refused("text-title-lone-surrogate.json", "1-60 printable");
  refused("text-title-bad-utf8.json", "1-60 printable");
  refused("text-notes-control.json", "community.notes must be valid UTF-8");
  refused("text-firmware-newline.json", "community.tested_firmware must be valid UTF-8");
  refused("defaults-extra-arguments.json", "defaults sets extra_arguments");
  refused("settings-only-defaults-arguments.json", "defaults sets arguments");
  refused("settings-only-defaults-graphics.json", "defaults sets graphics");
  refused("settings-only-program-system.json", "Bin/msiexec.exe is a Windows or Wine system program");
  // Settings-only recipes pass, with their program rows as defaults; only those are flagged.
  {
    const auto plain = Load(fixtures + "/accepted/settings-only.json");
    CHECK(IsSettingsOnlyRecipe(plain) && CommunityRecipeRefusals(plain, ci).empty() && CommunityRecipeFlags(plain).empty());
    const auto named = Load(fixtures + "/accepted/settings-only-program.json");
    const auto named_flags = CommunityRecipeFlags(named);
    CHECK(CommunityRecipeRefusals(named, ci).empty() && named_flags.size() == 2 &&
          named_flags[0] == "program: Bin/Darkwood.exe" && named_flags[1] == "arguments of Bin/Darkwood.exe: -windowed");
    // Filled from Playden's recipe, what Playden filled in is not listed.
    auto filled = plain;
    filled.recipe.launch_entries = {{"game", "Darkwood", "Darkwood.exe", "", "-x"}};
    filled.recipe.arguments = "-force-d3d11";
    filled.recipe.dll_overrides = "d3d11=n";
    CHECK(CommunityRecipeFlags(filled).empty());
    auto option = plain;
    option.recipe.defaults["launch_option"] = "dx11";
    option.recipe.defaults["program_folder"] = "bin";
    CHECK(CommunityRecipeRefusals(option, ci).empty() && Mentions(CommunityRecipeFlags(option), "starts Playden's launch option dx11") &&
          Mentions(CommunityRecipeFlags(option), "Playden's program starts in bin"));
    // Format 1 still refuses those defaults: its program rows are its own fields.
    auto format_1 = good;
    format_1.recipe.defaults["program"] = "Bin/Other.exe";
    CHECK(Mentions(CommunityRecipeRefusals(format_1, ci), "defaults sets program"));
    auto escaping = plain;
    escaping.recipe.defaults["program_folder"] = "bin. ";
    CHECK(Mentions(CommunityRecipeRefusals(escaping, ci), "defaults program_folder bin. "));
  }
  // The fake entry's AppDefaults key is still listed for review.
  CHECK(Mentions(CommunityRecipeFlags(Load(fixtures + "/hostile/program-system-msiexec.json")),
                 "registry: Software\\Wine\\AppDefaults\\msiexec.exe"));
  // A recipe shared without a name has no author: it shows as "from the community", in CI and on the console.
  CHECK(CommunityRecipeRefusals(Load(fixtures + "/accepted/no-author.json"), ci).empty());
  // An imported game's recipe under its name key, and one shared from an imported copy under its Steam app ID.
  const auto imported = Load(fixtures + "/recipes/imported/tmnt-mutant-melee/arrow-keys.json");
  CHECK(imported.game == (playden::GameRef{"imported", "tmnt-mutant-melee"}) && imported.game_name == "TMNT: Mutant Melee");
  CHECK(CommunityRecipeRefusals(imported, ci).empty());
  const auto made_on = Load(fixtures + "/accepted/made-on-imported.json");
  CHECK(made_on.made_on == "imported" && CommunityRecipeRefusals(made_on, ci).empty());
  refused("game-name-line-break.json", "game.name must be 1-120 printable characters");
  refused("game-name-invisible.json", "game.name must be 1-120 printable characters");
  refused("made-on-same-store.json", "made_on is only for a copy from another store");
  // Ids and games the parser can't take at all: too long, for another game, a key with a new line, the old app_id.
  CommunityRecipe ignored;
  std::string reason;
  CHECK(!ParseCommunityRecipe(Read(fixtures + "/hostile/id-129-bytes.json"), &ignored, &reason) &&
        reason.find("128 bytes") != std::string::npos);
  CHECK(!ParseCommunityRecipe(Read(fixtures + "/hostile/id-other-game.json"), &ignored, &reason) &&
        reason.find("recipe_id") != std::string::npos);
  CHECK(!ParseCommunityRecipe(Read(fixtures + "/hostile/game-key-newline.json"), &ignored, &reason) &&
        reason.find("game.key") != std::string::npos);
  CHECK(!ParseCommunityRecipe(Read(fixtures + "/hostile/obsolete-app-id.json"), &ignored, &reason) &&
        reason.find("obsolete") != std::string::npos);
  for (const char* game : {R"("game": {"store": "local", "key": "274520", "name": "Darkwood"})",
                           R"("game": {"store": "steam", "key": 274520, "name": "Darkwood"})",
                           R"("game": {"store": "steam", "key": "274520"})",
                           R"("game": {"store": "steam", "key": "274520", "name": "Darkwood", "app_id": 1})",
                           R"("game": "274520", "store": "local")"}) {
    auto text = Read(fixtures + "/recipes/steam/274520/120hz-left-stick-pointer.json");
    const std::string own = R"("game": {"store": "steam", "key": "274520", "name": "Darkwood"})";
    text.replace(text.find(own), own.size(), game);
    CHECK(!ParseCommunityRecipe(text, &ignored, &reason));
  }

  // Built by hand: ids, titles and names the parser lets through.
  auto community = good;
  for (const char* name : {"msiexec", "EXPLORER.exe", "Rundll32.EXE", "cmd", "steam", "steamwebhelper.exe", "powershell", "reg",
                           "wineboot", "taskmgr.exe"}) {
    community.recipe.launch_entries[0].executable = std::string("bin/") + name + (std::string(name).find('.') == std::string::npos ? ".exe" : "");
    CHECK(Mentions(CommunityRecipeRefusals(community), "system program"));
  }
  for (const char* ok : {"bin/startup.exe", "regedit2.exe", "control-panel.exe", "Dar kwood.exe"}) {
    community.recipe.launch_entries[0].executable = ok;
    CHECK(!Mentions(CommunityRecipeRefusals(community), "system program"));
  }
  community = good;
  community.recipe.launch_entries[0].working_directory = "bin/x ";
  CHECK(Mentions(CommunityRecipeRefusals(community), "only dots and spaces or ending"));
  community.recipe.launch_entries[0].working_directory = "bin/.. ";
  CHECK(Mentions(CommunityRecipeRefusals(community), "only dots and spaces or ending"));
  community.recipe.launch_entries[0].working_directory = "bin/.hidden";
  CHECK(!Mentions(CommunityRecipeRefusals(community), "only dots and spaces or ending"));
  community = good;
  community.recipe.name = "Dark\nwood";
  CHECK(Mentions(CommunityRecipeRefusals(community), "the recipe's name must be valid UTF-8"));
  community = good;
  community.notes = "Line one.\n\tIndented.";
  CHECK(CommunityRecipeRefusals(community).empty());
  for (const char* bad : {"a\x1b" "[31mb", "a\xe2\x80\xae" "b", "a\xe2\x81\xa6" "b", "a\xed\xa0\x80" "b", "bad \xff", "a\xc2\x85z", "a\xe2\x80\x8f" "b"}) {
    community.notes = bad;
    CHECK(Mentions(CommunityRecipeRefusals(community), "community.notes must be valid UTF-8"));
  }
  community = good;
  for (const char* bad : {"\xc2\xa0x", "x\xc2\xa0", "x\xe3\x80\x80", "\xe2\x80\x83x", "x\xe2\x80\x8b", "x\xe2\x80\xaa"}) {
    community.author = bad;
    CHECK(Mentions(CommunityRecipeRefusals(community), "community.author must be 1-64 printable"));
    community.author = "TodorPenchev";
    community.contributors = {bad};
    CHECK(Mentions(CommunityRecipeRefusals(community), "community.contributors must be 1-64 printable"));
    community.contributors.clear();
    community.title = bad;
    CHECK(Mentions(CommunityRecipeRefusals(community), "1-60 printable"));
    community.title = good.title;
  }
  for (auto field : {&CommunityRecipe::status, &CommunityRecipe::tested_firmware, &CommunityRecipe::tested_date}) {
    community = good;
    community.*field = "bad\x01";
    CHECK(Mentions(CommunityRecipeRefusals(community), "must be valid UTF-8"));
  }
  community = good;
  community.recipe.id = "community-steam-274520-" + std::string(48, 'a');
  CHECK(Mentions(CommunityRecipeRefusals(community), "must be community-steam-274520-<slug>: a slug of 1-32"));
  community.recipe.id = "community-steam-274520-" + std::string(106, 'a');
  CHECK(Mentions(CommunityRecipeRefusals(community), "longer than 128 bytes"));
  community = good;
  community.game_name = "\u3000";
  CHECK(Mentions(CommunityRecipeRefusals(community), "game.name must be 1-120"));
  community.game_name = std::string(121, 'a');
  CHECK(Mentions(CommunityRecipeRefusals(community), "game.name must be 1-120"));
  community.game_name = "\uff26\uff35\uff2c\uff2c\u3000\uff37\uff29\uff24\uff34\uff28";
  CHECK(CommunityRecipeRefusals(community).empty());
  community = good;
  community.title = std::string(60, 'x');
  CHECK(CommunityRecipeRefusals(community).empty());
  community.title = std::string(61, 'x');
  CHECK(Mentions(CommunityRecipeRefusals(community), "1-60 printable"));
  community.title = "Pointeur à gauche, 120 Hz";  // accents count as one character each
  CHECK(CommunityRecipeRefusals(community).empty());
  for (const char* bad : {"Tab\there", " leading", "trailing ", "bad \xff utf-8", "line\nbreak"}) {
    community.title = bad;
    CHECK(Mentions(CommunityRecipeRefusals(community), "1-60 printable"));
  }
  community = good;
  community.contributors = {"Ana", "Ana"};
  CHECK(Mentions(CommunityRecipeRefusals(community), "twice"));
  community.contributors = {"TodorPenchev"};
  CHECK(Mentions(CommunityRecipeRefusals(community), "twice"));
  community.contributors = {"Ana", "Bo"};
  CHECK(CommunityRecipeRefusals(community).empty());
  community.tested_date = "2026-13-01";
  CHECK(Mentions(CommunityRecipeRefusals(community), "YYYY-MM-DD"));
  community = good;
  community.recipe.defaults["program"] = "Darkwood.exe";
  CHECK(Mentions(CommunityRecipeRefusals(community), "defaults sets program"));
  community = good;
  community.recipe.defaults["extra_arguments"] = "-x";
  CHECK(Mentions(CommunityRecipeRefusals(community), "defaults sets extra_arguments"));
  community = good;
  community.recipe.launch_entries[0].executable = "readme.txt";
  community.executable = "readme.txt";
  CHECK(Mentions(CommunityRecipeRefusals(community), "not a Windows program"));
  community = good;
  community.recipe.defaults["library_overrides"] = "dinput8=n;xinput*=n,b";
  CHECK(Mentions(CommunityRecipeRefusals(community), "names xinput*"));
  community = good;
  community.recipe.defaults["environment"] = "VKD3D_CONFIG=dxr;PW_MEDIA_DISABLE=1;MESA_GL_VERSION_OVERRIDE=4.5";
  CHECK(Mentions(CommunityRecipeRefusals(community), "MESA_GL_VERSION_OVERRIDE") &&
        CommunityRecipeRefusals(community).size() == 1);
  // Online can be shared; the sign-in and invites stay each player's own; the emulator's network value can be shared.
  community = good;
  community.recipe.defaults["steam_features"] = "online";
  CHECK(CommunityRecipeRefusals(community).empty() && CommunityRecipeFeature(community) == "online-steam");
  community = good;
  community.recipe.defaults["steam_sign_in"] = "kept";
  CHECK(Mentions(CommunityRecipeRefusals(community), "defaults sets steam_sign_in"));
  community = good;
  community.recipe.defaults["game_invites"] = "join";
  CHECK(Mentions(CommunityRecipeRefusals(community), "defaults sets game_invites") &&
        CommunityRecipeFeature(community) == "online-steam");
  community = good;
  community.recipe.defaults["steam_features"] = "emulate";
  community.recipe.defaults["steam_network"] = "offline";
  CHECK(CommunityRecipeRefusals(community).empty());
  // Steam network is offered only to players with online-steam in features.txt.
  CHECK(CommunityRecipeFeature(community) == "online-steam" && CommunityRecipeFeature(good).empty());
  // A program's AppDefaults key is its own; letter case doesn't matter.
  community = good;
  community.recipe.registry = {{"Software\\Wine\\AppDefaults\\DARKWOOD.EXE\\Direct3D", "VideoMemorySize", "\"4096\""}};
  CHECK(CommunityRecipeRefusals(community).empty() && Mentions(CommunityRecipeFlags(community), "registry: "));

  // Only the variables that take no path; the player's own row and the profile still take all of them.
  CHECK(!IsPlaydenEnvironmentVariable("VKD3D_LOG_FILE") && !IsPlaydenEnvironmentVariable("VKD3D_SHADER_CACHE_PATH") &&
        IsPlaydenEnvironmentVariable("VKD3D_CONFIG") && IsPlaydenEnvironmentVariable("PW_MEDIA_DISABLE"));
  CHECK(IsBottleEnvironmentEntry("VKD3D_LOG_FILE", "Z:\\x.log"));
  // A launch option naming a later program is allowed and flagged.
  const auto second = Load(fixtures + "/flagged/launch-option-second.json");
  CHECK(CommunityRecipeRefusals(second, ci).empty() && Mentions(CommunityRecipeFlags(second), "starts the program config"));

  // A publisher key is allowed and flagged for review.
  const auto flagged = Load(fixtures + "/flagged/publisher-key.json");
  CHECK(CommunityRecipeRefusals(flagged, ci).empty());
  const auto flagged_items = CommunityRecipeFlags(flagged);
  CHECK(Mentions(flagged_items, "publisher registry key, needs review: Software\\Acid Wizard Studio\\Darkwood\\Language"));
  CHECK(Mentions(flagged_items, "registry: Software\\Wine\\AppDefaults\\Darkwood.exe\\Direct3D\\VideoMemorySize"));
  CHECK(Mentions(flagged_items, "environment: PW_MEDIA_DISABLE=1;WINE_CPU_TOPOLOGY=4:0,1,2,3"));
}

void Fits() {
  auto community = CommunityRecipe{};
  community.recipe.id = "community-steam-274520-a";
  VerifiedKitView kit;
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")).empty());
  CHECK(CommunityRecipeFits(community, kit, std::nullopt).empty());  // no min_kit: any kit
  community.min_kit = "0.12";
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")).empty());
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12-dev.20261019.1")).empty());
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("1.0")).empty());
  community.min_kit = "0.13";
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")) == "The recipe needs runtime kit version 0.13 or newer.");
  CHECK(CommunityRecipeFits(community, kit, std::nullopt) == "The recipe needs runtime kit version 0.13 or newer.");
  community.min_kit = "0.13-dev.20261101.2";
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.13")).empty());
  community.min_kit.clear();
  community.recipe.requirements = {{"audio", "ps5"}};
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")) == "The runtime kit lacks what this recipe needs (PS5 audio).");
  kit.template_audio_ps5 = true;
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")).empty());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: community_rules_test <tests/fixtures/community-recipes>\n");
    return 2;
  }
  KitVersions();
  Ids();
  Rules(argv[1]);
  Fits();
  if (failures) {
    std::fprintf(stderr, "%d community rules checks failed\n", failures);
    return 1;
  }
  std::puts("community rules tests passed");
  return 0;
}
