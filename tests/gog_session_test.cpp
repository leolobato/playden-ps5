// SPDX-License-Identifier: GPL-3.0-or-later
// The GOG token owner: restore at startup, the sign-in exchange, token refresh (one at a time), revocation, and
// generations that drop late answers after Back, a sign-out or a newer sign-in. A stub transport scripts GOG's answers
// and a fake clock drives expiry. Synthetic tokens and accounts only.
#include "gog/account_key.h"
#include "gog/session.h"
#include "gog/session_store.h"
#include "gog_stub_transport.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace playden;
using namespace gog_test;
using playden::gog::AccessToken;
using playden::gog::ExchangeOutcome;
using playden::gog::RestoreOutcome;
using playden::gog::SessionStore;
using playden::gog::StoredSession;
namespace fs = std::filesystem;

namespace {

std::string g_fixtures;
fs::path g_base;
int g_roots = 0;

const std::atomic<bool> kNoCancel{false};

fs::path FreshRoot() {
  const fs::path root = g_base / ("root-" + std::to_string(++g_roots));
  fs::create_directories(root);
  CHECK(::chmod(root.c_str(), 0755) == 0);
  return root;
}

SessionStore::Options Options(const fs::path& root, SessionStore::Writer write = {}) {
  SessionStore::Options options;
  options.user_root = root.string();
  options.write = std::move(write);
  return options;
}

bool Exists(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0;
}

fs::path SessionFile(const fs::path& root) { return root / "gog" / "session"; }

// The refresh token saved in root, read back by a new store; empty when nothing is saved.
std::string SavedRefresh(const fs::path& root) {
  SessionStore store(Options(root));
  auto loaded = store.Load();
  return loaded.status == gog::LoadStatus::kOk ? loaded.session.refresh_token : std::string();
}

void SaveSession(const fs::path& root, const std::string& refresh) {
  SessionStore store(Options(root));
  StoredSession session;
  session.refresh_token = refresh;
  session.user_id = kUserId;
  session.username = kUsername;
  CHECK(store.Save(session).ok);
}

HttpResponse UserData() { return Answer(200, ReadFile(g_fixtures + "/user-data.json")); }

// A session over a fresh root, with its stub, clock and log.
struct Fixture {
  fs::path root = FreshRoot();
  std::unique_ptr<SessionStore> store;
  Stub stub;
  std::atomic<std::int64_t> now{100000};
  std::atomic<int> clock_reads{0};
  std::shared_ptr<std::vector<std::string>> lines = std::make_shared<std::vector<std::string>>();
  std::shared_ptr<std::mutex> lines_mutex = std::make_shared<std::mutex>();
  std::unique_ptr<gog::Session> session;

  explicit Fixture(SessionStore::Writer write = {}) {
    store = std::make_unique<SessionStore>(Options(root, std::move(write)));
    auto lines_copy = lines;
    auto mutex_copy = lines_mutex;
    session = std::make_unique<gog::Session>(
        *store, stub.Transport(),
        [this] {
          ++clock_reads;
          return now.load();
        },
        [lines_copy, mutex_copy](const std::string& line) {
          std::lock_guard<std::mutex> lock(*mutex_copy);
          lines_copy->push_back(line);
        });
  }

  // Signs in with the n-th synthetic tokens.
  ExchangeOutcome SignIn(int n) {
    stub.Script(Endpoint::kToken, Tokens(n));
    stub.Script(Endpoint::kUser, UserData());
    const auto epoch = session->BeginSignIn();
    return session->Exchange("SyntheticCode-" + std::to_string(n) + "-0123456789", epoch, kNoCancel);
  }

  // Starts a Token() call on another thread and returns once it waits for the refresh already running: Token() reads
  // the clock under the session's lock just before it finds the refresh running, and holds the lock until it waits.
  std::thread WaitingToken(AccessToken* out) {
    const int before = clock_reads.load();
    std::thread thread([this, out] { *out = session->Token(kNoCancel); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (clock_reads.load() == before) {
      CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return thread;
  }

  bool Logged(const std::string& part) {
    std::lock_guard<std::mutex> lock(*lines_mutex);
    for (const auto& line : *lines)
      if (line.find(part) != std::string::npos) return true;
    return false;
  }

  // No log line names the account, a token or a code.
  void CheckLogClean() {
    std::lock_guard<std::mutex> lock(*lines_mutex);
    for (const auto& line : *lines) {
      CHECK(line.find(kUsername) == std::string::npos);
      CHECK(line.find(kUserId) == std::string::npos);
      CHECK(line.find("synthetic-") == std::string::npos);
      CHECK(line.find("SyntheticCode") == std::string::npos);
      CHECK(line.find("gog.com") == std::string::npos);
    }
  }
};

void TestRestore() {
  {  // No file: signed out.
    Fixture f;
    const auto outcome = f.session->Restore(kNoCancel);
    CHECK(outcome.kind == RestoreOutcome::Kind::kNone);
    CHECK(!f.session->signed_in());
    CHECK(f.stub.Calls(Endpoint::kToken) == 0);
  }
  {  // A good refresh signs in and saves the rotated refresh token.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    f.stub.Script(Endpoint::kToken, Tokens(2));
    const auto outcome = f.session->Restore(kNoCancel);
    CHECK(outcome.kind == RestoreOutcome::Kind::kSignedIn);
    CHECK(Param(f.stub.seen()[0].url, "refresh_token") == RefreshText(1));
    CHECK(SavedRefresh(f.root) == RefreshText(2));
    const auto identity = f.session->identity();
    CHECK(identity.username == kUsername && identity.user_id == kUserId);
    CHECK(identity.account_key == gog::AccountKey(kUserId));
    CHECK(identity.generation == outcome.generation && outcome.generation != 0);
    const auto token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kOk && token.token == AccessText(2));
    CHECK(f.stub.Calls(Endpoint::kToken) == 1);
    CHECK(f.Logged("refresh status=200 result=ok"));
    CHECK(f.Logged("restore result=signed-in"));
    f.CheckLogClean();
  }
  {  // 400 invalid_grant: expired, and the file is gone.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    f.stub.Script(Endpoint::kToken, InvalidGrant());
    CHECK(f.session->Restore(kNoCancel).kind == RestoreOutcome::Kind::kExpired);
    CHECK(!Exists(SessionFile(f.root)));
    CHECK(!f.session->signed_in() && f.session->identity().account_key == 0);
    CHECK(f.session->Token(kNoCancel).status == AccessToken::Status::kSignedOut);
    CHECK(f.Logged("refresh status=400 result=rejected"));
    f.CheckLogClean();
  }
  {  // A timeout: offline, with the file kept and the identity from it; Token() refreshes later.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
    const auto outcome = f.session->Restore(kNoCancel);
    CHECK(outcome.kind == RestoreOutcome::Kind::kOffline);
    CHECK(SavedRefresh(f.root) == RefreshText(1));
    const auto identity = f.session->identity();
    CHECK(identity.username == kUsername && identity.user_id == kUserId);
    CHECK(identity.account_key == gog::AccountKey(kUserId));
    CHECK(f.Logged("restore result=offline"));
    f.stub.Script(Endpoint::kToken, Tokens(3));
    const auto token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kOk && token.token == AccessText(3));
    CHECK(SavedRefresh(f.root) == RefreshText(3));
    f.CheckLogClean();
  }
  {  // A corrupt file: a storage failure.
    Fixture f;
    fs::create_directories(f.root / "gog");
    CHECK(::chmod((f.root / "gog").c_str(), 0700) == 0);
    FILE* file = std::fopen(SessionFile(f.root).c_str(), "wb");
    CHECK(file != nullptr);
    std::fputs("not a session file", file);
    std::fclose(file);
    CHECK(::chmod(SessionFile(f.root).c_str(), 0600) == 0);
    const auto outcome = f.session->Restore(kNoCancel);
    CHECK(outcome.kind == RestoreOutcome::Kind::kStorage);
    CHECK(!f.session->signed_in());
    CHECK(f.stub.Calls(Endpoint::kToken) == 0);
  }
  {  // A sign-out while the restore's refresh runs: its late answer installs nothing.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(2), gate);
    RestoreOutcome outcome;
    std::thread restore([&] { outcome = f.session->Restore(kNoCancel); });
    gate->WaitEntered(1);
    CHECK(f.session->SignOut().ok);
    gate->Open();
    restore.join();
    CHECK(outcome.kind == RestoreOutcome::Kind::kCanceled);
    CHECK(!f.session->signed_in() && f.session->identity().account_key == 0);
    CHECK(!Exists(SessionFile(f.root)));
    CHECK(f.session->Token(kNoCancel).status == AccessToken::Status::kSignedOut);
  }
}

void TestToken() {
  {  // No refresh while 600 s or more remain; one at 599 s.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    CHECK(f.stub.Calls(Endpoint::kToken) == 1);
    const auto start = f.now.load();
    f.now = start + 3600 - 600;
    auto token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kOk && token.token == AccessText(1));
    CHECK(f.stub.Calls(Endpoint::kToken) == 1);
    f.now = start + 3600 - 599;
    f.stub.Script(Endpoint::kToken, Tokens(2));
    token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kOk && token.token == AccessText(2));
    CHECK(f.stub.Calls(Endpoint::kToken) == 2);
    CHECK(Param(f.stub.seen().back().url, "refresh_token") == RefreshText(1));
    CHECK(SavedRefresh(f.root) == RefreshText(2));
    // The new token is good for another hour.
    token = f.session->Token(kNoCancel);
    CHECK(token.token == AccessText(2) && f.stub.Calls(Endpoint::kToken) == 2);
    f.CheckLogClean();
  }
  {  // An API refused the token: only that token of that generation is dropped, and the next Token() refreshes.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    const auto generation = f.session->generation();
    f.session->InvalidateAccessToken(generation + 1, AccessText(1));
    f.session->InvalidateAccessToken(generation, AccessText(7));
    auto token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kOk && token.token == AccessText(1));
    CHECK(f.stub.Calls(Endpoint::kToken) == 1);
    f.session->InvalidateAccessToken(generation, AccessText(1));
    f.stub.Script(Endpoint::kToken, Tokens(2));
    token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kOk && token.token == AccessText(2));
    CHECK(f.stub.Calls(Endpoint::kToken) == 2 && SavedRefresh(f.root) == RefreshText(2));
    f.CheckLogClean();
  }
  {  // Two callers at once: one refresh, and both get the new token.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    f.now += 3600;
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(2), gate);
    AccessToken first, second;
    std::thread a([&] { first = f.session->Token(kNoCancel); });
    gate->WaitEntered(1);
    std::thread b = f.WaitingToken(&second);
    gate->Open();
    a.join();
    b.join();
    CHECK(first.status == AccessToken::Status::kOk && first.token == AccessText(2));
    CHECK(second.status == AccessToken::Status::kOk && second.token == AccessText(2));
    CHECK(f.stub.Calls(Endpoint::kToken) == 2);  // the exchange and one refresh
  }
  {  // Two callers while the refresh fails: one call, and both are told to try later.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    f.now += 3600;
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kConnectionFailed), gate);
    AccessToken first, second;
    std::thread a([&] { first = f.session->Token(kNoCancel); });
    gate->WaitEntered(1);
    std::thread b = f.WaitingToken(&second);
    gate->Open();
    a.join();
    b.join();
    CHECK(first.status == AccessToken::Status::kNetwork);
    CHECK(second.status == AccessToken::Status::kNetwork);
    CHECK(f.stub.Calls(Endpoint::kToken) == 2);
  }
  {  // 401: revoked, the listener is told once, the file is gone, and the next call is signed out.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    const auto generation = f.session->generation();
    std::vector<std::uint64_t> revoked;
    f.session->SetRevokedListener([&](std::uint64_t old) { revoked.push_back(old); });
    f.now += 3600;
    f.stub.Script(Endpoint::kToken, Answer(401, "{}"));
    auto token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kRevoked && token.token.empty());
    CHECK(revoked.size() == 1 && revoked[0] == generation);
    CHECK(f.session->generation() > generation);
    CHECK(!Exists(SessionFile(f.root)));
    CHECK(f.session->identity().account_key == 0 && f.session->identity().username.empty());
    token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kSignedOut);
    CHECK(revoked.size() == 1);
    CHECK(f.stub.Calls(Endpoint::kToken) == 2);
    f.CheckLogClean();
  }
  {  // A network error keeps the file and the session.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    f.now += 3600;
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
    CHECK(f.session->Token(kNoCancel).status == AccessToken::Status::kNetwork);
    CHECK(f.session->signed_in() && f.session->identity().username == kUsername);
    CHECK(SavedRefresh(f.root) == RefreshText(1));
    f.stub.Script(Endpoint::kToken, Answer(503, "busy"));
    CHECK(f.session->Token(kNoCancel).status == AccessToken::Status::kNetwork);
    f.stub.Script(Endpoint::kToken, Tokens(2));
    const auto token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kOk && token.token == AccessText(2));
    CHECK(Param(f.stub.seen().back().url, "refresh_token") == RefreshText(1));
  }
  {  // A canceled caller gets kCanceled and nothing changes.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    f.now += 3600;
    std::atomic<bool> cancel{true};
    CHECK(f.session->Token(cancel).status == AccessToken::Status::kCanceled);
    CHECK(f.stub.Calls(Endpoint::kToken) == 1);
    CHECK(f.session->signed_in());
  }
  {  // A refresh that finishes after the sign-out is dropped: nothing saved, no identity.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    int revoked = 0;
    f.session->SetRevokedListener([&](std::uint64_t) { ++revoked; });
    f.now += 3600;
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(2), gate);
    AccessToken token;
    std::thread refresh([&] { token = f.session->Token(kNoCancel); });
    gate->WaitEntered(1);
    CHECK(f.session->SignOut().ok);
    CHECK(!Exists(SessionFile(f.root)));
    gate->Open();
    refresh.join();
    CHECK(token.status == AccessToken::Status::kSignedOut && token.token.empty());
    CHECK(!Exists(SessionFile(f.root)));
    CHECK(SavedRefresh(f.root).empty());
    const auto identity = f.session->identity();
    CHECK(identity.username.empty() && identity.user_id.empty() && identity.account_key == 0);
    CHECK(!f.session->signed_in() && revoked == 0);
    CHECK(f.Logged("refresh result=dropped"));
  }
  {  // Signed out: no call.
    Fixture f;
    CHECK(f.session->Token(kNoCancel).status == AccessToken::Status::kSignedOut);
    CHECK(f.stub.Calls(Endpoint::kToken) == 0);
  }
}

void TestExchange() {
  {  // The good path: saved, and the username comes back.
    Fixture f;
    const auto outcome = f.SignIn(1);
    CHECK(outcome.kind == ExchangeOutcome::Kind::kSignedIn);
    CHECK(outcome.username == kUsername);
    CHECK(outcome.generation == f.session->generation());
    CHECK(SavedRefresh(f.root) == RefreshText(1));
    const auto seen = f.stub.seen();
    CHECK(seen.size() == 2 && seen[0].endpoint == Endpoint::kToken && seen[1].endpoint == Endpoint::kUser);
    CHECK(Param(seen[0].url, "code") == "SyntheticCode-1-0123456789");
    CHECK(seen[1].authorization == "Bearer " + AccessText(1));
    const auto identity = f.session->identity();
    CHECK(identity.username == kUsername && identity.user_id == kUserId);
    CHECK(identity.account_key == gog::AccountKey(kUserId));
    CHECK(f.Logged("exchange status=200 result=ok"));
    CHECK(f.Logged("user status=200 result=ok"));
    // The epoch is spent.
    CHECK(f.session->Exchange("SyntheticCode-again-0123456789", 1, kNoCancel).kind == ExchangeOutcome::Kind::kStale);
    CHECK(f.stub.Calls(Endpoint::kToken) == 1);
    f.CheckLogClean();
  }
  {  // A userData answer with many other fields is read the same way.
    Fixture f;
    f.stub.Script(Endpoint::kToken, Tokens(1));
    f.stub.Script(Endpoint::kUser, Answer(200, ReadFile(g_fixtures + "/user-data-extra-fields.json")));
    const auto outcome = f.session->Exchange("SyntheticCode-0123456789", f.session->BeginSignIn(), kNoCancel);
    CHECK(outcome.kind == ExchangeOutcome::Kind::kSignedIn && outcome.username == kUsername);
  }
  {  // Two exchanges for one sign-in at once: the first signs in, the second is stale; the first's token is saved.
    Fixture f;
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(1), gate);
    f.stub.Script(Endpoint::kUser, UserData());
    f.stub.Script(Endpoint::kToken, Tokens(2));
    f.stub.Script(Endpoint::kUser, UserData());
    const auto epoch = f.session->BeginSignIn();
    ExchangeOutcome first, second;
    std::thread a([&] { first = f.session->Exchange("SyntheticCode-A-0123456789", epoch, kNoCancel); });
    gate->WaitEntered(1);
    std::thread b([&] { second = f.session->Exchange("SyntheticCode-B-0123456789", epoch, kNoCancel); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    gate->Open();
    a.join();
    b.join();
    CHECK(first.kind == ExchangeOutcome::Kind::kSignedIn);
    CHECK(second.kind == ExchangeOutcome::Kind::kStale && second.username.empty());
    CHECK(SavedRefresh(f.root) == RefreshText(1));
    CHECK(f.session->Token(kNoCancel).token == AccessText(1));
  }
  {  // An exchange that finishes after Back is stale, and nothing is saved.
    Fixture f;
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(1), gate);
    f.stub.Script(Endpoint::kUser, UserData());
    const auto epoch = f.session->BeginSignIn();
    ExchangeOutcome outcome;
    std::thread a([&] { outcome = f.session->Exchange("SyntheticCode-0123456789", epoch, kNoCancel); });
    gate->WaitEntered(1);
    f.session->EndSignIn(epoch);
    gate->Open();
    a.join();
    CHECK(outcome.kind == ExchangeOutcome::Kind::kStale);
    CHECK(!Exists(SessionFile(f.root)));
    CHECK(!f.session->signed_in() && f.session->identity().account_key == 0);
    CHECK(f.Logged("exchange result=dropped"));
  }
  {  // A newer sign-in screen makes an older screen's exchange stale too.
    Fixture f;
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(1), gate);
    f.stub.Script(Endpoint::kUser, UserData());
    const auto epoch = f.session->BeginSignIn();
    ExchangeOutcome outcome;
    std::thread a([&] { outcome = f.session->Exchange("SyntheticCode-0123456789", epoch, kNoCancel); });
    gate->WaitEntered(1);
    const auto newer = f.session->BeginSignIn();
    CHECK(newer != epoch);
    gate->Open();
    a.join();
    CHECK(outcome.kind == ExchangeOutcome::Kind::kStale);
    CHECK(!Exists(SessionFile(f.root)));
  }
  {  // A rejected code keeps the earlier session as it was.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    const auto generation = f.session->generation();
    f.stub.Script(Endpoint::kToken, Answer(400, ReadFile(g_fixtures + "/token-code-invalid-grant.json")));
    const auto epoch = f.session->BeginSignIn();
    const auto outcome = f.session->Exchange("SyntheticCode-bad-0123456789", epoch, kNoCancel);
    CHECK(outcome.kind == ExchangeOutcome::Kind::kFailed && outcome.rejected);
    CHECK(f.session->generation() == generation);
    CHECK(f.session->identity().username == kUsername);
    CHECK(SavedRefresh(f.root) == RefreshText(1));
    const auto token = f.session->Token(kNoCancel);
    CHECK(token.status == AccessToken::Status::kOk && token.token == AccessText(1));
    CHECK(f.Logged("exchange status=400 result=rejected"));
    // The sign-in stays open: the next code works.
    f.stub.Script(Endpoint::kToken, Tokens(2));
    f.stub.Script(Endpoint::kUser, UserData());
    CHECK(f.session->Exchange("SyntheticCode-good-0123456789", epoch, kNoCancel).kind ==
          ExchangeOutcome::Kind::kSignedIn);
    CHECK(SavedRefresh(f.root) == RefreshText(2));
    f.CheckLogClean();
  }
  {  // No network: failed, not rejected.
    Fixture f;
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kNameResolutionFailed));
    const auto outcome = f.session->Exchange("SyntheticCode-0123456789", f.session->BeginSignIn(), kNoCancel);
    CHECK(outcome.kind == ExchangeOutcome::Kind::kFailed && !outcome.rejected);
    CHECK(!f.session->signed_in());
  }
  {  // userData without a username: failed, nothing saved.
    Fixture f;
    f.stub.Script(Endpoint::kToken, Tokens(1));
    f.stub.Script(Endpoint::kUser, Answer(200, ReadFile(g_fixtures + "/user-data-no-username.json")));
    const auto outcome = f.session->Exchange("SyntheticCode-0123456789", f.session->BeginSignIn(), kNoCancel);
    CHECK(outcome.kind == ExchangeOutcome::Kind::kFailed && !outcome.rejected);
    CHECK(!f.session->signed_in() && !Exists(SessionFile(f.root)));
    CHECK(f.Logged("user status=200 result=protocol"));
  }
  {  // userData refused (401) or unreachable: failed.
    Fixture f;
    f.stub.Script(Endpoint::kToken, Tokens(1));
    f.stub.Script(Endpoint::kUser, Answer(401, "{}"));
    auto epoch = f.session->BeginSignIn();
    CHECK(f.session->Exchange("SyntheticCode-0123456789", epoch, kNoCancel).kind == ExchangeOutcome::Kind::kFailed);
    f.stub.Script(Endpoint::kToken, Tokens(2));
    f.stub.Script(Endpoint::kUser, Failure(HttpError::kTimeout));
    CHECK(f.session->Exchange("SyntheticCode-0123456789", epoch, kNoCancel).kind == ExchangeOutcome::Kind::kFailed);
    CHECK(!f.session->signed_in());
  }
  {  // No sign-in open: stale, and the code is never sent.
    Fixture f;
    CHECK(f.session->Exchange("SyntheticCode-0123456789", 0, kNoCancel).kind == ExchangeOutcome::Kind::kStale);
    CHECK(f.session->Exchange("SyntheticCode-0123456789", 7, kNoCancel).kind == ExchangeOutcome::Kind::kStale);
    CHECK(f.stub.Calls(Endpoint::kToken) == 0);
  }
  {  // A failed save still signs in, in memory, and says storage is full.
    Fixture f([](const Storage&, const std::string&, const Bytes&) -> Result<void> {
      errno = ENOSPC;
      return Result<void>::Failure(StorageError::kWriteFailed, "synthetic write failure");
    });
    const auto outcome = f.SignIn(1);
    CHECK(outcome.kind == ExchangeOutcome::Kind::kSignedIn && outcome.storage_full);
    CHECK(f.session->signed_in());
    CHECK(f.session->Token(kNoCancel).token == AccessText(1));
    CHECK(!Exists(SessionFile(f.root)));
    CHECK(f.Logged("session save failed"));
    f.CheckLogClean();
  }
  {  // A sign-out after a sign-in: everything goes, and a new sign-in works.
    Fixture f;
    CHECK(f.SignIn(1).kind == ExchangeOutcome::Kind::kSignedIn);
    const auto generation = f.session->generation();
    CHECK(f.session->SignOut().ok);
    CHECK(f.session->generation() > generation);
    CHECK(!Exists(SessionFile(f.root)) && !f.session->signed_in());
    CHECK(f.SignIn(2).kind == ExchangeOutcome::Kind::kSignedIn);
    CHECK(SavedRefresh(f.root) == RefreshText(2));
  }
}

void TestAccountKey() {
  const auto key = gog::AccountKey(kUserId);
  CHECK(key != 0);
  CHECK(key == gog::AccountKey(kUserId));
  CHECK(key != gog::AccountKey("1000000000000000002"));
  CHECK(gog::AccountKey("1") != gog::AccountKey("2"));
  for (const std::string id : {kUserId, std::string("1"), std::string("18446744073709551615")}) {
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(gog::AccountKey(id)));
    CHECK(id != hex);
    char dec[21];
    std::snprintf(dec, sizeof dec, "%llu", static_cast<unsigned long long>(gog::AccountKey(id)));
    CHECK(id != dec);
  }
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  g_fixtures = argv[1];
  g_base = argv[2];
  std::error_code error;
  fs::remove_all(g_base, error);
  fs::create_directories(g_base);

  TestRestore();
  TestToken();
  TestExchange();
  TestAccountKey();

  fs::remove_all(g_base, error);
  std::puts("gog_session: ok");
  return 0;
}
