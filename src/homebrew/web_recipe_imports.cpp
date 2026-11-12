// web_recipe_imports.cpp - Recipe files a paired browser sends, from the server worker to the main thread.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/web_recipe_imports.hpp"

#include "community/recipe_import.h"
#include "homebrew/controller.hpp"
#include "runtime/artifact_hash.h"
#include "runtime/community_rules.h"

namespace playden::homebrew {

WebRecipeImports::WebRecipeImports(std::chrono::milliseconds wait) : wait_(wait) {}

void WebRecipeImports::SetKeeper(Keeper keeper, std::shared_ptr<CommunityKeepSet> keep) {
  std::lock_guard<std::mutex> lock(mutex_);
  keeper_ = std::move(keeper);
  keep_ = std::move(keep);
}

void WebRecipeImports::SetFeatureCheck(std::function<bool(const std::string& feature)> enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  feature_enabled_ = std::move(enabled);
}

web::RecipeImportAnswer WebRecipeImports::Import(const std::string& game, std::string file) {
  std::string error;
  const auto imported = community::PrepareImportedRecipe(file, &error);
  if (!imported) return {422, error};
  const auto revision = runtime::HashArtifactBytes(imported->bytes);
  if (!revision) return {500, "Playden couldn't read the recipe."};
  const auto feature = runtime::CommunityRecipeFeature(imported->recipe);
  Keeper keeper;
  std::shared_ptr<CommunityKeepSet> keep;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return {503, "Playden is closing."};
    if (!feature.empty() && !(feature_enabled_ && feature_enabled_(feature)))
      return {422, "This recipe needs an experimental feature that is off on this console (" + feature + ")."};
    keeper = keeper_;
    keep = keep_;
    // Kept before it is written, so a refresh's garbage collection never removes it before the game uses it.
    revisions_.insert(*revision);
  }
  if (!keeper) return {503, "Playden can't take recipes now."};
  if (keep) {
    std::lock_guard<std::mutex> lock(keep->mutex);
    if (keep->revisions) keep->revisions->insert(*revision);
  }
  if (!keeper(imported->bytes, &error)) return {500, "Playden couldn't keep the recipe (" + error + ")."};

  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) return {503, "Playden is closing."};
  const auto ticket = next_ticket_++;
  tickets_[ticket] = Ticket{game, *revision};
  const bool done = answered_.wait_for(lock, wait_, [&] { return tickets_.at(ticket).answered; });
  const auto found = tickets_.find(ticket);
  auto result = found->second;
  tickets_.erase(found);
  if (done) return result.answer;
  // Not answered: still waiting means the main thread never looked, so it never will; taken means it is at work.
  if (!result.taken) return {503, "Playden didn't answer. Is it busy on the PS5? Try again in a moment."};
  return {504, "Playden is still working on it. Look at the PS5."};
}

std::vector<WebRecipeImports::Pending> WebRecipeImports::Take() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Pending> pending;
  for (auto& [ticket, entry] : tickets_) {
    if (entry.taken) continue;
    entry.taken = true;
    pending.push_back({ticket, entry.game, entry.revision});
  }
  return pending;
}

void WebRecipeImports::Answer(std::uint64_t ticket, web::RecipeImportAnswer answer) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tickets_.find(ticket);
    if (found == tickets_.end()) return;  // the worker gave up waiting
    found->second.answered = true;
    found->second.answer = std::move(answer);
  }
  answered_.notify_all();
}

std::set<std::string> WebRecipeImports::Revisions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return revisions_;
}

void WebRecipeImports::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    keeper_ = nullptr;
    keep_ = nullptr;
    for (auto& [ticket, entry] : tickets_) {
      if (entry.answered) continue;
      entry.answered = true;
      entry.answer = {503, "Playden is closing."};
    }
  }
  answered_.notify_all();
}

}  // namespace playden::homebrew
