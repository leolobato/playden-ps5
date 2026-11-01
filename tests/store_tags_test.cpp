// store_tags_test.cpp - Steam store page user tag parsing.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "steam/store_tags_service.h"

#include <cstdio>
#include <cstdlib>
#include "steam_id.h"

using namespace playden::steam;

#define CHECK(x) \
  do { \
    if (!(x)) { \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1); \
    } \
  } while (0)

int main() {
  const std::string page =
      R"(<html><script>{"tags":["Adventure","Roguelike","Pixel Graphics"],"other":1}</script></html>)";
  const auto tags = ParseStoreTagsFromPage(page);
  CHECK(tags.size() == 3);
  CHECK(tags[0] == "Adventure" && tags[1] == "Roguelike" && tags[2] == "Pixel Graphics");
  CHECK(ParseStoreTagsFromPage("no tags here").empty());
  CHECK(StorePageUrl(588650) == "https://store.steampowered.com/app/588650/");
  std::puts("store tags tests passed");
  return 0;
}
