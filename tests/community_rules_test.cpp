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
  CHECK(CommunityRecipeIdFor(274520, "mouse") == "community-274520-mouse");
  CHECK(CommunityRecipeIdFor(274520, "") == "community-274520");
  // 64 bytes is the limit; a 7-digit app ID and a 32-character slug make 50.
  const auto longest = CommunityRecipeIdFor(4294967295u, std::string(32, 'z'));
  CHECK(longest.size() == 53 && IsCommunityRecipeId(longest));
  CHECK(CommunityRecipeIdFor(1234567, std::string(32, 'a')).size() == 50);
  std::uint32_t app = 0;
  std::string slug;
  CHECK(SplitCommunityRecipeId("community-274520-120hz-left-stick-pointer", &app, &slug) && app == 274520 &&
        slug == "120hz-left-stick-pointer");
  CHECK(SplitCommunityRecipeId("community-1234567-" + std::string(32, 'a'), &app, &slug) && app == 1234567);
  CHECK(!SplitCommunityRecipeId("community-1234567-" + std::string(33, 'a'), &app, &slug));
  CHECK(!SplitCommunityRecipeId("community-274520", &app, &slug));  // the export's form has no slug
  CHECK(IsCommunityRecipeId("community-274520"));                   // but is a community recipe
  for (const char* bad : {"community-", "community-0-a", "community-0274520-a", "community-4294967296-a",
                          "community-274520-", "community-274520--a", "community-274520-A", "community-abc",
                          "community-274520-a-", "steam-274520-auto", "community274520"})
    CHECK(!IsCommunityRecipeId(bad));
  CHECK(!IsCommunityRecipeId("community-274520-" + std::string(48, 'a')));  // 65 bytes
}

CommunityRecipe Load(const std::string& path) {
  CommunityRecipe community;
  std::string reason;
  if (!ParseCommunityRecipe(Read(path), &community, &reason)) std::fprintf(stderr, "%s: %s\n", path.c_str(), reason.c_str());
  return community;
}

void Rules(const std::string& fixtures) {
  CommunityCheckOptions ci;
  const auto good = Load(fixtures + "/recipes/274520/120hz-left-stick-pointer.json");
  CHECK(good.recipe.id == "community-274520-120hz-left-stick-pointer" && good.title == "120 Hz, pointer on left stick");
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
  // The fake entry's AppDefaults key is still listed for review.
  CHECK(Mentions(CommunityRecipeFlags(Load(fixtures + "/hostile/program-system-msiexec.json")),
                 "registry: Software\\Wine\\AppDefaults\\msiexec.exe"));
  // A recipe shared without a name has no author: it shows as "from the community", in CI and on the console.
  CHECK(CommunityRecipeRefusals(Load(fixtures + "/accepted/no-author.json"), ci).empty());
  // Ids the parser can't take at all: too long, or for another app.
  CommunityRecipe ignored;
  std::string reason;
  CHECK(!ParseCommunityRecipe(Read(fixtures + "/hostile/id-65-bytes.json"), &ignored, &reason) &&
        reason.find("64 bytes") != std::string::npos);
  CHECK(!ParseCommunityRecipe(Read(fixtures + "/hostile/id-other-app.json"), &ignored, &reason) &&
        reason.find("recipe_id") != std::string::npos);

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
  community.recipe.id = "community-274520-" + std::string(48, 'a');
  CHECK(Mentions(CommunityRecipeRefusals(community), "longer than 64 bytes") &&
        Mentions(CommunityRecipeRefusals(community), "a slug of 1-32"));
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
  community.recipe.id = "community-274520-a";
  VerifiedKitView kit;
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")).empty());
  CHECK(CommunityRecipeFits(community, kit, std::nullopt).empty());  // no min_kit: any kit
  community.min_kit = "0.12";
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")).empty());
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12-dev.20261019.1")).empty());
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("1.0")).empty());
  community.min_kit = "0.13";
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")) == "The recipe needs kit 0.13.");
  CHECK(CommunityRecipeFits(community, kit, std::nullopt) == "The recipe needs kit 0.13.");
  community.min_kit = "0.13-dev.20261101.2";
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.13")).empty());
  community.min_kit.clear();
  community.recipe.requirements = {{"audio", "ps5"}};
  CHECK(CommunityRecipeFits(community, kit, ParseKitVersion("0.12")) == "The kit lacks what this recipe needs (PS5 audio).");
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
