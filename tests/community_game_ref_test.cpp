// community_game_ref_test.cpp - What a community recipe is for: stores, keys, name keys and game names, against the
// recipes repository's scripts/check.py (the table below is its output).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/game_ref.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace playden;

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                           \
    }                                                                       \
  } while (0)

std::string Repeat(const std::string& text, int count) {
  std::string out;
  for (int index = 0; index < count; ++index) out += text;
  return out;
}

void NameKeys() {
  struct Example {
    std::string name;
    std::string key;
  };
  // The contract's table (plan 2026-11-06 Phase E), then more of check.py's answers.
  const std::vector<Example> examples = {
      {"TMNT: Mutant Melee", "tmnt-mutant-melee"},
      {"Cr\u00e8me Br\u00fbl\u00e9e & Clockwork \u2014 d\u00e9j\u00e0 vu", "creme-brulee-clockwork-deja-vu"},
      {"  ---  ", "game"},
      {"Half-Life 2", "half-life-2"},
      {"\uff26\uff35\uff2c\uff2c\u3000\uff37\uff29\uff24\uff34\uff28", "full-width"},
      // Every mark goes, not only those with a combining class: an enclosing mark, a variation selector.
      {"a\u20ddb", "ab"},
      {"Heart\u2764\ufe0fQuest", "heart-quest"},
      {"e\u0301", "e"},
      {std::string(60, 'a'), std::string(48, 'a')},
      {std::string(47, 'a') + " b", std::string(47, 'a')},  // cut at 48, then the trailing hyphen trimmed
      {"\u216b Kingdoms", "xii-kingdoms"},
      {"\ufb01nal \ufb02ight", "final-flight"},
      {"Stra\u00dfe", "stra-e"},
      {"\u03a9mega", "mega"},
      {"\u0130stanbul", "istanbul"},
      {"K\u212a", "kk"},
      {"\u2460\u2461", "12"},
      {"", "game"},
      {"\xff\xfe", "game"},  // not UTF-8
  };
  for (const auto& example : examples) {
    const auto key = NameKey(example.name);
    if (key != example.key) {
      std::fprintf(stderr, "NameKey(%s) = %s, expected %s\n", example.name.c_str(), key.c_str(), example.key.c_str());
      ++failures;
    }
    CHECK(IsStoreKey("imported", key));
  }
}

void GameNames() {
  for (const std::string ok : {std::string("Darkwood"), std::string("\uff26\uff35\uff2c\uff2c\u3000\uff37\uff29\uff24\uff34\uff28"),
                               std::string("Darkwood\u00a0Deluxe"), std::string(120, 'a'), Repeat("\u00e9", 120),
                               std::string("TMNT: Mutant Melee")})
    CHECK(IsGameName(ok));
  for (const std::string bad : {std::string(), std::string("\u3000"), std::string("\u034f"), std::string("\u3000\u034f"),
                                std::string("Dark\u2028wood"), std::string("Dark\u2029wood"), std::string("Dark\nwood"),
                                std::string("Dark\u200bwood"), std::string("\ue000"), std::string(121, 'a'),
                                Repeat("\u00e9", 121), std::string("\u0378x"), std::string("Dark\xffwood")})
    CHECK(!IsGameName(bad));
  CHECK(GameNameFromTitle("Darkwood") == "Darkwood");
  CHECK(GameNameFromTitle("  Dark\nwood\u2028 ") == "Dark wood");
  CHECK(GameNameFromTitle("Dark\u200bwood\x01") == "Darkwood");
  CHECK(GameNameFromTitle("\u200b\u3000").empty());
  CHECK(GameNameFromTitle(std::string(130, 'a')) == std::string(120, 'a'));
}

void StoreKeys() {
  for (const char* ok : {"1", "274520", "4294967295"}) CHECK(IsStoreKey("steam", ok));
  for (const char* bad : {"", "0", "0274520", "4294967296", "99999999999", "274520\n", "274520 ", " 274520", "27452a", "-1"})
    CHECK(!IsStoreKey("steam", bad));
  CHECK(IsStoreKey("gog", "1207658924") && IsStoreKey("gog", "1" + std::string(19, '0')));
  for (const std::string bad : {std::string("1") + std::string(20, '0'), std::string("01"), std::string("1207658924\n"), std::string()})
    CHECK(!IsStoreKey("gog", bad));
  const std::string hex = "0123456789abcdef0123456789abcdef";
  CHECK(IsStoreKey("epic", hex));
  for (const std::string bad : {hex.substr(1), hex + "0", hex + "\n", std::string("0123456789ABCDEF0123456789ABCDEF")})
    CHECK(!IsStoreKey("epic", bad));
  for (const std::string ok : {std::string("tmnt-mutant-melee"), std::string("a"), std::string("12"), std::string(48, 'a')})
    CHECK(IsStoreKey("imported", ok));
  for (const std::string bad : {std::string(), std::string(49, 'a'), std::string("a--b"), std::string("-a"), std::string("a-"),
                                std::string("a\n"), std::string("A"), std::string("a_b"), std::string("\u00e9")})
    CHECK(!IsStoreKey("imported", bad));
  for (const char* store : {"steam", "epic", "gog", "imported"}) CHECK(IsRecipeStore(store));
  for (const char* store : {"local", "Steam", "", "itch", "steam\n"}) CHECK(!IsRecipeStore(store) && !IsStoreKey(store, "1"));
}

void Refs() {
  CHECK(GameRef::Steam(274520) == (GameRef{"steam", "274520"}) && !GameRef::Steam(0).valid());
  CHECK(ToText(GameRef::Steam(274520)) == "steam/274520");
  CHECK(ParseGameRefText("steam/274520") == GameRef::Steam(274520));
  CHECK(ParseGameRefText("imported/tmnt-mutant-melee") == (GameRef{"imported", "tmnt-mutant-melee"}));
  for (const char* bad : {"steam/274520/x", "steam/0", "local/x", "steam", "/274520", "steam/274520\n"})
    CHECK(!ParseGameRefText(bad));
  // A release's order: steam, epic, gog, imported; Steam and GOG keys by value, the others as text.
  const std::vector<GameRef> ordered = {GameRef::Steam(9), GameRef::Steam(10), GameRef::Steam(274520),
                                        {"epic", std::string(31, '0') + "1"}, {"epic", "a" + std::string(31, '0')},
                                        {"gog", "2"}, {"gog", "10"}, {"imported", "a-b"}, {"imported", "ab"}};
  for (std::size_t index = 0; index + 1 < ordered.size(); ++index)
    CHECK(ordered[index] < ordered[index + 1] && !(ordered[index + 1] < ordered[index]));

  const auto local = GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
  CHECK(MatchingGameRef(GameId::Steam(274520), 0) == GameRef::Steam(274520));
  CHECK(MatchingGameRef(GameId::Steam(274520), 220) == GameRef::Steam(274520));  // a Steam game's own ID wins
  // An imported game matches only by its Steam app ID, never by its name.
  CHECK(!MatchingGameRef(local, 0));
  CHECK(MatchingGameRef(local, 274520) == GameRef::Steam(274520));
  CHECK(!MatchingGameRef(GameId{}, 274520));
  // It shares under its Steam app ID, else under its name key.
  CHECK(SharingGameRef(local, 274520, "Darkwood") == GameRef::Steam(274520));
  CHECK(SharingGameRef(local, 0, "TMNT: Mutant Melee") == (GameRef{"imported", "tmnt-mutant-melee"}));
  CHECK(SharingGameRef(local, 0, "") == (GameRef{"imported", "game"}));
  CHECK(SharingGameRef(GameId::Steam(274520), 0, "Darkwood") == GameRef::Steam(274520));
  CHECK(RecipeStoreOf(Store::kLocal) == "imported" && RecipeStoreOf(Store::kSteam) == "steam");
}

}  // namespace

int main() {
  NameKeys();
  GameNames();
  StoreKeys();
  Refs();
  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
