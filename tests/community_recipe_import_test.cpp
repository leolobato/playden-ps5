// community_recipe_import_test.cpp - Recipe files a player imports over web access: made local, checked like a
// community recipe, and refused when the recipe service couldn't take them later.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/recipe_import.h"
#include "community/recipe_store.h"
#include "runtime/artifact_hash.h"
#include "runtime/community_rules.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace playden;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

std::string fixtures;

std::string Read(const std::string& relative) {
  std::string text;
  if (std::FILE* file = std::fopen((fixtures + "/" + relative).c_str(), "rb")) {
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, count);
    std::fclose(file);
  }
  if (text.empty()) {
    std::fprintf(stderr, "missing fixture %s\n", relative.c_str());
    ++failures;
  }
  return text;
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  if (at == std::string::npos) {
    std::fprintf(stderr, "fixture lacks %s\n", from.c_str());
    ++failures;
    return text;
  }
  return text.replace(at, from.size(), to);
}

// An "Export as recipe" file: no slug, no title. Imported, it gets a local id and a title, and passes the strict rules.
void ExportImports() {
  const auto file = Read("hostile/export-no-slug-no-title.json");
  std::string error;
  const auto imported = community::PrepareImportedRecipe(file, &error);
  CHECK(imported && error.empty());
  if (!imported) return std::fprintf(stderr, "refused: %s\n", error.c_str()), void();
  const auto digest = runtime::HashArtifactBytes(file);
  CHECK(imported->recipe.recipe.id == "community-steam-274520-imported-" + digest->substr(0, 16));
  CHECK(community::IsImportedRecipeId(imported->recipe.recipe.id));
  CHECK(imported->recipe.title == community::kImportedRecipeTitle);
  CHECK(imported->recipe.author == "TodorPenchev");
  CHECK(imported->recipe.game == GameRef::Steam(274520));
  CHECK(runtime::CommunityRecipeRefusals(imported->recipe).empty());
  // What Playden keeps reads back as the same recipe.
  runtime::CommunityRecipe again;
  std::string reason;
  CHECK(runtime::ParseCommunityRecipe(imported->bytes, &again, &reason) && again.recipe.id == imported->recipe.recipe.id);
  // The same file twice is the same recipe.
  const auto twice = community::PrepareImportedRecipe(file, &error);
  CHECK(twice && twice->bytes == imported->bytes);
}

// A published recipe imported from the repository still becomes local: a share must not update it as if it were.
void PublishedBecomesLocal() {
  const auto file = Read("accepted/no-author.json");
  std::string error;
  const auto imported = community::PrepareImportedRecipe(file, &error);
  CHECK(imported);
  if (!imported) return std::fprintf(stderr, "refused: %s\n", error.c_str()), void();
  CHECK(community::IsImportedRecipeId(imported->recipe.recipe.id));
  runtime::CommunityRecipe original;
  std::string reason;
  CHECK(runtime::ParseCommunityRecipe(file, &original, &reason));
  CHECK(original.recipe.id != imported->recipe.recipe.id && !community::IsImportedRecipeId(original.recipe.id));
  CHECK(!original.title.empty() && imported->recipe.title == original.title);
  // A settings-only recipe stays one.
  const auto settings_only = community::PrepareImportedRecipe(Read("accepted/settings-only.json"), &error);
  CHECK(settings_only && runtime::IsSettingsOnlyRecipe(settings_only->recipe));
}

void LocksDropped() {
  std::string error;
  const auto imported = community::PrepareImportedRecipe(Read("hostile/locked.json"), &error);
  CHECK(imported && imported->recipe.recipe.locked.empty());
}

void Refused() {
  std::string error;
  // The strict rules still apply.
  CHECK(!community::PrepareImportedRecipe(Read("hostile/program-absolute.json"), &error) && !error.empty());
  CHECK(!community::PrepareImportedRecipe(Read("hostile/env-vkd3d-log-file.json"), &error) && !error.empty());
  // Not a recipe.
  CHECK(!community::PrepareImportedRecipe("", &error) && !error.empty());
  CHECK(!community::PrepareImportedRecipe("{\"hello\":1}", &error) && !error.empty());
  CHECK(!community::PrepareImportedRecipe("not json", &error) && !error.empty());
  // Larger than the recipe service takes.
  const auto base = Read("hostile/export-no-slug-no-title.json");
  CHECK(!community::PrepareImportedRecipe(base + std::string(community::kMaxImportedRecipeBytes, ' '), &error) &&
        error.find("16 KB") != std::string::npos);
  // INI values and fields the recipe service refuses: the recipe couldn't be shared later.
  const auto ini = Replace(base, "\"locked\": [],", "\"locked\": [], \"ini\": {\"a.ini\": {\"S\": {\"k\": \"v\"}}},");
  CHECK(!community::PrepareImportedRecipe(ini, &error) && error.find("INI") != std::string::npos);
  const auto community_field = Replace(base, "\"status\": \"works\",", "\"status\": \"works\", \"surprise\": \"x\",");
  CHECK(!community::PrepareImportedRecipe(community_field, &error) && error.find("community.surprise") != std::string::npos);
}

void Ids() {
  CHECK(community::IsImportedRecipeId("community-steam-220-imported-0123456789abcdef"));
  CHECK(community::IsImportedRecipeId("community-imported-tmnt-imported-0123456789abcdef"));
  CHECK(!community::IsImportedRecipeId("community-steam-220-imported-0123456789ABCDEF"));
  CHECK(!community::IsImportedRecipeId("community-steam-220-imported"));
  CHECK(!community::IsImportedRecipeId("community-steam-220-imported-4k"));
  CHECK(!community::IsImportedRecipeId("community-steam-220-mouse"));
  CHECK(!community::IsImportedRecipeId("other-steam-220-imported-0123456789abcdef"));
}

// What the import keeps is an object the recipe store reads back by its revision.
void StoreKeepsIt() {
  const auto root = std::filesystem::temp_directory_path() / ("playden-recipe-import-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::string error;
  const auto imported = community::PrepareImportedRecipe(Read("hostile/export-no-slug-no-title.json"), &error);
  CHECK(imported);
  if (!imported) return;
  community::RecipeStore store(std::filesystem::weakly_canonical(root.parent_path()) / root.filename());
  CHECK(store.Prepare(&error));
  CHECK(store.WriteObjects({imported->bytes}, &error));
  const auto revision = runtime::HashArtifactBytes(imported->bytes);
  CHECK(revision && store.ReadObject(*revision) == imported->bytes);
  std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: community_recipe_import_test <fixtures/community-recipes>\n");
    return 2;
  }
  fixtures = argv[1];
  ExportImports();
  PublishedBecomesLocal();
  LocksDropped();
  Refused();
  Ids();
  StoreKeepsIt();
  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
