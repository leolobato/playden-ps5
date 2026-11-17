// SPDX-License-Identifier: GPL-3.0-or-later
// GOG's library answers: the owned list (also one far above steam::json's default 4096 values), gamesdb entries on
// recorded public answers and synthetic variants (game, DLC, spam, hidden, no Windows, another product), and the art
// address shape. No network.
#include "gog/api.h"
#include "gog/art_url.h"
#include "gog_stub_transport.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace playden;
using namespace gog_test;
using Kind = gog::ProductInfo::Kind;

namespace {

std::string g_fixtures;

std::string Fixture(const std::string& name) { return ReadFile(g_fixtures + "/" + name); }

const char kJazzCover[] =
    "https://images.gog.com/815f695a093eb35325ac84c66af106b993a940a40097d1ed63149c0bb87562fe.jpg?namespace=gamesdb";
const char kJazzHero[] =
    "https://images.gog.com/53669def0ca2900b0ab3cacd7846a66cdc2036179be1d68b4c68062907b7c199_bg_crop_1920x655.jpg"
    "?namespace=gamesdb";

void TestGame() {
  const auto info = gog::ParseProduct(Fixture("gamesdb-game-1351891846.json"), 1351891846);
  CHECK(info.kind == Kind::kGame);
  CHECK(info.title == "Jazz Jackrabbit 2: The Secret Files");
  // The en-US summary, not the "*" one.
  CHECK(info.summary.rfind("The Secret Files contains Jazz Jackrabbit 2", 0) == 0);
  CHECK(info.developer.empty());  // gamesdb names none for it
  CHECK(info.cover == kJazzCover);
  CHECK(info.hero == kJazzHero);
  CHECK(info.dlc_ids.empty());

  const auto prison = gog::ParseProduct(Fixture("gamesdb-game-with-dlcs-1441974651.json"), 1441974651);
  CHECK(prison.kind == Kind::kGame);
  CHECK(prison.title == "Prison Architect");
  CHECK(prison.developer == "Introversion Software");
  CHECK(prison.dlc_ids.size() == 12);
  CHECK(prison.dlc_ids.front() == 1619024184 && prison.dlc_ids.back() == 1126027458);
  CHECK(gog::IsArtUrl(prison.cover, gog::ArtKind::kCover) && gog::IsArtUrl(prison.hero, gog::ArtKind::kHero));
}

void TestHidden() {
  CHECK(gog::ParseProduct(Fixture("gamesdb-spam-1440166342.json"), 1440166342).kind == Kind::kHidden);
  CHECK(gog::ParseProduct(Fixture("gamesdb-dlc.json"), 1900000101).kind == Kind::kHidden);
  CHECK(gog::ParseProduct(Fixture("gamesdb-no-windows.json"), 1900000102).kind == Kind::kHidden);
  CHECK(gog::ParseProduct(Fixture("gamesdb-invisible.json"), 1900000103).kind == Kind::kHidden);
  // A hidden entry keeps nothing to show.
  const auto spam = gog::ParseProduct(Fixture("gamesdb-spam-1440166342.json"), 1440166342);
  CHECK(spam.title.empty() && spam.cover.empty() && spam.hero.empty());
}

void TestInvalid() {
  const std::string jazz = Fixture("gamesdb-game-1351891846.json");
  // gamesdb answers for any ID: an entry naming another product is not this one.
  CHECK(gog::ParseProduct(jazz, 1351891847).kind == Kind::kInvalid);
  CHECK(gog::ParseProduct(jazz.substr(0, jazz.size() / 2), 1351891846).kind == Kind::kInvalid);
  CHECK(gog::ParseProduct("[1,2,3]", 1351891846).kind == Kind::kInvalid);
  CHECK(gog::ParseProduct("\"game\"", 1351891846).kind == Kind::kInvalid);
  CHECK(gog::ParseProduct("", 1351891846).kind == Kind::kInvalid);
  CHECK(gog::ParseProduct(jazz, 0).kind == Kind::kInvalid);
  // A game with no usable title can't be listed.
  CHECK(gog::ParseProduct(R"({"external_id":"5","type":"game","title":{"*":"  "},"supported_operating_systems":)"
                          R"([{"slug":"windows"}],"game":{"visible_in_library":true}})",
                          5)
            .kind == Kind::kInvalid);
}

void TestLocalizedAndLimits() {
  // "*" when there is no en-US; game.title when the top level has none; an over-long summary is cut on a character.
  const std::string body = R"({"external_id":7,"type":"mod","summary":{"de-DE":"x","*":")" + std::string(65535, 'b') +
                           "\xC3\xA9" + R"("},"supported_operating_systems":[{"slug":"windows"}],)" +
                           R"("game":{"visible_in_library":true,"title":{"*":"Default title"},)" +
                           R"("developers":[{"name":"Dev"}],"cover":{"url_format":"https://images.gog.com/)" +
                           std::string(64, 'a') + R"({formatter}.{ext}?namespace=gamesdb"}}})";
  const auto info = gog::ParseProduct(body, 7);
  CHECK(info.kind == Kind::kGame);
  CHECK(info.title == "Default title");
  CHECK(info.summary == std::string(65535, 'b'));  // é would end past 64 KiB: cut before it
  CHECK(info.developer == "Dev");
  CHECK(info.cover == "https://images.gog.com/" + std::string(64, 'a') + ".jpg?namespace=gamesdb");
  CHECK(info.hero.empty());
}

void TestOwned() {
  const auto owned = gog::ParseOwned(Fixture("owned.json"));
  CHECK(owned.kind == gog::OwnedResult::Kind::kOk);
  CHECK((owned.ids == std::vector<std::uint64_t>{1351891846, 1441974651, 1440166342, 1900000101, 1900000102,
                                                  1900000103}));
  // More than steam::json's default 4096 values.
  std::string big = "{\"owned\":[";
  for (int i = 1; i <= 5000; ++i) big += (i > 1 ? "," : "") + std::to_string(1000000000 + i);
  big += "]}";
  const auto large = gog::ParseOwned(big);
  CHECK(large.kind == gog::OwnedResult::Kind::kOk && large.ids.size() == 5000);
  CHECK(large.ids.front() == 1000000001 && large.ids.back() == 1000005000);
  // Duplicates once, where first seen; non-integers, zero, negatives and too-large numbers skipped.
  const auto mixed = gog::ParseOwned(
      R"({"owned":[3,1,3,"2",2.5,-4,0,1e3,null,{},[],18446744073709551615,18446744073709551616,1]})");
  CHECK(mixed.kind == gog::OwnedResult::Kind::kOk);
  CHECK((mixed.ids == std::vector<std::uint64_t>{3, 1, 18446744073709551615ull}));
  CHECK(gog::ParseOwned(R"({"owned":5})").kind == gog::OwnedResult::Kind::kProtocol);
  CHECK(gog::ParseOwned(R"({"games":[1]})").kind == gog::OwnedResult::Kind::kProtocol);
  CHECK(gog::ParseOwned(R"([1,2])").kind == gog::OwnedResult::Kind::kProtocol);
  CHECK(gog::ParseOwned(R"({"owned":[1,2)").kind == gog::OwnedResult::Kind::kProtocol);
  CHECK(gog::ParseOwned(R"({"owned":[]})").kind == gog::OwnedResult::Kind::kOk);
}

void TestArtUrl() {
  const std::string hash(64, 'c');
  const std::string format = "https://images.gog.com/" + hash + "{formatter}.{ext}?namespace=gamesdb";
  const std::string url = "https://images.gog.com/" + hash + ".jpg?namespace=gamesdb";
  const std::string hero = "https://images.gog.com/" + hash + "_bg_crop_1920x655.jpg?namespace=gamesdb";
  CHECK(gog::ArtUrl(format) == url);
  CHECK(gog::ArtUrl(format, gog::ArtKind::kHero) == hero);
  CHECK(gog::IsArtUrl(url) && gog::IsArtUrl(hero));
  CHECK(gog::IsArtUrl(url, gog::ArtKind::kCover) && !gog::IsArtUrl(url, gog::ArtKind::kHero));
  CHECK(gog::IsArtUrl(hero, gog::ArtKind::kHero) && !gog::IsArtUrl(hero, gog::ArtKind::kCover));
  std::string_view seen;
  gog::ArtKind kind = gog::ArtKind::kCover;
  CHECK(gog::IsArtUrl(hero, &seen, &kind) && seen == hash && kind == gog::ArtKind::kHero);
  for (const std::string& bad : {
           "http://images.gog.com/" + hash + "{formatter}.{ext}?namespace=gamesdb",
           "https://images.gog.com.example/" + hash + "{formatter}.{ext}?namespace=gamesdb",
           "https://images.gog.com/" + hash + "{formatter}.{ext}",
           "https://images.gog.com/" + std::string(64, 'C') + "{formatter}.{ext}?namespace=gamesdb",
           "https://images.gog.com/" + std::string(63, 'c') + "{formatter}.{ext}?namespace=gamesdb",
           "https://images.gog.com/" + hash + "{formatter}.{ext}?namespace=gamesdb&x=1",
           "https://images.gog.com/" + hash + "_product_card.{ext}?namespace=gamesdb",
           std::string(),
       })
    CHECK(gog::ArtUrl(bad).empty());
  for (const std::string& bad : {
           "http://images.gog.com/" + hash + ".jpg?namespace=gamesdb",
           "https://images.gog.com/" + hash + ".png?namespace=gamesdb",
           "https://images.gog.com/" + std::string(64, 'C') + ".jpg?namespace=gamesdb",
           "https://images.gog.com/" + hash + ".jpg?namespace=gamesdb#",
           "https://images.gog.com/../" + hash + ".jpg?namespace=gamesdb",
           "https://images.gog.com/" + hash + "_bg_crop_1920x656.jpg?namespace=gamesdb",
           "https://images.gog.com/" + hash + "_bg_crop_1920x655.png?namespace=gamesdb",
           "https://images.gog.com/" + hash + "_product_card.jpg?namespace=gamesdb",
           "https://images.gog.com/" + hash + "_bg_crop_1920x655",
           format,
       })
    CHECK(!gog::IsArtUrl(bad));
}

void TestFetch() {
  // The owned list: Bearer, the embed address, and the status mapping.
  struct Seen {
    std::string url, authorization, if_none_match;
  } seen;
  HttpResponse next = Answer(200, R"({"owned":[1,2]})");
  gog::Transport transport = [&](const HttpRequest& request, const std::atomic<bool>&) {
    seen = {};
    seen.url = request.url;
    for (const auto& header : request.headers) {
      if (header.first == "Authorization") seen.authorization = header.second;
      if (header.first == "If-None-Match") seen.if_none_match = header.second;
    }
    return next;
  };
  const std::atomic<bool> cancel{false};
  auto owned = gog::FetchOwned(transport, "synthetic-access", cancel);
  CHECK(owned.kind == gog::OwnedResult::Kind::kOk && owned.ids.size() == 2);
  CHECK(seen.url == "https://embed.gog.com/user/data/games");
  CHECK(seen.authorization == "Bearer synthetic-access");
  next = Answer(401, "");
  CHECK(gog::FetchOwned(transport, "x", cancel).kind == gog::OwnedResult::Kind::kRejected);
  next = Answer(503, "");
  CHECK(gog::FetchOwned(transport, "x", cancel).kind == gog::OwnedResult::Kind::kNetwork);
  next = Failure(HttpError::kTimeout);
  CHECK(gog::FetchOwned(transport, "x", cancel).kind == gog::OwnedResult::Kind::kNetwork);

  // A product: no Authorization, If-None-Match only with a usable ETag, the ETag header read in any case.
  next = Answer(200, "{}");
  next.headers.emplace_back("etag", "W/\"abc\"");
  auto product = gog::FetchProduct(transport, 1351891846, "", cancel);
  CHECK(product.kind == gog::ProductResult::Kind::kModified && product.etag == "W/\"abc\"" && product.body == "{}");
  CHECK(seen.url == "https://gamesdb.gog.com/platforms/gog/external_releases/1351891846");
  CHECK(seen.authorization.empty() && seen.if_none_match.empty());
  next = Answer(304, "");
  CHECK(gog::FetchProduct(transport, 5, "W/\"abc\"", cancel).kind == gog::ProductResult::Kind::kNotModified);
  CHECK(seen.if_none_match == "W/\"abc\"");
  // A value that could break the header is neither sent nor kept.
  gog::FetchProduct(transport, 5, "W/\"a\"\r\nX: y", cancel);
  CHECK(seen.if_none_match.empty());
  next = Answer(200, "{}");
  next.headers.emplace_back("ETag", std::string(300, 'e'));
  CHECK(gog::FetchProduct(transport, 5, "", cancel).etag.empty());
  next = Answer(404, "");
  CHECK(gog::FetchProduct(transport, 5, "", cancel).kind == gog::ProductResult::Kind::kMissing);
  next = Answer(500, "");
  CHECK(gog::FetchProduct(transport, 5, "", cancel).kind == gog::ProductResult::Kind::kFailed);
  next = Failure(HttpError::kConnectionFailed);
  CHECK(gog::FetchProduct(transport, 5, "", cancel).kind == gog::ProductResult::Kind::kFailed);
}

void TestSecureLinks() {
  // Gen 1 and gen 2 secure links: the query each sends, the Bearer header, the endpoints kept and the status mapping.
  const std::string gen1 = Fixture("secure-link-gen1-200.json");
  const std::string gen2 = Fixture("secure-link-200.json");
  struct Seen {
    int calls = 0;
    std::string url, authorization, user_agent;
  } seen;
  HttpResponse next = Answer(200, gen1);
  gog::Transport transport = [&](const HttpRequest& request, const std::atomic<bool>&) {
    ++seen.calls;
    seen.url = request.url;
    seen.authorization.clear();
    for (const auto& header : request.headers) {
      if (header.first == "Authorization") seen.authorization = header.second;
      if (header.first == "User-Agent") seen.user_agent = header.second;
    }
    return next;
  };
  const std::atomic<bool> cancel{false};

  auto link = gog::FetchSecureLinkV1(transport, "synthetic-access-gen1", 1900000001, 31000007, cancel);
  CHECK(seen.url ==
        "https://content-system.gog.com/products/1900000001/secure_link?_version=2&type=depot&path=/windows/31000007/");
  CHECK(seen.authorization == "Bearer synthetic-access-gen1" && seen.user_agent == "Playden");
  CHECK(link.kind == gog::LinkResult::Kind::kOk && link.status == 200);
  CHECK(link.endpoints.size() == 1 && link.endpoints[0].name == "fastly");  // gcore lists only generation 2
  CHECK(link.endpoints[0].parameters.at("path") == "/content-system/v1/depots/1900000001/windows/31000007");
  CHECK(link.expires_at == 1900000000);
  CHECK(gog::FillUrl(link.endpoints[0], "/main.bin") ==
        std::optional<std::string>("https://cdn.example.invalid/token=nva=1900000000~dirs=6~token=synthetic-gen1-fastly-"
                                   "signature/content-system/v1/depots/1900000001/windows/31000007/main.bin"));
  next = Answer(401, Fixture("secure-link-401.json"));
  link = gog::FetchSecureLinkV1(transport, "x", 1900000001, 31000007, cancel);
  CHECK(link.kind == gog::LinkResult::Kind::kRejected && link.status == 401 && link.endpoints.empty());
  next = Answer(403, "");
  CHECK(gog::FetchSecureLinkV1(transport, "x", 1900000001, 31000007, cancel).kind == gog::LinkResult::Kind::kForbidden);
  next = Answer(503, "");
  CHECK(gog::FetchSecureLinkV1(transport, "x", 1900000001, 31000007, cancel).kind == gog::LinkResult::Kind::kNetwork);
  next = Answer(200, "{\"urls\":[]}");
  CHECK(gog::FetchSecureLinkV1(transport, "x", 1900000001, 31000007, cancel).kind == gog::LinkResult::Kind::kProtocol);
  next = Failure(HttpError::kTimeout);
  CHECK(gog::FetchSecureLinkV1(transport, "x", 1900000001, 31000007, cancel).kind == gog::LinkResult::Kind::kNetwork);
  next = Failure(HttpError::kCanceled);
  CHECK(gog::FetchSecureLinkV1(transport, "x", 1900000001, 31000007, cancel).kind == gog::LinkResult::Kind::kCanceled);
  // No product or no timestamp: nothing is asked.
  const int calls = seen.calls;
  CHECK(gog::FetchSecureLinkV1(transport, "x", 0, 31000007, cancel).kind == gog::LinkResult::Kind::kNetwork);
  CHECK(gog::FetchSecureLinkV1(transport, "x", 1900000001, 0, cancel).kind == gog::LinkResult::Kind::kNetwork);
  CHECK(seen.calls == calls);

  // Gen 2 keeps its own query and both endpoints.
  next = Answer(200, gen2);
  link = gog::FetchSecureLink(transport, "synthetic-access-gen2", 1900000001, cancel);
  CHECK(seen.url == "https://content-system.gog.com/products/1900000001/secure_link?_version=2&generation=2&path=/");
  CHECK(seen.authorization == "Bearer synthetic-access-gen2");
  CHECK(link.kind == gog::LinkResult::Kind::kOk && link.endpoints.size() == 2);

  // ParseLink reads for the generation asked: gen 1 drops gcore, from either answer.
  const auto gen2_as_gen1 = gog::ParseLink(gen2, 1);
  CHECK(gen2_as_gen1.kind == gog::LinkResult::Kind::kOk && gen2_as_gen1.endpoints.size() == 1);
  CHECK(gen2_as_gen1.endpoints[0].name == "fastly" && gen2_as_gen1.expires_at == 1900000000);
  CHECK(gog::ParseLink(gen2).endpoints.size() == 2);
  CHECK(gog::ParseLink(gen1, 2).endpoints.size() == 2);
  // A link with only gen 2 endpoints has nothing for gen 1.
  const std::string gcore_only = R"({"urls":[{"endpoint_name":"gcore","url_format":"https://g.example.invalid/{path}",)"
                                 R"("parameters":{"path":"x"},"priority":1,"supports_generation":[2],)"
                                 R"("fallback_only":true}]})";
  CHECK(gog::ParseLink(gcore_only, 1).kind == gog::LinkResult::Kind::kProtocol);
  CHECK(gog::ParseLink(gcore_only, 2).kind == gog::LinkResult::Kind::kOk);
}

}  // namespace

// The library tags (#536), in the shape of the account library listing, with synthetic tags and products.
std::string TagsPage(int page, int total, const std::string& products,
                     const std::string& tags = R"([{"id":"100000001","name":"Couch","productCount":2},)"
                                               R"({"id":"100000002","name":"To finish","productCount":1},)"
                                               R"({"id":"100000003","name":"Unused","productCount":0}])") {
  return R"({"sortBy":null,"page":)" + std::to_string(page) + R"(,"totalProducts":3,"totalPages":)" +
         std::to_string(total) + R"(,"productsPerPage":100,"contentSystemCompatibility":null,"moviesCount":0,"tags":)" +
         tags + R"(,"products":[)" + products + R"(],"updatedProductsCount":0,"hiddenUpdatedProductsCount":0,)" +
         R"("appliedFilters":{"tags":null},"hasHiddenProducts":true})";
}
std::string TagsProduct(std::uint64_t id, const std::string& tags, bool hidden = false) {
  return R"({"isGalaxyCompatible":true,"tags":)" + tags + R"(,"id":)" + std::to_string(id) +
         R"(,"availability":{"isAvailable":true,"isAvailableInAccount":true},"title":"Synthetic","isHidden":)" +
         (hidden ? "true" : "false") + R"(,"worksOn":{"Windows":true,"Mac":false,"Linux":false}})";
}

void TestLibraryTags() {
  auto page = gog::ParseLibraryTagsPage(TagsPage(1, 1, TagsProduct(1900000101, R"(["100000001"])") + "," +
                                                           TagsProduct(1900000103, R"(["100000002","bad"])") + "," +
                                                           TagsProduct(1900000102, "[]")));
  CHECK(page.ok && page.total_pages == 1 && page.tags.size() == 3 && page.tags[0].first == "100000001" &&
        page.tags[0].second == "Couch");
  CHECK(page.products.size() == 3 && page.products[1].second == std::vector<std::string>{"100000002"});
  CHECK(!gog::ParseLibraryTagsPage("{not json").ok);
  CHECK(!gog::ParseLibraryTagsPage(R"({"products":[]})").ok);
  // A tag without a name or a canonical ID is skipped.
  page = gog::ParseLibraryTagsPage(TagsPage(1, 1, "", R"([{"id":"012","name":"A"},{"id":"5","name":""},{"id":"6","name":"B"}])"));
  CHECK(page.ok && page.tags.size() == 1 && page.tags[0].first == "6");

  // Every page of the listing, then the hidden view's: the hidden product's tags count too.
  std::vector<std::string> urls;
  std::vector<HttpResponse> answers = {
      Answer(200, TagsPage(1, 2, TagsProduct(1900000101, R"(["100000001"])"))),
      Answer(200, TagsPage(2, 2, TagsProduct(1900000102, R"(["100000001","100000002"])"))),
      Answer(200, TagsPage(1, 1, TagsProduct(1900000104, R"(["100000002"])", true))),
  };
  std::string authorization;
  gog::Transport transport = [&](const HttpRequest& request, const std::atomic<bool>&) {
    urls.push_back(request.url);
    for (const auto& header : request.headers)
      if (header.first == "Authorization") authorization = header.second;
    if (urls.size() > answers.size()) return Answer(500, "");
    return answers[urls.size() - 1];
  };
  const std::atomic<bool> cancel{false};
  auto tags = gog::FetchLibraryTags(transport, "synthetic-access", cancel);
  CHECK(tags.kind == gog::TagsResult::Kind::kOk && tags.pages == 3 && authorization == "Bearer synthetic-access");
  CHECK((urls == std::vector<std::string>{
             "https://embed.gog.com/account/getFilteredProducts?mediaType=1&page=1",
             "https://embed.gog.com/account/getFilteredProducts?mediaType=1&page=2",
             "https://embed.gog.com/account/getFilteredProducts?mediaType=1&page=1&hiddenFlag=1"}));
  CHECK(tags.tags.size() == 3 && tags.tags[0].id == "100000001" &&
        (tags.tags[0].products == std::vector<std::uint64_t>{1900000101, 1900000102}));
  CHECK((tags.tags[1].products == std::vector<std::uint64_t>{1900000102, 1900000104}) && tags.tags[2].products.empty());
  // Any page that fails fails the whole read.
  urls.clear();
  answers = {Answer(200, TagsPage(1, 2, "")), Answer(401, "")};
  CHECK(gog::FetchLibraryTags(transport, "x", cancel).kind == gog::TagsResult::Kind::kRejected);
  urls.clear();
  answers = {Answer(503, "")};
  CHECK(gog::FetchLibraryTags(transport, "x", cancel).kind == gog::TagsResult::Kind::kNetwork);
  urls.clear();
  answers = {Answer(200, "[]")};
  CHECK(gog::FetchLibraryTags(transport, "x", cancel).kind == gog::TagsResult::Kind::kProtocol);
  urls.clear();
  answers = {Failure(HttpError::kTimeout)};
  CHECK(gog::FetchLibraryTags(transport, "x", cancel).kind == gog::TagsResult::Kind::kNetwork);
  // More pages than the cap: refused at once, never a partial list.
  urls.clear();
  answers.assign(200, Answer(200, TagsPage(1, gog::kMaxTagPages + 1, "")));
  CHECK(gog::FetchLibraryTags(transport, "x", cancel).kind == gog::TagsResult::Kind::kProtocol && urls.size() == 1);
  // Exactly the cap is read whole.
  urls.clear();
  answers.assign(200, Answer(200, TagsPage(1, gog::kMaxTagPages, "")));
  CHECK(gog::FetchLibraryTags(transport, "x", cancel).kind == gog::TagsResult::Kind::kOk &&
        urls.size() == gog::kMaxTagPages + gog::kMaxTagPages);
}

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_fixtures = argv[1];
  TestGame();
  TestHidden();
  TestInvalid();
  TestLocalizedAndLimits();
  TestOwned();
  TestArtUrl();
  TestFetch();
  TestSecureLinks();
  TestLibraryTags();
  return 0;
}
