#pragma once
// recipe_release.h - The files of a community recipes release (playden-ps5-recipes): index.json,
// recipes.zip read in memory, and the API's vote totals (summary.json). Pure functions, no IO.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "library/game_ref.h"

namespace playden::community {

inline constexpr std::size_t kMaxReleaseJsonBytes = 64 << 10;
inline constexpr std::size_t kMaxIndexBytes = 256 << 10;
inline constexpr std::size_t kMaxZipBytes = 2 << 20;
inline constexpr std::size_t kMaxSummaryBytes = 256 << 10;
inline constexpr std::size_t kMaxZipEntries = 2000;
inline constexpr std::size_t kMaxRecipeBytes = 16 << 10;
inline constexpr const char* kIndexAsset = "index.json";
inline constexpr const char* kZipAsset = "recipes.zip";

// One recipe as index.json lists it.
struct IndexEntry {
  std::string id;        // community-<store>-<key>-<slug>
  std::string slug;
  GameRef game;          // the game it is listed under
  std::string revision;  // sha256 hex of the file
  std::string title, status, author, tested_kit, min_kit, tested_firmware, tested_date;
  std::string made_on;   // the store of the copy it was made with, when not game.store
  std::vector<std::string> contributors;
};

// A game index.json lists: its name (the newest recipe's game.name; empty when not a game name) and its recipes.
struct IndexGame {
  std::string name;
  std::vector<IndexEntry> recipes;  // in the index's order
};

struct RecipeIndex {
  std::string generated_at;
  std::string zip_sha256;
  std::map<GameRef, IndexGame> games;
  std::size_t count = 0;
  const IndexEntry* Find(const GameRef& game, std::string_view id) const;
};

// index.json, format_version 2: {"games": [{"store", "key", "name", "recipes": [entries]}]}. The whole index is
// refused for another format (version 1 included), a missing or bad zip_sha256, more than kMaxZipEntries recipes or
// bad JSON; a game that is not a store and its key, or listed twice, and a single bad entry (an id that is not
// community-<store>-<key>-<slug> for its game, a revision that is not sha256 hex, an id listed twice) are dropped and
// reported through log.
std::optional<RecipeIndex> ParseRecipeIndex(std::string_view json, std::string* error,
                                            const std::function<void(const std::string&)>& log = {});

// A recipe file taken from the zip: its path matched recipes/<store>/<key>/<slug>.json for an index
// entry and its bytes hash to that entry's revision. Not parsed yet.
struct ZipRecipe {
  const IndexEntry* entry = nullptr;  // into the index passed to ReadRecipesZip
  std::string bytes;
};

// Reads recipes.zip in memory. Refused whole (nullopt) when it is not a zip or holds more than
// kMaxZipEntries entries. Each entry must be a file named exactly recipes/<store>/<key>/<valid
// slug>.json, at most kMaxRecipeBytes (checked before and after extracting), listed in the index
// with the revision its bytes hash to, and not seen before; anything else is dropped and logged.
std::optional<std::vector<ZipRecipe>> ReadRecipesZip(std::string_view zip, const RecipeIndex& index,
                                                     std::string* error,
                                                     const std::function<void(const std::string&)>& log = {});

struct RecipeVotes {
  std::uint32_t up = 0;
  std::uint32_t down = 0;
  std::vector<std::pair<std::string, std::uint32_t>> reasons;  // thumbs-down reasons, most first
  // Published revisions (plan 2026-11-06): marked Verified by the team or shared by a trusted account,
  // and who the author is: "account", "named" or "anonymous" ("" when the API didn't say).
  bool verified = false;
  std::string author;
  bool author_trusted = false;  // the author's account is trusted (a star by the name), apart from verified
};

// summary.json: recipe id -> revision -> votes. Unusable entries are skipped. *generated_at: when the API made it (Unix
// seconds, its "generated_at"); 0 when it doesn't say.
using VoteTotals = std::map<std::string, std::map<std::string, RecipeVotes>>;
std::optional<VoteTotals> ParseVoteSummary(std::string_view json, std::string* error, std::int64_t* generated_at = nullptr);

// A game of the compatibility list, from summary.json's "games": how it is listed ("works", "partial", "broken" or
// "refused") and how many players confirmed a game listed as Works.
struct ListedGame {
  std::string status;
  std::uint32_t reports = 0;
  bool operator==(const ListedGame&) const = default;
};
using CompatList = std::map<GameRef, ListedGame>;
// summary.json's compatibility list. Nothing when the summary has none (an API without it, or an unusable file):
// that is "not known", apart from a list the game isn't on. Unusable entries are skipped.
std::optional<CompatList> ParseCompatList(std::string_view json);

// Unix seconds of a UTC time as "2026-11-07T10:00:00Z" (ISO 8601, what summary.json says) or
// "Sat, 07 Nov 2026 10:00:00 GMT" (an HTTP Date header); nothing for anything else.
std::optional<std::int64_t> ParseUtcTime(std::string_view text);

// sha256 hex of bytes.
std::string Sha256Hex(std::string_view bytes);
bool IsRevision(std::string_view text);        // 64 lowercase hex
bool IsRevisionPrefix(std::string_view text);  // 32 lowercase hex

}  // namespace playden::community
