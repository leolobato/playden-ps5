#include "homebrew/controller.hpp"
#include "steam/local_session_repository.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <unistd.h>
#include <mutex>
#include <thread>
#include "fake_library_source.h"
#include "steam_id.h"

using namespace playden;
using namespace playden::homebrew;
using namespace playden::steam;
using namespace std::chrono_literals;
namespace {
StoredSession Session() { return {12345,"Synthetic account","fixture-refresh-secret","fixture-access-secret",{}}; }
class Repository final : public SessionRepository {
 public:
  mutable std::mutex mutex;
  mutable std::optional<StoredSession> value;
  mutable int clears = 0;
  std::atomic<bool> fail_load{false};
  std::atomic<bool> fail_clear{false};
  Result<std::optional<StoredSession>> Load() const override {
    std::lock_guard<std::mutex> lock(mutex);
    if (fail_load) return Result<std::optional<StoredSession>>::Failure(StorageError::kReadFailed,"fixture failure");
    return Result<std::optional<StoredSession>>::Success(value);
  }
  Result<void> Save(const StoredSession& s) const override {
    std::lock_guard<std::mutex> lock(mutex); value=s; return Result<void>::Success();
  }
  Result<void> Clear() const override {
    std::lock_guard<std::mutex> lock(mutex); ++clears;
    if (fail_clear) return Result<void>::Failure(StorageError::kRemoveFailed,"fixture failure");
    value.reset(); return Result<void>::Success();
  }
};
class Backend final : public AuthBackend {
 public:
  std::atomic<int> calls{0};
  std::atomic<bool> approve{false};
  std::atomic<bool> replace{false};
  bool ignore_cancel = false;
  AuthError error = AuthError::kNone;
  AuthResult AuthenticateQr(const std::atomic<bool>& cancel,const ChallengeCallback& cb) override {
    ++calls; cb("https://s.team/q/synthetic-first");
    bool sent = false;
    const auto deadline = std::chrono::steady_clock::now()+3s;
    while (!approve && (ignore_cancel || !cancel) && std::chrono::steady_clock::now()<deadline) {
      if (replace && !sent) { sent=true; cb("https://s.team/q/synthetic-replaced"); }
      std::this_thread::sleep_for(1ms);
    }
    if (cancel && !ignore_cancel) return {AuthError::kCanceled,{}};
    return {error,error == AuthError::kNone ? std::optional<StoredSession>(Session()) : std::nullopt};
  }
  std::atomic<int> restores{0};
  std::atomic<bool> hold_restore{false};  // Restore answers once this is false again (at most 3 s)
  AuthResult Restore(const StoredSession& s,const std::atomic<bool>&) override {
    ++restores;
    for (const auto deadline=std::chrono::steady_clock::now()+3s; hold_restore && std::chrono::steady_clock::now()<deadline;)
      std::this_thread::sleep_for(1ms);
    return {error,error == AuthError::kNone ? std::optional<StoredSession>(s) : std::nullopt};
  }
  // Password sign-in by account name: "wrong" is refused, "email-user" needs an email code ("WRONG" is
  // rejected), "approve" waits for approval (or an app code); any other name signs in.
  std::atomic<int> credential_calls{0};
  std::atomic<bool> credential_canceled{false};
  std::mutex seen_mutex;
  std::string password_seen;
  AuthResult AuthenticateCredentials(Credentials& c,const std::atomic<bool>& cancel,const GuardPromptCallback& prompt,
                                     const GuardCodeSource& code) override {
    ++credential_calls;
    { std::lock_guard<std::mutex> lock(seen_mutex); password_seen=c.password; }
    ZeroizeSecret(c.password);
    if (c.account_name=="wrong") return {AuthError::kInvalidPassword,{}};
    GuardPrompt shown;
    if (c.account_name=="approve") { shown.kind=GuardPromptKind::kApproval; shown.code_allowed=true; }
    else if (c.account_name=="email-user") { shown.kind=GuardPromptKind::kEmailCode; shown.email_domain="example.invalid"; }
    else return {AuthError::kNone,Session()};
    prompt(shown);
    const auto deadline=std::chrono::steady_clock::now()+3s;
    while (!cancel && !approve && std::chrono::steady_clock::now()<deadline) {
      if (auto typed=code()) {
        if (*typed=="WRONG") {
          GuardPrompt again; again.kind=GuardPromptKind::kEmailCode; again.email_domain="example.invalid"; again.code_rejected=true;
          prompt(again); continue;
        }
        return {AuthError::kNone,Session()};
      }
      std::this_thread::sleep_for(1ms);
    }
    if (cancel) { credential_canceled=true; return {AuthError::kCanceled,{}}; }
    return {AuthError::kNone,Session()};
  }
};
class LibraryBackendFixture final : public LibraryBackend {
 public:
  std::atomic<int> calls{0};
  std::atomic<bool> block{false};
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>& cancel) override {
    ++calls;
    while (block && !cancel) std::this_thread::sleep_for(1ms);
    return {LibraryError::kNone,{{SteamId(42),"Connected synthetic game",{},{},{}}},false};
  }
};
class CacheFixture final : public LibraryRepository {
 public:
  mutable std::optional<LibrarySnapshot> value;
  bool fail_clear=false;
  Result<LibrarySnapshot> Load(std::uint64_t) const override {
    return value ? Result<LibrarySnapshot>::Success(*value)
                 : Result<LibrarySnapshot>::Failure(StorageError::kNotFound,"missing");
  }
  Result<void> Save(std::uint64_t,const LibrarySnapshot& snapshot) const override {value=snapshot;return Result<void>::Success();}
  Result<void> Clear(std::uint64_t) const override {
    if(fail_clear)return Result<void>::Failure(StorageError::kRemoveFailed,"failure");
    value.reset();return Result<void>::Success();
  }
};
// Holds an install in its license check until released.
class LicenseCheckRunner final : public SessionInstallRunner {
 public:
  explicit LicenseCheckRunner(SessionRepository& sessions) : SessionInstallRunner(sessions) {}
  std::atomic<bool> release{false};
  Result InstallApp(const StoredSession&,std::uint32_t,const std::string&,const InstallOptions&,const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t,std::uint64_t,std::uint64_t)>&,
                 const std::function<void(InstallStep)>& step) override {
    step(InstallStep::kCheckingLicenses);
    while (!release && !cancel) std::this_thread::sleep_for(1ms);
    return {false,true,{},0};
  }
  std::string Remove(const std::string&) override { return {}; }
};
void Wait(Controller& controller,const std::function<bool()>& predicate) {
  const auto deadline = std::chrono::steady_clock::now()+4s;
  do { controller.Tick(); if (predicate()) return; std::this_thread::sleep_for(1ms); } while (std::chrono::steady_clock::now()<deadline);
  assert(false && "controller event deadline exceeded");
}
void Choose(Simulation& model,const std::string& action) {
  const auto& rows=model.view().rows;
  const auto found=std::find_if(rows.begin(),rows.end(),[&](const ViewRow& row){return row.action==action;});
  assert(found!=rows.end());
  const auto index=static_cast<int>(found-rows.begin());
  for (int i=0; i<80 && model.view().focused_row!=index; ++i)
    assert(model.action(model.view().focused_row<index?"down":"up"));
  assert(model.view().focused_row==index); assert(model.action("cross"));
}
void OpenSettings(Simulation& model) {
  for (int i=0; i<4 && model.state().navigation.route!=Route::Settings; ++i) model.action("r1");
  assert(model.state().navigation.route==Route::Settings);
}
void SignOut(Simulation& model) { OpenSettings(model); Choose(model,"signout-confirm"); Choose(model,"confirm-destructive"); }
void Connect(Simulation& model) { Choose(model,"connect"); }
// Opens a sign-in field, types into it and commits it.
void Type(Simulation& model,const std::string& action,const std::string& text) {
  Choose(model,action); assert(model.state().editor.active);
  assert(model.action("clear")); model.text(text); assert(model.action("done")); assert(!model.state().editor.active);
}
void PasswordSignIn(Simulation& model,const std::string& account,const std::string& password) {
  Type(model,"signin-account",account); Type(model,"signin-password",password); Choose(model,"signin-submit");
}
void PasswordSignInWithGuardCode() {
  Simulation model({}, {},ModelMode::Live); Repository repo; Backend backend;
  Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
  // QR first, as before; the row below switches to the password form and stops the QR sign-in.
  Connect(model); Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
  assert(model.state().sign_in.step==SignInStep::Qr);
  Choose(model,"signin-password-mode"); controller.Tick();
  assert(model.state().sign_in.step==SignInStep::Password && model.state().navigation.focus_key=="signin-account");
  Wait(controller,[&]{return backend.calls==1;});
  // Sign in stays disabled until both fields are filled.
  Type(model,"signin-account","email-user");
  const auto submit=std::find_if(model.view().rows.begin(),model.view().rows.end(),[](const ViewRow& r){return r.key=="signin-submit";});
  assert(submit!=model.view().rows.end() && !submit->enabled);
  Type(model,"signin-password","hunter2-fixture");
  assert(model.snapshot().find("hunter2")==std::string::npos);
  Choose(model,"signin-submit");
  assert(model.state().sign_in.password.empty() && model.state().sign_in.step==SignInStep::Submitting);
  Wait(controller,[&]{return model.state().sign_in.step==SignInStep::EmailCode;});
  { std::lock_guard<std::mutex> lock(backend.seen_mutex); assert(backend.password_seen=="hunter2-fixture"); }
  assert(model.state().sign_in.email_domain=="example.invalid" && model.state().navigation.focus_key=="guard-code");
  assert(model.view().subtitle=="Enter the code sent to …@example.invalid.");
  // Codes are five characters, uppercase, from the on-screen or a USB keyboard alike.
  Type(model,"guard-code","wrong!x"); assert(model.state().sign_in.code=="WRONG");
  Choose(model,"guard-submit");
  Wait(controller,[&]{return model.state().sign_in.code_rejected;});
  assert(model.state().sign_in.code.empty() && model.view().subtitle.rfind("That code was not right.",0)==0);
  Type(model,"guard-code","ab12c"); Choose(model,"guard-submit");
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
  assert(model.state().sign_in_password && model.ExportPreferences().steam_sign_in_password);
  assert(repo.Load().value() && backend.credential_calls==1);
  // After a sign-out, Connect opens the password form without starting a QR sign-in.
  SignOut(model);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
  Connect(model); controller.Tick();
  assert(model.state().navigation.route==Route::SignIn && model.state().sign_in.step==SignInStep::Password);
  assert(model.state().sign_in.account_name=="email-user" && model.state().navigation.focus_key=="signin-password");
  std::this_thread::sleep_for(20ms); controller.Tick();
  assert(backend.calls==1 && model.state().account().authentication==AuthState::Waiting);
  // A QR sign-in remembers QR again.
  Choose(model,"signin-qr-mode"); backend.approve=true;
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
  assert(!model.state().sign_in_password && backend.calls==2);
  controller.Shutdown();
}
void PasswordFailuresAndCancel() {
  Simulation model({}, {},ModelMode::Live); Repository repo; Backend backend;
  Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
  Connect(model); Choose(model,"signin-password-mode"); controller.Tick();
  // A refused password returns to the form with the reason; the name stays and the password goes.
  PasswordSignIn(model,"wrong","bad-password");
  Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed;});
  assert(model.state().navigation.route==Route::SignIn && model.state().sign_in.step==SignInStep::Password);
  assert(model.state().sign_in.account_name=="wrong" && model.state().sign_in.password.empty());
  assert(model.view().subtitle.find("did not accept the account name or password")!=std::string::npos);
  assert(model.state().navigation.focus_key=="signin-password" && !model.state().sign_in_password);
  // Approval in the app, with a code allowed; Cancel stops the sign-in and wipes the form.
  PasswordSignIn(model,"approve","pw");
  Wait(controller,[&]{return model.state().sign_in.step==SignInStep::Approval;});
  assert(model.state().sign_in.code_allowed && model.view().subtitle=="Approve the sign-in in the Steam Mobile app.");
  Choose(model,"guard-use-code");
  assert(model.state().sign_in.code_entry && model.state().navigation.focus_key=="guard-code");
  // A wrong typed code keeps the approval: the screen says both still work.
  Type(model,"guard-code","wrong"); Choose(model,"guard-submit");
  Wait(controller,[&]{return model.state().sign_in.code_rejected;});
  assert(model.state().sign_in.step==SignInStep::Approval && model.state().sign_in.code_entry);
  assert(model.view().subtitle.find("also approve")!=std::string::npos);
  // Tab switching does not leave the sign-in half done.
  assert(model.action("r1") && model.state().navigation.route==Route::SignIn);
  Choose(model,"guard-cancel"); controller.Tick();
  Wait(controller,[&]{return backend.credential_canceled.load();});
  Wait(controller,[&]{return model.state().account().authentication==AuthState::Cancelled;});
  assert(model.state().navigation.route==Route::Startup && model.state().sign_in.password.empty());
  // Approval arriving from the phone.
  Connect(model); Choose(model,"signin-password-mode"); controller.Tick();
  PasswordSignIn(model,"approve","pw");
  Wait(controller,[&]{return model.state().sign_in.step==SignInStep::Approval;});
  backend.approve=true;
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
  assert(model.state().sign_in_password);
  // Signing in again opens the remembered form; a refused password keeps the form, Cancel keeps the account.
  backend.approve=false;
  OpenSettings(model); Choose(model,"account"); Choose(model,"account-reauth"); controller.Tick();
  assert(model.state().sign_in.step==SignInStep::Password && model.state().account().id=="12345");
  PasswordSignIn(model,"wrong","bad");
  Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed;});
  assert(model.state().navigation.route==Route::SignIn && model.state().account().id=="12345");
  Choose(model,"signin-qr-mode"); controller.Tick();
  Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
  assert(model.state().account().id=="12345");
  Choose(model,"qr-cancel"); controller.Tick();
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
  assert(repo.clears==0 && model.state().account().id=="12345");
  controller.Shutdown();
}
void ChallengeAndCancel() {
  Simulation model({}, {},ModelMode::Live); Repository repo; Backend backend; backend.ignore_cancel=true;
  Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
  assert(model.state().games.empty() && backend.calls==0);
  Connect(model); Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
  backend.replace=true;
  Wait(controller,[&]{return model.state().account().challenge_url.find("replaced")!=std::string::npos;});
  assert(model.snapshot().find("s.team")==std::string::npos);
  Choose(model,"qr-cancel"); controller.Tick(); backend.approve=true;
  Wait(controller,[&]{return model.state().account().authentication==AuthState::Cancelled;});
  controller.Shutdown();
  assert(!repo.Load().value() && model.state().account().challenge_url.empty());
}
void AuthenticationRestoreAndSignOut() {
  Repository repo;
  {
    Simulation model({}, {},ModelMode::Live); Backend backend; Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
    Connect(model); Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
    backend.approve=true;
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    assert(model.state().account().id=="12345" && model.state().account().challenge_url.empty());
    assert(model.state().games.empty() && model.state().jobs.empty());
    assert(!model.state().account().cache_available);
    assert(model.action("account-sync"));
    controller.Tick();
    assert(model.state().account().authentication==AuthState::SignedIn);
    assert(!model.state().library_syncing && !model.state().library_loading && !model.state().account().error.empty());
    assert(model.snapshot().find("fixture-refresh-secret")==std::string::npos);
    assert(model.snapshot().find("Synthetic account")==std::string::npos);
  }
  {
    Simulation model({}, {},ModelMode::Live); Backend backend; Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    assert(backend.calls==0);
    SignOut(model); controller.Tick();
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
    assert(!repo.Load().value() && model.state().account().id.empty());
  }
}
void FailedClearDoesNotSwitch() {
  Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_clear=true;
  Backend backend; Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
  OpenSettings(model); Choose(model,"account"); Choose(model,"account-switch"); Choose(model,"confirm-destructive");
  controller.Tick(); Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn && !model.state().account().error.empty();});
  assert(backend.calls==0 && repo.Load().value() && !model.state().account().error.empty());
  assert(model.state().account().id=="12345" && model.state().account().display_name=="Synthetic account");
  assert(model.state().navigation.route==Route::Settings);
  assert(model.view().subtitle.find("Sign-out failed")!=std::string::npos);
  assert(std::none_of(model.view().rows.begin(),model.view().rows.end(),[](const ViewRow& row){return row.action=="qr-retry";}));
  repo.fail_clear=false;
  SignOut(model); controller.Tick();
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
  assert(backend.calls==0 && !repo.Load().value());
}
void SignInAgainKeepsTheAccount() {
  // Approved: a new token is saved and nothing account-specific is cleared.
  {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); Backend backend;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    OpenSettings(model); Choose(model,"account"); Choose(model,"account-reauth");
    Wait(controller,[&]{return model.state().account().authentication==AuthState::Waiting && !model.state().account().challenge_url.empty();});
    assert(model.state().navigation.route==Route::SignIn && model.state().account().id=="12345");
    backend.approve=true;
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    assert(backend.calls==1 && repo.clears==0 && repo.Load().value() && model.state().account().id=="12345");
  }
  // Cancelled: back to the saved sign-in.
  {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); Backend backend;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    OpenSettings(model); Choose(model,"account"); Choose(model,"account-reauth");
    Wait(controller,[&]{return model.state().account().authentication==AuthState::Waiting && !model.state().account().challenge_url.empty();});
    Choose(model,"qr-cancel"); controller.Tick();
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    assert(repo.clears==0 && repo.Load().value() && model.state().account().id=="12345");
  }
}
void ExitHonorsClearWithoutStartingNewQr() {
  Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); Backend backend;
  Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
  OpenSettings(model); Choose(model,"account"); Choose(model,"account-switch"); Choose(model,"confirm-destructive");
  controller.Shutdown();
  assert(backend.calls==0 && !repo.Load().value());
}
void StorageFailureRetriesSavedSession() {
  for (bool manual : {true,false}) {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_load=true;
    Backend backend; Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed;});
    assert(model.state().account().challenge_url.empty());
    assert(model.view().subtitle.find("sign out")==std::string::npos);
    assert(backend.calls==0 && repo.clears==0);
    repo.fail_load=false;
    if (manual) Choose(model,"qr-retry");
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    assert(backend.calls==0 && repo.clears==0);
    assert(model.state().account().id=="12345" && model.state().account().challenge_url.empty());
    assert(repo.Load().value()->refresh_token==Session().refresh_token);
  }
}
void StorageFailureKeepsVisibleAccount() {
  Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session();
  Backend backend; backend.error=AuthError::kNetwork;
  Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::Offline;});
  model.UpdateLibrary({{{SteamId(42),"Cached game",{},{},{}}},false,{},true,true,{}});
  repo.fail_load=true;
  assert(model.action("connect"));
  Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed;});
  assert(model.state().account().id=="12345" && model.state().games.size()==1);
  assert(model.state().account().challenge_url.empty() && backend.calls==0 && repo.clears==0);
  backend.error=AuthError::kNone; repo.fail_load=false;
  Choose(model,"qr-retry");
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
  assert(model.state().account().id=="12345" && model.state().games.size()==1);
  assert(backend.calls==0 && repo.clears==0);
}
void CanceledStorageRetryKeepsSavedSessionIntent() {
  Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_load=true;
  Backend backend; Controller controller(model,backend,repo);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed;});
  assert(model.action("circle")); controller.Tick();
  repo.fail_load=false;
  const auto deadline=std::chrono::steady_clock::now()+2200ms;
  while (std::chrono::steady_clock::now()<deadline) {
    controller.Tick();
    assert(model.state().account().authentication!=AuthState::SignedIn && backend.calls==0);
    std::this_thread::sleep_for(10ms);
  }
  assert(model.action("connect"));
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
  assert(backend.calls==0 && repo.clears==0 && repo.Load().value());
}
void RevokedSessionCleanupRetriesAfterStorageRecovery() {
  for (bool manual : {true, false}) {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_clear=true;
    Backend backend; backend.error=AuthError::kCredentialsRevoked;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed;});
    assert(repo.Load().value() && backend.calls==0);
    repo.fail_clear=false;
    if (manual) Choose(model,"qr-retry");
    Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
    assert(model.state().account().authentication==AuthState::Waiting);
    assert(!repo.Load().value() && backend.calls==1);
    controller.Shutdown();
  }
}
void StorageFailureAndRevocation() {
  for (const bool corrupt : {false,true}) {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); repo.fail_load=corrupt;
    Backend backend; backend.error=AuthError::kCredentialsRevoked;
    Controller controller(model,backend,repo);
    if (corrupt) {
      Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed;});
      assert(model.state().account().challenge_url.empty() && model.state().account().id.empty());
    } else {
      // A revoked session shows a new QR code right away.
      Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
      assert(model.state().account().authentication==AuthState::Waiting && model.state().account().id.empty());
    }
    controller.Shutdown();
    std::lock_guard<std::mutex> lock(repo.mutex); assert(corrupt?repo.value.has_value():!repo.value.has_value());
  }
  {
    // Steam refuses the saved session (eresult 63 on the console): a new QR code, not a failure.
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session();
    Backend backend; backend.error=AuthError::kRejected;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
    assert(model.state().account().authentication==AuthState::Waiting && backend.calls==1);
    controller.Shutdown();
  }
}
void ConnectedLibraryAndOfflineRestart() {
  Repository repo;repo.value=Session();CacheFixture cache;
  {
    Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
    LibraryService library_service(library_backend,repo,&cache);
    Controller controller(model,backend,repo,&library_service);
    Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
    assert(model.state().account().authentication==AuthState::SignedIn && library_backend.calls==1);
    assert(model.state().games[0].id==SteamId(42) && model.state().account().cache_available);
  }
  {
    Simulation model({}, {},ModelMode::Live);Backend backend;backend.error=AuthError::kNetwork;
    LibraryBackendFixture library_backend;LibraryService library_service(library_backend,repo,&cache);
    Controller controller(model,backend,repo,&library_service);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::Offline && model.state().games.size()==1 && !model.state().library_loading;});
    assert(library_backend.calls==0 && model.state().library_offline);
    cache.fail_clear=true;SignOut(model);controller.Tick();
    Wait(controller,[&]{return model.state().account().authentication==AuthState::Offline && !model.state().account().error.empty();});
    assert(repo.Load().value() && repo.clears==0 && cache.value && model.state().games.size()==1);
    cache.fail_clear=false;SignOut(model);controller.Tick();
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
    assert(!repo.Load().value() && !cache.value && model.state().games.empty());
  }
}
// Ticks for a while, for checks that something does not happen.
void TickFor(Controller& controller,std::chrono::milliseconds duration) {
  const auto until=std::chrono::steady_clock::now()+duration;
  while (std::chrono::steady_clock::now()<until) { controller.Tick(); std::this_thread::sleep_for(1ms); }
}
// Steam pushed a changed license list (a game added to the library elsewhere): the library refreshes
// once the changes settle, never during another refresh, and only while signed in.
void LicenseChangesRefreshTheLibrary() {
  Repository repo;repo.value=Session();CacheFixture cache;
  {
    Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
    LibraryService library_service(library_backend,repo,&cache);
    Controller controller(model,backend,repo,&library_service);
    std::atomic<std::uint64_t> changes{0};
    controller.SetLicenseChanges([&changes] { return changes.load(); },150ms);
    Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
    assert(library_backend.calls==1);
    TickFor(controller,300ms);
    assert(library_backend.calls==1);
    // A burst of lists makes one refresh, after the wait.
    changes=1;controller.Tick();changes=2;controller.Tick();
    TickFor(controller,100ms);
    assert(library_backend.calls==1);
    Wait(controller,[&]{return library_backend.calls==2 && !model.state().library_loading;});
    TickFor(controller,300ms);
    assert(library_backend.calls==2);
    // A change while a refresh runs waits for it to finish.
    library_backend.block=true;changes=3;
    Wait(controller,[&]{return library_backend.calls==3;});
    changes=4;
    TickFor(controller,400ms);
    assert(library_backend.calls==3 && model.state().library_loading);
    library_backend.block=false;
    Wait(controller,[&]{return library_backend.calls==4 && !model.state().library_loading;});
    TickFor(controller,300ms);
    assert(library_backend.calls==4 && model.state().games.size()==1);
  }
  {
    // Offline: the saved library stays as it is.
    Simulation model({}, {},ModelMode::Live);Backend backend;backend.error=AuthError::kNetwork;
    LibraryBackendFixture library_backend;LibraryService library_service(library_backend,repo,&cache);
    Controller controller(model,backend,repo,&library_service);
    std::atomic<std::uint64_t> changes{0};
    controller.SetLicenseChanges([&changes] { return changes.load(); },50ms);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::Offline && model.state().games.size()==1 &&
                               !model.state().library_loading;});
    changes=1;
    TickFor(controller,300ms);
    assert(library_backend.calls==0 && model.state().account().authentication==AuthState::Offline);
  }
}
// Refresh library in Sort and filters, offline: as Sync now, the saved sign-in is restored first.
void OfflineRefreshFromTheLibraryReconnects() {
  Repository repo;repo.value=Session();CacheFixture cache;cache.value=LibrarySnapshot{};
  cache.value->games={{SteamId(42),"Cached synthetic game",{},{},{}}};
  Simulation model({}, {},ModelMode::Live);Backend backend;backend.error=AuthError::kNetwork;
  LibraryBackendFixture library_backend;LibraryService library_service(library_backend,repo,&cache);
  Controller controller(model,backend,repo,&library_service);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::Offline && model.state().games.size()==1 &&
                             !model.state().library_loading;});
  assert(backend.restores==1 && library_backend.calls==0);
  backend.error=AuthError::kNone;
  assert(model.action("r1") && model.state().navigation.route==Route::Library);
  if (model.state().navigation.library_rail_focused) assert(model.action("right"));
  assert(model.action("triangle"));
  Choose(model,"refresh-library");
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn && library_backend.calls==1 &&
                             !model.state().library_loading;});
  assert(backend.restores==2 && model.state().games.front().title=="Connected synthetic game");
}
void RepeatedClearPreservesFirstIntent() {
  for(bool switching:{false,true}) {
    Repository repo;repo.value=Session();CacheFixture cache;
    Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
    LibraryService library_service(library_backend,repo,&cache);
    Controller controller(model,backend,repo,&library_service);
    Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
    assert(model.action(switching?"account-switch":"signout"));
    assert(model.action(switching?"signout":"account-switch"));
    controller.Tick();
    Wait(controller,[&]{return switching ? !model.state().account().challenge_url.empty()
                                        : model.state().account().authentication==AuthState::SignedOut;});
    assert(repo.clears==1 && !cache.value && backend.calls==(switching?1:0));
  }
}
void ShutdownClearsLibraryBeforeSession() {
  Repository repo;repo.value=Session();CacheFixture cache;
  Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
  LibraryService library_service(library_backend,repo,&cache);
  Controller controller(model,backend,repo,&library_service);
  Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
  SignOut(model);controller.Shutdown();
  assert(!repo.Load().value() && !cache.value);
}
}
// A license check shows only once it has run for a few seconds, even with no other install change (issue #47).
void SlowLicenseCheckShowsAfterAFewSeconds() {
  const auto root=std::filesystem::temp_directory_path()/("playden-license-check-"+std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  {
    Simulation model({}, {},ModelMode::Live); Repository repo; repo.value=Session(); Backend backend;
    LicenseCheckRunner runner(repo);
    InstallService installs(runner,root.string());
    Controller controller(model,backend,repo,nullptr,nullptr,nullptr,&installs);
    const auto job=[&]() -> const Job* {
      for (const auto& value : model.state().jobs) if (value.game_id==SteamId(70)) return &value;
      return nullptr;
    };
    const auto start=std::chrono::steady_clock::now();
    installs.Install(SteamId(70));
    Wait(controller,[&]{return job() && job()->state==JobState::Downloading;});
    assert(!job()->checking_licenses);
    const auto checking=[&]{ return job() && job()->checking_licenses; };
    for (auto deadline=start+6s; !checking() && std::chrono::steady_clock::now()<deadline; std::this_thread::sleep_for(5ms))
      controller.Tick();
    assert(checking() && std::chrono::steady_clock::now()-start>=3s);
    runner.release=true;
    controller.Shutdown();
  }
  std::filesystem::remove_all(root);
}
// Without a Steam session, another store's games open Playden on Home; with none, it waits on Startup.
void NoSteamSessionWithOtherGamesOpensHome() {
  for (const bool other : {true, false}) {
    Repository repo;CacheFixture cache;
    Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
    LibraryService library_service(library_backend,repo,&cache);
    stores::Registry registry;
    registry.Add(std::make_unique<stores::SteamAccount>(backend,repo));
    registry.Add(std::make_unique<stores::SteamLibrarySource>(library_service));
    auto imported=std::make_unique<test::FakeLibrarySource>();
    imported->games={test::ImportedGame("0f8fad5b-d9cb-469f-a165-70867728950e","Imported title")};
    if (other) registry.Add(std::move(imported));
    Controller controller(model,registry);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut && !model.state().library_loading;});
    if (other) {
      assert(model.state().navigation.route==Route::Home && model.state().games.size()==1);
      assert(model.state().games[0].source=="Imported" && library_backend.calls==0);
    } else {
      assert(model.state().navigation.route==Route::Startup && model.state().games.empty());
    }
    controller.Shutdown();
  }
}
// Signing out of Steam takes only Steam's games away: the other store's game stays on Home.
void SteamSignOutKeepsOtherGames() {
  Repository repo;repo.value=Session();CacheFixture cache;
  Simulation model({}, {},ModelMode::Live);Backend backend;LibraryBackendFixture library_backend;
  LibraryService library_service(library_backend,repo,&cache);
  stores::Registry registry;
  registry.Add(std::make_unique<stores::SteamAccount>(backend,repo));
  registry.Add(std::make_unique<stores::SteamLibrarySource>(library_service));
  auto imported=std::make_unique<test::FakeLibrarySource>();
  imported->games={test::ImportedGame("0f8fad5b-d9cb-469f-a165-70867728950e","Imported title")};
  registry.Add(std::move(imported));
  Controller controller(model,registry);
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn && model.state().games.size()==2 &&
                             !model.state().library_loading;});
  SignOut(model);controller.Tick();
  Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
  assert(!repo.Load().value() && !cache.value && model.state().games.size()==1);
  assert(model.state().games[0].id==GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e"));
  assert(model.state().navigation.route==Route::Home);
  { const auto& v=model.view(); assert(v.focused_row>=0 && v.rows[static_cast<std::size_t>(v.focused_row)].key==model.state().navigation.focus_key); }
  controller.Shutdown();
}
// A saved Steam sign-in that can't be restored (Steam answers with an error; out of reach, it plays offline)
// opens the sign-in screen, unless another store's games are there: then Playden opens on Home, with
// Steam's failure on its account.
void FailedRestoreWithOtherGamesOpensHome() {
  for (const bool other : {true, false}) {
    Repository repo;repo.value=Session();CacheFixture cache;
    Simulation model({}, {},ModelMode::Live);Backend backend;backend.error=AuthError::kProtocol;
    LibraryBackendFixture library_backend;
    LibraryService library_service(library_backend,repo,&cache);
    stores::Registry registry;
    registry.Add(std::make_unique<stores::SteamAccount>(backend,repo));
    registry.Add(std::make_unique<stores::SteamLibrarySource>(library_service));
    auto imported=std::make_unique<test::FakeLibrarySource>();
    imported->games={test::ImportedGame("0f8fad5b-d9cb-469f-a165-70867728950e","Imported title")};
    if (other) registry.Add(std::move(imported));
    Controller controller(model,registry);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed && backend.restores>=1 &&
                               (!other || model.state().games.size()==1);});
    TickFor(controller,50ms);
    assert(!model.state().account().error.empty() && backend.calls==0);
    if (other) {
      assert(model.state().navigation.route==Route::Home);
      const auto& v=model.view();
      assert(v.focused_row>=0 && v.rows[static_cast<std::size_t>(v.focused_row)].key==model.state().navigation.focus_key);
    } else {
      assert(model.state().navigation.route==Route::SignIn && model.state().games.empty());
    }
    controller.Shutdown();
  }
}
// Steam's other restore failures (account storage busy, a revoked session) don't take the screen from
// another store's games either: Playden stays on Home, a notification and the Account row give the reason,
// and Sign in still opens a QR code. Without other games: the sign-in screen, as before (a retry, a new
// QR code at once).
void OtherRestoreFailuresWithOtherGamesOpenHome() {
  for (const bool revoked : {false, true})
  for (const bool other : {true, false}) {
    Repository repo;repo.value=Session();repo.fail_load=!revoked;CacheFixture cache;
    Simulation model({}, {},ModelMode::Live);Backend backend;
    backend.error=revoked?AuthError::kCredentialsRevoked:AuthError::kNone;backend.hold_restore=true;
    LibraryBackendFixture library_backend;
    LibraryService library_service(library_backend,repo,&cache);
    stores::Registry registry;
    registry.Add(std::make_unique<stores::SteamAccount>(backend,repo));
    registry.Add(std::make_unique<stores::SteamLibrarySource>(library_service));
    auto imported=std::make_unique<test::FakeLibrarySource>();
    imported->games={test::ImportedGame("0f8fad5b-d9cb-469f-a165-70867728950e","Imported title")};
    if (other) registry.Add(std::move(imported));
    Controller controller(model,registry);
    if (other && revoked) Wait(controller,[&]{return model.state().games.size()==1;});
    backend.hold_restore=false;
    const auto notice=[&]() -> const Notice* {
      for (const auto& value : model.state().notices) if (value.id=="steam-restore") return &value;
      return nullptr;
    };
    if (revoked && other) {
      Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut && backend.restores>=1;});
      TickFor(controller,50ms);
      assert(model.state().navigation.route==Route::Home && backend.calls==0);
      assert(model.state().account().challenge_url.empty() && !model.state().account().error.empty());
    } else if (revoked) {
      Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
      assert(model.state().navigation.route==Route::SignIn && backend.calls==1);
    } else {
      Wait(controller,[&]{return model.state().account().authentication==AuthState::Failed &&
                                 (!other || model.state().games.size()==1);});
      TickFor(controller,50ms);
      assert(model.state().account().error.find("storage")!=std::string::npos && backend.restores==0);
      assert(model.state().navigation.route==(other?Route::Home:Route::SignIn));
    }
    if (other) {
      const auto& v=model.view();
      assert(v.focused_row>=0 && v.rows[static_cast<std::size_t>(v.focused_row)].key==model.state().navigation.focus_key);
      assert(notice() && notice()->detail==model.state().account().error && !notice()->alert);
    } else {
      assert(!notice());
    }
    if (revoked && other) {
      // Sign in from Settings still shows a new QR code.
      assert(model.action("connect"));
      Wait(controller,[&]{return !model.state().account().challenge_url.empty();});
      assert(model.state().navigation.route==Route::SignIn && backend.calls==1);
    }
    controller.Shutdown();
  }
}
// Another store's account for tests: RestoreSession hands out request numbers, and its events are the ones the
// test pushes.
class FakeStoreAccount final : public stores::StoreAccount {
 public:
  explicit FakeStoreAccount(Store store) : store_(store) {}
  std::mutex mutex;
  int restores = 0;
  int sign_outs = 0;
  int sign_ins = 0;
  bool refuse_sign_in = false;  // BeginSignIn answers 0
  std::uint64_t last_request = 0;
  std::vector<std::uint64_t> canceled;
  Store store() const override { return store_; }
  std::uint64_t RestoreSession() override {
    std::lock_guard<std::mutex> lock(mutex);
    ++restores;
    return last_request = ++next_;
  }
  std::uint64_t BeginSignIn() override {
    std::lock_guard<std::mutex> lock(mutex);
    ++sign_ins;
    if (refuse_sign_in) return 0;
    return last_request = ++next_;
  }
  bool Canceled(std::uint64_t request) {
    std::lock_guard<std::mutex> lock(mutex);
    return std::find(canceled.begin(), canceled.end(), request) != canceled.end();
  }
  void Cancel(std::uint64_t request) override { std::lock_guard<std::mutex> lock(mutex); canceled.push_back(request); }
  void SignOut() override { std::lock_guard<std::mutex> lock(mutex); ++sign_outs; }
  std::vector<stores::AccountEvent> PumpEvents(std::size_t max_events) override {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<stores::AccountEvent> result;
    while (!events_.empty() && result.size() < max_events) { result.push_back(std::move(events_.front())); events_.pop_front(); }
    return result;
  }
  void Push(stores::AccountEvent event) { std::lock_guard<std::mutex> lock(mutex); events_.push_back(std::move(event)); }
 private:
  Store store_;
  std::uint64_t next_ = 100;
  std::deque<stores::AccountEvent> events_;
};
// Synthetic GOG account details: none of them may reach a log line.
constexpr const char* kGogName = "Synthetic GOG player";
constexpr const char* kGogUserId = "48151623421";
constexpr std::uint64_t kGogKey = 0xabc;
stores::AccountEvent GogEvent(stores::AccountEventKind kind, std::uint64_t request) {
  stores::AccountEvent event;
  event.store = Store::kGog;
  event.kind = kind;
  event.request_id = request;
  event.generation = 1;
  if (kind == stores::AccountEventKind::kSignedIn || kind == stores::AccountEventKind::kOffline) {
    event.account_key = kGogKey;
    event.display_name = kGogName;
    event.user_id = kGogUserId;
  }
  return event;
}
// One store's details for tests: what was asked (and with which sign-in generation), what was invalidated; answers
// are the ones the test queues.
class RecordingDetails final : public stores::DetailsSource {
 public:
  explicit RecordingDetails(Store store) : store_(store) {}
  struct Ask {
    GameId id;
    std::uint64_t generation = 0;
    bool first = false;  // RequestFirst: ahead of anything queued
    bool operator==(const Ask&) const = default;
  };
  std::mutex mutex;
  std::vector<Ask> asked;
  std::vector<std::uint64_t> invalidated;
  std::vector<stores::GameDetails> answers;
  Store store() const override { return store_; }
  void Request(const GameId& id, std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex);
    asked.push_back({id, generation, false});
  }
  void RequestFirst(const GameId& id, std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex);
    asked.push_back({id, generation, true});
  }
  void Invalidate(std::uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex);
    invalidated.push_back(generation);
  }
  std::vector<stores::GameDetails> Pump(std::size_t) override {
    std::lock_guard<std::mutex> lock(mutex);
    return std::exchange(answers, {});
  }
  void Shutdown() override {}
  void Answer(const GameId& id, std::uint64_t generation, std::uint64_t download_bytes, std::uint64_t install_bytes) {
    stores::GameDetails details;
    details.id = id;
    details.account_generation = generation;
    details.available = true;
    details.download_bytes = download_bytes;
    details.install_bytes = install_bytes;
    details.windows_installable = true;
    std::lock_guard<std::mutex> lock(mutex);
    answers.push_back(details);
  }
  std::vector<Ask> Asked() {
    std::lock_guard<std::mutex> lock(mutex);
    return asked;
  }
  std::vector<std::uint64_t> Invalidated() {
    std::lock_guard<std::mutex> lock(mutex);
    return invalidated;
  }
 private:
  Store store_;
};
// Steam's fixtures with a saved session, a GOG account and a GOG library with one game, all in one registry.
struct TwoStores {
  Repository repo;
  CacheFixture cache;
  Simulation model{{}, {}, ModelMode::Live};
  Backend backend;
  LibraryBackendFixture library_backend;
  LibraryService library_service{library_backend, repo, &cache};
  stores::Registry registry;
  FakeStoreAccount* gog = nullptr;
  test::FakeLibrarySource* gog_library = nullptr;
  RecordingDetails* steam_details = nullptr;  // with_details only
  RecordingDetails* gog_details = nullptr;
  std::mutex log_mutex;
  std::vector<std::string> log;
  std::unique_ptr<Controller> controller;
  // with_details: each store has a details source too. steam_saved: Steam has a saved sign-in to restore.
  explicit TwoStores(bool with_details = false, bool steam_saved = true) {
    if (steam_saved) repo.value = Session();
    registry.Add(std::make_unique<stores::SteamAccount>(backend, repo));
    registry.Add(std::make_unique<stores::SteamLibrarySource>(library_service));
    if (with_details) {
      auto steam = std::make_unique<RecordingDetails>(Store::kSteam);
      steam_details = steam.get();
      registry.Add(std::move(steam));
      auto gog_source = std::make_unique<RecordingDetails>(Store::kGog);
      gog_details = gog_source.get();
      registry.Add(std::move(gog_source));
    }
    auto account = std::make_unique<FakeStoreAccount>(Store::kGog);
    gog = account.get();
    registry.Add(std::move(account));
    auto library = std::make_unique<test::FakeLibrarySource>(Store::kGog);
    GameSummary game;
    game.id = GameId::Gog(1000000001);
    game.title = "Synthetic GOG game";
    library->games = {game};
    gog_library = library.get();
    registry.Add(std::move(library));
    controller = std::make_unique<Controller>(model, registry, nullptr, nullptr, nullptr, nullptr, [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(log_mutex);
      log.push_back(line);
    });
  }
  ~TwoStores() { controller->Shutdown(); }
  bool Has(const GameId& id) const {
    const auto& games = model.state().games;
    return std::any_of(games.begin(), games.end(), [&](const homebrew::Game& game) { return game.id == id; });
  }
  // Both stores signed in, with both games shown.
  void SignInBoth() {
    gog->Push(GogEvent(stores::AccountEventKind::kSignedIn, gog->last_request));
    Wait(*controller, [&] {
      return model.state().account().authentication == AuthState::SignedIn &&
             model.state().account(Store::kGog).authentication == AuthState::SignedIn && Has(SteamId(42)) &&
             Has(GameId::Gog(1000000001)) && !model.state().library_loading;
    });
  }
  // The auth log names neither the GOG account nor its key.
  void CheckLog() {
    std::lock_guard<std::mutex> lock(log_mutex);
    assert(!log.empty());
    for (const auto& line : log) {
      assert(line.find(kGogName) == std::string::npos && line.find("Synthetic GOG") == std::string::npos);
      assert(line.find(kGogUserId) == std::string::npos);
      assert(line.find("0abc") == std::string::npos && line.find("2748") == std::string::npos);
    }
  }
  const homebrew::Game* Find(const GameId& id) const {
    for (const auto& game : model.state().games)
      if (game.id == id) return &game;
    return nullptr;
  }
  bool Logged(const std::string& part) {
    std::lock_guard<std::mutex> lock(log_mutex);
    return std::any_of(log.begin(), log.end(), [&](const std::string& line) { return line.find(part) != std::string::npos; });
  }
};
// Another store's details reach the model for the account that asked for them: a GOG game's page asks GOG's details
// source, not Steam's, and an answer from an earlier sign-in is dropped. Steam's details flow as before.
void OtherStoreDetailsReachTheModel() {
  TwoStores stores(true);
  auto& model = stores.model;
  auto& controller = *stores.controller;
  const auto gog_game = GameId::Gog(1000000001);
  stores.SignInBoth();
  controller.Tick();
  const auto generation = controller.StoreGeneration(Store::kGog);
  assert(generation == 2 && stores.gog_details->Invalidated().back() == generation);
  assert(stores.gog_details->Asked().empty());
  const auto steam_asked = stores.steam_details->Asked().size();
  // The GOG game's page asks GOG first, for this sign-in; Steam's source hears nothing of it.
  assert(model.action("details:" + ToKey(gog_game)));
  TickFor(controller, 10ms);
  assert((stores.gog_details->Asked() == std::vector<RecordingDetails::Ask>{{gog_game, generation, true}}));
  assert(stores.steam_details->Asked().size() == steam_asked);
  // An answer for an earlier sign-in is dropped; this one's shows on the game.
  stores.gog_details->Answer(gog_game, generation - 1, 7, 9);
  TickFor(controller, 10ms);
  assert(!stores.Find(gog_game)->download_bytes && !stores.Find(gog_game)->windows_installable);
  stores.gog_details->Answer(gog_game, generation, 300'000'000, 600'000'000);
  TickFor(controller, 10ms);
  const auto* game = stores.Find(gog_game);
  assert(game->download_bytes == 300'000'000u && game->install_bytes == 600'000'000u && game->windows_installable == true);
  // Steam's game page asks Steam's source, and its answer shows as before.
  assert(model.action("details:42"));
  TickFor(controller, 10ms);
  const auto steam = stores.steam_details->Asked();
  assert(steam.size() == steam_asked + 1 && steam.back().id == SteamId(42) && steam.back().first &&
         steam.back().generation == model.state().account_generation);
  assert(stores.gog_details->Asked().size() == 1);
  stores.steam_details->Answer(SteamId(42), model.state().account_generation, 1000, 2000);
  TickFor(controller, 10ms);
  assert(stores.Find(SteamId(42))->download_bytes == 1000u && stores.Find(SteamId(42))->install_bytes == 2000u);
  // GOG's sign-in expires on its page: the source drops the old sign-in's work, and nothing is asked while signed out.
  assert(model.action("details:" + ToKey(gog_game)));
  TickFor(controller, 10ms);
  const auto asked = stores.gog_details->Asked().size();
  auto expired = GogEvent(stores::AccountEventKind::kExpired, 0);
  expired.error = "Your GOG sign-in expired. Sign in again.";
  stores.gog->Push(expired);
  Wait(controller, [&] { return model.state().account(Store::kGog).authentication == AuthState::Expired; });
  controller.Tick();
  assert(controller.StoreGeneration(Store::kGog) == generation + 1);
  assert(stores.gog_details->Invalidated().back() == generation + 1);
  stores.gog_details->Answer(gog_game, generation, 1, 1);
  TickFor(controller, 10ms);
  assert(stores.Find(gog_game) && stores.Find(gog_game)->download_bytes == 300'000'000u);
  assert(model.action("details:42") && model.action("details:" + ToKey(gog_game)));
  TickFor(controller, 10ms);
  assert(model.state().navigation.detail_id == gog_game && stores.gog_details->Asked().size() == asked);
  stores.CheckLog();
}
// A Steam sign-out drops what Steam said about how its games start; what GOG said about its own stays, so a GOG
// game prepared next still has its launch options.
void SteamSignOutKeepsOtherStoresLaunches() {
  TwoStores stores(true);
  auto& model = stores.model;
  auto& controller = *stores.controller;
  const auto gog_game = GameId::Gog(1000000001);
  stores.SignInBoth();
  controller.Tick();
  assert(model.action("details:" + ToKey(gog_game)));
  TickFor(controller, 10ms);
  stores.gog_details->Answer(gog_game, controller.StoreGeneration(Store::kGog), 300'000'000, 600'000'000);
  assert(model.action("details:42"));
  TickFor(controller, 10ms);
  stores.steam_details->Answer(SteamId(42), model.state().account_generation, 1000, 2000);
  TickFor(controller, 10ms);
  assert(controller.KnowsLaunchOptions(gog_game) && controller.KnowsLaunchOptions(SteamId(42)));
  const auto generation = model.state().account_generation;
  SignOut(model);
  controller.Tick();
  Wait(controller, [&] { return model.state().account().authentication == AuthState::SignedOut; });
  TickFor(controller, 10ms);
  assert(model.state().account_generation != generation);
  assert(!controller.KnowsLaunchOptions(SteamId(42)) && controller.KnowsLaunchOptions(gog_game));
  assert(model.state().account(Store::kGog).authentication == AuthState::SignedIn && stores.Has(gog_game));
  stores.CheckLog();
}
// GOG's saved sign-in restores next to Steam's, and both stores' games show.
void OtherStoreAccountRestoresBesideSteam() {
  TwoStores stores;
  auto& model = stores.model;
  assert(stores.gog->restores == 1 && model.state().account(Store::kGog).authentication == AuthState::Restoring);
  assert(stores.controller->StoreGeneration(Store::kGog) == 1 && stores.controller->StoreGeneration(Store::kSteam) == 0);
  // An answer to another request is stale: nothing changes.
  stores.gog->Push(GogEvent(stores::AccountEventKind::kSignedIn, stores.gog->last_request + 7));
  TickFor(*stores.controller, 30ms);
  assert(model.state().account(Store::kGog).authentication == AuthState::Restoring);
  assert(stores.gog_library->refreshes == 0 && stores.controller->StoreGeneration(Store::kGog) == 1);
  assert(stores.Logged("\"store\":\"gog\",\"kind\":\"signed-in\"") && stores.Logged("ignored=other-request"));
  stores.SignInBoth();
  const auto& gog = model.state().account(Store::kGog);
  assert(gog.id == "0000000000000abc" && gog.display_name == kGogName && gog.error.empty());
  assert(stores.gog_library->refreshes == 1 && stores.gog_library->last_account == kGogKey);
  assert(stores.controller->StoreGeneration(Store::kGog) == 2);
  assert(stores.backend.restores == 1 && stores.backend.calls == 0 && model.state().account().id == "12345");
  assert(model.state().navigation.route == Route::Home && model.state().games.size() == 2);
  // The answer came: nothing is left to cancel at shutdown.
  stores.controller->Shutdown();
  assert(stores.gog->canceled.empty());
  stores.CheckLog();
}
// A GOG sign-out signs GOG out and resets its library; Steam stays signed in with its games and saved session.
void OtherStoreSignOutLeavesSteam() {
  TwoStores stores;
  auto& model = stores.model;
  stores.SignInBoth();
  // A GOG library refresh refreshes GOG's library only.
  const int steam_fetches = stores.library_backend.calls;
  model.RequestServiceAction({ServiceAction::RefreshLibrary, Store::kGog});
  stores.controller->Tick();
  assert(stores.gog_library->refreshes == 2 && stores.library_backend.calls == steam_fetches);
  Wait(*stores.controller, [&] { return !model.state().library_loading; });
  const int invalidated = stores.gog_library->invalidated;
  // A sign-out that fails leaves GOG signed in, with the reason; nothing is reset.
  model.RequestServiceAction({ServiceAction::SignOut, Store::kGog});
  stores.controller->Tick();
  assert(stores.gog->sign_outs == 1 && model.state().account(Store::kGog).authentication == AuthState::SigningOut);
  auto failed = GogEvent(stores::AccountEventKind::kFailure, 0);
  failed.error = "Sign-out failed. Retry sign-out in Settings.";
  stores.gog->Push(failed);
  Wait(*stores.controller, [&] { return model.state().account(Store::kGog).authentication == AuthState::SignedIn; });
  assert(model.state().account(Store::kGog).error == failed.error && model.state().account(Store::kGog).display_name == kGogName);
  assert(stores.gog_library->invalidated == invalidated && stores.controller->StoreGeneration(Store::kGog) == 2);
  model.RequestServiceAction({ServiceAction::SignOut, Store::kGog});
  stores.controller->Tick();
  assert(stores.gog->sign_outs == 2 && model.state().account(Store::kGog).authentication == AuthState::SigningOut);
  // An answer to a request from before the sign-out (a canceled restore) is not the sign-out's.
  stores.gog->Push(GogEvent(stores::AccountEventKind::kFailure, stores.gog->last_request));
  stores.controller->Tick();
  assert(model.state().account(Store::kGog).authentication == AuthState::SigningOut);
  // A sign-in answer that arrives now is from before the sign-out: it doesn't count.
  stores.gog->Push(GogEvent(stores::AccountEventKind::kSignedIn, 0));
  stores.controller->Tick();
  assert(model.state().account(Store::kGog).authentication == AuthState::SigningOut);
  stores.gog->Push(GogEvent(stores::AccountEventKind::kSignedOut, 0));
  Wait(*stores.controller, [&] { return model.state().account(Store::kGog).authentication == AuthState::SignedOut; });
  assert(model.state().account(Store::kGog).id.empty() && model.state().account(Store::kGog).display_name.empty());
  assert(stores.gog_library->invalidated == invalidated + 1 && stores.controller->StoreGeneration(Store::kGog) == 3);
  // Its library is reset: a refresh now has no account to load.
  model.RequestServiceAction({ServiceAction::RefreshLibrary, Store::kGog});
  TickFor(*stores.controller, 20ms);
  assert(stores.gog_library->refreshes == 2);
  assert(model.state().account().authentication == AuthState::SignedIn && stores.Has(SteamId(42)));
  { std::lock_guard<std::mutex> lock(stores.repo.mutex); assert(stores.repo.value && stores.repo.clears == 0); }
  assert(stores.backend.calls == 0 && model.state().navigation.route == Route::Home);
  stores.CheckLog();
}
// A GOG session that expires shows on GOG's account; the screen stays where it is.
void OtherStoreExpiryKeepsTheScreen() {
  {
    TwoStores stores;
    auto& model = stores.model;
    stores.SignInBoth();
    const auto route = model.state().navigation.route;
    const auto focus = model.state().navigation.focus_key;
    const int invalidated = stores.gog_library->invalidated;
    auto expired = GogEvent(stores::AccountEventKind::kExpired, 0);
    expired.error = "Your GOG sign-in expired. Sign in again.";
    stores.gog->Push(expired);
    Wait(*stores.controller, [&] { return model.state().account(Store::kGog).authentication == AuthState::Expired; });
    assert(model.state().account(Store::kGog).error == expired.error && model.state().account(Store::kGog).id.empty());
    assert(model.state().navigation.route == route && model.state().navigation.focus_key == focus);
    assert(stores.gog_library->invalidated == invalidated + 1 && stores.controller->StoreGeneration(Store::kGog) == 3);
    assert(model.state().account().authentication == AuthState::SignedIn && stores.Has(SteamId(42)));
    stores.CheckLog();
  }
  {
    // It expires while GOG's library is still loading: the load is dropped and the library stops loading, with
    // GOG's games still listed and the expiry on its account.
    TwoStores stores;
    auto& model = stores.model;
    stores.SignInBoth();
    stores.gog_library->hold = true;
    model.RequestServiceAction({ServiceAction::RefreshLibrary, Store::kGog});
    stores.controller->Tick();
    assert(stores.gog_library->held.size() == 1 && model.state().library_loading);
    auto expired = GogEvent(stores::AccountEventKind::kExpired, 0);
    expired.error = "Your GOG sign-in expired. Sign in again.";
    stores.gog->Push(expired);
    Wait(*stores.controller, [&] { return model.state().account(Store::kGog).authentication == AuthState::Expired; });
    assert(!model.state().library_loading && !model.state().library_syncing);
    assert(model.state().account(Store::kGog).error == expired.error);
    assert(stores.Has(GameId::Gog(1000000001)) && stores.Has(SteamId(42)));
    // Its late answer changes nothing.
    stores.gog_library->Release();
    TickFor(*stores.controller, 10ms);
    assert(!model.state().library_loading && model.state().account(Store::kGog).error == expired.error);
    stores.CheckLog();
  }
  {
    // A restore that never answers is canceled at shutdown.
    TwoStores stores;
    const auto request = stores.gog->last_request;
    TickFor(*stores.controller, 10ms);
    stores.controller->Shutdown();
    assert(stores.gog->canceled == std::vector<std::uint64_t>{request});
  }
}
// GOG's phone relay for tests: tokens are 1, 2, 3… in 32 hex digits, its clock is set by the test, and every code
// exchange fails (the sign-in stays open for another).
struct RelayStub {
  std::atomic<long long> seconds{1000};
  std::atomic<int> minted{0};
  std::shared_ptr<web::GogRelay> relay;
  RelayStub() {
    web::GogRelayConfig config;
    config.parse_code = [](std::string_view paste) -> std::optional<std::string> {
      if (paste.empty()) return std::nullopt;
      return std::string(paste);
    };
    config.exchange = [](std::string, std::uint64_t) {
      web::GogExchangeAnswer answer;
      answer.kind = web::GogExchangeAnswer::Kind::kFailed;
      return answer;
    };
    config.random_token = [this]() -> std::optional<std::string> { return Token(++minted); };
    config.now = [this] { return std::chrono::steady_clock::time_point(std::chrono::seconds(seconds.load())); };
    relay = std::make_shared<web::GogRelay>(std::move(config));
  }
  static std::string Token(int number) {
    char text[40];
    std::snprintf(text, sizeof text, "%032x", number);
    return text;
  }
};
// Web access on a free loopback port, for the address the QR code carries.
struct WebAccess {
  std::filesystem::path root;
  std::unique_ptr<web::Service> service;
  WebAccess() {
    root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           ("playden-account-web-" + std::to_string(getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "logs");
    web::ServiceConfig config;
    config.paths = {(root / "logs").string(), (root / "homebrew").string(), (root / "kits").string(),
                    (root / "homebrew/FAKE77031").string()};
    config.server.first_port = config.server.last_port = 0;
    config.server.loopback_only = true;
    config.address_fallback = [] { return std::string("192.0.2.10"); };
    service = std::make_unique<web::Service>(std::move(config));
  }
  ~WebAccess() {
    service->Shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};
bool Notified(const Simulation& model, const std::string& message) {
  const auto& notices = model.state().notices;
  return std::any_of(notices.begin(), notices.end(), [&](const Notice& notice) { return notice.message == message; });
}
// Steam signed in, GOG signed out (its restore found nothing), and the relay set.
void GogSignedOut(TwoStores& stores, RelayStub* relay) {
  if (relay) stores.controller->SetGogRelay(relay->relay);
  stores.gog->Push(GogEvent(stores::AccountEventKind::kSignedOut, stores.gog->last_request));
  Wait(*stores.controller, [&] {
    return stores.model.state().account(Store::kGog).authentication == AuthState::SignedOut &&
           stores.model.state().account().authentication == AuthState::SignedIn && !stores.model.state().library_loading;
  });
}
// The TV's GOG sign-in: BeginQr opens the relay for the account's sign-in, and the model's QR address follows the
// relay's token and web access's address. Back closes it; a late answer to it is ignored; a sign-in closes the relay.
void GogSignInFromTheTv() {
  // Declared before the stores: the controller uses them until it shuts down.
  RelayStub relay;
  WebAccess web;
  TwoStores stores;
  auto& model = stores.model;
  auto& controller = *stores.controller;
  controller.SetWebAccess(web.service.get());
  GogSignedOut(stores, &relay);
  Wait(controller, [&] { return web.service->Snapshot().state == web::ServiceSnapshot::State::kOn; });
  const auto url = web.service->Snapshot().url;
  assert(!url.empty() && url.back() == '/');
  const auto restore = stores.gog->last_request;
  model.RequestServiceAction({ServiceAction::BeginQr, Store::kGog});
  controller.Tick();
  const auto request = stores.gog->last_request;
  assert(stores.gog->sign_ins == 1 && request != restore && relay.relay->request() == request);
  const auto& gog = model.state().account(Store::kGog);
  assert(gog.authentication == AuthState::Waiting && gog.challenge_url == url + "gog#" + RelayStub::Token(1));
  // A paste that GOG refuses: a new token, the same sign-in.
  const auto failed = relay.relay->Submit(RelayStub::Token(1), "a-code", "192.0.2.50");
  assert(failed.status == 422 && failed.token == RelayStub::Token(2));
  controller.Tick();
  assert(gog.challenge_url == url + "gog#" + RelayStub::Token(2) && gog.authentication == AuthState::Waiting);
  assert(relay.relay->request() == request && !stores.gog->Canceled(request));
  // Web access off, then on: no address, then the address again.
  model.ApplyPreferences(model.BeginPreferencesLoad(), [] {
    playden::PreferencesLoadResult loaded;
    loaded.preferences.web_access = false;
    return loaded;
  }());
  Wait(controller, [&] { return gog.challenge_url.empty(); });
  assert(gog.authentication == AuthState::Waiting && relay.relay->request() == request);
  model.ApplyPreferences(model.BeginPreferencesLoad(), [] {
    playden::PreferencesLoadResult loaded;
    loaded.preferences.web_access = true;
    return loaded;
  }());
  Wait(controller, [&] { return !gog.challenge_url.empty(); });
  const auto again = web.service->Snapshot().url;
  assert(gog.challenge_url == again + "gog#" + RelayStub::Token(2));
  // Back: the relay closes and the sign-in is cancelled; its late answer is ignored.
  model.RequestServiceAction({ServiceAction::CancelQr, Store::kGog});
  controller.Tick();
  assert(relay.relay->request() == 0 && !relay.relay->Token() && stores.gog->Canceled(request));
  assert(gog.authentication == AuthState::SignedOut && gog.challenge_url.empty());
  stores.gog->Push(GogEvent(stores::AccountEventKind::kSignedIn, request));
  TickFor(controller, 20ms);
  assert(gog.authentication == AuthState::SignedOut && stores.Logged("ignored=other-request"));
  // A CancelQr with no sign-in open changes nothing.
  model.RequestServiceAction({ServiceAction::CancelQr, Store::kGog});
  controller.Tick();
  assert(gog.authentication == AuthState::SignedOut);
  // Signed in from the TV: the relay closes, and the model hears it answered the player's sign-in.
  model.RequestServiceAction({ServiceAction::BeginQr, Store::kGog});
  controller.Tick();
  const auto second = stores.gog->last_request;
  assert(relay.relay->request() == second && gog.challenge_url == again + "gog#" + RelayStub::Token(3));
  stores.gog->Push(GogEvent(stores::AccountEventKind::kSignedIn, second));
  Wait(controller, [&] { return gog.authentication == AuthState::SignedIn; });
  assert(relay.relay->request() == 0 && !relay.relay->Token() && gog.challenge_url.empty());
  assert(Notified(model, std::string("Signed in to GOG as ") + kGogName));
  assert(stores.gog_library->refreshes == 1 && stores.gog_library->last_account == kGogKey);
  // Nothing on the sign-in is logged but its request number.
  {
    std::lock_guard<std::mutex> lock(stores.log_mutex);
    for (const auto& line : stores.log)
      assert(line.find("gog#") == std::string::npos && line.find(RelayStub::Token(2)) == std::string::npos &&
             line.find(url) == std::string::npos && line.find(again) == std::string::npos);
  }
  stores.CheckLog();
}
// The token's ten minutes run out with nobody signing in: the sign-in ends with a reason, and Try again starts anew.
void GogSignInExpires() {
  RelayStub relay;
  TwoStores stores;
  auto& model = stores.model;
  GogSignedOut(stores, &relay);
  model.RequestServiceAction({ServiceAction::BeginQr, Store::kGog});
  stores.controller->Tick();
  const auto request = stores.gog->last_request;
  const auto& gog = model.state().account(Store::kGog);
  // No web access: no address, but the sign-in is open.
  assert(gog.authentication == AuthState::Waiting && gog.challenge_url.empty() && relay.relay->request() == request);
  relay.seconds += 10 * 60 + 1;
  stores.controller->Tick();
  assert(gog.authentication == AuthState::Failed && gog.error.find("expired") != std::string::npos);
  assert(stores.gog->Canceled(request) && relay.relay->request() == 0);
  model.RequestServiceAction({ServiceAction::BeginQr, Store::kGog});
  stores.controller->Tick();
  assert(gog.authentication == AuthState::Waiting && relay.relay->request() == stores.gog->last_request);
  stores.CheckLog();
}
// Without a relay, or when the account refuses a sign-in, Sign in to GOG fails with a reason.
void GogSignInUnavailable() {
  {
    TwoStores stores;
    GogSignedOut(stores, nullptr);
    stores.model.RequestServiceAction({ServiceAction::BeginQr, Store::kGog});
    stores.controller->Tick();
    const auto& gog = stores.model.state().account(Store::kGog);
    assert(gog.authentication == AuthState::Failed && gog.error == "GOG sign-in is unavailable." && stores.gog->sign_ins == 0);
    // Back from the failure: signed out again.
    stores.model.RequestServiceAction({ServiceAction::CancelQr, Store::kGog});
    stores.controller->Tick();
    assert(gog.authentication == AuthState::SignedOut && gog.error.empty());
  }
  {
    RelayStub relay;
    TwoStores stores;
    GogSignedOut(stores, &relay);
    stores.gog->refuse_sign_in = true;
    stores.model.RequestServiceAction({ServiceAction::BeginQr, Store::kGog});
    stores.controller->Tick();
    const auto& gog = stores.model.state().account(Store::kGog);
    assert(gog.authentication == AuthState::Failed && gog.error == "GOG sign-in is unavailable." && stores.gog->sign_ins == 1);
    assert(relay.relay->request() == 0 && !relay.relay->Token());
  }
}
// The player's GOG sign-out deletes the saved library while the account is still set, then resets the store; an
// expiry and a restore that finds nothing keep the saved library.
void GogSignOutClearsTheLibrary() {
  {
    TwoStores stores;
    auto& model = stores.model;
    stores.SignInBoth();
    const int invalidated = stores.gog_library->invalidated;
    model.RequestServiceAction({ServiceAction::SignOut, Store::kGog});
    stores.controller->Tick();
    stores.gog->Push(GogEvent(stores::AccountEventKind::kSignedOut, 0));
    Wait(*stores.controller, [&] { return model.state().account(Store::kGog).authentication == AuthState::SignedOut; });
    assert(stores.gog_library->clears == 1 && stores.gog_library->last_account == kGogKey);
    assert(stores.gog_library->invalidated == invalidated + 1 && model.state().account(Store::kGog).error.empty());
    assert(!stores.Has(GameId::Gog(1000000001)) && stores.Has(SteamId(42)));
    // Its late answer changes nothing.
    TickFor(*stores.controller, 10ms);
    assert(model.state().account(Store::kGog).error.empty() && !model.state().library_loading);
    stores.CheckLog();
  }
  {
    TwoStores stores;
    auto& model = stores.model;
    stores.SignInBoth();
    auto expired = GogEvent(stores::AccountEventKind::kExpired, 0);
    expired.error = "Your GOG sign-in expired. Sign in again.";
    stores.gog->Push(expired);
    Wait(*stores.controller, [&] { return model.state().account(Store::kGog).authentication == AuthState::Expired; });
    assert(stores.gog_library->clears == 0 && stores.Has(GameId::Gog(1000000001)));
  }
  {
    TwoStores stores;
    GogSignedOut(stores, nullptr);
    assert(stores.gog_library->clears == 0);
  }
}
// Steam signed out, GOG signed in: refreshing the library (the Library's Refresh) refreshes GOG's.
void RefreshWithoutSteamRefreshesGog() {
  TwoStores stores(false, false);
  auto& model = stores.model;
  stores.gog->Push(GogEvent(stores::AccountEventKind::kSignedIn, stores.gog->last_request));
  Wait(*stores.controller, [&] {
    return model.state().account().authentication == AuthState::SignedOut &&
           model.state().account(Store::kGog).authentication == AuthState::SignedIn && stores.Has(GameId::Gog(1000000001)) &&
           !model.state().library_loading;
  });
  assert(model.state().navigation.route == Route::Home);
  const int refreshes = stores.gog_library->refreshes;
  model.RequestServiceAction({ServiceAction::RefreshLibrary, Store::kSteam});
  stores.controller->Tick();
  assert(stores.gog_library->refreshes == refreshes + 1 && stores.library_backend.calls == 0);
  Wait(*stores.controller, [&] { return !model.state().library_loading; });
  assert(stores.Has(GameId::Gog(1000000001)) && model.state().account().authentication == AuthState::SignedOut);
  stores.CheckLog();
}
// PS5 title names carry their store with "Store name in PS5 titles": Steam's, GOG's and Epic's; imported
// games have no store to name.
void TitleNamesCarryTheirStore() {
  assert(TitleStoreSuffix(Store::kSteam)==" (Steam)");
  assert(TitleStoreSuffix(Store::kGog)==" (GOG)" && TitleStoreSuffix(Store::kEpic)==" (Epic)");
  assert(TitleStoreSuffix(Store::kLocal).empty());
}
// "Stay signed in" off on the sign-in screen: the token stays in memory, session.v1 never appears,
// and the next start asks for a sign-in again.
void StaySignedInOffKeepsTheTokenInMemory() {
  const auto root=std::filesystem::temp_directory_path()/("playden-stay-signed-in-"+std::to_string(getpid()));
  std::filesystem::remove_all(root); std::filesystem::create_directories(root);
  const auto file=root/"session.v1";
  {
    SessionVault vault;
    LocalSessionRepository repo(root.string(),{},&vault);
    LocalSessionRepository library(root.string(),{},&vault);
    Simulation model({}, {},ModelMode::Live); Backend backend;
    Controller controller(model,backend,repo);
    controller.SetStaySignedIn([&](bool stay) { assert(repo.SetPersist(stay).ok()); });
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
    Connect(model); controller.Tick();
    const auto stay=[&]{
      const auto& rows=model.view().rows;
      return std::find_if(rows.begin(),rows.end(),[](const ViewRow& row){return row.key=="signin-stay";});
    };
    assert(stay()!=model.view().rows.end() && stay()->kind==RowKind::Toggle && stay()->selected);
    Choose(model,"signin-stay");
    assert(!model.state().stay_signed_in && !model.ExportPreferences().steam_stay_signed_in && !stay()->selected);
    controller.Tick();
    assert(!vault.persist());
    // The password form offers the same toggle.
    Choose(model,"signin-password-mode"); controller.Tick();
    assert(stay()!=model.view().rows.end() && !stay()->selected);
    Choose(model,"signin-qr-mode"); backend.approve=true;
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    assert(!std::filesystem::exists(file));
    const auto shared=library.Load(); assert(shared.ok() && shared.value() && shared.value()->steam_id==12345);
    // Sign-out drops it from memory too.
    SignOut(model);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
    assert(library.Load().ok() && !library.Load().value() && !std::filesystem::exists(file));
    // Signed in again, then Playden closes.
    Connect(model); backend.approve=true;
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    controller.Shutdown();
  }
  {
    // The next start: nothing on disk, so the sign-in screen comes back.
    SessionVault vault(false);
    LocalSessionRepository repo(root.string(),{},&vault);
    Simulation model({}, {},ModelMode::Live); Backend backend;
    Controller controller(model,backend,repo);
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
    assert(backend.restores==0 && !std::filesystem::exists(file));
    controller.Shutdown();
  }
  {
    // Turned off just before quitting, while signed in from session.v1: the file still goes.
    { LocalSessionRepository writer(root.string()); assert(writer.Save(Session()).ok()); }
    SessionVault vault;
    LocalSessionRepository repo(root.string(),{},&vault);
    Simulation model({}, {},ModelMode::Live); Backend backend;
    Controller controller(model,backend,repo);
    controller.SetStaySignedIn([&](bool stay) { assert(repo.SetPersist(stay).ok()); });
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedIn;});
    OpenSettings(model); Choose(model,"account"); Choose(model,"account-reauth");
    Choose(model,"signin-stay");
    controller.Shutdown();
    assert(!vault.persist() && !std::filesystem::exists(file));
  }
  std::filesystem::remove_all(root);
}
// Friends tab › Appear invisible: the controller passes the choice on (to Playden's Steam session through the host)
// once, then again only when it changes, as when the saved choice loads.
void SteamPersonaFollowsThePreference() {
  const auto root=std::filesystem::temp_directory_path()/("playden-steam-persona-"+std::to_string(getpid()));
  std::filesystem::remove_all(root); std::filesystem::create_directories(root);
  {
    LocalSessionRepository repo(root.string());
    Simulation model({}, {},ModelMode::Live); Backend backend;
    Controller controller(model,backend,repo);
    std::vector<bool> applied;
    controller.SetSteamPersona([&](bool invisible) { applied.push_back(invisible); });
    controller.Tick(); controller.Tick();
    assert(applied==std::vector<bool>{false});
    playden::PreferencesLoadResult loaded; loaded.preferences.steam_invisible=true;
    assert(model.ApplyPreferences(model.BeginPreferencesLoad(),loaded) && model.state().steam_invisible);
    controller.Tick(); controller.Tick();
    assert((applied==std::vector<bool>{false,true}));
    controller.Shutdown();
  }
  std::filesystem::remove_all(root);
}
int main() { GogSignInFromTheTv(); GogSignInExpires(); GogSignInUnavailable(); GogSignOutClearsTheLibrary(); RefreshWithoutSteamRefreshesGog(); OtherStoreDetailsReachTheModel(); SteamSignOutKeepsOtherStoresLaunches(); OtherStoreAccountRestoresBesideSteam(); OtherStoreSignOutLeavesSteam(); OtherStoreExpiryKeepsTheScreen(); SteamPersonaFollowsThePreference(); StaySignedInOffKeepsTheTokenInMemory(); TitleNamesCarryTheirStore(); NoSteamSessionWithOtherGamesOpensHome(); FailedRestoreWithOtherGamesOpensHome(); OtherRestoreFailuresWithOtherGamesOpenHome(); SteamSignOutKeepsOtherGames(); PasswordSignInWithGuardCode(); PasswordFailuresAndCancel(); SlowLicenseCheckShowsAfterAFewSeconds(); CanceledStorageRetryKeepsSavedSessionIntent(); RevokedSessionCleanupRetriesAfterStorageRecovery(); StorageFailureKeepsVisibleAccount(); StorageFailureRetriesSavedSession(); ChallengeAndCancel(); AuthenticationRestoreAndSignOut(); FailedClearDoesNotSwitch(); SignInAgainKeepsTheAccount(); ExitHonorsClearWithoutStartingNewQr(); StorageFailureAndRevocation(); ConnectedLibraryAndOfflineRestart(); LicenseChangesRefreshTheLibrary(); OfflineRefreshFromTheLibraryReconnects(); ShutdownClearsLibraryBeforeSession(); RepeatedClearPreservesFirstIntent(); }
