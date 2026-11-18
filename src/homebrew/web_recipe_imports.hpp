#pragma once
// web_recipe_imports.hpp - Recipe files a paired browser sends over web access (POST /api/files/recipe). The server
// worker that received one checks it (community::PrepareImportedRecipe) and keeps it in the community recipe store;
// the main thread (Controller) then makes the game use it and answers that worker, which waits for the answer.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/local/library_source.h"
#include "web/files_api.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace playden::homebrew {

struct CommunityKeepSet;

class WebRecipeImports {
 public:
  // Writes one recipe object into the community recipe store; false with a reason in *error.
  using Keeper = std::function<bool(const std::string& bytes, std::string* error)>;

  // wait: how long a worker waits for the main thread.
  explicit WebRecipeImports(std::chrono::milliseconds wait = std::chrono::seconds(10));

  // Where recipes are kept (empty: imports answer 503), set once the store's owner exists; keep: the revisions its
  // garbage collection keeps, which a recipe joins before it is written (null: nothing is collected).
  void SetKeeper(Keeper keeper, std::shared_ptr<CommunityKeepSet> keep = nullptr);
  // Whether an experimental feature (its features.txt line) is on: a recipe that needs one that is off is refused, as
  // the catalog leaves it out (runtime::CommunityRecipeFeature). Empty: every such recipe is refused.
  void SetFeatureCheck(std::function<bool(const std::string& feature)> enabled);

  // Server worker: checks the file and keeps it, then waits for the main thread to use it for the game.
  web::RecipeImportAnswer Import(const std::string& game, std::string file);

  // What Keep did with a file: the kept recipe's sha256, or why not (an HTTP status and a sentence for the player).
  struct Kept {
    std::string revision;  // empty when refused
    web::RecipeImportAnswer refusal;
  };
  // Any worker: checks the file and keeps it as Import does, without waiting for a game to use it. The revision is kept
  // until Playden closes (Revisions), so the main thread may use it later (the recipe.json in an imported game's folder).
  Kept Keep(std::string file);

  struct Pending {
    std::uint64_t ticket = 0;
    std::string game;      // the FileGame id the browser named
    std::string revision;  // the kept recipe's sha256
  };
  // Main thread: the recipes waiting to be used, each answered once with Answer.
  std::vector<Pending> Take();
  void Answer(std::uint64_t ticket, web::RecipeImportAnswer answer);
  // The revisions imported this session; the store keeps them.
  std::set<std::string> Revisions() const;
  // Answers every waiting worker and refuses new imports (Playden is closing). Safe to call twice.
  void Close();

 private:
  struct Ticket {
    std::string game;
    std::string revision;
    bool taken = false;
    bool answered = false;
    web::RecipeImportAnswer answer;
  };
  std::shared_ptr<CommunityKeepSet> keep_;
  std::chrono::milliseconds wait_;
  mutable std::mutex mutex_;
  std::condition_variable answered_;
  Keeper keeper_;
  std::function<bool(const std::string&)> feature_enabled_;
  bool closed_ = false;
  std::uint64_t next_ticket_ = 1;
  std::map<std::uint64_t, Ticket> tickets_;
  std::set<std::string> revisions_;
};

// The Local store's keep_folder_recipe: the recipe.json of a game being added, kept through `imports` (Keep).
std::function<stores::local::FolderRecipeKept(std::string bytes)> FolderRecipeKeeper(std::shared_ptr<WebRecipeImports> imports);

}  // namespace playden::homebrew
