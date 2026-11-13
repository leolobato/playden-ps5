// SPDX-License-Identifier: GPL-3.0-or-later
// GOG's library answers: the owned list (also one far above steam::json's default 4096 values), gamesdb entries on
// recorded public answers and synthetic variants (game, DLC, spam, hidden, no Windows, another product), and the art
// address shape. No network.
#include "gog/api.h"
#include "gog/art_url.h"
#include "gog_stub_transport.h"

#include <atomic>
#include <cstdint>
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

}  // namespace

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
  return 0;
}
