#pragma once
// game_ref.h - What a community recipe is for: one store's game, {store, key} (docs/community-recipes.md). The
// rules match the recipes repository's scripts/check.py and the API's src/recipe/slug.ts exactly.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "library/store.h"

namespace playden {

// The stores a recipe can be for, in the order a release lists them.
inline constexpr std::string_view kRecipeStoreSteam = "steam";
inline constexpr std::string_view kRecipeStoreEpic = "epic";
inline constexpr std::string_view kRecipeStoreGog = "gog";
inline constexpr std::string_view kRecipeStoreImported = "imported";

inline constexpr std::size_t kNameKeyMaxBytes = 48;
inline constexpr std::size_t kGameNameMaxCharacters = 120;

// A recipe's game. store: steam, epic, gog or imported; key: the store's game ID (Steam and GOG digits, Epic its
// catalog item ID in lowercase hex) or, for an imported game, a name key (NameKey). A default one is invalid.
struct GameRef {
  std::string store;
  std::string key;

  bool valid() const;
  static GameRef Steam(std::uint32_t app);
};

bool operator==(const GameRef& left, const GameRef& right);
bool operator!=(const GameRef& left, const GameRef& right);
// A release's order: steam, epic, gog, imported; Steam and GOG keys by value, the others as text.
bool operator<(const GameRef& left, const GameRef& right);

bool IsRecipeStore(std::string_view store);
// Whether key follows the store's rule: steam ^[1-9][0-9]{0,9}$ up to 4294967295, gog ^[1-9][0-9]{0,19}$, epic
// ^[0-9a-f]{32}$, imported a name key ^[a-z0-9]+(-[a-z0-9]+)*$ of 1-48 bytes. Nothing else, a trailing newline included.
bool IsStoreKey(std::string_view store, std::string_view key);

// "<store>/<key>", as revisions.lst and slugs.lst name a game; ParseGameRefText reads it back (valid refs only).
std::string ToText(const GameRef& ref);
std::optional<GameRef> ParseGameRefText(std::string_view text);

// The key of an imported game, made from its name: Unicode NFKD, every mark (category M) dropped, lowercase ASCII,
// every run of other characters one "-", "-" trimmed at both ends, cut to 48 bytes and trimmed again; "game" when
// nothing is left (invalid UTF-8 included). The same name gives the same key on every console, the API and the checker.
std::string NameKey(std::string_view name);

// The words a recipe search compares (the chooser's "Search recipes by game name…"): normalised the NameKey way, Unicode
// NFKD with every mark dropped and case folded, but letters and digits of every script are kept: each run of letters
// and digits is a word, everything else separates words. For an ASCII name they are the words NameKey joins with "-".
std::vector<std::string> SearchWords(std::string_view text);
// Whether a game's name matches a search: every word of the query begins a word of the name, in any order ("brulee
// cr" matches "Crème Brûlée & Clockwork"). A query without words matches every name.
bool NameMatchesSearch(std::string_view name, std::string_view query);

// A game's name as a recipe's game.name holds it: valid UTF-8 of 1-120 characters (code points) with at least one
// visible character (outside categories Z, M and C), and no control, format, private-use or unassigned character and
// no line or paragraph separator. Spaces of any width are fine.
bool IsGameName(std::string_view name);

// A library title as a recipe's game.name: control, format and other invisible characters dropped, line breaks made
// spaces, spaces trimmed, cut to 120 characters; empty when nothing visible is left (IsGameName holds otherwise).
std::string GameNameFromTitle(std::string_view title);

// The recipe store a library game's own store is ("imported" for a Local game).
std::string_view RecipeStoreOf(Store store);

// The recipes a library game gets automatically, matched only by ID: a Steam game's steam/<app>; another store's game
// the Steam app ID its Steamworks emulator uses (steam_appid.txt or Game settings) when it has one, else nothing (a
// player finds other recipes by searching).
std::optional<GameRef> MatchingGameRef(const GameId& game, std::uint32_t steam_app_id);
// What a library game's shares are for: its matching ref, else for an imported game imported/<NameKey(title)>.
std::optional<GameRef> SharingGameRef(const GameId& game, std::uint32_t steam_app_id, std::string_view title);

}  // namespace playden
