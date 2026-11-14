// SPDX-License-Identifier: GPL-3.0-or-later
// GOG's gen 1 content system without the network: repository.json (IDs and sizes as strings, redist depots, manifest
// names that must be safe), the depots an install takes (Neutral and language depots, the English fallback, systems,
// owned DLC, redists), the plain depot manifests read as a stream (byte ranges of main.bin, folders, links, malformed
// and truncated input, a manifest far past steam::json's limits) and where gen 1 support files go. Fixtures are GOG's
// public CDN answers for Monkey Island 2 Special Edition (fixtures/gog/README.md); everything else is synthetic.
#include "gog/content.h"
#include "gog/depot_reader.h"
#include "gog/manifest_v1.h"
#include "gog/path_rules.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

using namespace playden::gog;
using RStatus = DepotItemReader::Status;

namespace {

constexpr std::uint64_t kMonkey = 1425039730;
std::string g_fixtures;

std::string ReadFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file != nullptr);
  std::string data;
  char buffer[65536];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) data.append(buffer, read);
  CHECK(!std::ferror(file));
  std::fclose(file);
  return data;
}

std::string Fixture(const std::string& name) { return ReadFile(g_fixtures + "/" + name); }

const std::uint8_t* Bytes(const std::string& text) { return reinterpret_cast<const std::uint8_t*>(text.data()); }

// --- Repository -------------------------------------------------------------------------------------------------------

void TestRepositoryFixture() {
  const auto repository = ParseRepositoryV1(Fixture("repository-v1-1425039730.json"));
  CHECK(repository.has_value());
  CHECK(repository->root_product == kMonkey && repository->timestamp == 32971092);
  CHECK(repository->install_directory == "Monkey Island 2 SE");
  CHECK(repository->products == std::vector<std::uint64_t>{kMonkey});
  CHECK(repository->depots.size() == 2);
  const auto& neutral = repository->depots[0];
  CHECK(neutral.languages == std::vector<std::string>{"Neutral"} && neutral.size == 1280640 &&
        neutral.products == std::vector<std::uint64_t>{kMonkey} &&
        neutral.systems == std::vector<std::string>{"Windows"} &&
        neutral.manifest == "1bf89566-2d44-40e7-81b1-06b0c8e716e4.json");
  const auto& english = repository->depots[1];
  CHECK(english.languages == std::vector<std::string>{"English"} && english.size == 1912238511 &&
        english.manifest == "44877313-0ade-439d-88ae-0538513fb165.json");
  CHECK(repository->redists.size() == 4);
  CHECK(repository->redists[0].id == "MSVC2008" &&
        repository->redists[0].executable == "__redist/MSVC2008/vcredist_x86.exe" &&
        repository->redists[0].argument == "/q");
  CHECK(repository->redists[1].id == "dotNet35" && repository->redists[1].argument == "/q /norestart");
  CHECK(repository->redists[2].id == "DirectX" && repository->redists[2].executable == "__redist/DirectX/DXSETUP.exe");
  CHECK(repository->redists[3].id == "language_setup" && repository->redists[3].executable.empty() &&
        repository->redists[3].target_dir == "/");
  CHECK(repository->support_commands == 1 && repository->skipped == 0 && repository->bad_manifests == 0);

  const auto selection = SelectDepotsV1(*repository, {}, "english");
  CHECK(selection.depots.size() == 2 && selection.language == "en-US");
  CHECK(selection.products == std::vector<std::uint64_t>{kMonkey});
  CHECK(selection.size == 1280640ull + 1912238511ull && selection.compressed_size == selection.size);
  CHECK(selection.dependencies ==
        (std::vector<std::string>{"MSVC2008", "dotNet35", "DirectX", "language_setup"}));
}

std::string FileDepot(const std::string& languages, const std::string& products, const std::string& manifest,
                      const std::string& size = "\"100\"", const std::string& extra = "") {
  return R"({"languages":[)" + languages + R"(],"gameIDs":[)" + products + R"(],"manifest":")" + manifest +
         R"(","size":)" + size + extra + "}";
}

std::string Repository(const std::string& depots, const std::string& game_ids = R"({"gameID":"1900000001"})",
                       const std::string& head = R"("version":1,)") {
  return "{" + head + R"("product":{"rootGameID":"1900000001","timestamp":12345,"installDirectory":"Game",)"
                      R"("gameIDs":[)" + game_ids + R"(],"depots":[)" + depots + R"(]}})";
}

void TestRepositorySynthetic() {
  // Manifest names: one that would leave its folder, one with "..", one 70 bytes long, and one with no ".json" are
  // dropped and counted; 64 characters and ".json" is the longest kept.
  const std::string longest = std::string(64, 'a') + ".json";
  const auto names = ParseRepositoryV1(Repository(
      FileDepot(R"("Neutral")", "1900000001", "../x.json") + "," +
      FileDepot(R"("Neutral")", "1900000001", "a/b.json") + "," +
      FileDepot(R"("Neutral")", "1900000001", "a..b.json") + "," +
      FileDepot(R"("Neutral")", "1900000001", std::string(65, 'a') + ".json") + "," +
      FileDepot(R"("Neutral")", "1900000001", "abc") + "," +
      FileDepot(R"("Neutral")", "1900000001", ".json") + "," +
      FileDepot(R"("Neutral")", "1900000001", longest)));
  CHECK(names.has_value() && names->bad_manifests == 6 && names->depots.size() == 1 &&
        names->depots[0].manifest == longest);

  // IDs and sizes as integers too; a size that isn't decimal, a non-canonical ID, no gameIDs, no languages, a bad
  // redist ID: skipped, the rest kept.
  const auto mixed = ParseRepositoryV1(Repository(
      FileDepot(R"("Neutral")", "1900000001", "a.json", "100") + "," +
      FileDepot(R"("Neutral")", "1900000001", "b.json", "\"1.5\"") + "," +
      FileDepot(R"("Neutral")", "1900000001", "c.json", "\"ten\"") + "," +
      FileDepot(R"("Neutral")", "1900000001", "d.json", "-1") + "," +
      FileDepot(R"("Neutral")", "1900000001", "e.json", "\"0100\"") + "," +
      FileDepot(R"("Neutral")", "1900000001", "f.json", "\"2199023255553\"") + "," +  // 2 TiB + 1
      FileDepot(R"("Neutral")", "\"01900000001\"", "g.json") + "," +
      FileDepot(R"("Neutral")", "", "h.json") + "," +
      FileDepot("", "1900000001", "i.json") + "," +
      FileDepot(R"("Neutral")", "1900000001", "j.json", "\"0\"", R"(,"systems":"Windows")") + "," +
      R"({"redist":"../evil"},{"redist":"Ok","executable":7},"not a depot",)" +
      FileDepot(R"("Neutral")", "1900000001", "k.json", "\"0\"")));
  CHECK(mixed.has_value() && mixed->depots.size() == 2 && mixed->depots[0].manifest == "a.json" &&
        mixed->depots[0].size == 100 && mixed->depots[1].manifest == "k.json" && mixed->depots[1].size == 0);
  CHECK(mixed->skipped == 12 && mixed->redists.empty());
  // A gameIDs entry that isn't canonical is skipped, the rest kept.
  const auto game_ids = ParseRepositoryV1(Repository("", R"({"gameID":"1900000001"},{"gameID":"x"},{"gameID":1900000002},5)"));
  CHECK(game_ids.has_value() && game_ids->products == (std::vector<std::uint64_t>{1900000001, 1900000002}) &&
        game_ids->skipped == 2);

  // The root's shape.
  CHECK(!ParseRepositoryV1(""));
  CHECK(!ParseRepositoryV1("not json"));
  CHECK(!ParseRepositoryV1("[]"));
  CHECK(!ParseRepositoryV1(R"([{"version":1}])"));
  CHECK(!ParseRepositoryV1(Repository("", R"({"gameID":"1900000001"})", R"("version":2,)")));
  CHECK(!ParseRepositoryV1(Repository("", R"({"gameID":"1900000001"})", R"("version":"1",)")));
  CHECK(!ParseRepositoryV1(Repository("", R"({"gameID":"1900000001"})", "")));
  CHECK(ParseRepositoryV1(Repository("")).has_value());
  CHECK(!ParseRepositoryV1(R"({"version":1,"product":{"timestamp":1,"depots":[]}})"));
  CHECK(!ParseRepositoryV1(R"({"version":1,"product":{"rootGameID":"0","timestamp":1,"depots":[]}})"));
  CHECK(!ParseRepositoryV1(R"({"version":1,"product":{"rootGameID":"1900000001","depots":[]}})"));
  CHECK(!ParseRepositoryV1(R"({"version":1,"product":{"rootGameID":"1900000001","timestamp":1}})"));
  CHECK(!ParseRepositoryV1(R"({"version":1,"product":{"rootGameID":"1900000001","timestamp":1,"depots":{}}})"));
  CHECK(!ParseRepositoryV1(R"({"version":1,"product":[]})"));
  // Over the 1 MiB cap.
  CHECK(!ParseRepositoryV1(R"({"version":1,"pad":")" + std::string(kMaxRepositoryV1Bytes, 'x') + "\"}"));
}

// --- Selection --------------------------------------------------------------------------------------------------------

RepositoryV1 Parsed(const std::string& json) {
  auto repository = ParseRepositoryV1(json);
  CHECK(repository.has_value());
  return *repository;
}

std::vector<std::string> Manifests(const SelectionV1& selection) {
  std::vector<std::string> names;
  for (const auto& depot : selection.depots) names.push_back(depot.manifest);
  return names;
}

void TestSelection() {
  const std::string base = "1900000001";
  const std::string dlc = "1900000002";
  const std::string other_dlc = "1900000003";
  const auto repository = Parsed(Repository(
      FileDepot(R"("Neutral")", base, "neutral.json", "\"10\"", R"(,"systems":["Windows"])") + "," +
      FileDepot(R"("English")", base, "english.json", "\"20\"", R"(,"systems":["windows"])") + "," +
      FileDepot(R"("German")", base, "german.json", "\"30\"") + "," +
      FileDepot(R"("neutral")", base, "mac.json", "\"40\"", R"(,"systems":["OSX"])") + "," +
      FileDepot(R"("Neutral")", dlc, "dlc.json", "\"50\"") + "," +
      FileDepot(R"("English")", other_dlc, "other-dlc.json", "\"60\"") + "," +
      FileDepot(R"("English","German")", base + "," + dlc, "shared.json", "\"70\"", R"(,"systems":["OSX","Windows"])") +
      R"(,{"redist":"MSVC2008","executable":"__redist/MSVC2008/vcredist_x86.exe","size":"0"},)"
      R"({"redist":"DirectX","size":0},{"redist":"MSVC2008","size":"0"})",
      R"({"gameID":"1900000001"},{"gameID":"1900000002"},{"gameID":"1900000003"})"));

  // English: Neutral and English depots of the base, Windows ones only; no DLC owned.
  const auto english = SelectDepotsV1(repository, {}, "english");
  CHECK(english.language == "en-US");
  CHECK(english.products == std::vector<std::uint64_t>{1900000001});
  CHECK(Manifests(english) == (std::vector<std::string>{"neutral.json", "english.json", "shared.json"}));
  CHECK(english.size == 100 && english.compressed_size == 100);
  // Redists once each, in order, and never among the file depots.
  CHECK(english.dependencies == (std::vector<std::string>{"MSVC2008", "DirectX"}));

  // German is there: it replaces English.
  const auto german = SelectDepotsV1(repository, {}, "german");
  CHECK(german.language == "de-DE");
  CHECK(Manifests(german) == (std::vector<std::string>{"neutral.json", "german.json", "shared.json"}));

  // An owned DLC's depot is kept, an unowned one's dropped; not-a-product IDs in owned change nothing.
  const auto owned = SelectDepotsV1(repository, {1900000002, 1999999999}, "english");
  CHECK(owned.products == (std::vector<std::uint64_t>{1900000001, 1900000002}));
  CHECK(Manifests(owned) == (std::vector<std::string>{"neutral.json", "english.json", "dlc.json", "shared.json"}));
  const auto both = SelectDepotsV1(repository, {1900000003, 1900000002}, "english");
  CHECK(both.products == (std::vector<std::uint64_t>{1900000001, 1900000002, 1900000003}));
  CHECK(Manifests(both) ==
        (std::vector<std::string>{"neutral.json", "english.json", "dlc.json", "other-dlc.json", "shared.json"}));

  // German asked, only English there: English, recorded as en-US.
  const auto english_only = Parsed(Repository(FileDepot(R"("Neutral")", base, "neutral.json") + "," +
                                              FileDepot(R"("English")", base, "english.json")));
  const auto fallback = SelectDepotsV1(english_only, {}, "german");
  CHECK(fallback.language == "en-US");
  CHECK(Manifests(fallback) == (std::vector<std::string>{"neutral.json", "english.json"}));
  // A language the table doesn't know asks for English too.
  CHECK(SelectDepotsV1(english_only, {}, "klingon").language == "en-US");

  // Neither the requested language nor English: the first one the base's depots name.
  const auto french_only = Parsed(Repository(FileDepot(R"("Neutral")", base, "neutral.json") + "," +
                                             FileDepot(R"("French","Polish")", base, "french.json") + "," +
                                             FileDepot(R"("Polish")", base, "polish.json")));
  const auto french = SelectDepotsV1(french_only, {}, "german");
  CHECK(french.language == "fr-FR");
  CHECK(Manifests(french) == (std::vector<std::string>{"neutral.json", "french.json"}));
  // A name the table doesn't know but can be recorded.
  const auto odd = SelectDepotsV1(Parsed(Repository(FileDepot(R"("Neutral")", base, "neutral.json") + "," +
                                                    FileDepot(R"("Klingon")", base, "klingon.json"))),
                                  {}, "english");
  CHECK(odd.language == "Klingon" && odd.depots.size() == 2);
  // Every depot Neutral: the requested language's code is recorded.
  const auto neutral_only = Parsed(Repository(FileDepot(R"("Neutral")", base, "neutral.json")));
  CHECK(SelectDepotsV1(neutral_only, {}, "german").language == "de-DE");
  CHECK(SelectDepotsV1(neutral_only, {}, "klingon").language == "en-US");
  // A German depot for macOS only doesn't make German the language.
  const auto mac_german = Parsed(Repository(FileDepot(R"("German")", base, "german.json", "\"1\"",
                                                      R"(,"systems":["OSX"])") + "," +
                                            FileDepot(R"("English")", base, "english.json")));
  const auto mac = SelectDepotsV1(mac_german, {}, "german");
  CHECK(mac.language == "en-US" && Manifests(mac) == std::vector<std::string>{"english.json"});

  // Each depot's selected product: the first of its gameIDs the install takes. The shared depot names the base game
  // first; a depot naming an unowned DLC first belongs to the base game it was kept for, an owned DLC first to that DLC.
  for (const auto& depot : both.depots)
    CHECK(depot.selected_product == (depot.manifest == "dlc.json"         ? 1900000002u
                                     : depot.manifest == "other-dlc.json" ? 1900000003u
                                                                          : 1900000001u));
  const auto dlc_first = Parsed(Repository(FileDepot(R"("Neutral")", dlc + "," + base, "mixed.json"),
                                           R"({"gameID":"1900000001"},{"gameID":"1900000002"})"));
  const auto unowned = SelectDepotsV1(dlc_first, {}, "english");
  CHECK(unowned.depots.size() == 1 && unowned.depots[0].selected_product == 1900000001);
  CHECK(unowned.depots[0].products.front() == 1900000002);  // the manifest's URL still names it
  const auto owned_first = SelectDepotsV1(dlc_first, {1900000002}, "english");
  CHECK(owned_first.depots.size() == 1 && owned_first.depots[0].selected_product == 1900000002);
}

// --- Depot manifests --------------------------------------------------------------------------------------------------

struct Read {
  RStatus status = RStatus::kOk;
  std::vector<DepotItem> items;
  std::size_t peak = 0;
};

Read ReadItems(const std::string& json, std::size_t piece = 0) {
  Read read;
  auto reader = DepotItemReaderV1([&read](const DepotItem& item) {
    read.items.push_back(item);
    return true;
  });
  const std::size_t step = piece ? piece : std::max<std::size_t>(json.size(), 1);
  for (std::size_t at = 0; at < json.size() && read.status == RStatus::kOk; at += step)
    read.status = reader.Feed(Bytes(json) + at, std::min(step, json.size() - at));
  if (read.status == RStatus::kOk) read.status = reader.Finish();
  CHECK(reader.items() == read.items.size());
  read.peak = reader.peak_item_bytes();
  return read;
}

bool SameItems(const std::vector<DepotItem>& left, const std::vector<DepotItem>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const auto& a = left[i];
    const auto& b = right[i];
    if (a.type != b.type || a.path != b.path || a.size != b.size || a.md5 != b.md5 || a.support != b.support ||
        a.executable != b.executable || a.offset != b.offset || a.blob_product != b.blob_product ||
        !a.chunks.empty() || !b.chunks.empty())
      return false;
  }
  return true;
}

const DepotItem* FindItem(const std::vector<DepotItem>& items, const std::string& path) {
  for (const auto& item : items)
    if (item.path == path) return &item;
  return nullptr;
}

std::string Manifest(const std::string& files) {
  return R"({"version":1,"depot":{"name":"Game","files":[)" + files + "]}}";
}

const char kHash[] = "4bc0f38e5298ab0ac22e913c52568064";

std::string File(const std::string& path, const std::string& extra = "") {
  return R"({"offset":0,"path":")" + path + R"(","size":10,"hash":")" + kHash + R"(","url":"1900000001/main.bin")" +
         extra + "}";
}

void TestDepotFixtures() {
  // The Neutral depot: one support file, the Galaxy setup program, at the start of main.bin.
  const auto setup = ReadItems(Fixture("depot-v1-1425039730-1bf89566-2d44-40e7-81b1-06b0c8e716e4.json"));
  CHECK(setup.status == RStatus::kOk && setup.items.size() == 1);
  const auto& galaxy = setup.items[0];
  CHECK(galaxy.type == DepotItem::Type::kFile && galaxy.path == "1425039730/galaxy_monkey_island2_se_2.0.0.10.exe" &&
        galaxy.offset == 0 && galaxy.size == 1280640 && galaxy.md5 == "4bc0f38e5298ab0ac22e913c52568064" &&
        galaxy.support && !galaxy.executable && galaxy.blob_product == kMonkey && galaxy.chunks.empty());

  // The English depot: 40 files, in pieces as small as a byte.
  const std::string game_json = Fixture("depot-v1-1425039730-44877313-0ade-439d-88ae-0538513fb165.json");
  const auto game = ReadItems(game_json);
  CHECK(game.status == RStatus::kOk && game.items.size() == 40);
  for (const std::size_t piece : {1, 7, 4096}) {
    const auto pieces = ReadItems(game_json, piece);
    CHECK(pieces.status == RStatus::kOk && SameItems(game.items, pieces.items));
  }
  CHECK(std::all_of(game.items.begin(), game.items.end(), [](const DepotItem& item) {
    return item.type == DepotItem::Type::kFile && item.blob_product == kMonkey && item.md5.size() == 32;
  }));
  CHECK(std::count_if(game.items.begin(), game.items.end(), [](const DepotItem& item) { return item.support; }) == 1);
  const auto* lang = FindItem(game.items, "1425039730/app/lang.ini");
  CHECK(lang && lang->support && lang->offset == 1913519129 && lang->size == 22 &&
        lang->md5 == "be631539c7e5b61bdc2c2cc43dee871c");
  const auto* language_setup = FindItem(game.items, "/language_setup.ini");
  CHECK(language_setup && !language_setup->support && language_setup->offset == 1280640 &&
        language_setup->size == 514 && language_setup->md5 == "c18fe9aa538bec2159c509875cf610ee");
  const auto* info = FindItem(game.items, "/goggame-1425039730.info");
  CHECK(info && info->offset == 509672286 && info->size == 798);
  // The two depots' files sum to the repository's sizes.
  std::uint64_t total = 0;
  for (const auto& item : game.items) total += item.size;
  CHECK(total == 1912238511ull);

  // The gen 2 reader finds no items[] in a gen 1 manifest, and the gen 1 reader no files[] in a gen 2 one.
  {
    DepotItemReader v2([](const DepotItem&) { return true; });
    CHECK(v2.Feed(Bytes(game_json), game_json.size()) == RStatus::kOk && v2.Finish() == RStatus::kMalformed);
  }
  CHECK(ReadItems(R"({"depot":{"items":[]},"version":2})").status == RStatus::kMalformed);
  CHECK(ReadItems(Manifest("")).status == RStatus::kOk);

  // Where the files go: <product>/app/ support into the game folder, other <product>/ support to the support folder,
  // a plain path at the root.
  PathSet set;
  std::vector<std::string> placed;
  std::size_t index = 0;
  for (const auto* items : {&setup.items, &game.items})
    for (const auto& item : *items) {
      const auto added = set.Add(item.path, PathKind::kFile, kMonkey, item.support, index++, 1);
      CHECK(added.ok());
      placed.push_back(added.path);
    }
  const auto has = [&placed](const std::string& path) {
    return std::find(placed.begin(), placed.end(), path) != placed.end();
  };
  CHECK(has("lang.ini"));
  CHECK(has(".playden-gog/support/1425039730/galaxy_monkey_island2_se_2.0.0.10.exe"));
  CHECK(has("language_setup.ini") && has("goggame-1425039730.info"));
  CHECK(!has("1425039730/app/lang.ini"));
}

void TestPlaceFileV1() {
  using Placed = std::optional<std::string>;
  CHECK(PlaceFileV1("1425039730/app/lang.ini", kMonkey, true) == Placed("lang.ini"));
  CHECK(PlaceFileV1("1425039730/App/sub/x.ini", kMonkey, true) == Placed("sub/x.ini"));
  CHECK(PlaceFileV1("1425039730/galaxy_x.exe", kMonkey, true) ==
        Placed(".playden-gog/support/1425039730/galaxy_x.exe"));
  // Without the prefix, a support path is placed as gen 2 places it.
  CHECK(PlaceFileV1("app/x.conf", kMonkey, true) == Placed("x.conf"));
  CHECK(PlaceFileV1("save/x.dat", kMonkey, true) == Placed(".playden-gog/support/1425039730/save/x.dat"));
  // Another product's prefix stays; a non-support file keeps its path; the prefix alone is not stripped.
  CHECK(PlaceFileV1("1900000001/x.exe", kMonkey, true) ==
        Placed(".playden-gog/support/1425039730/1900000001/x.exe"));
  CHECK(PlaceFileV1("1425039730/app/lang.ini", kMonkey, false) == Placed("1425039730/app/lang.ini"));
  CHECK(PlaceFileV1("14250397300/x", kMonkey, true) == Placed(".playden-gog/support/1425039730/14250397300/x"));
  // Playden's own names stay out of reach.
  PathRefusal why = PathRefusal::kNone;
  CHECK(!PlaceFileV1("1425039730/app/.playden-files", kMonkey, true, &why) && why == PathRefusal::kReserved);
  CHECK(PlaceFileV1("1425039730/.playden-files", kMonkey, true) ==
        Placed(".playden-gog/support/1425039730/.playden-files"));
  // Through the path set: the same rules after NormalizePath, and gen 2 keeps its own.
  PathSet set;
  CHECK(set.Add("/1425039730/app/lang.ini", PathKind::kFile, kMonkey, true, 0, 1).path == "lang.ini");
  CHECK(set.Add("1425039730\\app\\other.ini", PathKind::kFile, kMonkey, true, 1, 1).path == "other.ini");
  CHECK(set.Add("1425039730/app/third.ini", PathKind::kFile, kMonkey, true, 2).path ==
        ".playden-gog/support/1425039730/1425039730/app/third.ini");
}

void TestDepotShapes() {
  const std::string json = Manifest(
      File("a.exe", R"(,"executable":true)") + "," + R"({"directory":true,"path":"/Saves"},)" +
      R"({"path":"/Contents/Frameworks/SDL2","target":"Versions/Current/SDL2","symlinkType":"file"},)" +
      R"({"path":"/link","symlinkType":"directory","size":10,"offset":0,"hash":")" + kHash + R"("},)" +
      R"({"path":"/nosize","offset":0,"hash":")" + kHash + R"("},)" +
      R"({"path":"/noblob","offset":"5","size":"10","hash":")" + kHash + R"("},)" +
      R"({"path":"/folder-not","directory":false,"offset":0,"size":0,"hash":")" + kHash + R"(","url":"7/main.bin"},)" +
      R"({"offset":18446744073709551605,"path":"/last","size":10,"hash":")" + kHash + R"("})");
  const auto read = ReadItems(json, 3);
  CHECK(read.status == RStatus::kOk && read.items.size() == 8);
  CHECK(read.items[0].type == DepotItem::Type::kFile && read.items[0].executable && read.items[0].size == 10 &&
        read.items[0].blob_product == 1900000001);
  CHECK(read.items[1].type == DepotItem::Type::kDirectory && read.items[1].path == "/Saves");
  CHECK(read.items[2].type == DepotItem::Type::kLink && read.items[3].type == DepotItem::Type::kLink &&
        read.items[4].type == DepotItem::Type::kLink);
  CHECK(read.items[5].type == DepotItem::Type::kFile && read.items[5].blob_product == 0 && read.items[5].offset == 5 &&
        read.items[5].size == 10);
  CHECK(read.items[6].type == DepotItem::Type::kFile && read.items[6].blob_product == 7 && read.items[6].size == 0);
  CHECK(read.items[7].offset == 18446744073709551605ull);
  // A link is refused by the path rules.
  PathSet set;
  CHECK(set.Add(read.items[2].path, PathKind::kLink, kMonkey, false, 0, 1).refusal == PathRefusal::kLink);
}

void TestDepotMalformed() {
  const std::string hash = std::string(R"(,"hash":")") + kHash + "\"";
  const std::vector<std::string> bad = {
      R"({"offset":0,"path":"a","size":10,"url":"1900000001/main.bin"})",                        // no hash
      R"({"offset":0,"path":"a","size":10,"hash":"4BC0F38E5298AB0AC22E913C52568064"})",          // not lowercase hex
      R"({"offset":0,"path":"a","size":10,"hash":"zzc0f38e5298ab0ac22e913c52568064"})",          // not hex
      R"({"offset":0,"path":"a","size":10,"hash":"4bc0f38e"})",                                  // too short
      R"({"offset":0,"path":"a","size":10,"hash":7})",                                           // not a string
      R"({"offset":18446744073709551610,"path":"a","size":10)" + hash + "}",                    // offset + size > 2^64
      R"({"offset":18446744073709551616,"path":"a","size":0)" + hash + "}",                     // offset > 2^64
      R"({"offset":0,"path":"a","size":1099511627777)" + hash + "}",                            // size > kMaxFileBytes
      R"({"offset":0,"path":"a","size":-1)" + hash + "}",
      R"({"offset":0,"path":"a","size":1.5)" + hash + "}",
      R"({"offset":0,"path":"a","size":"010")" + hash + "}",
      R"({"path":"a","size":10)" + hash + "}",                                                  // no offset
      R"({"offset":-1,"path":"a","size":10)" + hash + "}",
      R"({"offset":0,"size":10)" + hash + "}",                                                  // no path
      R"({"offset":0,"path":7,"size":10)" + hash + "}",
      R"({"directory":true})",                                                                   // a folder needs one
      R"({"directory":"yes","path":"a"})",
      R"({"offset":0,"path":"a","size":10,"url":"x/main.bin")" + hash + "}",                    // url's product
      R"({"offset":0,"path":"a","size":10,"url":"01900000001/main.bin")" + hash + "}",
      R"({"offset":0,"path":"a","size":10,"url":"1900000001")" + hash + "}",
      R"({"offset":0,"path":"a","size":10,"url":"1900000001/")" + hash + "}",
      R"({"offset":0,"path":"a","size":10,"url":7)" + hash + "}",
      R"({"offset":0,"path":"a","size":10,"support":"true")" + hash + "}",
      R"({"offset":0,"path":"a","size":10,"executable":1)" + hash + "}",
  };
  for (const auto& element : bad) {
    const auto json = Manifest(File("ok") + "," + element);
    const auto read = ReadItems(json);
    if (read.status != RStatus::kMalformed) std::fprintf(stderr, "not refused: %s\n", element.c_str());
    CHECK(read.status == RStatus::kMalformed && read.items.size() == 1);
    CHECK(ReadItems(json, 1).status == RStatus::kMalformed);
  }
  // Not an object; not the manifest's shape.
  CHECK(ReadItems(Manifest(R"("a")")).status == RStatus::kMalformed);
  CHECK(ReadItems(R"({"version":1})").status == RStatus::kMalformed);
  CHECK(ReadItems("[]").status == RStatus::kMalformed);
  CHECK(ReadItems("").status == RStatus::kMalformed);

  // Truncated mid-element, or anywhere else: Feed accepts every piece, Finish refuses.
  const std::string json = Fixture("depot-v1-1425039730-44877313-0ade-439d-88ae-0538513fb165.json");
  const auto element = json.find("\"hash\"", json.find("/language_setup.ini") - 200);
  CHECK(element != std::string::npos);
  for (const std::size_t cut : {std::size_t{1}, element, json.size() / 2, json.size() - 1}) {
    auto reader = DepotItemReaderV1([](const DepotItem&) { return true; });
    CHECK(reader.Feed(Bytes(json), cut) == RStatus::kOk);
    CHECK(reader.Finish() == RStatus::kMalformed);
  }

  // The callback says stop.
  std::size_t seen = 0;
  auto reader = DepotItemReaderV1([&seen](const DepotItem&) { return ++seen < 2; });
  const auto three = Manifest(File("a") + "," + File("b") + "," + File("c"));
  CHECK(reader.Feed(Bytes(three), three.size()) == RStatus::kRefused && seen == 2);
}

void TestDepotLarge() {
  // 100000 files: several MiB and 600000 values, far past steam::json's 1 MiB and 4096, through a buffer the size of
  // one file.
  std::string json = R"({"version":1,"depot":{"name":"Big","files":[)";
  std::uint64_t offset = 0;
  for (int i = 0; i < 100000; ++i) {
    if (i) json += ",";
    const std::uint64_t size = 1000 + static_cast<std::uint64_t>(i % 13);
    json += R"({"offset":)" + std::to_string(offset) + R"(,"path":"/data/folder)" + std::to_string(i % 97) +
            "/file" + std::to_string(i) + R"(.dat","size":)" + std::to_string(size) + R"(,"hash":")" + kHash +
            R"(","url":"1900000001/main.bin"})";
    offset += size;
  }
  json += "]}}";
  CHECK(json.size() > 10 * 1024 * 1024);
  CHECK(!playden::steam::json::Parse(json).has_value());
  const auto read = ReadItems(json, 65536);
  CHECK(read.status == RStatus::kOk && read.items.size() == 100000);
  CHECK(read.peak > 0 && read.peak < 512);
  CHECK(read.items.back().path == "/data/folder89/file99999.dat");
  std::uint64_t total = 0;
  for (const auto& item : read.items) total += item.size;
  CHECK(total == offset && read.items.back().offset + read.items.back().size == offset);
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_fixtures = argv[1];
  TestRepositoryFixture();
  TestRepositorySynthetic();
  TestSelection();
  TestDepotFixtures();
  TestPlaceFileV1();
  TestDepotShapes();
  TestDepotMalformed();
  TestDepotLarge();
  std::printf("gog_manifest_v1 ok\n");
  return 0;
}
