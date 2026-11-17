// SPDX-License-Identifier: GPL-3.0-or-later
// The GOG library worker over a stub transport: the first refresh fetches every product and saves the library; the
// next asks with If-None-Match and reuses 304 answers; a 404 drops a product and a failure keeps its saved copy; a
// large library arrives in batches; a revoked token, the saved copy offline, Clear (also with Invalidate right after)
// and Invalidate during a refresh. Synthetic tokens, accounts and products only.
#include "gog/account_key.h"
#include "gog/product_cache.h"
#include "gog/session.h"
#include "gog/session_store.h"
#include "gog_stub_transport.h"
#include "library/cache.h"
#include "library/collections_cache.h"
#include "stores/gog_library.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace playden;
using namespace gog_test;
using playden::stores::GogLibrarySource;
namespace fs = std::filesystem;

namespace {

fs::path g_base;
int g_roots = 0;
const std::atomic<bool> kNoCancel{false};

fs::path FreshRoot() {
  const fs::path root = g_base / ("root-" + std::to_string(++g_roots));
  fs::create_directories(root);
  CHECK(::chmod(root.c_str(), 0755) == 0);
  return root;
}

bool Exists(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0;
}

std::string Hash(std::uint64_t id, char kind) {
  char digits[65];
  std::snprintf(digits, sizeof(digits), "%064" PRIx64, id);
  std::string hash(digits);
  hash[0] = kind;  // a or b: cover or hero
  return hash;
}

std::string ArtFormat(std::uint64_t id, char kind) {
  return "https://images.gog.com/" + Hash(id, kind) + "{formatter}.{ext}?namespace=gamesdb";
}

std::string Cover(std::uint64_t id) { return "https://images.gog.com/" + Hash(id, 'a') + ".jpg?namespace=gamesdb"; }
std::string Hero(std::uint64_t id) {
  return "https://images.gog.com/" + Hash(id, 'b') + "_bg_crop_1920x655.jpg?namespace=gamesdb";
}
std::string Title(std::uint64_t id) { return "Synthetic Game " + std::to_string(id); }

// A gamesdb entry in the shape the real ones have (only the fields Playden reads).
std::string ProductJson(std::uint64_t id, const std::string& type = "game") {
  const std::string text = std::to_string(id);
  return R"({"external_id":")" + text + R"(","type":")" + type + R"(","title":{"*":"Default","en-US":")" + Title(id) +
         R"("},"summary":{"en-US":"Summary )" + text +
         R"("},"supported_operating_systems":[{"slug":"windows","name":"Windows"}],"dlcs":[],)" +
         R"("game":{"visible_in_library":true,"developers":[{"name":"Synthetic Studio"}],)" +
         R"("vertical_cover":{"url_format":")" + ArtFormat(id, 'a') + R"("},"background":{"url_format":")" +
         ArtFormat(id, 'b') + R"("}}})";
}

Stub::Step Product(std::uint64_t id, const std::string& type = "game", int version = 1) {
  auto response = Answer(200, ProductJson(id, type));
  response.headers.emplace_back("ETag", "W/\"" + std::to_string(id) + "-" + std::to_string(version) + "\"");
  return {response, nullptr};
}

Stub::Step Status(long status) { return {Answer(status, ""), nullptr}; }

std::string OwnedJson(const std::vector<std::uint64_t>& ids) {
  std::string body = "{\"owned\":[";
  for (std::size_t i = 0; i < ids.size(); ++i) body += (i ? "," : "") + std::to_string(ids[i]);
  return body + "]}";
}

gog::SessionStore::Options Options(const fs::path& root) {
  gog::SessionStore::Options options;
  options.user_root = root.string();
  return options;
}

const std::uint64_t kAccount = gog::AccountKey(kUserId);

struct Fixture {
  fs::path root = FreshRoot();
  fs::path library_root = root / "library";
  gog::SessionStore store{Options(root)};
  Stub stub;
  std::atomic<std::int64_t> now{100000};
  gog::Session session{store, stub.Transport(), [this] { return now.load(); }};
  Storage storage{(fs::create_directories(library_root), library_root.string())};
  LibraryCache cache{storage, "library-gog-"};
  CollectionsCache tags{storage, "collections-gog-"};
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  GogLibrarySource source{session, stub.Transport(), cache, root.string(),
                          [this](const std::string& line) {
                            std::lock_guard<std::mutex> lock(lines_mutex);
                            lines.push_back(line);
                          },
                          [] { return std::uint64_t{1'800'000'000}; }, &tags};

  Fixture() { SignIn(); }

  void SignIn() {
    gog::StoredSession saved;
    saved.refresh_token = RefreshText(1);
    saved.user_id = kUserId;
    saved.username = kUsername;
    CHECK(store.Save(saved).ok);
    stub.Script(Endpoint::kToken, Tokens(2));
    CHECK(session.Restore(kNoCancel).kind == gog::RestoreOutcome::Kind::kSignedIn);
  }

  fs::path CacheFile() const { return library_root / ("library-gog-" + std::to_string(kAccount) + ".cache"); }
  fs::path TagsFile() const { return library_root / ("collections-gog-" + std::to_string(kAccount) + ".cache"); }
  fs::path ProductFile(std::uint64_t id) const { return root / "gog" / "products" / std::to_string(id); }

  // Events until one of the terminal kinds (10 s at most), then a short wait shows nothing follows.
  std::vector<LibraryEvent> Until(LibraryEventKind last) {
    std::vector<LibraryEvent> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
      for (auto& event : source.Pump(64)) events.push_back(std::move(event));
      if (!events.empty() && (events.back().kind == last || events.back().kind == LibraryEventKind::kFailure)) break;
      CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(source.Pump(64).empty());
    return events;
  }

  std::string LogText() {
    std::lock_guard<std::mutex> lock(lines_mutex);
    std::string text;
    for (const auto& line : lines) text += line + "\n";
    return text;
  }
};

std::vector<std::uint64_t> Ids(const std::vector<GameSummary>& games) {
  std::vector<std::uint64_t> ids;
  for (const auto& game : games) {
    CHECK(game.id.store == Store::kGog);
    ids.push_back(std::stoull(game.id.value));
  }
  return ids;
}

std::string ReadBytes(const fs::path& path) { return ReadFile(path.string()); }

void FirstAndSecondRefresh() {
  Fixture f;
  // Two games, a DLC, a spam entry and a product gamesdb doesn't know.
  const std::vector<std::uint64_t> owned = {1207658924, 1207658925, 1207658926, 1207658927, 1207658928};
  std::map<std::uint64_t, std::string> types = {
      {1207658924, "game"}, {1207658925, "dlc"}, {1207658926, "game"}, {1207658927, "spam"}};
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson(owned)));
  f.stub.SetProducts([&](std::uint64_t id, const std::string& if_none_match) {
    CHECK(if_none_match.empty());
    if (id == 1207658928) return Status(404);
    return Product(id, types.at(id));
  });
  const auto request = f.source.Refresh(kAccount, 1);
  CHECK(request != 0);
  auto events = f.Until(LibraryEventKind::kComplete);
  CHECK(events.front().kind == LibraryEventKind::kLoading);
  const auto& complete = events.back();
  CHECK(complete.kind == LibraryEventKind::kComplete && complete.request_id == request);
  CHECK(complete.account_id == kAccount && complete.account_generation == 1);
  CHECK((Ids(complete.games) == std::vector<std::uint64_t>{1207658924, 1207658926}));
  const auto& game = complete.games.front();
  CHECK(game.title == Title(1207658924) && game.description == "Summary 1207658924");
  CHECK(game.developer == "Synthetic Studio");
  CHECK(game.artwork_source == Cover(1207658924) && game.hero_source == Hero(1207658924));
  CHECK(complete.cache_available && complete.cache_error == StorageError::kNone);
  CHECK(complete.retrieved_at_unix_seconds == 1'800'000'000);
  // The saved library is named by the account key, and neither its name nor its bytes hold the user ID.
  CHECK(Exists(f.CacheFile()));
  CHECK(f.CacheFile().filename().string().find(kUserId) == std::string::npos);
  CHECK(ReadBytes(f.CacheFile()).find(kUserId) == std::string::npos);
  CHECK(ReadBytes(f.CacheFile()).find(kUsername) == std::string::npos);
  const auto saved = f.cache.Load(kAccount);
  CHECK(saved.ok() && saved.value().schema_version == 8 && saved.value().games.size() == 2);
  CHECK(saved.value().games[1].hero_source == Hero(1207658926));
  // Every product with an entry is kept, hidden ones too; the unknown one is not.
  for (const auto id : {1207658924, 1207658925, 1207658926, 1207658927}) CHECK(Exists(f.ProductFile(id)));
  CHECK(!Exists(f.ProductFile(1207658928)));
  CHECK(f.LogText().find("library owned=5 fetched=4 not_modified=0 missing=1 hidden=2 failed=0") != std::string::npos);

  // The second refresh: the saved library first, then every product asked with its ETag; 304 reuses it.
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson(owned)));
  std::vector<std::string> asked;
  f.stub.SetProducts([&](std::uint64_t id, const std::string& if_none_match) {
    asked.push_back(if_none_match);
    if (id == 1207658928) {
      CHECK(if_none_match.empty());
      return Status(404);
    }
    CHECK(if_none_match == "W/\"" + std::to_string(id) + "-1\"");
    return Status(304);
  });
  f.source.Refresh(kAccount, 2);
  events = f.Until(LibraryEventKind::kComplete);
  CHECK(events[0].kind == LibraryEventKind::kLoading);
  CHECK(events[1].kind == LibraryEventKind::kCached && events[1].cache_available);
  CHECK((Ids(events[1].games) == std::vector<std::uint64_t>{1207658924, 1207658926}));
  CHECK(asked.size() == 5);
  CHECK((Ids(events.back().games) == std::vector<std::uint64_t>{1207658924, 1207658926}));
  CHECK(events.back().games[0].hero_source == Hero(1207658924));
  CHECK(f.LogText().find("library owned=5 fetched=0 not_modified=4 missing=1 hidden=2 failed=0") != std::string::npos);

  // A product gone from gamesdb leaves the library and its saved entry; one that fails keeps its saved copy; a
  // malformed answer counts as a failure too and keeps the saved copy.
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson(owned)));
  f.stub.SetProducts([&](std::uint64_t id, const std::string&) {
    if (id == 1207658924 || id == 1207658928) return Status(404);
    if (id == 1207658926) return Status(500);
    if (id == 1207658925) return Stub::Step{Answer(200, "{\"external_id\":\"1\"}"), nullptr};
    return Status(304);
  });
  f.source.Refresh(kAccount, 3);
  events = f.Until(LibraryEventKind::kComplete);
  CHECK((Ids(events.back().games) == std::vector<std::uint64_t>{1207658926}));
  CHECK(events.back().games[0].title == Title(1207658926));
  CHECK(!Exists(f.ProductFile(1207658924)) && Exists(f.ProductFile(1207658926)));
  CHECK(Exists(f.ProductFile(1207658925)));
  CHECK(f.LogText().find("library owned=5 fetched=0 not_modified=1 missing=2 hidden=2 failed=2") != std::string::npos);

  // A product that failed with nothing saved is skipped this time and fetched on the next refresh.
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({1207658924, 1207658926})));
  f.stub.SetProducts([&](std::uint64_t id, const std::string&) {
    return id == 1207658924 ? Status(503) : Status(304);
  });
  f.source.Refresh(kAccount, 4);
  events = f.Until(LibraryEventKind::kComplete);
  CHECK((Ids(events.back().games) == std::vector<std::uint64_t>{1207658926}));
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({1207658924, 1207658926})));
  f.stub.SetProducts([&](std::uint64_t id, const std::string&) {
    return id == 1207658924 ? Product(id, "game", 2) : Status(304);
  });
  f.source.Refresh(kAccount, 5);
  events = f.Until(LibraryEventKind::kComplete);
  CHECK((Ids(events.back().games) == std::vector<std::uint64_t>{1207658924, 1207658926}));

  // Nothing of the account, a title or an address is logged.
  const auto log = f.LogText();
  CHECK(log.find(kUserId) == std::string::npos && log.find("https://") == std::string::npos);
  CHECK(log.find("Synthetic Game") == std::string::npos && log.find("1207658924") == std::string::npos);
}

void Batches() {
  Fixture f;
  std::vector<std::uint64_t> owned;
  for (std::uint64_t i = 1; i <= 120; ++i) owned.push_back(2000000000 + i);
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson(owned)));
  // The first product of the second and third batches waits for the test, so no batch event is replaced by the next
  // before it is read.
  auto second = std::make_shared<Gate>();
  auto third = std::make_shared<Gate>();
  f.stub.SetProducts([&](std::uint64_t id, const std::string&) {
    auto step = Product(id);
    if (id == 2000000051) step.gate = second;
    if (id == 2000000101) step.gate = third;
    return step;
  });
  f.source.Refresh(kAccount, 1);
  std::vector<std::size_t> batches;
  bool complete = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!complete) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    for (auto& event : f.source.Pump(16)) {
      CHECK(event.kind != LibraryEventKind::kFailure);
      if (event.kind == LibraryEventKind::kCached) {
        batches.push_back(event.games.size());
        // Products in owned order, the fetched ones only.
        CHECK(Ids(event.games).front() == 2000000001);
        if (batches.size() == 1) second->Open();
        if (batches.size() == 2) third->Open();
      }
      if (event.kind == LibraryEventKind::kComplete) {
        CHECK(event.games.size() == 120);
        CHECK(Ids(event.games) == owned);
        complete = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK((batches == std::vector<std::size_t>{50, 100, 120}));

  // The next refresh shows the saved library first, then batches: products not fetched yet keep their saved entry.
  // The first product waits until the saved library was read, and the first of the second batch until the first
  // batch was, so no event is replaced by the next before it is read.
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson(owned)));
  auto first = std::make_shared<Gate>();
  auto held = std::make_shared<Gate>();
  f.stub.SetProducts([&](std::uint64_t id, const std::string&) {
    Stub::Step step = Status(304);
    if (id == 2000000001) step.gate = first;
    if (id == 2000000051) step.gate = held;
    return step;
  });
  f.source.Refresh(kAccount, 2);
  std::vector<LibraryEvent> events;
  const auto wait = [&](std::size_t count) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (events.size() < count) {
      CHECK(std::chrono::steady_clock::now() < until);
      for (auto& event : f.source.Pump(16)) events.push_back(std::move(event));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  wait(2);
  CHECK(events[0].kind == LibraryEventKind::kLoading);
  CHECK(events[1].kind == LibraryEventKind::kCached && events[1].games.size() == 120 && events[1].cache_available);
  first->Open();
  wait(3);
  CHECK(events[2].kind == LibraryEventKind::kCached && events[2].games.size() == 120);
  CHECK(Ids(events[2].games) == owned);
  held->Open();
  const auto rest = f.Until(LibraryEventKind::kComplete);
  CHECK(rest.back().kind == LibraryEventKind::kComplete && rest.back().games.size() == 120);
}

void Failures() {
  {  // A revoked token: the session is cleared and the library says so.
    Fixture f;
    f.now += 3600;  // the access token has expired: Token refreshes
    f.stub.Script(Endpoint::kToken, InvalidGrant());
    f.source.Refresh(kAccount, 1);
    const auto events = f.Until(LibraryEventKind::kComplete);
    CHECK(events.back().kind == LibraryEventKind::kFailure);
    CHECK(events.back().error == LibraryError::kCredentialsRevoked && !events.back().cache_available);
    CHECK(!f.session.signed_in());
  }
  {  // No answer while refreshing the token: a network failure, with the saved library still available.
    Fixture f;
    LibrarySnapshot snapshot;
    snapshot.schema_version = 8;
    snapshot.retrieved_at_unix_seconds = 5;
    GameSummary game;
    game.id = GameId::Gog(7);
    game.title = "Saved";
    snapshot.games.push_back(game);
    CHECK(f.cache.Save(kAccount, snapshot).ok());
    f.now += 3600;
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
    f.source.Refresh(kAccount, 1);
    const auto events = f.Until(LibraryEventKind::kComplete);
    CHECK(events[1].kind == LibraryEventKind::kCached && events[1].games.size() == 1);
    CHECK(events.back().kind == LibraryEventKind::kFailure && events.back().error == LibraryError::kNetwork);
    CHECK(events.back().cache_available);
  }
  {  // Another account, or none: no session for it.
    Fixture f;
    f.source.Refresh(kAccount + 1, 1);
    auto events = f.Until(LibraryEventKind::kComplete);
    CHECK(events.back().kind == LibraryEventKind::kFailure && events.back().error == LibraryError::kNoSession);
    CHECK(f.session.SignOut().ok);
    f.source.Refresh(kAccount, 2);
    events = f.Until(LibraryEventKind::kComplete);
    CHECK(events.back().kind == LibraryEventKind::kFailure && events.back().error == LibraryError::kNoSession);
  }
  {  // The owned list refuses the access token: the session drops it, refreshes once and asks again.
    Fixture f;
    f.stub.Script(Endpoint::kOwned, Answer(401, ""));
    f.stub.Script(Endpoint::kToken, Tokens(3));
    f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({})));
    f.source.Refresh(kAccount, 1);
    const auto events = f.Until(LibraryEventKind::kComplete);
    CHECK(events.back().kind == LibraryEventKind::kComplete);
    std::vector<std::string> sent;
    for (const auto& seen : f.stub.seen())
      if (seen.endpoint == Endpoint::kOwned) sent.push_back(seen.authorization);
    CHECK((sent == std::vector<std::string>{"Bearer " + AccessText(2), "Bearer " + AccessText(3)}));
    CHECK(f.session.signed_in() && f.stub.Unused() == 0);
  }
  {  // 401, and GOG refuses the refresh token too: revoked, and the session is cleared.
    Fixture f;
    f.stub.Script(Endpoint::kOwned, Answer(401, ""));
    f.stub.Script(Endpoint::kToken, InvalidGrant());
    f.source.Refresh(kAccount, 1);
    CHECK(f.Until(LibraryEventKind::kComplete).back().error == LibraryError::kCredentialsRevoked);
    CHECK(!f.session.signed_in() && f.stub.Unused() == 0);
  }
  {  // 401 again with the new token: revoked, with no third try.
    Fixture f;
    f.stub.Script(Endpoint::kOwned, Answer(401, ""));
    f.stub.Script(Endpoint::kToken, Tokens(3));
    f.stub.Script(Endpoint::kOwned, Answer(401, ""));
    f.source.Refresh(kAccount, 1);
    CHECK(f.Until(LibraryEventKind::kComplete).back().error == LibraryError::kCredentialsRevoked);
    CHECK(f.stub.Calls(Endpoint::kOwned) == 2 && f.stub.Calls(Endpoint::kToken) == 2);
  }
  {  // The owned list: unreachable, malformed.
    Fixture f;
    f.stub.Script(Endpoint::kOwned, Answer(502, ""));
    f.source.Refresh(kAccount, 1);
    CHECK(f.Until(LibraryEventKind::kComplete).back().error == LibraryError::kNetwork);
    f.stub.Script(Endpoint::kOwned, Answer(200, "{\"owned\":5}"));
    f.source.Refresh(kAccount, 2);
    CHECK(f.Until(LibraryEventKind::kComplete).back().error == LibraryError::kProtocol);
    CHECK(!Exists(f.CacheFile()));
  }
}

// The network drops in the middle of a refresh: after a few products in a row get no answer, the refresh stops with a
// network failure instead of waiting out every remaining product. An answered product, even a refused one, starts the
// count again.
void NetworkDropsDuringProducts() {
  constexpr auto kLimit = GogLibrarySource::kMaxProductsWithoutAnswer;
  {
    Fixture f;
    std::vector<std::uint64_t> ids;
    for (std::uint64_t id = 1207658900; id < 1207658900 + 40; ++id) ids.push_back(id);
    f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson(ids)));
    f.stub.SetProducts([](std::uint64_t id, const std::string&) -> Stub::Step {
      if (id < 1207658902) return Product(id);
      return {Failure(HttpError::kTimeout), nullptr};
    });
    f.source.Refresh(kAccount, 1);
    const auto events = f.Until(LibraryEventKind::kComplete);
    CHECK(events.back().kind == LibraryEventKind::kFailure && events.back().error == LibraryError::kNetwork);
    CHECK(f.stub.Calls(Endpoint::kProduct) == static_cast<int>(2 + kLimit));
    CHECK(!Exists(f.CacheFile()) && Exists(f.ProductFile(1207658901)));
  }
  {  // Failures between answers: every product is asked, and the refresh completes.
    Fixture f;
    std::vector<std::uint64_t> ids;
    for (std::uint64_t id = 1207658900; id < 1207658900 + 4 * kLimit; ++id) ids.push_back(id);
    f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson(ids)));
    f.stub.SetProducts([](std::uint64_t id, const std::string&) -> Stub::Step {
      if (id % kLimit == 0) return Status(500);
      if (id % kLimit == 1) return Product(id);
      return {Failure(HttpError::kTimeout), nullptr};
    });
    f.source.Refresh(kAccount, 1);
    const auto events = f.Until(LibraryEventKind::kComplete);
    CHECK(events.back().kind == LibraryEventKind::kComplete);
    CHECK(f.stub.Calls(Endpoint::kProduct) == static_cast<int>(ids.size()));
  }
}

void LoadCachedOffline() {
  Fixture f;
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({1207658924})));
  f.stub.SetProducts([](std::uint64_t id, const std::string&) { return Product(id); });
  f.source.Refresh(kAccount, 1);
  f.Until(LibraryEventKind::kComplete);
  // No answer is scripted: any request would end the test.
  f.stub.SetProducts(nullptr);
  const auto request = f.source.LoadCached(kAccount, 2);
  auto events = f.Until(LibraryEventKind::kCached);
  CHECK(events.back().kind == LibraryEventKind::kCached && events.back().request_id == request);
  CHECK(events.back().cache_available && (Ids(events.back().games) == std::vector<std::uint64_t>{1207658924}));
  CHECK(events.back().games[0].hero_source == Hero(1207658924));
  // Another account has nothing saved.
  f.source.LoadCached(kAccount + 1, 3);
  events = f.Until(LibraryEventKind::kCached);
  CHECK(events.back().kind == LibraryEventKind::kFailure && events.back().error == LibraryError::kPersistence);
  CHECK(!events.back().cache_available);
}

void ClearAndInvalidate() {
  Fixture f;
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({1207658924, 1207658925})));
  f.stub.SetProducts([](std::uint64_t id, const std::string&) { return Product(id); });
  f.source.Refresh(kAccount, 1);
  f.Until(LibraryEventKind::kComplete);
  CHECK(Exists(f.CacheFile()) && Exists(f.ProductFile(1207658924)));
  // Clear is answered even when Invalidate follows at once (a sign-out resets the store right after).
  const auto request = f.source.Clear(kAccount, 2);
  CHECK(request != 0);
  f.source.Invalidate(3);
  const auto events = f.Until(LibraryEventKind::kCleared);
  CHECK(events.size() == 1 && events[0].kind == LibraryEventKind::kCleared && events[0].request_id == request);
  CHECK(!Exists(f.CacheFile()));
  CHECK(fs::is_empty(f.root / "gog" / "products"));
  CHECK(Exists(f.root / "gog" / "session"));  // the session is the account's to clear
  // An older generation is refused afterwards.
  CHECK(f.source.Refresh(kAccount, 2) == 0);
}

void InvalidateDuringRefresh() {
  Fixture f;
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({1207658924, 1207658925, 1207658926})));
  auto gate = std::make_shared<Gate>();
  f.stub.SetProducts([&](std::uint64_t id, const std::string&) {
    auto step = Product(id);
    if (id == 1207658925) step.gate = gate;
    return step;
  });
  f.source.Refresh(kAccount, 5);
  gate->WaitEntered(1);
  f.source.Invalidate(6);
  gate->Open();
  // Nothing more of that generation: no batch, no completion, no saved library, no further product asked.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& event : f.source.Pump(16)) CHECK(event.kind == LibraryEventKind::kLoading);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(f.stub.Calls(Endpoint::kProduct) == 2);
  CHECK(!Exists(f.CacheFile()));
  // The next generation works.
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({1207658924})));
  f.stub.SetProducts([](std::uint64_t, const std::string&) { return Status(304); });
  f.source.Refresh(kAccount, 7);
  const auto events = f.Until(LibraryEventKind::kComplete);
  CHECK(events.back().kind == LibraryEventKind::kComplete && events.back().account_generation == 7);
  CHECK((Ids(events.back().games) == std::vector<std::uint64_t>{1207658924}));
}

}  // namespace

// The library tags (#536): read after the products, sent with kComplete and saved; a failed read keeps the saved
// ones (sent with the saved library first); offline they come with the saved library; Clear deletes them.
std::string TagsPageJson(const std::string& products) {
  return R"({"page":1,"totalPages":1,"tags":[{"id":"100000001","name":"Couch","productCount":1},)"
         R"({"id":"100000003","name":"Unused","productCount":0}],"products":[)" + products + "]}";
}
void TagsWithTheLibrary() {
  Fixture f;
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({1207658924, 1207658925})));
  f.stub.SetProducts([](std::uint64_t id, const std::string&) { return Product(id); });
  f.stub.Script(Endpoint::kTags, Answer(200, TagsPageJson(R"({"id":1207658924,"tags":["100000001"]})")));
  f.stub.Script(Endpoint::kTags, Answer(200, TagsPageJson("")));  // the hidden view
  f.source.Refresh(kAccount, 1);
  auto events = f.Until(LibraryEventKind::kComplete);
  const auto& complete = events.back();
  CHECK(complete.kind == LibraryEventKind::kComplete && complete.collections_known && complete.collections.size() == 2);
  CHECK(complete.collections[0].id == "100000001" && complete.collections[0].name == "Couch" &&
        (complete.collections[0].members == std::vector<GameId>{GameId::Gog(1207658924)}));
  CHECK(complete.collections[1].members.empty());
  CHECK(Exists(f.TagsFile()) && f.LogText().find("library tags=2 members=1 pages=2") != std::string::npos);
  CHECK(f.LogText().find("Couch") == std::string::npos);  // never a name in the log

  // GOG doesn't answer the tags: the saved ones come with the saved library and stay with the fresh one.
  f.stub.Script(Endpoint::kOwned, Answer(200, OwnedJson({1207658924, 1207658925})));
  f.source.Refresh(kAccount, 2);
  events = f.Until(LibraryEventKind::kComplete);
  const auto first_cached = std::find_if(events.begin(), events.end(),
                                         [](const LibraryEvent& event) { return event.kind == LibraryEventKind::kCached; });
  CHECK(first_cached != events.end() && first_cached->collections_known && first_cached->collections.size() == 2);
  CHECK(events.back().collections_known && events.back().collections.size() == 2);

  // Offline: with the saved library.
  f.source.LoadCached(kAccount, 3);
  events = f.Until(LibraryEventKind::kCached);
  CHECK(events.back().kind == LibraryEventKind::kCached && events.back().collections_known &&
        events.back().collections.size() == 2);

  f.source.Clear(kAccount, 4);
  f.Until(LibraryEventKind::kCleared);
  CHECK(!Exists(f.TagsFile()));
}

int main(int argc, char** argv) {
  CHECK(argc == 3);
  const fs::path parent(argv[2]);
  fs::remove_all(parent);
  fs::create_directories(parent);
  g_base = fs::canonical(parent);
  FirstAndSecondRefresh();
  Batches();
  Failures();
  NetworkDropsDuringProducts();
  LoadCachedOffline();
  ClearAndInvalidate();
  InvalidateDuringRefresh();
  TagsWithTheLibrary();
  fs::remove_all(g_base);
  std::printf("gog library ok\n");
  return 0;
}
