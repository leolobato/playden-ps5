// recipe_release.cpp - index.json, recipes.zip (in memory) and summary.json of community recipes.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/recipe_release.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>

#include "miniz.h"
#include "runtime/artifact_hash.h"
#include "runtime/game_settings.h"
#include "steam/backend/json.h"

namespace playden::community {
namespace {

using Json = steam::json::Value;

void Log(const std::function<void(const std::string&)>& log, const std::string& line) {
  if (log) log(line);
}

bool LowerHex(std::string_view text, std::size_t size) {
  if (text.size() != size) return false;
  for (char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string Text(const Json& object, std::string_view key) {
  const auto* value = object.Find(key);
  if (!value || !value->is_string() || value->text().size() > 1024) return {};
  return value->text();
}

// recipes/<store>/<key>/<slug>.json -> the recipe id it must hold (made from the path), or empty.
std::string IdForPath(std::string_view name, GameRef* game, std::string* slug) {
  constexpr std::string_view kPrefix = "recipes/";
  constexpr std::string_view kSuffix = ".json";
  if (name.size() <= kPrefix.size() + kSuffix.size() || name.substr(0, kPrefix.size()) != kPrefix ||
      name.substr(name.size() - kSuffix.size()) != kSuffix)
    return {};
  const auto rest = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
  const auto first = rest.find('/');
  if (first == std::string_view::npos) return {};
  const auto second = rest.find('/', first + 1);
  if (second == std::string_view::npos || rest.find('/', second + 1) != std::string_view::npos) return {};
  GameRef path_game{std::string(rest.substr(0, first)), std::string(rest.substr(first + 1, second - first - 1))};
  const std::string file_slug(rest.substr(second + 1));
  if (!path_game.valid() || !runtime::IsValidCommunitySlug(file_slug)) return {};
  const std::string id = runtime::CommunityRecipeIdFor(path_game, file_slug);
  if (id.size() > runtime::kCommunityRecipeIdMaxBytes) return {};
  *game = std::move(path_game);
  *slug = file_slug;
  return id;
}

struct ZipReader {
  mz_zip_archive archive{};
  bool open = false;
  ~ZipReader() {
    if (open) mz_zip_reader_end(&archive);
  }
};

}  // namespace

std::string Sha256Hex(std::string_view bytes) { return runtime::HashArtifactBytes(bytes).value_or(""); }
bool IsRevision(std::string_view text) { return LowerHex(text, 64); }
bool IsRevisionPrefix(std::string_view text) { return LowerHex(text, 32); }

const IndexEntry* RecipeIndex::Find(const GameRef& game, std::string_view id) const {
  const auto found = games.find(game);
  if (found == games.end()) return nullptr;
  for (const auto& entry : found->second.recipes)
    if (entry.id == id) return &entry;
  return nullptr;
}

std::optional<RecipeIndex> ParseRecipeIndex(std::string_view json, std::string* error,
                                            const std::function<void(const std::string&)>& log) {
  auto refuse = [&](const char* reason) -> std::optional<RecipeIndex> {
    if (error) *error = reason;
    return std::nullopt;
  };
  if (json.size() > kMaxIndexBytes) return refuse("index.json is too large");
  steam::json::Limits limits;
  limits.max_bytes = kMaxIndexBytes;
  limits.max_values = 1 << 18;
  limits.max_string_bytes = 16 << 10;
  const auto doc = steam::json::Parse(json, limits);
  if (!doc || !doc->is_object()) return refuse("index.json is not a JSON object");
  const auto* format = doc->Find("format_version");
  // Version 1 (recipes by Steam app ID) was never released: only version 2 is read.
  if (!format || format->AsUint64() != 2u) return refuse("index.json has an unknown format_version");
  RecipeIndex index;
  index.zip_sha256 = Text(*doc, "zip_sha256");
  if (!IsRevision(index.zip_sha256)) return refuse("index.json has no zip_sha256");
  index.generated_at = Text(*doc, "generated_at");
  if (index.generated_at.size() > 64) index.generated_at.clear();
  const auto* games = doc->Find("games");
  if (!games || !games->is_array()) return refuse("index.json has no games");
  std::size_t listed = 0;
  for (const auto& game : games->array()) {
    const auto* recipes = game.is_object() ? game.Find("recipes") : nullptr;
    if (recipes && recipes->is_array()) listed += recipes->array().size();
  }
  if (listed > kMaxZipEntries) return refuse("index.json lists too many recipes");
  std::set<std::string> ids;
  for (const auto& item : games->array()) {
    if (!item.is_object()) continue;
    GameRef game{Text(item, "store"), Text(item, "key")};
    const auto* recipes = item.Find("recipes");
    const auto shown = (game.store + "/" + game.key).substr(0, 96);
    if (!game.valid() || !recipes || !recipes->is_array()) {
      Log(log, "community recipes: index game \"" + shown + "\" dropped: not a store and its key");
      continue;
    }
    if (index.games.count(game)) {
      Log(log, "community recipes: index game " + shown + " dropped: listed twice");
      continue;
    }
    auto& listed_game = index.games[game];
    listed_game.name = Text(item, "name");
    if (!IsGameName(listed_game.name)) listed_game.name.clear();
    for (const auto& recipe : recipes->array()) {
      if (!recipe.is_object()) continue;
      IndexEntry entry;
      entry.id = Text(recipe, "id");
      entry.game = game;
      entry.revision = Text(recipe, "revision");
      // The id must be the one made from the game and a slug; nothing splits it.
      const auto slug = runtime::CommunityRecipeSlug(entry.id, game);
      if (!slug || !IsRevision(entry.revision)) {
        Log(log, "community recipes: index entry under " + shown + " dropped: bad id or revision");
        continue;
      }
      entry.slug = *slug;
      if (!ids.insert(entry.id).second) {
        Log(log, "community recipes: index entry " + entry.id + " dropped: listed twice");
        continue;
      }
      entry.title = Text(recipe, "title");
      entry.status = Text(recipe, "status");
      entry.author = Text(recipe, "author");
      entry.tested_kit = Text(recipe, "tested_kit");
      entry.min_kit = Text(recipe, "min_kit");
      entry.tested_firmware = Text(recipe, "tested_firmware");
      entry.tested_date = Text(recipe, "tested_date");
      entry.made_on = Text(recipe, "made_on");
      if (!IsRecipeStore(entry.made_on)) entry.made_on.clear();
      if (const auto* contributors = recipe.Find("contributors"); contributors && contributors->is_array())
        for (const auto& name : contributors->array())
          if (name.is_string() && name.text().size() <= 256) entry.contributors.push_back(name.text());
      listed_game.recipes.push_back(std::move(entry));
      ++index.count;
    }
  }
  return index;
}

std::optional<std::vector<ZipRecipe>> ReadRecipesZip(std::string_view zip, const RecipeIndex& index,
                                                     std::string* error,
                                                     const std::function<void(const std::string&)>& log) {
  auto refuse = [&](const char* reason) -> std::optional<std::vector<ZipRecipe>> {
    if (error) *error = reason;
    return std::nullopt;
  };
  if (zip.size() > kMaxZipBytes) return refuse("recipes.zip is too large");
  // On the heap: the worker's stack is small, and miniz's structures are not.
  auto reader = std::make_unique<ZipReader>();
  if (!mz_zip_reader_init_mem(&reader->archive, zip.data(), zip.size(), 0)) return refuse("recipes.zip is not a zip");
  reader->open = true;
  const mz_uint count = mz_zip_reader_get_num_files(&reader->archive);
  if (count > kMaxZipEntries) return refuse("recipes.zip has too many entries");
  auto stat = std::make_unique<mz_zip_archive_file_stat>();
  std::vector<ZipRecipe> recipes;
  std::set<std::string> seen;
  for (mz_uint i = 0; i < count; ++i) {
    if (!mz_zip_reader_file_stat(&reader->archive, i, stat.get())) {
      Log(log, "community recipes: zip entry " + std::to_string(i) + " dropped: unreadable");
      continue;
    }
    const std::size_t name_size = strnlen(stat->m_filename, sizeof stat->m_filename);
    const std::string name(stat->m_filename, name_size);
    const std::string shown = name.substr(0, 96);
    auto drop = [&](const char* reason) { Log(log, "community recipes: " + shown + " dropped: " + reason); };
    if (name_size >= sizeof stat->m_filename - 1 || mz_zip_reader_is_file_a_directory(&reader->archive, i) ||
        stat->m_is_directory) {
      drop("not a recipe file");
      continue;
    }
    GameRef game;
    std::string slug;
    const std::string id = IdForPath(name, &game, &slug);
    if (id.empty()) {
      drop("not recipes/<store>/<key>/<slug>.json");
      continue;
    }
    if (!seen.insert(name).second) {
      drop("in the zip twice");
      continue;
    }
    const IndexEntry* entry = index.Find(game, id);
    if (!entry) {
      drop("not listed in index.json");
      continue;
    }
    if (stat->m_is_encrypted || !stat->m_is_supported) {
      drop("encrypted or unsupported");
      continue;
    }
    if (stat->m_uncomp_size == 0 || stat->m_uncomp_size > kMaxRecipeBytes) {
      drop("empty or larger than 16 KB");
      continue;
    }
    std::string bytes(static_cast<std::size_t>(stat->m_uncomp_size), '\0');
    if (!mz_zip_reader_extract_to_mem(&reader->archive, i, bytes.data(), bytes.size(), 0)) {
      drop("cannot be unpacked");
      continue;
    }
    if (bytes.size() != stat->m_uncomp_size || bytes.size() > kMaxRecipeBytes) {
      drop("size changed while unpacking");
      continue;
    }
    if (Sha256Hex(bytes) != entry->revision) {
      drop("does not match its revision in index.json");
      continue;
    }
    recipes.push_back(ZipRecipe{entry, std::move(bytes)});
  }
  return recipes;
}

namespace {

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
std::int64_t DaysFromCivil(std::int64_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

// The digits at text[at, at + count) as a number, or -1.
int Digits(std::string_view text, std::size_t at, std::size_t count) {
  if (at + count > text.size()) return -1;
  int value = 0;
  for (std::size_t i = at; i < at + count; ++i) {
    if (text[i] < '0' || text[i] > '9') return -1;
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

}  // namespace

std::optional<std::int64_t> ParseUtcTime(std::string_view text) {
  int year = -1, month = -1, day = -1, hour = -1, minute = -1, second = -1;
  if (text.size() == 20 && text[4] == '-' && text[7] == '-' && text[10] == 'T' && text[13] == ':' && text[16] == ':' &&
      text[19] == 'Z') {
    year = Digits(text, 0, 4), month = Digits(text, 5, 2), day = Digits(text, 8, 2);
    hour = Digits(text, 11, 2), minute = Digits(text, 14, 2), second = Digits(text, 17, 2);
  } else if (text.size() == 29 && text[3] == ',' && text[4] == ' ' && text[7] == ' ' && text[11] == ' ' &&
             text[16] == ' ' && text[19] == ':' && text[22] == ':' && text.substr(25) == " GMT") {
    static constexpr std::string_view kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const auto found = kMonths.find(text.substr(8, 3));
    if (found == std::string_view::npos || found % 3 != 0) return std::nullopt;
    month = static_cast<int>(found / 3) + 1;
    day = Digits(text, 5, 2), year = Digits(text, 12, 4);
    hour = Digits(text, 17, 2), minute = Digits(text, 20, 2), second = Digits(text, 23, 2);
  } else {
    return std::nullopt;
  }
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59 || second < 0 || second > 60)
    return std::nullopt;
  return DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 +
         minute * 60 + second;
}

std::optional<VoteTotals> ParseVoteSummary(std::string_view json, std::string* error, std::int64_t* generated_at) {
  if (generated_at) *generated_at = 0;
  if (json.size() > kMaxSummaryBytes) {
    if (error) *error = "summary.json is too large";
    return std::nullopt;
  }
  steam::json::Limits limits;
  limits.max_bytes = kMaxSummaryBytes;
  limits.max_values = 1 << 17;
  limits.max_string_bytes = 4096;
  const auto doc = steam::json::Parse(json, limits);
  const auto* recipes = doc && doc->is_object() ? doc->Find("recipes") : nullptr;
  if (!recipes || !recipes->is_object()) {
    if (error) *error = "summary.json has no recipes";
    return std::nullopt;
  }
  if (const auto* made = doc->Find("generated_at"); generated_at && made && made->is_string())
    *generated_at = ParseUtcTime(made->text()).value_or(0);
  auto count = [](const Json* value) -> std::uint32_t {
    const auto number = value ? value->AsUint64() : std::nullopt;
    return number ? static_cast<std::uint32_t>(std::min<std::uint64_t>(*number, 0xffffffffu)) : 0;
  };
  VoteTotals totals;
  for (const auto& [id, revisions] : recipes->object()) {
    if (!runtime::IsCommunityRecipeId(id) || !revisions.is_object()) continue;
    for (const auto& [revision, value] : revisions.object()) {
      if (!IsRevision(revision) || !value.is_object()) continue;
      RecipeVotes votes;
      votes.up = count(value.Find("up"));
      votes.down = count(value.Find("down"));
      if (const auto* reasons = value.Find("reasons"); reasons && reasons->is_object()) {
        for (const auto& [reason, number] : reasons->object()) {
          bool ok = !reason.empty() && reason.size() <= 32;
          for (char c : reason)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) ok = false;
          if (ok && votes.reasons.size() < 16) votes.reasons.emplace_back(reason, count(&number));
        }
        std::stable_sort(votes.reasons.begin(), votes.reasons.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
      }
      if (const auto* verified = value.Find("verified"); verified && verified->is_bool()) votes.verified = verified->boolean();
      if (const auto* author = value.Find("author"); author && author->is_string() &&
          (author->text() == "account" || author->text() == "named" || author->text() == "anonymous"))
        votes.author = author->text();
      if (const auto* trusted = value.Find("author_trusted"); trusted && trusted->is_bool() && votes.author == "account")
        votes.author_trusted = trusted->boolean();
      totals[id][revision] = std::move(votes);
    }
  }
  return totals;
}

std::optional<CompatList> ParseCompatList(std::string_view json) {
  if (json.size() > kMaxSummaryBytes) return std::nullopt;
  steam::json::Limits limits;
  limits.max_bytes = kMaxSummaryBytes;
  limits.max_values = 1 << 17;
  limits.max_string_bytes = 4096;
  const auto doc = steam::json::Parse(json, limits);
  const auto* games = doc && doc->is_object() ? doc->Find("games") : nullptr;
  if (!games || !games->is_object()) return std::nullopt;
  CompatList list;
  for (const auto& [store, entries] : games->object()) {
    if (!entries.is_object()) continue;
    for (const auto& [key, value] : entries.object()) {
      const GameRef game{store, key};
      const auto* status = value.is_object() ? value.Find("status") : nullptr;
      if (!game.valid() || !status || !status->is_string()) continue;
      const std::string how = status->text();
      if (how != "works" && how != "partial" && how != "broken" && how != "refused") continue;
      ListedGame listed;
      listed.status = how;
      if (const auto* reports = value.Find("reports"); reports && how == "works")
        if (const auto number = reports->AsUint64())
          listed.reports = static_cast<std::uint32_t>(std::min<std::uint64_t>(*number, 0xffffffffu));
      list[game] = std::move(listed);
    }
  }
  return list;
}

}  // namespace playden::community
