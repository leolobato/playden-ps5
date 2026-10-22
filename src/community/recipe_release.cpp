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

// A canonical decimal app ID: digits, no leading zero, 1 to 4294967295.
std::optional<std::uint32_t> CanonicalAppId(std::string_view text) {
  if (text.empty() || text.size() > 10 || text.front() == '0') return std::nullopt;
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  if (value == 0 || value > 0xffffffffull) return std::nullopt;
  return static_cast<std::uint32_t>(value);
}

std::string Text(const Json& object, std::string_view key) {
  const auto* value = object.Find(key);
  if (!value || !value->is_string() || value->text().size() > 1024) return {};
  return value->text();
}

// recipes/<app id>/<slug>.json -> the recipe id it must hold, or empty.
std::string IdForPath(std::string_view name, std::uint32_t* app_id, std::string* slug) {
  constexpr std::string_view kPrefix = "recipes/";
  constexpr std::string_view kSuffix = ".json";
  if (name.size() <= kPrefix.size() + kSuffix.size() || name.substr(0, kPrefix.size()) != kPrefix ||
      name.substr(name.size() - kSuffix.size()) != kSuffix)
    return {};
  const auto rest = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos || rest.find('/', slash + 1) != std::string_view::npos) return {};
  const auto app = CanonicalAppId(rest.substr(0, slash));
  const std::string file_slug(rest.substr(slash + 1));
  if (!app || !runtime::IsValidCommunitySlug(file_slug)) return {};
  const std::string id = runtime::CommunityRecipeIdFor(*app, file_slug);
  std::uint32_t split_app = 0;
  std::string split_slug;
  if (!runtime::SplitCommunityRecipeId(id, &split_app, &split_slug) || split_app != *app || split_slug != file_slug)
    return {};
  *app_id = *app;
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

const IndexEntry* RecipeIndex::Find(std::uint32_t app_id, std::string_view id) const {
  const auto app = apps.find(app_id);
  if (app == apps.end()) return nullptr;
  for (const auto& entry : app->second)
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
  if (!format || format->AsUint64() != 1u) return refuse("index.json has an unknown format_version");
  RecipeIndex index;
  index.zip_sha256 = Text(*doc, "zip_sha256");
  if (!IsRevision(index.zip_sha256)) return refuse("index.json has no zip_sha256");
  index.generated_at = Text(*doc, "generated_at");
  if (index.generated_at.size() > 64) index.generated_at.clear();
  const auto* apps = doc->Find("apps");
  if (!apps || !apps->is_object()) return refuse("index.json has no apps");
  std::size_t listed = 0;
  for (const auto& [key, list] : apps->object()) {
    if (list.is_array()) listed += list.array().size();
  }
  if (listed > kMaxZipEntries) return refuse("index.json lists too many recipes");
  std::set<std::string> ids;
  for (const auto& [key, list] : apps->object()) {
    const auto app_id = CanonicalAppId(key);
    if (!app_id || !list.is_array()) {
      Log(log, "community recipes: index app \"" + key.substr(0, 16) + "\" dropped: not a canonical app ID");
      continue;
    }
    for (const auto& item : list.array()) {
      if (!item.is_object()) continue;
      IndexEntry entry;
      entry.id = Text(item, "id");
      entry.app_id = *app_id;
      entry.revision = Text(item, "revision");
      std::uint32_t id_app = 0;
      const bool id_ok = runtime::SplitCommunityRecipeId(entry.id, &id_app, &entry.slug) && id_app == *app_id &&
                         runtime::CommunityRecipeIdFor(*app_id, entry.slug) == entry.id;
      if (!id_ok || !IsRevision(entry.revision)) {
        Log(log, "community recipes: index entry under " + key + " dropped: bad id or revision");
        continue;
      }
      if (!ids.insert(entry.id).second) {
        Log(log, "community recipes: index entry " + entry.id + " dropped: listed twice");
        continue;
      }
      entry.title = Text(item, "title");
      entry.status = Text(item, "status");
      entry.author = Text(item, "author");
      entry.tested_kit = Text(item, "tested_kit");
      entry.min_kit = Text(item, "min_kit");
      entry.tested_firmware = Text(item, "tested_firmware");
      entry.tested_date = Text(item, "tested_date");
      if (const auto* contributors = item.Find("contributors"); contributors && contributors->is_array())
        for (const auto& name : contributors->array())
          if (name.is_string() && name.text().size() <= 256) entry.contributors.push_back(name.text());
      index.apps[*app_id].push_back(std::move(entry));
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
    std::uint32_t app_id = 0;
    std::string slug;
    const std::string id = IdForPath(name, &app_id, &slug);
    if (id.empty()) {
      drop("not recipes/<app id>/<slug>.json");
      continue;
    }
    if (!seen.insert(name).second) {
      drop("in the zip twice");
      continue;
    }
    const IndexEntry* entry = index.Find(app_id, id);
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

std::optional<VoteTotals> ParseVoteSummary(std::string_view json, std::string* error) {
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
  auto count = [](const Json* value) -> std::uint32_t {
    const auto number = value ? value->AsUint64() : std::nullopt;
    return number ? static_cast<std::uint32_t>(std::min<std::uint64_t>(*number, 0xffffffffu)) : 0;
  };
  VoteTotals totals;
  for (const auto& [id, revisions] : recipes->object()) {
    if (!runtime::SplitCommunityRecipeId(id, nullptr, nullptr) || !revisions.is_object()) continue;
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
      totals[id][revision] = std::move(votes);
    }
  }
  return totals;
}

}  // namespace playden::community
