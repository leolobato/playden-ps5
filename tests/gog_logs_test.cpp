// SPDX-License-Identifier: GPL-3.0-or-later
// gog.log after a stubbed run of everything GOG does so far: restore, a sign-in with a code, a library refresh, the
// library cleared and a sign-out, with the session, the account and the library writing to one log file. The file
// must not name the account (username, user ID), hold a token or the code, or carry an address. The install half
// comes with installs. Synthetic tokens, accounts and products only.
#include "gog/account_key.h"
#include "gog/session.h"
#include "gog/session_store.h"
#include "gog_stub_transport.h"
#include "library/cache.h"
#include "stores/gog.h"
#include "stores/gog_library.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace playden;
using namespace gog_test;
using playden::stores::AccountEvent;
using playden::stores::AccountEventKind;
namespace fs = std::filesystem;

namespace {

std::string g_fixtures;
fs::path g_base;
std::mutex g_log_mutex;

// Appends each line to gog.log, as the app's log sink does.
void Append(const fs::path& file, const std::string& line) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  FILE* out = std::fopen(file.c_str(), "ab");
  CHECK(out != nullptr);
  std::fputs(line.c_str(), out);
  std::fputc('\n', out);
  CHECK(std::fclose(out) == 0);
}

std::vector<AccountEvent> Events(stores::GogAccount& account, std::size_t count) {
  std::vector<AccountEvent> events;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (events.size() < count) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    for (auto& event : account.PumpEvents(16)) events.push_back(std::move(event));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return events;
}

LibraryEvent Last(stores::GogLibrarySource& source, LibraryEventKind kind) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    for (auto& event : source.Pump(16)) {
      CHECK(event.kind != LibraryEventKind::kFailure);
      if (event.kind == kind) return event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

std::string GameJson(std::uint64_t id) {
  const std::string text = std::to_string(id);
  const std::string art = "https://images.gog.com/" + std::string(64, 'd') + "{formatter}.{ext}?namespace=gamesdb";
  return R"({"external_id":")" + text + R"(","type":"game","title":{"en-US":"Synthetic Logged Game"},)" +
         R"("supported_operating_systems":[{"slug":"windows"}],"game":{"visible_in_library":true,)" +
         R"("vertical_cover":{"url_format":")" + art + R"("},"background":{"url_format":")" + art + R"("}}})";
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  g_fixtures = argv[1];
  const fs::path parent(argv[2]);
  fs::remove_all(parent);
  fs::create_directories(parent);
  g_base = fs::canonical(parent);
  const fs::path root = g_base / "user";
  const fs::path logs = g_base / "logs";
  const fs::path library = g_base / "library";
  fs::create_directories(root);
  fs::create_directories(logs);
  fs::create_directories(library);
  CHECK(::chmod(root.c_str(), 0755) == 0);
  const fs::path log_file = logs / "gog.log";
  const auto sink = [&](const std::string& line) { Append(log_file, line); };
  const std::string code = "SyntheticCode-logged-0123456789";
  {
    gog::SessionStore::Options options;
    options.user_root = root.string();
    gog::SessionStore store(options);
    Stub stub;
    std::atomic<std::int64_t> now{100000};
    gog::Session session(store, stub.Transport(), [&] { return now.load(); }, sink);
    stores::GogAccount account(session, store, sink);
    Storage storage(library.string());
    LibraryCache cache(storage, "library-gog-");
    stores::GogLibrarySource source(session, stub.Transport(), cache, root.string(), sink,
                                    [] { return std::uint64_t{1'800'000'000}; });

    // Startup with nothing saved.
    account.RestoreSession();
    CHECK(Events(account, 1)[0].kind == AccountEventKind::kSignedOut);

    // Sign-in through the relay's code.
    stub.Script(Endpoint::kToken, Tokens(1));
    stub.Script(Endpoint::kUser, Answer(200, ReadFile(g_fixtures + "/user-data.json")));
    const auto request = account.BeginSignIn();
    CHECK(request != 0);
    const auto answer = account.SignInWithCode(code, request);
    CHECK(answer.kind == stores::SignInAnswer::Kind::kSignedIn && answer.display_name == kUsername);
    const auto signed_in = Events(account, 1)[0];
    CHECK(signed_in.kind == AccountEventKind::kSignedIn);
    const auto key = signed_in.account_key;
    CHECK(key == gog::AccountKey(kUserId));

    // The library: one game, one product gamesdb doesn't know, one that fails.
    stub.Script(Endpoint::kOwned, Answer(200, "{\"owned\":[1207658924,1207658925,1207658926]}"));
    stub.SetProducts([](std::uint64_t id, const std::string&) {
      if (id == 1207658925) return Stub::Step{Answer(404, ""), nullptr};
      if (id == 1207658926) return Stub::Step{Failure(HttpError::kTimeout), nullptr};
      auto response = Answer(200, GameJson(id));
      response.headers.emplace_back("ETag", "W/\"synthetic-etag\"");
      return Stub::Step{response, nullptr};
    });
    CHECK(source.Refresh(key, 1) != 0);
    const auto complete = Last(source, LibraryEventKind::kComplete);
    CHECK(complete.games.size() == 1);

    // Sign-out: the library is cleared and the session forgotten.
    CHECK(source.Clear(key, 2) != 0);
    Last(source, LibraryEventKind::kCleared);
    account.SignOut();
    CHECK(Events(account, 1)[0].kind == AccountEventKind::kSignedOut);
    account.Shutdown();
    source.Shutdown();
  }

  const std::string log = ReadFile(log_file.string());
  std::printf("%s", log.c_str());
  CHECK(log.find("library owned=3 fetched=1 not_modified=0 missing=1 hidden=0 failed=1") != std::string::npos);
  for (const std::string& secret : {kUsername, kUserId, AccessText(1), RefreshText(1), code, std::string("https://"),
                                    std::string("images.gog.com"), std::string("synthetic-etag"),
                                    std::string("Synthetic Logged Game"), std::string("1207658924")})
    CHECK(log.find(secret) == std::string::npos);
  fs::remove_all(g_base);
  std::printf("gog logs ok\n");
  return 0;
}
