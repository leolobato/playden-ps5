// recipe_import.cpp - A recipe file a player brings in themselves, checked and made local.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/recipe_import.h"

#include "community/api_client.h"
#include "runtime/artifact_hash.h"
#include "runtime/community_rules.h"
#include "steam/backend/json.h"

namespace playden::community {
namespace {

constexpr std::size_t kImportedHashDigits = 16;

bool LowerHex(std::string_view text) {
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// What the recipe service would refuse in the file's own fields; empty when it takes them.
std::string FieldProblem(std::string_view file) {
  const auto doc = steam::json::Parse(file);
  if (!doc || !doc->is_object()) return "This file isn't a Playden recipe.";
  // The recipe service doesn't take INI values yet: a recipe with them couldn't be shared later.
  if (doc->Find("ini")) return "Playden can't import a recipe that changes the game's INI files yet.";
  for (const auto& [key, value] : doc->object())
    if (!AcceptedRecipeKeys().count(key)) return "The recipe has a field Playden can't share: " + key + ".";
  if (const auto* meta = doc->Find("community"); meta && meta->is_object())
    for (const auto& [key, value] : meta->object())
      if (!AcceptedCommunityKeys().count(key)) return "The recipe has a field Playden can't share: community." + key + ".";
  return {};
}

}  // namespace

std::optional<ImportedRecipe> PrepareImportedRecipe(std::string_view file, std::string* error) {
  const auto refuse = [error](std::string reason) -> std::optional<ImportedRecipe> {
    if (error) *error = std::move(reason);
    return std::nullopt;
  };
  if (error) error->clear();
  if (file.empty()) return refuse("The file is empty.");
  if (file.size() > kMaxImportedRecipeBytes) return refuse("The recipe is larger than 16 KB.");
  runtime::CommunityRecipe community;
  std::string reason;
  if (!runtime::ParseCommunityRecipe(file, &community, &reason)) return refuse("This file isn't a Playden recipe: " + reason);
  if (auto problem = FieldProblem(file); !problem.empty()) return refuse(std::move(problem));
  const auto digest = runtime::HashArtifactBytes(file);
  if (!digest || digest->size() < kImportedHashDigits) return refuse("Playden couldn't read the file.");
  // Always local: the file's own id (a published recipe's, or none) would make a share update a recipe that isn't it.
  community.recipe.id =
      runtime::CommunityRecipeIdFor(community.game, std::string(kImportedRecipeSlugPrefix) + digest->substr(0, kImportedHashDigits));
  community.recipe.revision.clear();
  community.recipe.locked.clear();
  if (community.title.empty()) community.title = kImportedRecipeTitle;
  ImportedRecipe imported;
  imported.bytes = runtime::SerializeCommunityRecipe(community);
  if (imported.bytes.size() > kMaxImportedRecipeBytes) return refuse("The recipe is larger than 16 KB.");
  // The file as Playden keeps it, read back the way the console reads every community recipe.
  if (!runtime::ParseCommunityRecipe(imported.bytes, &imported.recipe, &reason))
    return refuse("This file isn't a Playden recipe: " + reason);
  if (const auto refusals = runtime::CommunityRecipeRefusals(imported.recipe); !refusals.empty())
    return refuse("Playden refuses this recipe: " + refusals.front());
  if (auto problem = FieldProblem(imported.bytes); !problem.empty()) return refuse(std::move(problem));
  return imported;
}

bool IsImportedRecipeId(std::string_view id) {
  const std::string marker = std::string("-") + kImportedRecipeSlugPrefix;
  if (id.rfind("community-", 0) != 0 || id.size() < marker.size() + kImportedHashDigits) return false;
  const auto tail = id.substr(id.size() - marker.size() - kImportedHashDigits);
  return tail.substr(0, marker.size()) == marker && LowerHex(tail.substr(marker.size()));
}

}  // namespace playden::community
