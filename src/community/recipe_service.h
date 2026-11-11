#pragma once
// recipe_service.h - Community recipes for the chooser and the runtime: finds the latest release of
// the recipes repository, downloads its index.json and recipes.zip into memory, checks every file
// (hash, path, ParseCommunityRecipe and the community rules), keeps the good ones in a RecipeStore
// and offers a snapshot of them. Vote totals come from the API's summary.json. One background
// refresh at a time; the copy on disk loads at start and stays usable offline.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "community/endpoints.h"
#include "community/recipe_release.h"
#include "community/recipe_store.h"
#include "runtime/game_settings.h"
#include "update/asset_download.h"

namespace playden::community {

struct RecipeServiceConfig {
  // <user data>/community-recipes; its parent must exist. Console: /data/playden/user/community-recipes.
  std::string store_root;
  // Where to fetch from. Builds without PLAYDEN_DEV_ENDPOINTS ignore this and use ReleaseEndpoints().
  Endpoints endpoints = ReleaseEndpoints();
  update::Fetch fetch;  // one HTTP exchange (HttpClient::Perform); never called on the caller's thread
  std::function<void(const std::string& line)> log;
  std::function<std::int64_t()> now;  // seconds since the epoch; default time()
  // Every revision something still uses (saved choices, installed games' records, unfinished
  // settings transactions): full revisions or 32-hex prefixes. Called on the worker after a
  // successful refresh, before garbage collection; without it, or when it returns nothing (what is in use is not
  // known yet), nothing is collected.
  std::function<std::optional<std::set<std::string>>()> referenced;
  std::string user_agent = "Playden";
  std::size_t worker_stack_bytes = 1 << 20;
  // Whether an experimental feature (a features.txt line, runtime::CommunityRecipeFeature) is on. A recipe that needs
  // one that is off is not offered; a game that already uses it keeps it. Nothing: every feature is off.
  std::function<bool(const std::string& feature)> feature_enabled;
  // Test hook: stops a refresh's write of that step (RecipeStore::before_write).
  std::function<bool(const std::string& step)> before_write;
};

struct RecipeSummary {
  std::string id;  // community-<store>-<key>-<slug>
  GameRef game;    // the game it is for
  std::string made_on;  // the store of the copy it was made with, when not game.store
  std::string title, status, author;
  std::vector<std::string> contributors;
  std::string tested_kit, min_kit, tested_firmware, tested_date, notes;
  std::string revision;             // sha256 of the file (64 hex)
  std::vector<std::string> flags;   // CommunityRecipeFlags: what changes how the game starts
  std::optional<RecipeVotes> votes;  // nothing when the API had no totals for this revision
};

struct RecipeSnapshot {
  std::uint64_t revision = 0;  // changes whenever anything below changes
  std::map<GameRef, std::vector<RecipeSummary>> games;  // index order per game
  std::map<GameRef, std::string> game_names;            // the index's name of each game that has one
  std::string release_tag;     // empty: no copy yet
  std::int64_t fetched_at = 0;  // the copy's last successful refresh (unix seconds)
  // When the API made the vote totals (summary.json's generated_at, unix seconds): votes this console cast later are
  // not in them yet. 0: unknown.
  std::int64_t votes_generated_at = 0;
  // The compatibility list from the same summary.json. Nothing: not known (no summary yet, or one without a list).
  std::optional<CompatList> compat;
  bool refreshing = false;
  std::string last_error;      // the last refresh's failure, one short sentence; empty after success
  const std::vector<RecipeSummary>* For(const GameRef& game) const;
};

class CommunityRecipeService {
 public:
  static constexpr std::int64_t kRefreshInterval = 24 * 60 * 60;
  // A non-forced refresh after a failed one waits this long.
  static constexpr std::int64_t kRetryInterval = 60 * 60;

  explicit CommunityRecipeService(RecipeServiceConfig config);
  ~CommunityRecipeService();
  CommunityRecipeService(const CommunityRecipeService&) = delete;
  CommunityRecipeService& operator=(const CommunityRecipeService&) = delete;

  // On the worker: prepares the store, loads the copy on disk, then refreshes. Call once.
  void Start();
  // Refreshes in the background: always when force, otherwise only when the copy is more than
  // 24 h old (or missing) and no refresh failed in the last hour. Cheap to call often (the Install
  // sheet opening, a periodic tick). A request while a refresh runs is dropped: the running one
  // already gets the latest release.
  void RequestRefresh(bool force);
  // Thread-safe and cheap: the current snapshot (never null).
  std::shared_ptr<const RecipeSnapshot> Snapshot() const;
  bool Busy() const;
  // Waits for the worker to finish what it is doing (tests, and Shutdown).
  void Wait();
  // Cancels a refresh and joins the worker.
  void Shutdown();

  // The recipe stored under a full revision or a 32-hex prefix, its bytes checked against the
  // revision and ParseCommunityRecipe plus CommunityRecipeRefusals applied again. Any thread; works
  // offline and for revisions no longer in the current release, as long as the object is kept.
  std::optional<runtime::CommunityRecipe> LoadRecipe(std::string_view revision_or_prefix) const;
  // Deletes stored objects nothing references any more (keep: full revisions or 32-hex prefixes;
  // the current release's objects always stay). Waits for a running refresh.
  std::size_t CollectGarbage(const std::set<std::string>& keep);

 private:
  struct Shared;
  bool Launch(std::function<void()> work);
  void DoStart();
  void DoRefresh();
  bool Refresh(std::string* error);
  void LoadFromDisk();
  // A GET of at most limit bytes, following redirects to allowed hosts. Requests to the recipes
  // repository carry its auth header on its own origin; API requests never do.
  std::optional<std::string> Get(const std::string& url, std::size_t limit, const std::string& accept,
                                 std::string* error);
  std::optional<std::string> GetFrom(const std::string& url, std::size_t limit, const std::string& accept,
                                     bool recipes_request, std::string* error);
  void Publish(const std::function<void(RecipeSnapshot&)>& change);
  void Log(const std::string& line) const;
  std::int64_t Now() const;

  RecipeServiceConfig config_;
  std::shared_ptr<Shared> shared_;
  std::unique_ptr<RecipeStore> store_;
};

// The summaries of a release: every index entry whose object is in the store and passes the
// checks (its file's game the one it is listed under), in index order, with votes when totals has them. A recipe
// that needs an experimental feature feature_enabled doesn't name (or with no feature_enabled) is left out. Used at
// load and after a refresh.
std::map<GameRef, std::vector<RecipeSummary>> BuildSummaries(
    const RecipeIndex& index, const RecipeStore& store, const VoteTotals* totals,
    const std::function<void(const std::string&)>& log,
    const std::function<bool(const std::string& feature)>& feature_enabled = {});

// ParseCommunityRecipe and CommunityRecipeRefusals (console options) on bytes (the id made from the file's game);
// the reason in *error. The caller compares the game with the path or index entry it came from.
std::optional<runtime::CommunityRecipe> CheckRecipeFile(std::string_view bytes, std::string* error);

// A stored object by its revision (64 hex) or a 32-hex prefix, checked as CheckRecipeFile does; nothing when the store
// isn't prepared or has no such object. *error: why a stored object was refused (empty when there was none).
std::optional<runtime::CommunityRecipe> LoadStoredRecipe(const RecipeStore& store, std::string_view revision_or_prefix,
                                                         std::string* error);

}  // namespace playden::community
