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
  std::string id;        // community-<app id>-<slug>
  std::string slug;
  std::uint32_t app_id = 0;
  std::string revision;  // sha256 hex of the file
  std::string title, status, author, tested_kit, min_kit, tested_firmware, tested_date;
  std::vector<std::string> contributors;
};

struct RecipeIndex {
  std::string generated_at;
  std::string zip_sha256;
  std::map<std::uint32_t, std::vector<IndexEntry>> apps;  // in the index's order per app
  std::size_t count = 0;
  const IndexEntry* Find(std::uint32_t app_id, std::string_view id) const;
};

// index.json, format_version 1. The whole index is refused for another format, a missing or bad
// zip_sha256, more than kMaxZipEntries recipes or bad JSON; a single bad entry (an app ID that is
// not canonical, an id that is not community-<that app>-<slug>, a revision that is not sha256 hex,
// an id listed twice) is dropped and reported through log.
std::optional<RecipeIndex> ParseRecipeIndex(std::string_view json, std::string* error,
                                            const std::function<void(const std::string&)>& log = {});

// A recipe file taken from the zip: its path matched recipes/<app id>/<slug>.json for an index
// entry and its bytes hash to that entry's revision. Not parsed yet.
struct ZipRecipe {
  const IndexEntry* entry = nullptr;  // into the index passed to ReadRecipesZip
  std::string bytes;
};

// Reads recipes.zip in memory. Refused whole (nullopt) when it is not a zip or holds more than
// kMaxZipEntries entries. Each entry must be a file named exactly recipes/<canonical app id>/<valid
// slug>.json, at most kMaxRecipeBytes (checked before and after extracting), listed in the index
// with the revision its bytes hash to, and not seen before; anything else is dropped and logged.
std::optional<std::vector<ZipRecipe>> ReadRecipesZip(std::string_view zip, const RecipeIndex& index,
                                                     std::string* error,
                                                     const std::function<void(const std::string&)>& log = {});

struct RecipeVotes {
  std::uint32_t up = 0;
  std::uint32_t down = 0;
  std::vector<std::pair<std::string, std::uint32_t>> reasons;  // thumbs-down reasons, most first
};

// summary.json: recipe id -> revision -> votes. Unusable entries are skipped.
using VoteTotals = std::map<std::string, std::map<std::string, RecipeVotes>>;
std::optional<VoteTotals> ParseVoteSummary(std::string_view json, std::string* error);

// sha256 hex of bytes.
std::string Sha256Hex(std::string_view bytes);
bool IsRevision(std::string_view text);        // 64 lowercase hex
bool IsRevisionPrefix(std::string_view text);  // 32 lowercase hex

}  // namespace playden::community
