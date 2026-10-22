// community_recipe_check.cpp - Checks community recipe files the way Playden reads them, for the
// recipes repository's CI (docs/community-recipes.md).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   community_recipe_check [--json] <file>...
//
// For each file: Playden's parser, the strict community rules (CommunityRecipeRefusals, author
// required), the file name (recipes/<app id>/<slug>.json, when the path has a recipes folder),
// the effective settings on a kit with every capability, and the full prospero-win profile. It
// prints "ok <path> <revision> <recipe_id>" or a "refused <path>: <reason>" line per refusal,
// the command the game starts with, and a "flag <path>: <item>" line for each thing that changes
// how the game starts. --json prints one JSON object instead. Exit status 1 when any file is
// refused, 2 on a usage error.
#include "runtime/artifact_hash.h"
#include "runtime/community_rules.h"
#include "runtime/game_settings.h"
#include "runtime/prospero_profile.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace rt = playden::runtime;

namespace {

constexpr std::size_t kMaxRecipeBytes = 16 * 1024;

struct Result {
  std::string path;
  std::string revision;
  rt::CommunityRecipe community;
  bool parsed = false;
  std::string slug;
  std::string command;
  std::vector<std::string> refusals;
  std::vector<std::string> flags;
};

bool ReadFile(const std::string& path, std::string* bytes, std::string* why) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    *why = std::string("cannot open the file: ") + std::strerror(errno);
    return false;
  }
  char buffer[4096];
  std::size_t count = 0;
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    bytes->append(buffer, count);
    if (bytes->size() > kMaxRecipeBytes) break;
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    *why = "cannot read the file";
    return false;
  }
  if (bytes->size() > kMaxRecipeBytes) {
    *why = "the file is larger than 16 KB";
    return false;
  }
  return true;
}

std::vector<std::string> PathParts(const std::string& path) {
  std::vector<std::string> parts;
  for (std::size_t start = 0; start <= path.size();) {
    const auto end = std::min(path.find('/', start), path.size());
    if (end > start) parts.push_back(path.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

// A path with a recipes folder must end recipes/<app id>/<slug>.json, naming the recipe's own id.
void CheckLayout(Result* result) {
  const auto parts = PathParts(result->path);
  bool in_recipes = false;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) in_recipes |= parts[index] == "recipes";
  if (!in_recipes) return;
  const auto& id = result->community.recipe.id;
  const auto expected = "recipes/" + std::to_string(result->community.recipe.app_id) + "/" +
                        (result->slug.empty() ? std::string("<slug>") : result->slug) + ".json";
  const bool matches = parts.size() >= 3 && parts[parts.size() - 3] == "recipes" &&
                       parts[parts.size() - 2] == std::to_string(result->community.recipe.app_id) &&
                       !result->slug.empty() && parts.back() == result->slug + ".json";
  if (!matches) result->refusals.push_back("The file must be " + expected + " for recipe_id " + id + ".");
}

void Check(Result* result) {
  std::string bytes, why;
  if (!ReadFile(result->path, &bytes, &why)) {
    result->refusals.push_back(why);
    return;
  }
  if (const auto digest = rt::HashArtifactBytes(bytes)) result->revision = *digest;
  if (!rt::ParseCommunityRecipe(bytes, &result->community, &why)) {
    result->refusals.push_back("Playden can't read it: " + why);
    return;
  }
  result->parsed = true;
  auto& community = result->community;
  rt::SplitCommunityRecipeId(community.recipe.id, nullptr, &result->slug);
  rt::CommunityCheckOptions options;
  options.require_author = true;
  result->refusals = rt::CommunityRecipeRefusals(community, options);
  CheckLayout(result);
  result->flags = rt::CommunityRecipeFlags(community);

  // What a console with every capability would start, and the profile it would write.
  rt::KitCapabilities kit;
  kit.dxvk_config = kit.dxvk_present = kit.environment = kit.registry = kit.input_tuning = kit.input_wheel = true;
  kit.audio_volume = kit.shared_sections = kit.title_any_id = kit.game_root_unowned = kit.bottle_profile_shared = true;
  kit.opengl_zink = kit.wine_mono = kit.vkd3d_proton = kit.display_120hz = true;
  rt::EffectiveSettings settings;
  rt::SettingsError error;
  if (!rt::BuildEffectiveSettings(community.recipe, nullptr, {}, kit, &settings, &error)) {
    result->refusals.push_back("Its settings don't apply: " + error.setting_id + ": " + error.message + ".");
    return;
  }
  if (settings.launch) {
    result->command = settings.launch->executable;
    if (!settings.arguments.empty()) result->command += " " + settings.arguments;
  }
  const auto app = std::to_string(community.recipe.app_id);
  const rt::ProfileTarget target{rt::ProsperoProfileId(community.recipe.app_id), "/data/playden-games/" + app};
  rt::ProsperoProfile profile;
  if (!rt::RenderProsperoProfile(community.recipe, settings, target, &profile, &why))
    result->refusals.push_back("Its prospero-win profile is refused: " + why + ".");
}

// The text as valid UTF-8 with no control characters: each invalid byte becomes U+FFFD and each control character an
// escape, so neither the JSON nor the log lines can be forged or broken by a hostile file.
std::string Cleaned(const std::string& text, bool json) {
  std::string out;
  for (std::size_t index = 0; index < text.size();) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 0;
    std::uint32_t point = length == 1 ? lead : length == 2 ? lead & 0x1f : length == 3 ? lead & 0x0f : lead & 0x07;
    bool valid = length != 0 && index + length <= text.size();
    for (std::size_t next = 1; valid && next < length; ++next) {
      const auto byte = static_cast<unsigned char>(text[index + next]);
      valid = (byte & 0xc0) == 0x80;
      point = (point << 6) | (byte & 0x3f);
    }
    static constexpr std::uint32_t kShortest[] = {0, 0, 0x80, 0x800, 0x10000};
    if (valid && (point < kShortest[length] || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))) valid = false;
    if (!valid) {
      out += json ? "\\ufffd" : "\xef\xbf\xbd";
      ++index;
      continue;
    }
    if (point < 0x20 || (point >= 0x7f && point <= 0x9f) || point == 0x2028 || point == 0x2029) {
      char escaped[16];
      std::snprintf(escaped, sizeof(escaped), json ? "\\u%04x" : "\\u{%x}", static_cast<unsigned>(point));
      out += escaped;
    } else if (json && (point == '"' || point == '\\')) {
      out += '\\';
      out += static_cast<char>(point);
    } else {
      out.append(text, index, length);
    }
    index += length;
  }
  return out;
}

std::string Quoted(const std::string& text) { return "\"" + Cleaned(text, true) + "\""; }

std::string List(const std::vector<std::string>& items) {
  std::string out = "[";
  for (std::size_t index = 0; index < items.size(); ++index) out += (index ? ", " : "") + Quoted(items[index]);
  return out + "]";
}

std::string Json(const std::vector<Result>& results, bool ok) {
  std::string out = "{\n  \"ok\": " + std::string(ok ? "true" : "false") + ",\n  \"files\": [";
  for (std::size_t index = 0; index < results.size(); ++index) {
    const auto& result = results[index];
    const auto& community = result.community;
    out += std::string(index ? "," : "") + "\n    {\"path\": " + Quoted(result.path) +
           ", \"revision\": " + Quoted(result.revision) + ", \"recipe_id\": " + Quoted(community.recipe.id) +
           ", \"app_id\": " + std::to_string(community.recipe.app_id) + ", \"slug\": " + Quoted(result.slug) +
           ", \"title\": " + Quoted(community.title) + ", \"status\": " + Quoted(community.status) +
           ", \"author\": " + Quoted(community.author) + ", \"contributors\": " + List(community.contributors) +
           ", \"tested_kit\": " + Quoted(community.tested_kit) + ", \"min_kit\": " + Quoted(community.min_kit) +
           ", \"tested_firmware\": " + Quoted(community.tested_firmware) +
           ", \"tested_date\": " + Quoted(community.tested_date) +
           ", \"ok\": " + (result.refusals.empty() ? "true" : "false") + ", \"refusals\": " + List(result.refusals) +
           ", \"flags\": " + List(result.flags) + ", \"command\": " + Quoted(result.command) + "}";
  }
  return out + "\n  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool json = false;
  std::vector<std::string> paths;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--json") json = true;
    else if (argument == "--help" || argument == "-h" || (argument.size() > 1 && argument[0] == '-')) {
      std::fprintf(stderr, "usage: community_recipe_check [--json] <recipe.json>...\n");
      return argument == "--help" || argument == "-h" ? 0 : 2;
    } else paths.push_back(argument);
  }
  if (paths.empty()) {
    std::fprintf(stderr, "usage: community_recipe_check [--json] <recipe.json>...\n");
    return 2;
  }
  std::vector<Result> results;
  std::map<std::string, std::string> ids;  // recipe_id -> the first file with it
  bool ok = true;
  for (const auto& path : paths) {
    Result result;
    result.path = path;
    Check(&result);
    if (result.parsed) {
      const auto [found, added] = ids.emplace(result.community.recipe.id, path);
      if (!added) result.refusals.push_back("recipe_id " + result.community.recipe.id + " is also in " + found->second + ".");
    }
    ok = ok && result.refusals.empty();
    results.push_back(std::move(result));
  }
  if (json) {
    std::fputs(Json(results, ok).c_str(), stdout);
  } else {
    const auto clean = [](const std::string& text) { return Cleaned(text, false); };
    for (const auto& result : results) {
      const auto path = clean(result.path);
      if (result.refusals.empty())
        std::printf("ok %s %s %s\n", path.c_str(), result.revision.c_str(), clean(result.community.recipe.id).c_str());
      for (const auto& refusal : result.refusals) std::printf("refused %s: %s\n", path.c_str(), clean(refusal).c_str());
      if (!result.command.empty()) std::printf("command %s: %s\n", path.c_str(), clean(result.command).c_str());
      for (const auto& flag : result.flags) std::printf("flag %s: %s\n", path.c_str(), clean(flag).c_str());
    }
  }
  return ok ? 0 : 1;
}
