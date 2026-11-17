// SPDX-License-Identifier: GPL-3.0-or-later
// Steam collections (#536): CloudConfigStore.Download's answer for the library namespace, parsed as Playden keeps it,
// and the saved copy. Samples have the shape of real answers, with synthetic IDs, names and apps.
#include "library/collections_cache.h"
#include "steam/backend/collections_client.h"
#include "steam/backend/proto_wire.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

using namespace playden;
using namespace playden::steam;

namespace {
void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

struct Entry {
  std::string key;
  std::string value;
  bool deleted = false;
};

std::vector<std::uint8_t> Answer(const std::vector<Entry>& entries, std::uint32_t space = 1) {
  proto::Writer data(64u << 20);
  data.AddVarint(1, space);
  data.AddVarint(2, 971);
  for (const auto& entry : entries) {
    proto::Writer item;
    item.AddString(1, entry.key);
    if (entry.deleted) item.AddVarint(2, 1);
    if (!entry.value.empty()) item.AddString(3, entry.value);
    item.AddFixed32(4, 1700000000);
    item.AddVarint(5, 900);
    data.AddMessage(3, item);
  }
  data.AddVarint(4, 0);
  proto::Writer answer(64u << 20);
  answer.AddMessage(1, data);
  return answer.bytes();
}

std::string Collection(const std::string& id, const std::string& name, const std::string& added,
                       const std::string& extra = {}) {
  return "{\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"added\":[" + added + "],\"removed\":[]" + extra + "}";
}

void ParsesRealShapes() {
  const std::string filter =
      ",\"filterSpec\":{\"nFormatVersion\":2,\"strSearchText\":\"\",\"filterGroups\":[{\"rgOptions\":[],"
      "\"bAcceptUnion\":true},{\"rgOptions\":[4],\"bAcceptUnion\":false}],\"setSuggestions\":{}}";
  const auto reply = Answer({
      {"showcases.2", "{\"nShowcaseId\":2,\"strCollectionId\":\"recent-games\"}"},
      {"user-collections.favorite", Collection("favorite", "Favorites", "900001,900002")},
      {"user-collections.hidden", Collection("hidden", "Hidden", "900003")},
      {"user-collections.uc-AbCd*+Ef==", Collection("uc-AbCd*+Ef==", "Synthetic shooters", "900004,900005,900006,900006")},
      {"user-collections.srm-Xy12", Collection("srm-Xy12", "Synthetic emulators", "")},
      {"user-collections.uc-Dyn+1", Collection("uc-Dyn+1", "Synthetic filter", "", filter)},
      {"user-collections.uc-Gone", "", true},
      {"user-collections.uc-Utf8", Collection("uc-Utf8", "A\xC3\xA7\xC3\xA3o \xE2\x98\x85", "900001")},
      {"user-collections.uc-Mismatch", Collection("uc-Other", "Wrong ID", "900006")},
      {"user-collections.uc-Bad", "{not json"},
      {"user-collections.uc-NoName", "{\"id\":\"uc-NoName\",\"added\":[220]}"},
      {"user-collections.uc:colon", Collection("uc:colon", "Colon", "900006")},
      {"user-collections.uc-Removed", "{\"id\":\"uc-Removed\",\"name\":\"Removed\",\"added\":[10,20],\"removed\":[20]}"},
      {"NewContentRollup_900099", "{}"},
  });
  CollectionsReadStats stats;
  const auto parsed = ParseLibraryCollections(reply, &stats);
  Check(parsed.has_value(), "a real-shaped answer parses");
  const auto& list = *parsed;
  Check(list.size() == 4, "kept: the static, the empty, the non-ASCII and the one with a removal");
  Check(list[0].id == "uc-AbCd*+Ef==" && list[0].name == "Synthetic shooters" &&
            list[0].members == std::vector<GameId>{GameId::Steam(900004), GameId::Steam(900005), GameId::Steam(900006)},
        "a static collection: its ID as given, its games once each");
  Check(list[1].id == "srm-Xy12" && list[1].members.empty(), "an empty collection is kept");
  Check(list[2].name == "A\xC3\xA7\xC3\xA3o \xE2\x98\x85", "a non-ASCII name is kept as UTF-8");
  Check(list[3].members == std::vector<GameId>{GameId::Steam(10)}, "an app in removed is left out");
  Check(stats.special == 2 && stats.dynamic == 1 && stats.deleted == 1 && stats.invalid == 4 && stats.entries == 14 &&
            stats.collections == 4,
        "favorite/hidden, the dynamic one, the deleted one and four bad ones are counted, not kept");

  Check(ParseLibraryCollections(Answer({{"user-collections.uc-A", Collection("uc-A", "A", "1")}}, 2))->empty(),
        "another namespace's entries are ignored");
  std::vector<std::uint8_t> damaged = reply;
  damaged.resize(damaged.size() / 2);
  Check(!ParseLibraryCollections(damaged).has_value(), "a cut answer is refused as a whole");
  Check(ParseLibraryCollections({})->empty(), "an empty answer has no collections");
}

// More entries than the protocol reader's default field cap (256), as a big library has.
void ManyEntries() {
  std::vector<Entry> entries;
  for (int i = 0; i < 400; ++i) entries.push_back({"NewContentRollup_" + std::to_string(i), "{}"});
  for (int i = 0; i < 300; ++i) {
    const std::string id = "uc-" + std::to_string(i);
    entries.push_back({"user-collections." + id, Collection(id, "C" + std::to_string(i), std::to_string(i + 1))});
  }
  const auto parsed = ParseLibraryCollections(Answer(entries));
  Check(parsed && parsed->size() == 300, "700 entries, 300 collections");
}

void CacheRoundTrip() {
  char root[] = "/tmp/playden-collections-XXXXXX";
  Check(mkdtemp(root) != nullptr, "temporary root");
  Storage storage(root);
  CollectionsCache cache(storage);
  const std::vector<LibraryCollection> collections = {
      {"uc-AbCd*+Ef==", "Synthetic shooters", {GameId::Steam(900004), GameId::Steam(900006)}},
      {"srm-Xy12", "Synthetic emulators", {}},
      {"7", "Classics", {GameId::Gog(1207658924)}},
  };
  Check(cache.Save(76561198000000001ull, collections).ok(), "save");
  auto loaded = cache.Load(76561198000000001ull);
  Check(loaded.ok() && loaded.value() == collections, "round trip");
  Check(!cache.Load(76561198000000002ull).ok(), "another account's file is not read");
  Check(cache.Save(76561198000000001ull, {}).ok() && cache.Load(76561198000000001ull).ok() &&
            cache.Load(76561198000000001ull).value().empty(),
        "an empty list is a list");
  Check(!cache.Save(76561198000000001ull, {{"uc:x", "Bad", {}}}).ok(), "a bad ID is not saved");
  Check(!cache.Save(76561198000000001ull, {{"uc-a", "", {}}}).ok(), "an empty name is not saved");
  Check(!cache.Save(76561198000000001ull, {{"uc-a", "A", {}}, {"uc-a", "B", {}}}).ok(), "a duplicate is not saved");
  Check(cache.Save(76561198000000001ull, collections).ok(), "save again");
  // A damaged file reads as invalid data, never as other collections.
  auto bytes = storage.Read("collections-76561198000000001.cache");
  Check(bytes.ok(), "read the file");
  auto broken = bytes.value();
  broken[broken.size() / 2] ^= 0x5a;
  Check(storage.WriteAtomic("collections-76561198000000001.cache", broken).ok(), "write a damaged copy");
  Check(!cache.Load(76561198000000001ull).ok(), "a damaged file is refused");
  Check(cache.Clear(76561198000000001ull).ok() && !cache.Load(76561198000000001ull).ok(), "clear removes it");
  Check(cache.Clear(76561198000000001ull).ok(), "clearing nothing succeeds");
}
}  // namespace

int main() {
  ParsesRealShapes();
  ManyEntries();
  CacheRoundTrip();
  std::puts("steam collections tests passed");
  return 0;
}
