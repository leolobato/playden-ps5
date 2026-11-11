// store_details_test.cpp - Steam store details parsing and the fetch service.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "steam/store_details_service.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "steam_id.h"

using namespace playden::steam;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
  const std::string body =
      R"({"220":{"success":true,"data":{"steam_appid":220,"short_description":"<b>Half-Life 2</b> &amp; friends:\n )"
      R"(the &quot;best&quot;  game.","controller_support":"full","genres":[{"id":"1","description":"Action"}]}}})";
  const auto details = ParseStoreDetails(220, body);
  CHECK(details && details->id == SteamId(220));
  CHECK(details->description == "Half-Life 2 & friends: the \"best\" game.");
  CHECK(details->genres == std::vector<std::string>{"Action"} && details->controller_support == "full");
  CHECK(!details->vac);  // no categories
  // Category 8 is "Valve Anti-Cheat enabled".
  const auto vac = ParseStoreDetails(240, R"({"240":{"success":true,"data":{"steam_appid":240,"categories":[)"
                                          R"({"id":1,"description":"Multi-player"},{"id":8,"description":"Valve Anti-Cheat enabled"}]}}})");
  CHECK(vac && vac->vac);
  const auto other = ParseStoreDetails(240, R"({"240":{"success":true,"data":{"steam_appid":240,"categories":[{"id":2,"description":"Single-player"}]}}})");
  CHECK(other && !other->vac);
  CHECK(!ParseStoreDetails(221, body));  // another game's answer
  CHECK(!ParseStoreDetails(220, R"({"220":{"success":false}})"));
  CHECK(!ParseStoreDetails(220, R"({"220":{"success":true,"data":{"steam_appid":221}}})"));
  CHECK(!ParseStoreDetails(220, "not json"));
  CHECK(StoreDetailsUrl(220) == "https://store.steampowered.com/api/appdetails?appids=220&l=english");

  std::atomic<int> fetches{0};
  StoreDetailsService service([&](const std::string& url, const std::atomic<bool>&) -> std::optional<std::string> {
    ++fetches;
    if (url == StoreDetailsUrl(220)) return body;
    return std::nullopt;
  });
  service.Request(SteamId(220));
  service.Request(SteamId(220));  // asked once per run
  service.Request(SteamId(999));  // fails: nothing comes back
  std::vector<StoreDetails> results;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((results.empty() || fetches < 2) && std::chrono::steady_clock::now() < deadline) {
    for (auto& item : service.Pump()) results.push_back(std::move(item));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  service.Shutdown();
  CHECK(results.size() == 1 && results[0].id == SteamId(220) && fetches == 2);
  std::puts("store details tests passed");
  return 0;
}
