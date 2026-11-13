// SPDX-License-Identifier: GPL-3.0-or-later
// The GOG account worker: restore events, the relay's sign-in answers (signed in, failed, stale, busy, closing), Back
// while a code is exchanged, sign-out, a token revoked while in use, Reconnect after an offline restore, and "Stay signed
// in" on the worker. A stub transport scripts GOG's answers.
// Synthetic tokens and accounts only.
#include "gog/account_key.h"
#include "gog/session.h"
#include "gog/session_store.h"
#include "gog_stub_transport.h"
#include "stores/gog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace playden;
using namespace gog_test;
using playden::stores::AccountEvent;
using playden::stores::AccountEventKind;
using playden::stores::GogAccount;
using playden::stores::SignInAnswer;
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

bool Exists(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0;
}

fs::path SessionFile(const fs::path& root) { return root / "gog" / "session"; }

gog::SessionStore::Options Options(const fs::path& root) {
  gog::SessionStore::Options options;
  options.user_root = root.string();
  return options;
}

void SaveSession(const fs::path& root, const std::string& refresh) {
  gog::SessionStore store(Options(root));
  gog::StoredSession session;
  session.refresh_token = refresh;
  session.user_id = kUserId;
  session.username = kUsername;
  CHECK(store.Save(session).ok);
}

HttpResponse UserData() { return Answer(200, ReadFile(g_fixtures + "/user-data.json")); }

std::string Code(const std::string& tag) { return "SyntheticCode-" + tag + "-0123456789"; }

// Every log line of every fixture, checked at the end: none may name the account, a token or a code.
std::mutex g_lines_mutex;
std::vector<std::string> g_lines;

void LogLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_lines_mutex);
  g_lines.push_back(line);
}

struct Fixture {
  fs::path root = FreshRoot();
  gog::SessionStore store{Options(root)};
  Stub stub;
  std::atomic<std::int64_t> now{100000};
  gog::Session session{store, stub.Transport(), [this] { return now.load(); }, LogLine};
  GogAccount account{session, store, LogLine};

  // Waits for count events (10 s at most), then checks no more follow for a moment.
  std::vector<AccountEvent> Events(std::size_t count) {
    std::vector<AccountEvent> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (events.size() < count && std::chrono::steady_clock::now() < deadline) {
      for (auto& event : account.PumpEvents(16)) events.push_back(std::move(event));
      if (events.size() < count) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (auto& event : account.PumpEvents(16)) events.push_back(std::move(event));
    CHECK(events.size() == count);
    for (const auto& event : events) CHECK(event.store == Store::kGog);
    return events;
  }

  SignInAnswer SignIn(int n) {
    stub.Script(Endpoint::kToken, Tokens(n));
    stub.Script(Endpoint::kUser, UserData());
    const auto request = account.BeginSignIn();
    CHECK(request != 0);
    return account.SignInWithCode(Code(std::to_string(n)), request);
  }
};

void CheckSignedInEvent(const AccountEvent& event, AccountEventKind kind, std::uint64_t request,
                        const gog::Session& session) {
  CHECK(event.kind == kind);
  CHECK(event.request_id == request);
  CHECK(event.account_key == gog::AccountKey(kUserId));
  CHECK(event.display_name == kUsername);
  CHECK(event.user_id == kUserId);
  CHECK(event.generation == session.generation());
  CHECK(event.error.empty());
}

void TestRestore() {
  {  // Nothing saved: signed out, answering the request.
    Fixture f;
    const auto request = f.account.RestoreSession();
    CHECK(request != 0);
    const auto events = f.Events(1);
    CHECK(events[0].kind == AccountEventKind::kSignedOut && events[0].request_id == request);
    CHECK(events[0].account_key == 0 && events[0].display_name.empty() && events[0].user_id.empty());
  }
  {  // Saved and refreshed: signed in, with the account.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    f.stub.Script(Endpoint::kToken, Tokens(2));
    const auto request = f.account.RestoreSession();
    const auto events = f.Events(1);
    CheckSignedInEvent(events[0], AccountEventKind::kSignedIn, request, f.session);
  }
  {  // No answer from GOG: offline, with the account.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
    const auto request = f.account.RestoreSession();
    const auto events = f.Events(1);
    CheckSignedInEvent(events[0], AccountEventKind::kOffline, request, f.session);
    // Reconnect: still no answer keeps it offline; an answer signs in.
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
    auto again = f.account.Reconnect();
    CHECK(again != 0 && again != request);
    CheckSignedInEvent(f.Events(1)[0], AccountEventKind::kOffline, again, f.session);
    f.stub.Script(Endpoint::kToken, Tokens(2));
    again = f.account.Reconnect();
    CheckSignedInEvent(f.Events(1)[0], AccountEventKind::kSignedIn, again, f.session);
    CHECK(f.stub.Calls(Endpoint::kToken) == 3);
  }
  {  // Offline, then GOG refuses the refresh token on Reconnect: the revocation's own event, then the answer.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
    f.account.RestoreSession();
    f.Events(1);
    f.stub.Script(Endpoint::kToken, InvalidGrant());
    const auto request = f.account.Reconnect();
    const auto events = f.Events(2);
    CHECK(events[0].kind == AccountEventKind::kExpired && events[0].request_id == 0);
    CHECK(events[1].kind == AccountEventKind::kExpired && events[1].request_id == request);
    CHECK(events[1].error == "GOG sign-in expired. Sign in again." && events[1].user_id.empty());
    CHECK(!f.session.signed_in() && !Exists(SessionFile(f.root)));
  }
  {  // A reconnect canceled (Back, a sign-out or a new restore) answers nothing.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    f.stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
    f.account.RestoreSession();
    f.Events(1);
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(2), gate);
    const auto request = f.account.Reconnect();
    gate->WaitEntered(1);
    f.account.Cancel(request);
    gate->Open();
    f.Events(0);
  }
  {  // The refresh token is refused: expired.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    f.stub.Script(Endpoint::kToken, InvalidGrant());
    const auto request = f.account.RestoreSession();
    const auto events = f.Events(1);
    CHECK(events[0].kind == AccountEventKind::kExpired && events[0].request_id == request);
    CHECK(events[0].error == "GOG sign-in expired. Sign in again.");
    // The account's key, for its saved library; never its user ID or name.
    CHECK(events[0].account_key == gog::AccountKey(kUserId) && events[0].user_id.empty());
    CHECK(events[0].display_name.empty());
    CHECK(!Exists(SessionFile(f.root)));
  }
  {  // An unreadable file: a restoring failure.
    Fixture f;
    fs::create_directories(f.root / "gog");
    CHECK(::chmod((f.root / "gog").c_str(), 0700) == 0);
    FILE* file = std::fopen(SessionFile(f.root).c_str(), "wb");
    CHECK(file != nullptr);
    std::fputs("not a session file", file);
    std::fclose(file);
    CHECK(::chmod(SessionFile(f.root).c_str(), 0600) == 0);
    const auto request = f.account.RestoreSession();
    const auto events = f.Events(1);
    CHECK(events[0].kind == AccountEventKind::kFailure && events[0].request_id == request);
    CHECK(events[0].restoring && !events[0].storage_full);
    CHECK(events[0].error == "Playden couldn't read the saved GOG sign-in.");
  }
  {  // A canceled restore answers nothing and installs nothing.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(2), gate);
    const auto request = f.account.RestoreSession();
    gate->WaitEntered(1);
    f.account.Cancel(request);
    f.Events(0);
    CHECK(!f.session.signed_in());
  }
  {  // A sign-out while the restore runs: only the sign-out answers.
    Fixture f;
    SaveSession(f.root, RefreshText(1));
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(2), gate);
    f.account.RestoreSession();
    gate->WaitEntered(1);
    f.account.SignOut();
    gate->Open();
    const auto events = f.Events(1);
    CHECK(events[0].kind == AccountEventKind::kSignedOut && events[0].request_id == 0);
    CHECK(!f.session.signed_in() && !Exists(SessionFile(f.root)));
  }
}

void TestSignIn() {
  {  // A good code: signed in, and one event answers the request.
    Fixture f;
    f.stub.Script(Endpoint::kToken, Tokens(1));
    f.stub.Script(Endpoint::kUser, UserData());
    const auto request = f.account.BeginSignIn();
    CHECK(request != 0);
    const auto answer = f.account.SignInWithCode(Code("1"), request);
    CHECK(answer.kind == SignInAnswer::Kind::kSignedIn && answer.display_name == kUsername);
    const auto events = f.Events(1);
    CheckSignedInEvent(events[0], AccountEventKind::kSignedIn, request, f.session);
    // The request is spent.
    CHECK(f.account.SignInWithCode(Code("again"), request).kind == SignInAnswer::Kind::kStale);
    CHECK(f.stub.Calls(Endpoint::kToken) == 1);
  }
  {  // A refused code: failed, no event, and the same request takes the next code.
    Fixture f;
    f.stub.Script(Endpoint::kToken, Answer(400, ReadFile(g_fixtures + "/token-code-invalid-grant.json")));
    const auto request = f.account.BeginSignIn();
    const auto failed = f.account.SignInWithCode(Code("bad"), request);
    CHECK(failed.kind == SignInAnswer::Kind::kFailed && failed.rejected && failed.display_name.empty());
    f.Events(0);
    f.stub.Script(Endpoint::kToken, Tokens(2));
    f.stub.Script(Endpoint::kUser, UserData());
    const auto answer = f.account.SignInWithCode(Code("good"), request);
    CHECK(answer.kind == SignInAnswer::Kind::kSignedIn);
    const auto events = f.Events(1);
    CheckSignedInEvent(events[0], AccountEventKind::kSignedIn, request, f.session);
  }
  {  // Back while the code is exchanged: stale, and no event.
    Fixture f;
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(1), gate);
    f.stub.Script(Endpoint::kUser, UserData());
    const auto request = f.account.BeginSignIn();
    SignInAnswer answer;
    std::thread relay([&] { answer = f.account.SignInWithCode(Code("1"), request); });
    gate->WaitEntered(1);
    f.account.Cancel(request);
    gate->Open();
    relay.join();
    CHECK(answer.kind == SignInAnswer::Kind::kStale);
    f.Events(0);
    CHECK(!f.session.signed_in() && !Exists(SessionFile(f.root)));
  }
  {  // A second code for the same request while the first is exchanged: busy at once.
    Fixture f;
    auto gate = std::make_shared<Gate>();
    f.stub.Script(Endpoint::kToken, Tokens(1), gate);
    f.stub.Script(Endpoint::kUser, UserData());
    const auto request = f.account.BeginSignIn();
    SignInAnswer first;
    std::thread relay([&] { first = f.account.SignInWithCode(Code("1"), request); });
    gate->WaitEntered(1);
    const auto started = std::chrono::steady_clock::now();
    const auto second = f.account.SignInWithCode(Code("2"), request);
    CHECK(second.kind == SignInAnswer::Kind::kBusy);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    gate->Open();
    relay.join();
    CHECK(first.kind == SignInAnswer::Kind::kSignedIn);
    CHECK(f.Events(1)[0].request_id == request);
    CHECK(f.stub.Calls(Endpoint::kToken) == 1);
  }
  {  // A request that isn't the open sign-in: stale, and nothing is sent.
    Fixture f;
    CHECK(f.account.SignInWithCode(Code("1"), 0).kind == SignInAnswer::Kind::kStale);
    CHECK(f.account.SignInWithCode(Code("1"), 42).kind == SignInAnswer::Kind::kStale);
    const auto older = f.account.BeginSignIn();
    const auto newer = f.account.BeginSignIn();
    CHECK(newer != older);
    CHECK(f.account.SignInWithCode(Code("1"), older).kind == SignInAnswer::Kind::kStale);
    CHECK(f.stub.Calls(Endpoint::kToken) == 0);
    f.Events(0);
  }
}

void TestSignOut() {
  {  // Signed out, answering with request 0; the file is gone.
    Fixture f;
    CHECK(f.SignIn(1).kind == SignInAnswer::Kind::kSignedIn);
    f.Events(1);
    CHECK(Exists(SessionFile(f.root)));
    f.account.SignOut();
    const auto events = f.Events(1);
    CHECK(events[0].kind == AccountEventKind::kSignedOut && events[0].request_id == 0);
    CHECK(events[0].generation == f.session.generation());
    CHECK(!Exists(SessionFile(f.root)) && !f.session.signed_in());
  }
  {  // The file can't be removed: a failure with request 0.
    Fixture f;
    CHECK(f.SignIn(1).kind == SignInAnswer::Kind::kSignedIn);
    f.Events(1);
    // Something that isn't a regular file where the session file was: Clear refuses to remove it.
    CHECK(::unlink(SessionFile(f.root).c_str()) == 0);
    CHECK(::mkdir(SessionFile(f.root).c_str(), 0700) == 0);
    f.account.SignOut();
    const auto events = f.Events(1);
    CHECK(events[0].kind == AccountEventKind::kFailure && events[0].request_id == 0);
    CHECK(events[0].error == "Sign-out failed. Try again.");
    CHECK(!events[0].restoring);
    CHECK(::rmdir(SessionFile(f.root).c_str()) == 0);
  }
}

void TestRevoked() {
  Fixture f;
  CHECK(f.SignIn(1).kind == SignInAnswer::Kind::kSignedIn);
  f.Events(1);
  f.now += 3600;
  f.stub.Script(Endpoint::kToken, Answer(401, "{}"));
  CHECK(f.account.session().Token(kNoCancel).status == gog::AccessToken::Status::kRevoked);
  const auto events = f.Events(1);
  CHECK(events[0].kind == AccountEventKind::kExpired && events[0].request_id == 0);
  CHECK(events[0].error == "GOG sign-in expired. Sign in again.");
  CHECK(events[0].generation == f.session.generation());
  CHECK(!Exists(SessionFile(f.root)));
  CHECK(f.account.session().Token(kNoCancel).status == gog::AccessToken::Status::kSignedOut);
  f.Events(0);
}

void TestShutdown() {
  {  // A relay waiting for its answer is told Playden is closing.
    auto f = std::make_unique<Fixture>();
    auto gate = std::make_shared<Gate>();
    f->stub.Script(Endpoint::kToken, Tokens(1), gate);
    f->stub.Script(Endpoint::kUser, UserData());
    const auto request = f->account.BeginSignIn();
    SignInAnswer answer;
    std::thread relay([&] { answer = f->account.SignInWithCode(Code("1"), request); });
    gate->WaitEntered(1);
    f->account.Shutdown();
    relay.join();
    CHECK(answer.kind == SignInAnswer::Kind::kClosing);
    CHECK(f->account.SignInWithCode(Code("2"), request).kind == SignInAnswer::Kind::kClosing);
    CHECK(f->account.RestoreSession() == 0 && f->account.BeginSignIn() == 0);
    CHECK(!f->session.signed_in());
  }
  {  // Destroying the account with a relay waiting: the relay is answered and leaves before the account goes.
    auto f = std::make_unique<Fixture>();
    auto gate = std::make_shared<Gate>();
    f->stub.Script(Endpoint::kToken, Tokens(1), gate);
    f->stub.Script(Endpoint::kUser, UserData());
    const auto request = f->account.BeginSignIn();
    auto* account = &f->account;
    SignInAnswer answer;
    std::thread relay([&answer, account, request] { answer = account->SignInWithCode(Code("1"), request); });
    gate->WaitEntered(1);
    f.reset();
    relay.join();
    CHECK(answer.kind == SignInAnswer::Kind::kClosing);
  }
  {  // A sign-out asked for before the shutdown still clears the file.
    auto f = std::make_unique<Fixture>();
    CHECK(f->SignIn(1).kind == SignInAnswer::Kind::kSignedIn);
    f->Events(1);
    f->account.SignOut();
    f->account.Shutdown();
    CHECK(!Exists(SessionFile(f->root)));
  }
}

// "Stay signed in" runs on the account worker, never the caller's thread: the store's writes wait for the root lock,
// which a runtime batch can hold for long. The answer comes back on the worker.
gog::SaveResult Persist(GogAccount& account, bool persist) {
  std::mutex mutex;
  std::condition_variable answered;
  std::optional<gog::SaveResult> result;
  const auto caller = std::this_thread::get_id();
  account.SetPersist(persist, [&](const gog::SaveResult& saved) {
    CHECK(std::this_thread::get_id() != caller);
    std::lock_guard<std::mutex> lock(mutex);
    result = saved;
    answered.notify_all();
  });
  std::unique_lock<std::mutex> lock(mutex);
  CHECK(answered.wait_for(lock, std::chrono::seconds(10), [&] { return result.has_value(); }));
  return *result;
}

void TestPersist() {
  {
    Fixture f;
    CHECK(f.SignIn(1).kind == SignInAnswer::Kind::kSignedIn);
    f.Events(1);
    CHECK(Persist(f.account, false).ok && !Exists(SessionFile(f.root)));
    CHECK(Persist(f.account, true).ok && Exists(SessionFile(f.root)));
  }
  {  // Asked for in the last frame: it still runs at shutdown, as a sign-out does.
    auto f = std::make_unique<Fixture>();
    CHECK(f->SignIn(1).kind == SignInAnswer::Kind::kSignedIn);
    f->Events(1);
    f->account.SetPersist(false);
    f->account.Shutdown();
    CHECK(!Exists(SessionFile(f->root)));
  }
}

void CheckLogsClean() {
  std::lock_guard<std::mutex> lock(g_lines_mutex);
  CHECK(!g_lines.empty());
  for (const auto& line : g_lines) {
    CHECK(line.find(kUsername) == std::string::npos);
    CHECK(line.find(kUserId) == std::string::npos);
    CHECK(line.find("synthetic-") == std::string::npos);
    CHECK(line.find("SyntheticCode") == std::string::npos);
    CHECK(line.find("gog.com") == std::string::npos);
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
  TestSignIn();
  TestSignOut();
  TestRevoked();
  TestShutdown();
  TestPersist();
  CheckLogsClean();

  fs::remove_all(g_base, error);
  std::puts("gog_account: ok");
  return 0;
}
