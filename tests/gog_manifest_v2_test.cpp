// SPDX-License-Identifier: GPL-3.0-or-later
// GOG's gen 2 content system without the network: the build list (the default branch, gen 1, the link form),
// CDN endpoint order and URLs, build metas and the depots an install takes (languages, owned DLC, bitness), the
// streaming zlib inflate (bombs, truncation, trailing bytes), the streaming depot manifest reader (piece sizes, key
// order, malformed and truncated input, a manifest far past steam::json's limits) and the public dependency store.
// Fixtures are GOG's public CDN answers (fixtures/gog/README.md); everything else is synthetic.
#include "gog/content.h"
#include "gog/depot_reader.h"
#include "gog/hash.h"
#include "gog/inflate.h"
#include "gog/languages.h"
#include "gog/path_rules.h"

#include <miniz.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
using ZStatus = ZlibStream::Status;
using RStatus = DepotItemReader::Status;
using Kind = BuildChoice::Kind;

namespace {

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

std::string Compress(const std::string& raw) {
  mz_ulong size = mz_compressBound(static_cast<mz_ulong>(raw.size()));
  std::string out(size, '\0');
  CHECK(mz_compress2(reinterpret_cast<unsigned char*>(&out[0]), &size, Bytes(raw), static_cast<mz_ulong>(raw.size()),
                     9) == MZ_OK);
  out.resize(size);
  return out;
}

// Inflates `zlib` fed in pieces of `piece` bytes (0: whole); the result's status is Feed's first error or Finish's.
ZStatus Inflate(const std::string& zlib, std::uint64_t cap, std::string* out, std::size_t piece = 0) {
  ZlibStream stream(cap);
  const auto sink = [out](const std::uint8_t* data, std::size_t size) {
    out->append(reinterpret_cast<const char*>(data), size);
    return true;
  };
  const std::size_t step = piece ? piece : std::max<std::size_t>(zlib.size(), 1);
  for (std::size_t at = 0; at < zlib.size(); at += step) {
    const auto status = stream.Feed(Bytes(zlib) + at, std::min(step, zlib.size() - at), sink);
    if (status != ZStatus::kOk) return status;
  }
  return stream.Finish();
}

std::string InflateFixture(const std::string& name) {
  std::string out;
  CHECK(Inflate(Fixture(name), kMaxDepotManifestBytes, &out) == ZStatus::kOk);
  return out;
}

struct Read {
  RStatus status = RStatus::kOk;
  std::vector<DepotItem> items;
  std::size_t peak = 0;
};

Read ReadItems(const std::string& json, std::size_t piece = 0) {
  Read read;
  DepotItemReader reader([&read](const DepotItem& item) {
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
    if (a.type != b.type || a.path != b.path || a.size != b.size || a.md5 != b.md5 || a.sha256 != b.sha256 ||
        a.support != b.support || a.executable != b.executable || a.chunks.size() != b.chunks.size())
      return false;
    for (std::size_t c = 0; c < a.chunks.size(); ++c)
      if (a.chunks[c].md5 != b.chunks[c].md5 || a.chunks[c].compressed_md5 != b.chunks[c].compressed_md5 ||
          a.chunks[c].size != b.chunks[c].size || a.chunks[c].compressed_size != b.chunks[c].compressed_size)
        return false;
  }
  return true;
}

const DepotItem* FindItem(const std::vector<DepotItem>& items, const std::string& path) {
  for (const auto& item : items)
    if (item.path == path) return &item;
  return nullptr;
}

std::size_t CountWith(const std::vector<DepotItem>& items, char c) {
  return static_cast<std::size_t>(std::count_if(items.begin(), items.end(), [c](const DepotItem& item) {
    return item.path.find(c) != std::string::npos;
  }));
}

const char kChunk[] = R"({"compressedMd5":"9acee277e9c356401e7de39cb6549942","compressedSize":935,)"
                      R"("md5":"5cad677703c03f633abcdb750f6ed00c","size":14234})";

std::string Manifest(const std::string& items) { return R"({"depot":{"items":[)" + items + R"(]},"version":2})"; }

std::string FileItem(const std::string& path, const std::string& chunks = kChunk, const std::string& extra = "") {
  return R"({"chunks":[)" + chunks + R"(],"path":")" + path + R"(","type":"DepotFile")" + extra + "}";
}

// --- Hashes -----------------------------------------------------------------------------------------------------------

void TestHashes() {
  Md5 md5;
  CHECK(md5.FinalHex() == "d41d8cd98f00b204e9800998ecf8427e");
  md5.Update("a", 1);
  md5.Update("bc", 2);
  CHECK(md5.FinalHex() == "900150983cd24fb0d6963f7d28e17f72");
  md5.Update("abc", 3);  // FinalHex started over
  CHECK(md5.FinalHex() == "900150983cd24fb0d6963f7d28e17f72");
  Sha256 sha;
  CHECK(sha.FinalHex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  sha.Update("ab", 2);
  sha.Update("c", 1);
  CHECK(sha.FinalHex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  CHECK(IsMd5Hex("bab75eccd96f44493a4b715dbb9b53c4"));
  CHECK(!IsMd5Hex("BAB75ECCD96F44493A4B715DBB9B53C4"));
  CHECK(!IsMd5Hex("bab75eccd96f44493a4b715dbb9b53c"));
  CHECK(!IsMd5Hex("bab75eccd96f44493a4b715dbb9b53c4a"));
  CHECK(!IsMd5Hex("gab75eccd96f44493a4b715dbb9b53c4"));
  CHECK(!IsMd5Hex(""));
  CHECK(IsSha256Hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK(!IsSha256Hex("e3b0c44298fc1c149afbf4c8996fb924"));
}

// --- Builds and endpoints ---------------------------------------------------------------------------------------------

void TestChooseBuild() {
  const auto jazz = ChooseBuild(Fixture("builds-v2-branches-first-1351891846.json"));
  CHECK(jazz.kind == Kind::kGen2);
  CHECK(jazz.build_id == "50747586883966799");  // items[5], after five JJ2+ Mod builds; not items[6], also default
  CHECK(jazz.generation == 2);
  CHECK(jazz.meta_id == "bab75eccd96f44493a4b715dbb9b53c4");
  CHECK(jazz.meta.size() == 2 && jazz.meta[0].name == "fastly" && jazz.meta[1].name == "gcore");
  CHECK(FillUrl(jazz.meta[0], "") ==
        std::optional<std::string>(
            "https://gog-cdn-fastly.gog.com/content-system/v2/meta/ba/b7/bab75eccd96f44493a4b715dbb9b53c4"));

  // Gen 1: the item's own build ID, legacy_build_id as the timestamp, and only fastly (gcore supports gen 2 only).
  const std::string monkey_body = Fixture("builds-v2-gen1-only-1425039730.json");
  const auto monkey = ChooseBuild(monkey_body);
  CHECK(monkey.kind == Kind::kGen1);
  CHECK(monkey.build_id == "15751" && monkey.generation == 1 && monkey.timestamp == 32971092);
  CHECK(monkey.meta.empty() && monkey.meta_id.empty());
  CHECK(monkey.repository.size() == 1 && monkey.repository[0].name == "fastly");
  CHECK(FillUrl(monkey.repository[0], "") ==
        std::optional<std::string>("https://gog-cdn-fastly.gog.com/content-system/v1/manifests/1425039730/windows/"
                                   "32971092/repository.json"));
  const auto fallout_gen1 = ChooseBuild(Fixture("builds-gen1-1440148836.json"));
  CHECK(fallout_gen1.kind == Kind::kGen1 && fallout_gen1.build_id == "48338268125133736" &&
        fallout_gen1.timestamp == 46964530 && fallout_gen1.repository.size() == 1);
  {
    // A URL naming another timestamp, another product or another file; no legacy_build_id, a bad one, no product_id;
    // the link form; no endpoint for gen 1.
    const auto edited = [&monkey_body](const std::string& from, const std::string& to) {
      std::string body = monkey_body;
      CHECK(body.find(from) != std::string::npos);
      for (auto at = body.find(from); at != std::string::npos; at = body.find(from, at + to.size()))
        body.replace(at, from.size(), to);
      return ChooseBuild(body).kind;
    };
    CHECK(edited(R"(\/32971092\/)", R"(\/32971093\/)") == Kind::kProtocol);
    CHECK(edited(R"(\/1425039730\/)", R"(\/1425039731\/)") == Kind::kProtocol);
    CHECK(edited("repository.json", "repository.jso") == Kind::kProtocol);
    CHECK(edited(R"(\/content-system\/v1\/)", R"(\/content-system\/v2\/)") == Kind::kProtocol);
    CHECK(edited("\"legacy_build_id\": 32971092", "\"legacy\": 32971092") == Kind::kProtocol);
    CHECK(edited("\"legacy_build_id\": 32971092", "\"legacy_build_id\": \"032971092\"") == Kind::kProtocol);
    CHECK(edited("\"legacy_build_id\": 32971092", "\"legacy_build_id\": \"32971092\"") == Kind::kGen1);
    CHECK(edited("\"product_id\": \"1425039730\"", "\"product\": \"1425039730\"") == Kind::kProtocol);
    CHECK(edited("\"urls\"", "\"link\"") == Kind::kProtocol);
    CHECK(edited("1,\n                        2", "2") == Kind::kProtocol);
  }

  const auto fallout = ChooseBuild(Fixture("builds-v2-1440148836.json"));
  CHECK(fallout.kind == Kind::kGen2);
  CHECK(fallout.build_id == "51985043610075177");
  CHECK(fallout.meta_id == "fe5c3c87a9b7a30f5ce2f4547ab05309");

  // Asked without _version=2: a `link` and no `urls`.
  CHECK(ChooseBuild(Fixture("builds-link-form-1351891846.json")).kind == Kind::kProtocol);

  // No default-branch Windows build: a branch build and a macOS one only, or no items at all.
  const std::string urls =
      R"("urls":[{"endpoint_name":"fastly","url":"https://gog-cdn-fastly.gog.com/content-system/v2/meta/ba/b7/)"
      R"(bab75eccd96f44493a4b715dbb9b53c4","url_format":"https://gog-cdn-fastly.gog.com/content-system/v2/meta/ba/b7/)"
      R"(bab75eccd96f44493a4b715dbb9b53c4","parameters":{},"priority":10,"supports_generation":[1,2],)"
      R"("fallback_only":false}])";
  CHECK(ChooseBuild(R"({"items":[{"build_id":"1","os":"windows","branch":"beta","generation":2,)" + urls +
                    R"(},{"build_id":"2","os":"osx","branch":null,"generation":2,)" + urls + "}]}")
            .kind == Kind::kNone);
  CHECK(ChooseBuild(R"({"total_count":0,"count":0,"items":[]})").kind == Kind::kNone);
  // An absent branch is the default branch too.
  const auto absent = ChooseBuild(R"({"items":[{"build_id":"77","os":"windows","generation":2,)" + urls + "}]}");
  CHECK(absent.kind == Kind::kGen2 && absent.build_id == "77");
  // Not a builds answer.
  CHECK(ChooseBuild("").kind == Kind::kProtocol);
  CHECK(ChooseBuild("[]").kind == Kind::kProtocol);
  CHECK(ChooseBuild(R"({"items":{}})").kind == Kind::kProtocol);
  CHECK(ChooseBuild(R"({"items":[{"build_id":"07","os":"windows","generation":2,)" + urls + "}]}").kind ==
        Kind::kProtocol);
  CHECK(ChooseBuild(R"({"items":[{"build_id":"7","os":"windows","generation":3,)" + urls + "}]}").kind ==
        Kind::kProtocol);
  // A meta URL whose last segment isn't an MD5.
  std::string bad_meta = urls;
  bad_meta.replace(bad_meta.find("bab75ecc"), 8, "BAB75ECC");
  bad_meta.replace(bad_meta.find("bab75ecc"), 8, "BAB75ECC");
  CHECK(ChooseBuild(R"({"items":[{"build_id":"7","os":"windows","generation":2,)" + bad_meta + "}]}").kind ==
        Kind::kProtocol);
  // Over the 1 MiB cap.
  std::string big = R"({"items":[],"pad":")" + std::string(kMaxBuildsBytes, 'x') + R"("})";
  CHECK(ChooseBuild(big).kind == Kind::kProtocol);
}

std::vector<CdnEndpoint> Endpoints(const std::string& json) {
  playden::steam::json::Limits limits;
  const auto value = playden::steam::json::Parse(json, limits);
  CHECK(value.has_value());
  return ParseEndpoints(*value);
}

void TestOrderEndpoints() {
  // gcore listed first, fastly second: fastly is tried first anyway.
  const auto endpoints = Endpoints(
      R"([{"endpoint_name":"gcore","url":"https://gog-cdn.gcdn.co/m","url_format":"https://gog-cdn.gcdn.co/m",)"
      R"("parameters":{},"priority":1,"max_fails":100,"supports_generation":[2],"fallback_only":true},)"
      R"({"endpoint_name":"fastly","url":"https://gog-cdn-fastly.gog.com/m","url_format":"https://gog-cdn-fastly.gog.com/m",)"
      R"("parameters":{},"priority":10,"max_fails":100,"supports_generation":[1,2],"fallback_only":false}])");
  CHECK(endpoints.size() == 2);
  const auto gen2 = OrderEndpoints(endpoints, 2);
  CHECK(gen2.size() == 2 && gen2[0].name == "fastly" && gen2[1].name == "gcore");
  const auto gen1 = OrderEndpoints(endpoints, 1);
  CHECK(gen1.size() == 1 && gen1[0].name == "fastly");

  // fallback_only goes last whatever its priority; ties keep their order.
  const auto mixed = OrderEndpoints(Endpoints(
      R"([{"endpoint_name":"a","url":"https://a.example/","priority":99,"supports_generation":[2],"fallback_only":true},)"
      R"({"endpoint_name":"b","url":"https://b.example/","priority":5,"supports_generation":[2],"fallback_only":false},)"
      R"({"endpoint_name":"c","url":"https://c.example/","priority":5,"supports_generation":[2],"fallback_only":false},)"
      R"({"endpoint_name":"d","url":"https://d.example/","priority":7,"supports_generation":[2]},)"
      R"({"endpoint_name":"e","url":"https://e.example/","priority":100,"supports_generation":[2],"fallback_only":true},)"
      R"({"endpoint_name":"f","url":"https://f.example/","priority":3}])"), 2);
  CHECK(mixed.size() == 5);  // f supports no generation
  CHECK(mixed[0].name == "d" && mixed[1].name == "b" && mixed[2].name == "c" && mixed[3].name == "e" &&
        mixed[4].name == "a");

  // Entries of another shape are skipped.
  CHECK(Endpoints(R"([1,"x",{"endpoint_name":"none"},{"url":"https://x.example/","parameters":{"n":1.5}},)"
                  R"({"url":"https://x.example/","priority":"high"},{"url":"https://x.example/","parameters":[]},)"
                  R"({"url":"https://x.example/","supports_generation":["2"]},{"url":"https://ok.example/"}])")
            .size() == 1);
  CHECK(Endpoints(R"({"url":"https://x.example/"})").empty());
}

void TestFillUrl() {
  const std::string md5 = "abcdef0123456789abcdef0123456789";
  const auto galaxy = GalaxyPath(md5);
  CHECK(galaxy == std::optional<std::string>("ab/cd/" + md5));
  CHECK(!GalaxyPath("ABCDEF0123456789ABCDEF0123456789"));
  CHECK(!GalaxyPath("abcdef0123456789abcdef012345678"));
  CHECK(!GalaxyPath("../../etc/passwd"));
  CHECK(!GalaxyPath(""));

  // The secure link shapes (GOG_PROTOCOL.md §4), with made-up tokens.
  const auto fastly = Endpoints(
      R"([{"endpoint_name":"fastly","url":"","url_format":"{base_url}/token=nva={expires_at}~dirs={dirs}~token={token}{path}",)"
      R"("parameters":{"base_url":"https://gog-cdn-fastly.gog.com","path":"/content-system/v2/store/1900000001",)"
      R"("expires_at":1763200000,"dirs":4,"token":"madeUpToken0123"},"priority":10,"supports_generation":[1,2],)"
      R"("fallback_only":false}])");
  CHECK(fastly.size() == 1 && fastly[0].parameters.at("expires_at") == "1763200000");
  CHECK(FillUrl(fastly[0], "/" + *galaxy) ==
        std::optional<std::string>("https://gog-cdn-fastly.gog.com/token=nva=1763200000~dirs=4~token=madeUpToken0123"
                                   "/content-system/v2/store/1900000001/ab/cd/" + md5));
  const auto gcore = Endpoints(
      R"([{"endpoint_name":"gcore","url_format":"{base_url}/{path}?wsSecret={token}&wsTime={time}&prefix={prefix}",)"
      R"("parameters":{"base_url":"https://gog-cdn.gcdn.co","path":"content-system/v2/store/1900000001",)"
      R"("token":"madeUpSecret","time":1763200000,"prefix":"/content-system/v2/store/1900000001"},"priority":1,)"
      R"("supports_generation":[2],"fallback_only":true}])");
  CHECK(gcore.size() == 1);
  CHECK(FillUrl(gcore[0], "/" + *galaxy) ==
        std::optional<std::string>("https://gog-cdn.gcdn.co/content-system/v2/store/1900000001/ab/cd/" + md5 +
                                   "?wsSecret=madeUpSecret&wsTime=1763200000&prefix=/content-system/v2/store/1900000001"));
  // The endpoint is unchanged: the next chunk starts from the same path.
  CHECK(gcore[0].parameters.at("path") == "content-system/v2/store/1900000001");

  // An unknown or unclosed placeholder, a value that names a placeholder (not expanded again), not https.
  auto endpoint = fastly[0];
  endpoint.url_format = "{base_url}/{nope}{path}";
  CHECK(!FillUrl(endpoint, "/x"));
  endpoint.url_format = "{base_url}/{path";
  CHECK(!FillUrl(endpoint, "/x"));
  endpoint.url_format = "{base_url}{path}";
  endpoint.parameters["path"] = "/{token}";
  CHECK(FillUrl(endpoint, "/x") == std::optional<std::string>("https://gog-cdn-fastly.gog.com/{token}/x"));
  endpoint.parameters["base_url"] = "http://gog-cdn-fastly.gog.com";
  CHECK(!FillUrl(endpoint, "/x"));
  endpoint.parameters["base_url"] = "https://gog cdn";
  CHECK(!FillUrl(endpoint, "/x"));
  // No placeholder: url + append (open links and build metas).
  CdnEndpoint open;
  open.url = open.url_format = "https://gog-cdn-fastly.gog.com/content-system/v2/dependencies/store";
  CHECK(FillUrl(open, "/ab/cd/" + md5) ==
        std::optional<std::string>("https://gog-cdn-fastly.gog.com/content-system/v2/dependencies/store/ab/cd/" + md5));
  open.url = open.url_format = "ftp://example/x";
  CHECK(!FillUrl(open, ""));
}

// --- Metas and selection ----------------------------------------------------------------------------------------------

Meta MetaFixture(const std::string& name) {
  std::string raw;
  CHECK(Inflate(Fixture(name), kMaxMetaBytes, &raw) == ZStatus::kOk);
  const auto meta = ParseMeta(raw);
  CHECK(meta.has_value());
  return *meta;
}

std::vector<std::string> Manifests(const Selection& selection) {
  std::vector<std::string> manifests;
  for (const auto& depot : selection.depots) manifests.push_back(depot.manifest);
  return manifests;
}

void TestLanguages() {
  CHECK(GogCode("english") == "en-US" && GogCode("German") == "de-DE" && GogCode("schinese") == "zh-Hans");
  CHECK(GogCode("koreana") == "ko-KR" && GogCode("brazilian") == "pt-BR" && GogCode("latam") == "es-MX");
  CHECK(GogCode("klingon").empty() && GogCode("arabic").empty());
  CHECK(LanguageMatches({"en"}, "english") && LanguageMatches({"EN-us"}, "english") &&
        LanguageMatches({"English"}, "english"));
  CHECK(LanguageMatches({"*"}, "german") && LanguageMatches({"*"}, "klingon"));
  CHECK(LanguageMatches({"fr-FR", "de"}, "german"));
  CHECK(!LanguageMatches({"en-US"}, "german"));
  CHECK(!LanguageMatches({"es-ES"}, "latam") && !LanguageMatches({"es-MX"}, "spanish"));
  CHECK(!LanguageMatches({"pt-PT"}, "brazilian") && LanguageMatches({"br"}, "brazilian"));
  CHECK(LanguageMatches({"cn"}, "schinese") && !LanguageMatches({"zh-Hans"}, "tchinese"));
  CHECK(!LanguageMatches({"en-US"}, "klingon"));
  CHECK(SteamCodeFor("jp") == "japanese" && SteamCodeFor("en") == "english" && SteamCodeFor("xx-XX").empty());
}

void TestMetaAndSelection() {
  // Jazz Jackrabbit 2: depots use the deprecated "en"; offlineDepot is not a depot.
  const auto jazz = MetaFixture("meta-v2-1351891846.zlib");
  CHECK(jazz.base_product == 1351891846 && jazz.build_id == "50747586883966799");
  CHECK(jazz.install_directory == "Jazz Jackrabbit 2");
  CHECK(jazz.depots.size() == 4 && jazz.dependencies.empty());
  CHECK(jazz.products.size() == 1 && jazz.products[0].first == 1351891846 &&
        jazz.products[0].second == "Jazz Jackrabbit 2");
  const auto jazz_en = SelectDepots(jazz, {}, "english");
  CHECK(jazz_en.depots.size() == 4 && jazz_en.language == "en-US");
  CHECK(jazz_en.products == std::vector<std::uint64_t>{1351891846});
  CHECK(jazz_en.size == 60391253ull + 9469 + 2152 + 1313);
  CHECK(jazz_en.compressed_size == 53421013ull + 8550 + 378 + 596);
  // A language the game lacks resolves to English.
  const auto jazz_de = SelectDepots(jazz, {}, "german");
  CHECK(jazz_de.language == "en-US" && jazz_de.depots.size() == 4);

  // Prison Architect: every depot 64-bit, 11 DLC in the meta (gog depots only). Two are owned, one product in the
  // owned set isn't in the meta.
  const auto prison = MetaFixture("meta-v2-dlc-osbitness-1441974651.zlib");
  CHECK(prison.products.size() == 12 && prison.depots.size() == 25);
  CHECK(prison.dependencies == (std::vector<std::string>{"MSVC2010", "MSVC2013", "MSVC2017"}));
  const std::set<std::uint64_t> owned = {1441974651, 1126027458, 1619024184, 1900000999};
  const auto prison_pick = SelectDepots(prison, owned, "english");
  // Meta order of products, not the set's: Psych Ward (1619024184) before Jungle Pack (1126027458).
  CHECK(prison_pick.products == (std::vector<std::uint64_t>{1441974651, 1619024184, 1126027458}));
  CHECK(prison_pick.depots.size() == 7);
  CHECK(Manifests(prison_pick) ==
        (std::vector<std::string>{"b1b516678797013ca63b16803818cfc0", "87a25f1f87451fa36c15d31cb4c0b38a",
                                  "1172b09cd0174a6026b7cf35d02ea379", "f7554359d552ad6a50320b249fafb088",
                                  "8771e9cbafe3e33e24c5de31f8a3274a", "c170e98668773b25c51d1a1d15c9dcb1",
                                  "31d615f26de4965b3c19c9eb528de7ef"}));
  for (const auto& depot : prison_pick.depots) CHECK(depot.bitness == std::vector<std::string>{"64"});
  CHECK(SelectDepots(prison, {}, "english").depots.size() == 3);

  // Beneath a Steel Sky: 21 one-file gog depots, three per language, plus the main depot (7 languages) and a "*" one.
  const auto bass = MetaFixture("meta-v2-scummvm-1207658695.zlib");
  CHECK(bass.depots.size() == 23 && bass.dependencies == std::vector<std::string>{"ScummVM"});
  const auto bass_de = SelectDepots(bass, {}, "german");
  CHECK(bass_de.language == "de-DE");
  CHECK(Manifests(bass_de) ==
        (std::vector<std::string>{"50c89f991ff3404280e6a694d4a1559e", "3e3df6073b2ce049d214717d5e27429c",
                                  "830dd7fe17440e1769f850bdc6b66f09", "61cfeb0a38aadf8d5010fe58e59e5919",
                                  "a6077325740fc0c52578496d04eb4886"}));
  const auto bass_en = SelectDepots(bass, {}, "english");
  CHECK(bass_en.language == "en-US");
  CHECK(Manifests(bass_en) ==
        (std::vector<std::string>{"50c89f991ff3404280e6a694d4a1559e", "3e3df6073b2ce049d214717d5e27429c",
                                  "71501484c57871b262fc3d955e80f5cf", "72404ef9377d4a3ec3c42e7e76838775",
                                  "a6077325740fc0c52578496d04eb4886"}));
  for (const auto& depot : bass_en.depots)
    CHECK(!depot.gog_depot || depot.languages == std::vector<std::string>{"en-US"});
  // Swedish is there (sv-SE); Japanese isn't, so English.
  CHECK(SelectDepots(bass, {}, "swedish").language == "sv-SE");
  const auto bass_ja = SelectDepots(bass, {}, "japanese");
  CHECK(bass_ja.language == "en-US" && Manifests(bass_ja) == Manifests(bass_en));

  // The Elder Scrolls: Arena needs DOSBox from the dependency store.
  const auto arena = MetaFixture("meta-v2-dosbox-1435828982.zlib");
  CHECK(arena.dependencies == std::vector<std::string>{"DOSBox074_2CS"});
  CHECK(arena.install_directory == "Arena" && SelectDepots(arena, {}, "english").depots.size() == 5);
}

std::string Depot(const std::string& product, const std::string& languages, const std::string& manifest,
                  const std::string& extra = "") {
  return R"({"productId":")" + product + R"(","languages":[)" + languages + R"(],"manifest":")" + manifest +
         R"(","size":10,"compressedSize":5)" + extra + "}";
}

const char kM1[] = "11111111111111111111111111111111";
const char kM2[] = "22222222222222222222222222222222";
const char kM3[] = "33333333333333333333333333333333";
const char kM4[] = "44444444444444444444444444444444";
const char kM5[] = "55555555555555555555555555555555";

std::string SyntheticMeta(const std::string& depots, const std::string& extra = "") {
  return R"({"baseProductId":"1900000001","buildId":"1","clientId":"50000000000000001","clientSecret":"00",)"
         R"("depots":[)" + depots + R"(],"installDirectory":"Synthetic","products":[{"name":"Base","productId":)"
         R"("1900000001"},{"name":"DLC","productId":"1900000002"}],"version":2)" + extra + "}";
}

void TestSyntheticSelection() {
  // A product with only 32-bit depots keeps them; a product with both keeps the 64-bit ones and "*".
  const auto bits = ParseMeta(SyntheticMeta(
      Depot("1900000001", R"("en-US")", kM1, R"(,"osBitness":["32"])") + "," +
      Depot("1900000001", R"("en-US")", kM2, R"(,"osBitness":["32"],"isGogDepot":true)") + "," +
      Depot("1900000002", R"("*")", kM3, R"(,"osBitness":["32"])") + "," +
      Depot("1900000002", R"("*")", kM4, R"(,"osBitness":["64"])") + "," +
      Depot("1900000002", R"("*")", kM5, R"(,"osBitness":["*"])")));
  CHECK(bits.has_value());
  const auto picked = SelectDepots(*bits, {1900000001, 1900000002}, "english");
  CHECK(Manifests(picked) == (std::vector<std::string>{kM1, kM2, kM4, kM5}));
  // A 32-bit game depot next to an untagged gog depot and a "*" one: no depot names 64, so the game stays.
  const auto mixed = ParseMeta(SyntheticMeta(
      Depot("1900000001", R"("en-US")", kM1, R"(,"osBitness":["32"])") + "," +
      Depot("1900000001", R"("en-US")", kM2, R"(,"isGogDepot":true)") + "," +
      Depot("1900000001", R"("*")", kM3, R"(,"osBitness":["*"])")));
  CHECK(mixed.has_value());
  CHECK(Manifests(SelectDepots(*mixed, {}, "english")) == (std::vector<std::string>{kM1, kM2, kM3}));

  // A "*"-only main depot carries no language in particular: the gog depots' languages decide.
  const auto star = ParseMeta(SyntheticMeta(
      Depot("1900000001", R"("*")", kM1) + "," + Depot("1900000001", R"("en-US")", kM2, R"(,"isGogDepot":true)") +
      "," + Depot("1900000001", R"("de-DE")", kM3, R"(,"isGogDepot":true)")));
  CHECK(star.has_value());
  const auto star_de = SelectDepots(*star, {}, "german");
  CHECK(star_de.language == "de-DE" && Manifests(star_de) == (std::vector<std::string>{kM1, kM3}));
  const auto star_fr = SelectDepots(*star, {}, "french");
  CHECK(star_fr.language == "en-US" && Manifests(star_fr) == (std::vector<std::string>{kM1, kM2}));

  // Neither the requested language nor English: the base depots' first language, even one with no Steam name.
  const auto odd = ParseMeta(SyntheticMeta(Depot("1900000001", R"("xx-XX","yy-YY")", kM1) + "," +
                                           Depot("1900000001", R"("yy-YY")", kM2) + "," +
                                           Depot("1900000001", R"("xx-XX")", kM3, R"(,"isGogDepot":true)")));
  CHECK(odd.has_value());
  const auto odd_pick = SelectDepots(*odd, {}, "english");
  CHECK(odd_pick.language == "xx-XX" && Manifests(odd_pick) == (std::vector<std::string>{kM1, kM3}));
  // A first language the plan can't record (a tab, over 16 bytes) is passed over for the next one; with none left,
  // it is as if the depots named "*": the requested language is recorded and only "*" depots go.
  const auto tabbed = ParseMeta(SyntheticMeta(Depot("1900000001", R"("x\ty","yy-YY")", kM1) + "," +
                                              Depot("1900000001", R"("yy-YY")", kM2) + "," +
                                              Depot("1900000001", R"("*")", kM3, R"(,"isGogDepot":true)")));
  CHECK(tabbed.has_value());
  const auto tabbed_pick = SelectDepots(*tabbed, {}, "english");
  CHECK(tabbed_pick.language == "yy-YY" && Manifests(tabbed_pick) == (std::vector<std::string>{kM1, kM2, kM3}));
  const auto long_code = ParseMeta(SyntheticMeta(Depot("1900000001", R"("abcdefghijklmnopq")", kM1) + "," +
                                                 Depot("1900000001", R"("*")", kM2, R"(,"isGogDepot":true)")));
  CHECK(long_code.has_value());
  const auto long_pick = SelectDepots(*long_code, {}, "german");
  CHECK(long_pick.language == "de-DE" && Manifests(long_pick) == (std::vector<std::string>{kM2}));
  // A deprecated name the table knows, spaces and all, still counts, and its plain code is recorded.
  const auto named = ParseMeta(SyntheticMeta(Depot("1900000001", R"("Brazilian Portuguese")", kM1) + "," +
                                             Depot("1900000001", R"("pt-BR")", kM2, R"(,"isGogDepot":true)")));
  CHECK(named.has_value());
  const auto named_pick = SelectDepots(*named, {}, "german");
  CHECK(named_pick.language == "pt-BR" && Manifests(named_pick) == (std::vector<std::string>{kM1, kM2}));
  CHECK(PlainLanguageCode("en-US") && PlainLanguageCode("English") && PlainLanguageCode("zh_Hans"));
  CHECK(!PlainLanguageCode("") && !PlainLanguageCode("a b") && !PlainLanguageCode("x\ty") &&
        !PlainLanguageCode("abcdefghijklmnopq") && !PlainLanguageCode("*"));
  const auto ja = ParseMeta(SyntheticMeta(Depot("1900000001", R"("jp")", kM1) + "," +
                                          Depot("1900000001", R"("ja-JP")", kM2, R"(,"isGogDepot":true)")));
  CHECK(ja.has_value());
  const auto ja_pick = SelectDepots(*ja, {}, "german");
  CHECK(ja_pick.language == "ja-JP" && Manifests(ja_pick) == (std::vector<std::string>{kM1, kM2}));

  // Refused metas.
  const auto good = Depot("1900000001", R"("en-US")", kM1);
  CHECK(ParseMeta(SyntheticMeta(good)).has_value());
  CHECK(!ParseMeta(SyntheticMeta(Depot("01900000001", R"("en-US")", kM1))));
  CHECK(!ParseMeta(SyntheticMeta(Depot("18446744073709551616", R"("en-US")", kM1))));
  CHECK(ParseMeta(SyntheticMeta(Depot("18446744073709551615", R"("en-US")", kM1))).has_value());
  CHECK(!ParseMeta(SyntheticMeta(Depot("1900000001", R"("en-US")", "ABCDEF0123456789ABCDEF0123456789"))));
  CHECK(!ParseMeta(SyntheticMeta(Depot("1900000001", "", kM1))));
  CHECK(!ParseMeta(SyntheticMeta(Depot("1900000001", "1", kM1))));
  std::string negative = good;
  negative.replace(negative.find(R"("size":10)"), 9, R"("size":-1)");
  CHECK(!ParseMeta(SyntheticMeta(negative)));
  std::string huge = good;
  huge.replace(huge.find(R"("size":10)"), 9, R"("size":1099511627777)");
  CHECK(!ParseMeta(SyntheticMeta(huge)));
  std::string tib = good;
  tib.replace(tib.find(R"("size":10)"), 9, R"("size":1099511627776)");
  CHECK(ParseMeta(SyntheticMeta(tib)).has_value());
  std::string fraction = good;
  fraction.replace(fraction.find(R"("size":10)"), 9, R"("size":1.5)");
  CHECK(!ParseMeta(SyntheticMeta(fraction)));
  CHECK(!ParseMeta(SyntheticMeta(good, R"(,"dependencies":["../x"])")));
  std::string v1 = SyntheticMeta(good);
  v1.replace(v1.find(R"("version":2)"), 11, R"("version":1)");
  CHECK(!ParseMeta(v1));
  CHECK(!ParseMeta("[]"));
  CHECK(!ParseMeta(R"({"baseProductId":"1","buildId":"1"})"));
}

// --- ZlibStream -------------------------------------------------------------------------------------------------------

void TestZlibStream() {
  // Every fixture meta inflates, whole and a byte at a time, to the same bytes.
  for (const char* name : {"meta-v2-1351891846.zlib", "meta-v2-dlc-osbitness-1441974651.zlib",
                           "meta-v2-scummvm-1207658695.zlib", "meta-v2-dosbox-1435828982.zlib"}) {
    const auto zlib = Fixture(name);
    std::string whole, bytewise;
    CHECK(Inflate(zlib, kMaxMetaBytes, &whole) == ZStatus::kOk);
    CHECK(Inflate(zlib, kMaxMetaBytes, &bytewise, 1) == ZStatus::kOk);
    CHECK(!whole.empty() && whole == bytewise && whole.front() == '{');
  }

  // Many output buffers' worth from a few input bytes, fed a byte at a time: the full window drains.
  std::string text;
  for (int i = 0; text.size() < 1024 * 1024; ++i) text += "line " + std::to_string(i) + " of a long text\n";
  const auto compressed = Compress(text);
  std::string out;
  CHECK(Inflate(compressed, text.size(), &out, 1) == ZStatus::kOk && out == text);  // exactly at the cap
  out.clear();
  CHECK(Inflate(compressed, text.size(), &out, 3) == ZStatus::kOk && out == text);

  // A bomb: 64 MiB of zeros in a few KiB stops at the cap, having handed out no byte past it.
  const auto bomb = Compress(std::string(64 * 1024 * 1024, '\0'));
  CHECK(bomb.size() < 128 * 1024);
  const std::uint64_t cap = 1024 * 1024;
  std::uint64_t handed = 0;
  {
    ZlibStream stream(cap);
    const auto status = stream.Feed(Bytes(bomb), bomb.size(), [&handed](const std::uint8_t*, std::size_t size) {
      handed += size;
      return true;
    });
    CHECK(status == ZStatus::kTooLarge);
    CHECK(handed <= cap && stream.total_out() == handed);
    // The error sticks.
    CHECK(stream.Feed(Bytes(bomb), 1, [](const std::uint8_t*, std::size_t) { return true; }) == ZStatus::kTooLarge);
    CHECK(stream.Finish() == ZStatus::kTooLarge);
  }
  // One byte under the output: too large; at it: fine.
  out.clear();
  CHECK(Inflate(compressed, text.size() - 1, &out) == ZStatus::kTooLarge && out.size() <= text.size() - 1);

  // Truncated: Finish before the end, at any point.
  out.clear();
  CHECK(Inflate(compressed.substr(0, compressed.size() / 2), text.size(), &out) == ZStatus::kTruncated);
  out.clear();
  CHECK(Inflate(compressed.substr(0, compressed.size() - 1), text.size(), &out) == ZStatus::kTruncated);
  out.clear();
  CHECK(Inflate("", text.size(), &out) == ZStatus::kTruncated);
  // Garbage, no zlib header (raw deflate), a bad Adler-32.
  out.clear();
  CHECK(Inflate("this is not zlib at all", 1024, &out) == ZStatus::kCorrupt);
  out.clear();
  CHECK(Inflate(compressed.substr(2), text.size(), &out) == ZStatus::kCorrupt);
  std::string bad_trailer = compressed;
  bad_trailer.back() = static_cast<char>(bad_trailer.back() ^ 0x01);
  out.clear();
  CHECK(Inflate(bad_trailer, text.size(), &out) == ZStatus::kCorrupt);
  // Bytes after the end: in the same piece, or in a later one.
  out.clear();
  CHECK(Inflate(compressed + "xy", text.size(), &out) == ZStatus::kTrailing);
  {
    ZlibStream stream(text.size());
    const auto sink = [](const std::uint8_t*, std::size_t) { return true; };
    CHECK(stream.Feed(Bytes(compressed), compressed.size(), sink) == ZStatus::kOk && stream.ended());
    CHECK(stream.Feed(Bytes(compressed), 0, sink) == ZStatus::kOk);
    CHECK(stream.Feed(Bytes(compressed), 1, sink) == ZStatus::kTrailing);
    CHECK(stream.Finish() == ZStatus::kTrailing);
  }
  // The callback says stop.
  {
    ZlibStream stream(text.size());
    CHECK(stream.Feed(Bytes(compressed), compressed.size(), [](const std::uint8_t*, std::size_t) { return false; }) ==
          ZStatus::kRefused);
  }
  // An empty stream is valid.
  out.clear();
  CHECK(Inflate(Compress(""), 0, &out) == ZStatus::kOk && out.empty());
}

// --- DepotItemReader --------------------------------------------------------------------------------------------------

void TestDepotFixtures() {
  // Jazz Jackrabbit 2's main depot: `/` paths, sfcRef on some items, a small-files container after items.
  const auto jazz = InflateFixture("depot-v2-1351891846.zlib");
  const auto whole = ReadItems(jazz);
  CHECK(whole.status == RStatus::kOk && whole.items.size() == 250);
  for (const std::size_t piece : {1, 7, 4096}) {
    const auto pieces = ReadItems(jazz, piece);
    CHECK(pieces.status == RStatus::kOk && SameItems(whole.items, pieces.items));
  }
  CHECK(CountWith(whole.items, '/') > 0 && CountWith(whole.items, '\\') == 0);
  CHECK(std::all_of(whole.items.begin(), whole.items.end(), [](const DepotItem& item) {
    return item.type == DepotItem::Type::kFile && item.chunks.size() == 1 && item.md5 == item.chunks[0].md5 &&
           item.size == item.chunks[0].size && !item.support;
  }));
  CHECK(std::count_if(whole.items.begin(), whole.items.end(), [](const DepotItem& item) { return item.executable; }) ==
        2);
  const auto* colon = FindItem(whole.items, "Colon1.j2m");
  CHECK(colon && colon->size == 14234 && colon->chunks[0].compressed_md5 == "9acee277e9c356401e7de39cb6549942" &&
        colon->chunks[0].compressed_size == 935 && colon->md5 == "5cad677703c03f633abcdb750f6ed00c");
  CHECK(FindItem(whole.items, "Tiles/j1night.anm"));

  // The manifest straight from the zlib stream into the reader, in small pieces: what a download does.
  {
    const auto zlib = Fixture("depot-v2-1351891846.zlib");
    std::vector<DepotItem> streamed;
    DepotItemReader reader([&streamed](const DepotItem& item) {
      streamed.push_back(item);
      return true;
    });
    ZlibStream stream(kMaxDepotManifestBytes);
    RStatus read = RStatus::kOk;
    const auto sink = [&reader, &read](const std::uint8_t* data, std::size_t size) {
      read = reader.Feed(data, size);
      return read == RStatus::kOk;
    };
    for (std::size_t at = 0; at < zlib.size(); at += 13)
      CHECK(stream.Feed(Bytes(zlib) + at, std::min<std::size_t>(13, zlib.size() - at), sink) == ZStatus::kOk);
    CHECK(stream.Finish() == ZStatus::kOk && reader.Finish() == RStatus::kOk);
    CHECK(SameItems(whole.items, streamed));
  }

  // The Elder Scrolls: Arena's: `\` paths, a zero-chunk file, a four-chunk file with its own md5, sha256 on some.
  const auto arena = ReadItems(InflateFixture("depot-v2-1435828982.zlib"), 7);
  CHECK(arena.status == RStatus::kOk && arena.items.size() == 323);
  CHECK(CountWith(arena.items, '\\') > 0 && CountWith(arena.items, '/') == 0);
  CHECK(FindItem(arena.items, "SPEECH\\1302_10A.VOC"));
  const auto* empty = FindItem(arena.items, "LOG.65");
  CHECK(empty && empty->type == DepotItem::Type::kFile && empty->chunks.empty() && empty->size == 0 &&
        empty->md5.empty());
  const auto* codex = FindItem(arena.items, "Arena Codex Scientia.pdf");
  CHECK(codex && codex->chunks.size() == 4 && codex->size == 3ull * 10485760 + 19739146 &&
        codex->md5 == "e80b4bf72e97647b80b163b1cf5a9a3b");
  CHECK(std::count_if(arena.items.begin(), arena.items.end(), [](const DepotItem& item) {
          return !item.sha256.empty();
        }) == 4);

  // The gog depots: goggame-<id>.hashdb and .info for Jazz, the .hashdb for Arena's (its .info is another depot).
  const auto jazz_gog = ReadItems(InflateFixture("depot-v2-1351891846-gogdepot.zlib"));
  CHECK(jazz_gog.status == RStatus::kOk && jazz_gog.items.size() == 2);
  CHECK(FindItem(jazz_gog.items, "goggame-1351891846.hashdb") && FindItem(jazz_gog.items, "goggame-1351891846.info"));
  const auto arena_gog = ReadItems(InflateFixture("depot-v2-1435828982-gogdepot.zlib"));
  CHECK(arena_gog.status == RStatus::kOk && arena_gog.items.size() == 1);
  CHECK(FindItem(arena_gog.items, "goggame-1435828982.hashdb"));
}

void TestDepotShapes() {
  // Support files, a folder and a link, with `version` first and the small-files container before items. "depot" and
  // "items" as string values or nested keys are not the manifest's.
  const std::string support =
      R"({ "version" : 2, "note": {"depot": {"items": [1]}}, "label": "depot",)"
      R"( "depot" : { "smallFilesContainer": {"chunks": [)" + std::string(kChunk) + R"(]}, "items" : [ )" +
      FileItem("app\\\\dosbox_arena.conf", kChunk, R"(,"flags":["support"])") + " , " +
      FileItem("save\\\\NAMES.DAT", kChunk, R"(,"flags":["support"],"md5":"0123456789abcdef0123456789abcdef")") + "," +
      FileItem("ARENA.BAT", kChunk, R"(,"flags":["executable"],"sfcRef":{"offset":0,"size":14234})") + "," +
      R"({"path":"SAVE","type":"DepotDirectory"},{"path":"link","target":"ARENA.BAT","type":"DepotLink"},)"
      R"({"path":"odd","type":"SomethingNew"} ], "extra": [ "items", {"x": null} ] } , "tail": [true, false, -1.5e3] })";
  const auto read = ReadItems(support, 1);
  CHECK(read.status == RStatus::kOk && read.items.size() == 6);
  CHECK(SameItems(read.items, ReadItems(support).items));
  CHECK(read.items[0].path == "app\\dosbox_arena.conf" && read.items[0].support && !read.items[0].executable);
  CHECK(read.items[1].support && read.items[1].md5 == "0123456789abcdef0123456789abcdef");
  CHECK(read.items[2].executable && !read.items[2].support);
  CHECK(read.items[3].type == DepotItem::Type::kDirectory && read.items[4].type == DepotItem::Type::kLink &&
        read.items[5].type == DepotItem::Type::kOther);
  // Through the path rules: app\ support into the game folder, other support to the support folder, the link refused.
  PathSet set;
  std::vector<std::string> placed;
  std::size_t links = 0;
  for (std::size_t i = 0; i < read.items.size(); ++i) {
    const auto& item = read.items[i];
    if (item.type == DepotItem::Type::kOther) continue;
    const auto kind = item.type == DepotItem::Type::kFile        ? PathKind::kFile
                      : item.type == DepotItem::Type::kDirectory ? PathKind::kDirectory
                                                                 : PathKind::kLink;
    const auto added = set.Add(item.path, kind, 1435828982, item.support, i);
    if (added.refusal == PathRefusal::kLink) ++links;
    if (added.ok()) placed.push_back(added.path);
  }
  CHECK(links == 1);
  CHECK(placed == (std::vector<std::string>{"dosbox_arena.conf", ".playden-gog/support/1435828982/save/NAMES.DAT",
                                            "ARENA.BAT", "SAVE"}));

  // An empty items array is a valid manifest with no items; a manifest without one is not.
  CHECK(ReadItems(R"({"depot":{"items":[]},"version":2})").status == RStatus::kOk);
  CHECK(ReadItems(R"({"version":2})").status == RStatus::kMalformed);
  CHECK(ReadItems(R"({"depot":{"files":[]}})").status == RStatus::kMalformed);
}

void TestDepotMalformed() {
  const std::string bad_md5 = R"({"compressedMd5":"9ACEE277E9C356401E7DE39CB6549942","compressedSize":935,)"
                              R"("md5":"5cad677703c03f633abcdb750f6ed00c","size":14234})";
  const std::string zero = R"({"compressedMd5":"9acee277e9c356401e7de39cb6549942","compressedSize":935,)"
                           R"("md5":"5cad677703c03f633abcdb750f6ed00c","size":0})";
  const std::string over = R"({"compressedMd5":"9acee277e9c356401e7de39cb6549942","compressedSize":935,)"
                           R"("md5":"5cad677703c03f633abcdb750f6ed00c","size":67108865})";
  const std::string at_ceiling = R"({"compressedMd5":"9acee277e9c356401e7de39cb6549942","compressedSize":67108864,)"
                                 R"("md5":"5cad677703c03f633abcdb750f6ed00c","size":67108864})";
  CHECK(ReadItems(Manifest(FileItem("a", at_ceiling))).status == RStatus::kOk);
  const std::vector<std::string> malformed = {
      Manifest(FileItem("a", bad_md5)),
      Manifest(FileItem("a", zero)),
      Manifest(FileItem("a", over)),
      Manifest(FileItem("a", "[]")),                                         // a chunk that isn't an object
      Manifest(FileItem("a", "1")),
      Manifest(FileItem("a", kChunk, R"(,"md5":"nothex")")),
      Manifest(FileItem("a", kChunk, R"(,"sha256":"abc")")),
      Manifest(FileItem("a", kChunk, R"(,"flags":"support")")),
      Manifest(R"({"chunks":[],"type":"DepotFile"})"),                       // no path
      Manifest(R"({"chunks":[],"path":"a"})"),                               // no type
      Manifest(R"({"chunks":{},"path":"a","type":"DepotFile"})"),
      Manifest(R"(1)"),                                                      // an element that isn't an object
      Manifest(R"("a")"),
      Manifest(R"([])"),
      Manifest(FileItem("a") + ",," + FileItem("b")),
      Manifest(FileItem("a") + "," + FileItem("a", kChunk, R"(,"path":"b")")),  // a repeated key
      R"({"depot":{"items":{}}})",
      R"({"depot":[]})",
      R"({"depot":{"items":[]},"depot":{"items":[]}})",
      R"({"depot":{"items":[],"items":[]}})",
      R"([{"depot":{"items":[]}}])",
      R"({"depot":{"items":[]}} {})",
      R"({"depot":{"items":[]}}x)",
      R"({"depot":{"items":[]},"version":tru})",
      R"({"depot":{"items":[]},"version":2,})",
      R"({"depot":{"items":[]} "version":2})",
      R"({"depot":{"items":[]},"version"2})",
      R"({depot:{"items":[]}})",
      "{\"depot\":{\"items\":[]},\"v\":\"a\x01\"}",
  };
  for (const auto& json : malformed) {
    const auto read = ReadItems(json);
    if (read.status != RStatus::kMalformed) std::fprintf(stderr, "not refused: %s\n", json.c_str());
    CHECK(read.status == RStatus::kMalformed);
    CHECK(ReadItems(json, 1).status == RStatus::kMalformed);
  }

  // Truncated anywhere: Feed accepts every piece, Finish refuses.
  const auto jazz = InflateFixture("depot-v2-1351891846.zlib");
  for (const std::size_t cut : {std::size_t{1}, jazz.size() / 3, jazz.size() / 2, jazz.size() - 1}) {
    DepotItemReader reader([](const DepotItem&) { return true; });
    CHECK(reader.Feed(Bytes(jazz), cut) == RStatus::kOk);
    CHECK(reader.Finish() == RStatus::kMalformed);
  }

  // Deep nesting outside items stops at the scan depth, without growing.
  CHECK(ReadItems(R"({"x":)" + std::string(100000, '[')).status == RStatus::kMalformed);
  // Deep nesting inside an item stops at the item depth.
  CHECK(ReadItems(Manifest(R"({"x":)" + std::string(20, '[') + std::string(20, ']') + "}")).status ==
        RStatus::kMalformed);
  // An item over kMaxItemBytes stops before the buffer passes it.
  {
    DepotItemReader reader([](const DepotItem&) { return true; });
    const std::string head = R"({"depot":{"items":[{"path":")";
    CHECK(reader.Feed(Bytes(head), head.size()) == RStatus::kOk);
    const std::string block(1024 * 1024, 'p');
    RStatus status = RStatus::kOk;
    for (int i = 0; i < 9 && status == RStatus::kOk; ++i) status = reader.Feed(Bytes(block), block.size());
    CHECK(status == RStatus::kTooLarge);
  }
  // The callback says stop.
  {
    std::size_t seen = 0;
    DepotItemReader reader([&seen](const DepotItem&) { return ++seen < 2; });
    const auto json = Manifest(FileItem("a") + "," + FileItem("b") + "," + FileItem("c"));
    CHECK(reader.Feed(Bytes(json), json.size()) == RStatus::kRefused && seen == 2);
    CHECK(reader.Finish() == RStatus::kRefused);
  }
}

void TestDepotLarge() {
  // 20000 items: several MiB and over 100000 values, far past steam::json's 1 MiB and 4096, read through a buffer
  // the size of one item.
  std::string json = R"({"depot":{"items":[)";
  for (int i = 0; i < 20000; ++i) {
    if (i) json += ",";
    json += FileItem("data\\\\folder" + std::to_string(i % 97) + "\\\\file" + std::to_string(i) + ".dat",
                     std::string(kChunk) + "," + kChunk);
  }
  json += R"(],"smallFilesContainer":{"chunks":[]}},"version":2})";
  CHECK(json.size() > 4 * 1024 * 1024);
  CHECK(!playden::steam::json::Parse(json).has_value());
  const auto read = ReadItems(json, 65536);
  CHECK(read.status == RStatus::kOk && read.items.size() == 20000);
  CHECK(read.peak > 0 && read.peak < 1024);
  CHECK(read.items.back().path == "data\\folder17\\file19999.dat");
  CHECK(read.items[0].size == 2 * 14234 && read.items[0].md5.empty());  // two chunks, no file md5
  // Through zlib as well: the inflated cap is the manifest's.
  const auto zlib = Compress(json);
  std::size_t count = 0;
  DepotItemReader reader([&count](const DepotItem&) { return ++count, true; });
  ZlibStream stream(kMaxDepotManifestBytes);
  CHECK(stream.Feed(Bytes(zlib), zlib.size(), [&reader](const std::uint8_t* data, std::size_t size) {
    return reader.Feed(data, size) == RStatus::kOk;
  }) == ZStatus::kOk);
  CHECK(stream.Finish() == ZStatus::kOk && reader.Finish() == RStatus::kOk && count == 20000);
}

// --- Dependencies -----------------------------------------------------------------------------------------------------

void TestDependencies() {
  const auto repository = ParseDependencyRepository(Fixture("dependencies-repository.json"));
  CHECK(repository.has_value());
  CHECK(repository->manifest_url ==
        "https://gog-cdn-fastly.gog.com/content-system/v2/dependencies/meta/29/e3/29e3a805a0334ce7a1c85cecc2c78d9e");
  CHECK(repository->build_id == "60163282911088704" && repository->generation == 2);
  CHECK(!ParseDependencyRepository(R"({"repository_manifest":"http://x/y","build_id":"1","generation":2})"));
  CHECK(!ParseDependencyRepository("[]"));

  std::string raw;
  CHECK(Inflate(Fixture("dependencies-manifest.zlib"), kMaxDependencyManifestBytes, &raw) == ZStatus::kOk);
  const auto dependencies = ParseDependencyManifest(raw);
  CHECK(dependencies.has_value() && dependencies->size() == 69);
  const auto find = [&dependencies](const std::string& id) -> const Dependency* {
    for (const auto& dependency : *dependencies)
      if (dependency.id == id) return &dependency;
    return nullptr;
  };
  const auto* dosbox = find("DOSBox074_2CS");
  const auto* scummvm = find("ScummVM");
  const auto* isi = find("ISI");
  const auto* msvc = find("MSVC2017");
  CHECK(dosbox && scummvm && isi && msvc);
  CHECK(GameFolderDependency(*dosbox) && dosbox->exe_path.empty() && IsMd5Hex(dosbox->manifest) && dosbox->size > 0);
  CHECK(GameFolderDependency(*scummvm));
  CHECK(!GameFolderDependency(*isi) && isi->exe_path == "__redist/ISI/scriptinterpreter.exe");
  CHECK(!GameFolderDependency(*msvc));
  CHECK(std::count_if(dependencies->begin(), dependencies->end(), GameFolderDependency) == 20);
  // A bad entry is skipped, not the whole list.
  const auto partial = ParseDependencyManifest(
      R"({"depots":[{"dependencyId":"../x","manifest":"11111111111111111111111111111111","size":1,"compressedSize":1},)"
      R"({"dependencyId":"Ok","manifest":"11111111111111111111111111111111","size":1,"compressedSize":1,)"
      R"("executable":{"path":"","arguments":""},"signature":"ignored"}]})");
  CHECK(partial.has_value() && partial->size() == 1 && partial->front().id == "Ok");
  CHECK(!ParseDependencyManifest(R"({"depots":{}})"));

  // A dependency depot manifest reads like a game's: DOSBox's files sit under DOSBOX/ in the game folder.
  const auto dosbox_depot = ReadItems(Fixture("dependency-depot-DOSBox074_2CS.json"), 7);
  CHECK(dosbox_depot.status == RStatus::kOk && dosbox_depot.items.size() == 15);
  const auto* exe = FindItem(dosbox_depot.items, "DOSBOX/DOSBox.exe");
  CHECK(exe && !exe->sha256.empty() && IsSha256Hex(exe->sha256) && !exe->md5.empty());
  const auto scummvm_depot = ReadItems(Fixture("dependency-depot-ScummVM.json"));
  CHECK(scummvm_depot.status == RStatus::kOk && scummvm_depot.items.size() == 29);
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_fixtures = argv[1];
  TestHashes();
  TestChooseBuild();
  TestOrderEndpoints();
  TestFillUrl();
  TestLanguages();
  TestMetaAndSelection();
  TestSyntheticSelection();
  TestZlibStream();
  TestDepotFixtures();
  TestDepotShapes();
  TestDepotMalformed();
  TestDepotLarge();
  TestDependencies();
  return 0;
}
