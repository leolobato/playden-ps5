// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gog/session.h"
#include "gog/session_store.h"
#include "stores/account.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace playden::stores {

// What the phone relay tells the player after it handed a sign-in code over (SignInWithCode).
struct SignInAnswer {
  enum class Kind {
    kSignedIn,  // display_name is the GOG username
    kFailed,    // GOG refused the code, or didn't answer: the sign-in stays open for another code
    kStale,     // the request is not the open sign-in (Back, a newer screen, already signed in)
    kBusy,      // a code for this request is being exchanged already
    kClosing,   // Playden is closing
    kTimeout,   // no result within the wait; the exchange may still finish and sign in
  };
  Kind kind = Kind::kFailed;
  std::string display_name;  // kSignedIn only; never logged
  bool rejected = false;     // kFailed: GOG refused the code (not a network error)
};

// GOG's account over gog::Session, on its own worker thread (curl and steam::json, as steam::SteamService's worker).
// Sign-in is answered elsewhere: BeginSignIn opens a request for the TV screen, and the web relay hands the code over
// with SignInWithCode. Every event carries the session's generation. Log lines name the step, the request number and
// the outcome only.
class GogAccount final : public StoreAccount {
 public:
  GogAccount(gog::Session& session, gog::SessionStore& store, std::function<void(const std::string&)> log = {});
  ~GogAccount() override;
  GogAccount(const GogAccount&) = delete;
  GogAccount& operator=(const GogAccount&) = delete;

  Store store() const override { return Store::kGog; }
  // kSignedIn or kOffline (with the account), kSignedOut (nothing saved), kExpired, or kFailure (restoring) for the
  // saved session can't be read. A canceled restore answers nothing.
  std::uint64_t RestoreSession() override;
  // Offline since the restore: asks the session for an access token (a refresh when it has none). kSignedIn when GOG
  // answers, kOffline when it still doesn't, kExpired when the session was revoked or ended. Canceled as a restore is.
  std::uint64_t Reconnect() override;
  // A new sign-in request for the TV screen, replacing any open one.
  std::uint64_t BeginSignIn() override;
  // The open sign-in: a code for it, even one being exchanged, no longer signs in. A restore: canceled.
  void Cancel(std::uint64_t request) override;
  // Answered with kSignedOut, or kFailure ("Sign-out failed. Try again.") when the file can't be removed; request 0.
  void SignOut() override;
  std::vector<AccountEvent> PumpEvents(std::size_t max_events) override;

  // From a web server worker: exchanges the code on the account worker and waits up to kSignInWait for the answer.
  // Success also raises kSignedIn with request_id = request; a failure raises nothing and the request stays open. The
  // code is wiped on every path.
  SignInAnswer SignInWithCode(std::string code, std::uint64_t request);
  // "Stay signed in", passed to the session store on the account worker, never on the caller's thread: the store's
  // writes wait for the root lock, which a runtime batch can hold for long. done, if any, gets the result on the
  // worker. Asked for before Shutdown, it still runs, as a sign-out does.
  void SetPersist(bool persist, std::function<void(const gog::SaveResult&)> done = {});
  gog::Session& session() { return session_; }
  // Answers every waiting SignInWithCode with kClosing and stops the worker. A sign-out or a "Stay signed in" change
  // already asked for still runs.
  void Shutdown() override;

  static constexpr std::chrono::seconds kSignInWait{20};

 private:
  enum class Operation { kRestore, kReconnect, kExchange, kSignOut, kPersist };
  // One SignInWithCode: its code until the worker takes it, then its answer. Shared, because the waiter can leave
  // (timeout) while the worker still answers.
  struct Ticket {
    std::string code;
    bool done = false;
    SignInAnswer answer;
    ~Ticket();
  };
  struct Job {
    Operation operation = Operation::kRestore;
    std::uint64_t request = 0;
    std::uint64_t epoch = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::shared_ptr<Ticket> ticket;
    bool persist = false;                                  // kPersist
    std::function<void(const gog::SaveResult&)> persisted;  // kPersist
  };

  void Run();
  void Restore(const Job& job);
  void ReconnectNow(const Job& job);
  void Exchange(const Job& job);
  void SignOutNow();
  void Revoked(std::uint64_t generation);
  void Emit(AccountEvent event);
  AccountEvent Signed(AccountEventKind kind, std::uint64_t request, const gog::Identity& identity) const;
  void Log(const std::string& line) const;

  gog::Session& session_;
  gog::SessionStore& store_;
  std::function<void(const std::string&)> log_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;     // the worker: a job or stopping
  std::condition_variable answered_;  // SignInWithCode waiters, and Shutdown waiting for them to leave
  std::deque<Job> pending_;
  std::deque<AccountEvent> events_;
  std::uint64_t next_request_ = 0;
  std::uint64_t restore_request_ = 0;  // a restore or a reconnect
  std::shared_ptr<std::atomic<bool>> restore_cancel_;
  std::shared_ptr<std::atomic<bool>> running_cancel_;  // the job on the worker now
  std::uint64_t sign_in_request_ = 0;
  std::uint64_t sign_in_epoch_ = 0;
  std::uint64_t busy_request_ = 0;           // a code for this request is queued or being exchanged
  std::uint64_t signed_in_generation_ = 0;   // the session generation the last kSignedIn/kOffline announced
  std::size_t waiters_ = 0;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace playden::stores
