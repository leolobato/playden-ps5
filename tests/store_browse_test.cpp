// store_browse_test.cpp - batched Steam store tags, screenshots and trailers.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "platform/storage.h"
#include "steam/store_browse_service.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using playden::GameId;

using namespace playden::steam;

#define CHECK(x) \
  do { \
    if (!(x)) { \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
      std::exit(1); \
    } \
  } while (0)

const char* kTags = R"({"response":{"tags":[{"tagid":21,"name":"Adventure"},{"tagid":4182,"name":"Singleplayer"}]}})";

const char* kItems = R"({"response":{"store_items":[{
  "appid": 620,
  "tags": [{"tagid": 21, "weight": 10}, {"tagid": 4182, "weight": 9}, {"tagid": 999999, "weight": 1}],
  "screenshots": {"all_ages_screenshots": [
    {"filename": "steam/apps/620/ss_one.jpg?t=1", "ordinal": 1},
    {"filename": "steam/apps/620/ss_two.jpg?t=1", "ordinal": 2}
  ]},
  "trailers": {"highlights": [{
    "trailer_url_format": "steam/apps/${FILENAME}?t=5",
    "screenshot_medium": "5787/movie.293x165.jpg",
    "microtrailer": [
      {"filename": "620/3746/clip.webm", "type": "video/webm"},
      {"filename": "620/3746/clip.mp4", "type": "video/mp4"}
    ]
  }]},
  "basic_info": {"short_description": "A puzzle-platformer."},
  "categories": {"controller_categoryids": [28, 18]}
}]}})";

std::string ItemsFor(const std::string& url) {
  std::string items = "{\"response\":{\"store_items\":[";
  bool any = false;
  for (const char* id : {"11", "12", "13", "14", "620"}) {
    const std::string key = std::string("appid%22%3A") + id;
    if (url.find(key) == std::string::npos) continue;
    if (any) items += ',';
    any = true;
    items += std::string("{\"appid\":") + id + ",\"tags\":[{\"tagid\":21}]}";
  }
  items += "]}}";
  return items;
}

void ParsedFixtureNamesShotsAndTrailer() {
  const auto tags = ParseStoreTagList(kTags);
  CHECK(tags.at(21) == "Adventure" && tags.at(4182) == "Singleplayer");
  const auto items = ParseStoreBrowseItems(kItems, tags);
  CHECK(items.size() == 1 && items[0].app == 620 && items[0].tags_known);
  CHECK(items[0].tags.size() == 2 && items[0].tags[0] == "Adventure" && items[0].tags[1] == "Singleplayer");
  CHECK(items[0].screenshots.size() == 2);
  CHECK(items[0].screenshots[0] ==
        "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/620/ss_one.jpg?t=1");
  CHECK(items[0].trailer_thumbnail ==
        "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/5787/movie.293x165.jpg?t=5");
  CHECK(items[0].trailer_url ==
        "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/620/3746/clip.mp4?t=5");
  CHECK(items[0].description_known && items[0].description == "A puzzle-platformer.");
  CHECK(items[0].controller_support == "full");
  CHECK(!items[0].genres_known);
  const char* partial = R"({"response":{"store_items":[{"appid":1,"categories":{"controller_categoryids":[18]}}]}})";
  const auto partial_items = ParseStoreBrowseItems(partial, tags);
  CHECK(partial_items.size() == 1 && partial_items[0].controller_support == "partial" && !partial_items[0].description_known);
  CHECK(ParseStoreBrowseItems(kItems, {}).size() == 1 && !ParseStoreBrowseItems(kItems, {})[0].tags_known);
}

struct Script {
  std::mutex mutex;
  std::vector<std::string> items;
  std::atomic<int> item_calls{0};
  std::atomic<bool> release{false};
  std::optional<std::string> operator()(const std::string& url, const std::atomic<bool>& cancel) {
    if (url.find("GetTagList") != std::string::npos) return std::string(kTags);
    if (url.find("GetItems") == std::string::npos) return std::nullopt;
    const int call = ++item_calls;
    {
      std::lock_guard<std::mutex> lock(mutex);
      items.push_back(url);
    }
    if (call == 1) {
      while (!release.load() && !cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ItemsFor(url);
  }
};

void WaitItems(Script& script, int count) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (script.item_calls.load() < count && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(script.item_calls.load() >= count);
}

std::vector<StoreBrowseItem> WaitResults(StoreBrowseService& service, std::size_t count) {
  std::vector<StoreBrowseItem> got;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (got.size() < count && std::chrono::steady_clock::now() < deadline) {
    for (auto& item : service.Pump(8)) got.push_back(std::move(item));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(got.size() >= count);
  return got;
}

void OpenGameIsNextAndAFreshFileSendsNothing() {
  const auto root = std::filesystem::temp_directory_path() / "playden-store-browse-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  playden::Storage storage(root.string());
  CHECK(storage.EnsureRootDirectory().ok());
  Script script;
  const auto now = std::make_shared<std::atomic<std::uint64_t>>(1700000000);
  {
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return script(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.Request(GameId::Steam(11));
    WaitItems(script, 1);
    service.Request(GameId::Steam(12));
    service.Request(GameId::Steam(13));
    service.RequestFirst(GameId::Steam(14));
    script.release = true;
    const auto got = WaitResults(service, 4);
    CHECK(got[0].app == 11 && got[1].app == 14 && got[2].app == 12 && got[3].app == 13);
    CHECK(got[0].tags_known && got[0].tags.size() == 1 && got[0].tags[0] == "Adventure");
    std::lock_guard<std::mutex> lock(script.mutex);
    CHECK(script.items.size() >= 2);
    const auto first = script.items[1].find("appid%22%3A14");
    const auto second = script.items[1].find("appid%22%3A12");
    CHECK(first != std::string::npos && second != std::string::npos && first < second);
  }
  Script quiet;
  {
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return quiet(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.RequestFirst(GameId::Steam(14));
    const auto got = WaitResults(service, 1);
    CHECK(got[0].app == 14 && got[0].tags_known && quiet.item_calls.load() == 0);
    now->store(1700000000ull + kStoreBrowseTtlSeconds);
    service.Request(GameId::Steam(11));
    WaitItems(quiet, 1);
  }
  std::filesystem::remove_all(root);
}

void AppendNumber(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    out.push_back(static_cast<std::uint8_t>(value));
    value >>= 8;
  }
}

void AppendString(std::vector<std::uint8_t>& out, const std::string& value) {
  AppendNumber(out, value.size(), 2);
  out.insert(out.end(), value.begin(), value.end());
}

std::uint32_t Checksum(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t crc = 0xffffffffu;
  for (const auto byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// A version 1 record: tags and a trailer slot, and no description or genres.
std::vector<std::uint8_t> VersionOneRecord() {
  std::vector<std::uint8_t> out = {'P', 'D', 'N', 'S', 'T', 'B', 0, 1};
  AppendNumber(out, 1700000000, 8);
  AppendNumber(out, 1, 4);
  AppendNumber(out, 21, 4);
  AppendString(out, "Adventure");
  AppendNumber(out, 1, 4);
  AppendNumber(out, 42, 4);
  AppendNumber(out, 1700000000, 8);
  AppendNumber(out, 1, 1);
  AppendNumber(out, 1, 1);
  AppendString(out, "Adventure");
  AppendNumber(out, 0, 1);
  AppendString(out, "");
  AppendString(out, "");
  AppendNumber(out, Checksum(out), 4);
  return out;
}

void VersionOneStaysFreshUntilGenresArePatched() {
  const auto root = std::filesystem::temp_directory_path() / "playden-store-browse-details";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  playden::Storage storage(root.string());
  CHECK(storage.EnsureRootDirectory().ok());
  CHECK(storage.WriteAtomic("store-browse.cache", VersionOneRecord()).ok());
  Script quiet;
  const auto now = std::make_shared<std::atomic<std::uint64_t>>(1700000100);
  {
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return quiet(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    service.Request(GameId::Steam(42));
    const auto got = WaitResults(service, 1);
    CHECK(got[0].app == 42 && got[0].tags_known && got[0].tags.size() == 1 && got[0].tags[0] == "Adventure");
    CHECK(!got[0].description_known && !got[0].genres_known && quiet.item_calls.load() == 0);
    service.RememberDetails(GameId::Steam(42), "A short description.", {"Puzzle"}, "partial");
    const auto patched = service.Cached(GameId::Steam(42));
    CHECK(patched && patched->description == "A short description." && patched->controller_support == "partial");
    CHECK(patched->genres_known && patched->genres.size() == 1 && patched->genres[0] == "Puzzle");
    CHECK(patched->tags_known && patched->tags.size() == 1 && patched->tags[0] == "Adventure");
    CHECK(quiet.item_calls.load() == 0);
  }
  Script again;
  {
    StoreBrowseService service([&](const std::string& url, const std::atomic<bool>& cancel) { return again(url, cancel); },
                               std::chrono::milliseconds(0));
    service.SetStorage(&storage);
    service.SetNow([now] { return now->load(); });
    const auto cached = service.Cached(GameId::Steam(42));
    CHECK(cached && cached->description_known && cached->genres_known);
    CHECK(cached->description == "A short description." && cached->genres.size() == 1 && cached->genres[0] == "Puzzle");
    CHECK(cached->tags.size() == 1 && cached->tags[0] == "Adventure");
    service.Request(GameId::Steam(42));
    const auto got = WaitResults(service, 1);
    CHECK(got[0].description == "A short description." && got[0].genres_known && got[0].genres.size() == 1);
    CHECK(again.item_calls.load() == 0);
  }
  std::filesystem::remove_all(root);
}

void TwentyTagsAreKept() {
  std::string tags = R"({"response":{"tags":[)";
  std::string item = R"({"response":{"store_items":[{"appid":7,"tags":[)";
  for (int id = 1; id <= 20; ++id) {
    if (id > 1) {
      tags += ',';
      item += ',';
    }
    tags += "{\"tagid\":" + std::to_string(id) + ",\"name\":\"Tag" + std::to_string(id) + "\"}";
    item += "{\"tagid\":" + std::to_string(id) + "}";
  }
  tags += "]}}";
  item += "]}]}}";
  const auto names = ParseStoreTagList(tags);
  const auto items = ParseStoreBrowseItems(item, names);
  CHECK(items.size() == 1 && items[0].tags.size() == 20 && items[0].tags.back() == "Tag20");
}

int main() {
  ParsedFixtureNamesShotsAndTrailer();
  OpenGameIsNextAndAFreshFileSendsNothing();
  VersionOneStaysFreshUntilGenresArePatched();
  TwentyTagsAreKept();
  std::puts("store browse tests passed");
  return 0;
}
