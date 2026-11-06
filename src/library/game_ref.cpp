// game_ref.cpp - A community recipe's game: stores, keys, name keys and game names.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/game_ref.h"

#include <utf8proc.h>

#include <algorithm>
#include <cstdlib>

namespace playden {
namespace {

constexpr std::uint64_t kMaxSteamApp = 4294967295u;

bool Digit(char c) { return c >= '0' && c <= '9'; }
bool LowerAlnum(char c) { return (c >= 'a' && c <= 'z') || Digit(c); }

// ^[1-9][0-9]{0,max-1}$
bool CanonicalDigits(std::string_view key, std::size_t max_digits) {
  if (key.empty() || key.size() > max_digits || key.front() == '0') return false;
  for (const char c : key)
    if (!Digit(c)) return false;
  return true;
}

// ^[a-z0-9]+(-[a-z0-9]+)*$
bool HyphenWords(std::string_view text) {
  if (text.empty() || text.front() == '-' || text.back() == '-') return false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char c = text[index];
    if (c == '-') {
      if (text[index - 1] == '-') return false;
    } else if (!LowerAlnum(c)) {
      return false;
    }
  }
  return true;
}

int StoreRank(std::string_view store) {
  if (store == kRecipeStoreSteam) return 0;
  if (store == kRecipeStoreEpic) return 1;
  if (store == kRecipeStoreGog) return 2;
  if (store == kRecipeStoreImported) return 3;
  return 4;
}

bool NumericKeys(std::string_view store) { return store == kRecipeStoreSteam || store == kRecipeStoreGog; }

// Calls each(code point) for every code point of valid UTF-8; false (stopping) on invalid UTF-8 or when each does.
template <class Each>
bool ForEachCodePoint(std::string_view text, Each each) {
  const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(text.data());
  utf8proc_ssize_t offset = 0;
  const auto size = static_cast<utf8proc_ssize_t>(text.size());
  while (offset < size) {
    utf8proc_int32_t code_point = 0;
    const auto used = utf8proc_iterate(bytes + offset, size - offset, &code_point);
    if (used <= 0 || code_point < 0) return false;
    if (!each(code_point)) return false;
    offset += used;
  }
  return true;
}

bool Mark(utf8proc_category_t category) {
  return category == UTF8PROC_CATEGORY_MN || category == UTF8PROC_CATEGORY_MC || category == UTF8PROC_CATEGORY_ME;
}

bool Other(utf8proc_category_t category) {
  return category == UTF8PROC_CATEGORY_CN || category == UTF8PROC_CATEGORY_CC || category == UTF8PROC_CATEGORY_CF ||
         category == UTF8PROC_CATEGORY_CS || category == UTF8PROC_CATEGORY_CO;
}

bool Separator(utf8proc_category_t category) {
  return category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP;
}

}  // namespace

bool GameRef::valid() const { return IsStoreKey(store, key); }

GameRef GameRef::Steam(std::uint32_t app) {
  if (app == 0) return {};
  return {std::string(kRecipeStoreSteam), std::to_string(app)};
}

bool operator==(const GameRef& left, const GameRef& right) {
  return left.store == right.store && left.key == right.key;
}

bool operator!=(const GameRef& left, const GameRef& right) { return !(left == right); }

bool operator<(const GameRef& left, const GameRef& right) {
  const int left_rank = StoreRank(left.store), right_rank = StoreRank(right.store);
  if (left_rank != right_rank) return left_rank < right_rank;
  if (left.store != right.store) return left.store < right.store;
  // Canonical decimals: the shorter number is the smaller one.
  if (NumericKeys(left.store) && left.key.size() != right.key.size()) return left.key.size() < right.key.size();
  return left.key < right.key;
}

bool IsRecipeStore(std::string_view store) { return StoreRank(store) < 4; }

bool IsStoreKey(std::string_view store, std::string_view key) {
  if (store == kRecipeStoreSteam) {
    if (!CanonicalDigits(key, 10)) return false;
    std::uint64_t value = 0;
    for (const char c : key) value = value * 10 + static_cast<std::uint64_t>(c - '0');
    return value <= kMaxSteamApp;
  }
  if (store == kRecipeStoreGog) return CanonicalDigits(key, 20);
  if (store == kRecipeStoreEpic) {
    if (key.size() != 32) return false;
    for (const char c : key)
      if (!Digit(c) && !(c >= 'a' && c <= 'f')) return false;
    return true;
  }
  if (store == kRecipeStoreImported) return key.size() <= kNameKeyMaxBytes && HyphenWords(key);
  return false;
}

std::string ToText(const GameRef& ref) { return ref.store + "/" + ref.key; }

std::optional<GameRef> ParseGameRefText(std::string_view text) {
  const auto slash = text.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  GameRef ref{std::string(text.substr(0, slash)), std::string(text.substr(slash + 1))};
  if (!ref.valid()) return std::nullopt;
  return ref;
}

std::string NameKey(std::string_view name) {
  // NFKD; invalid UTF-8 has no key of its own.
  utf8proc_uint8_t* mapped = nullptr;
  const auto count = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(name.data()),
                                  static_cast<utf8proc_ssize_t>(name.size()), &mapped,
                                  static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPAT | UTF8PROC_DECOMPOSE));
  std::string key;
  if (count >= 0) {
    const std::string_view decomposed(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(count));
    bool gap = false;
    ForEachCodePoint(decomposed, [&](utf8proc_int32_t code_point) {
      // Every mark, not only those with a combining class: U+20DD or U+FE0F would otherwise split words.
      if (Mark(utf8proc_category(code_point))) return true;
      char c = code_point < 0x80 ? static_cast<char>(code_point) : '\0';
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (!LowerAlnum(c)) {
        gap = true;
        return true;
      }
      if (gap && !key.empty()) key += '-';
      gap = false;
      key += c;
      return true;
    });
  }
  std::free(mapped);
  if (key.size() > kNameKeyMaxBytes) key.resize(kNameKeyMaxBytes);
  while (!key.empty() && key.back() == '-') key.pop_back();
  return key.empty() ? std::string("game") : key;
}

std::vector<std::string> SearchWords(std::string_view text) {
  utf8proc_uint8_t* mapped = nullptr;
  const auto count = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
                                  static_cast<utf8proc_ssize_t>(text.size()), &mapped,
                                  static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPAT | UTF8PROC_DECOMPOSE |
                                                                 UTF8PROC_CASEFOLD));
  std::vector<std::string> words;
  if (count >= 0) {
    const std::string_view folded(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(count));
    bool gap = true;
    ForEachCodePoint(folded, [&](utf8proc_int32_t code_point) {
      const auto category = utf8proc_category(code_point);
      if (Mark(category)) return true;
      const bool letter = category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL ||
                          category == UTF8PROC_CATEGORY_LT || category == UTF8PROC_CATEGORY_LM ||
                          category == UTF8PROC_CATEGORY_LO || category == UTF8PROC_CATEGORY_ND ||
                          category == UTF8PROC_CATEGORY_NL || category == UTF8PROC_CATEGORY_NO;
      if (!letter) {
        gap = true;
        return true;
      }
      if (gap) words.emplace_back();
      gap = false;
      utf8proc_uint8_t encoded[4];
      const auto used = utf8proc_encode_char(code_point, encoded);
      words.back().append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(used));
      return true;
    });
  }
  std::free(mapped);
  return words;
}

std::string FoldedName(std::string_view name) {
  utf8proc_uint8_t* mapped = nullptr;
  const auto count = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(name.data()),
                                  static_cast<utf8proc_ssize_t>(name.size()), &mapped,
                                  static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPAT | UTF8PROC_DECOMPOSE |
                                                                 UTF8PROC_CASEFOLD));
  if (count < 0) return std::string(name);
  std::string folded;
  const std::string_view decomposed(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(count));
  ForEachCodePoint(decomposed, [&](utf8proc_int32_t code_point) {
    // Every mark, as NameKey drops them.
    if (Mark(utf8proc_category(code_point))) return true;
    utf8proc_uint8_t encoded[4];
    const auto used = utf8proc_encode_char(code_point, encoded);
    folded.append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(used));
    return true;
  });
  std::free(mapped);
  return folded;
}

bool NameMatchesSearch(std::string_view name, std::string_view query) {
  const auto names = SearchWords(name);
  for (const auto& word : SearchWords(query)) {
    const bool found = std::any_of(names.begin(), names.end(), [&](const std::string& candidate) {
      return candidate.compare(0, word.size(), word) == 0;
    });
    if (!found) return false;
  }
  return true;
}

bool IsGameName(std::string_view name) {
  std::size_t characters = 0;
  bool visible = false;
  const bool ok = ForEachCodePoint(name, [&](utf8proc_int32_t code_point) {
    const auto category = utf8proc_category(code_point);
    if (Other(category) || category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP) return false;
    if (!Separator(category) && !Mark(category)) visible = true;
    ++characters;
    return true;
  });
  return ok && visible && characters >= 1 && characters <= kGameNameMaxCharacters;
}

std::string GameNameFromTitle(std::string_view title) {
  std::string name;
  std::size_t characters = 0;
  const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(title.data());
  const auto size = static_cast<utf8proc_ssize_t>(title.size());
  for (utf8proc_ssize_t offset = 0; offset < size && characters < kGameNameMaxCharacters;) {
    utf8proc_int32_t code_point = 0;
    const auto used = utf8proc_iterate(bytes + offset, size - offset, &code_point);
    if (used <= 0) {  // an invalid byte is left out
      ++offset;
      continue;
    }
    const auto category = utf8proc_category(code_point);
    if (category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP || code_point == '\n' ||
        code_point == '\r' || code_point == '\t') {
      if (!name.empty() && name.back() != ' ') {
        name += ' ';
        ++characters;
      }
    } else if (!Other(category)) {
      name.append(title.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(used)));
      ++characters;
    }
    offset += used;
  }
  while (!name.empty() && name.back() == ' ') name.pop_back();
  std::size_t start = 0;
  while (start < name.size() && name[start] == ' ') ++start;
  name.erase(0, start);
  return IsGameName(name) ? name : std::string();
}

std::string_view RecipeStoreOf(Store store) {
  switch (store) {
    case Store::kSteam: return kRecipeStoreSteam;
    case Store::kEpic: return kRecipeStoreEpic;
    case Store::kGog: return kRecipeStoreGog;
    case Store::kLocal: return kRecipeStoreImported;
  }
  return kRecipeStoreSteam;
}

std::optional<GameRef> MatchingGameRef(const GameId& game, std::uint32_t steam_app_id) {
  if (!game.valid()) return std::nullopt;
  if (const auto app = game.steam_app()) return GameRef::Steam(*app);
  // Epic and GOG get their own refs when those stores exist; until then only a Steam app ID matches.
  if (steam_app_id != 0) return GameRef::Steam(steam_app_id);
  return std::nullopt;
}

std::optional<GameRef> SharingGameRef(const GameId& game, std::uint32_t steam_app_id, std::string_view title) {
  if (auto ref = MatchingGameRef(game, steam_app_id)) return ref;
  if (!game.valid() || game.store != Store::kLocal) return std::nullopt;
  return GameRef{std::string(kRecipeStoreImported), NameKey(title)};
}

}  // namespace playden
