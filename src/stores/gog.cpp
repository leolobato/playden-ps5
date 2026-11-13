// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/gog.h"

#include "platform/secret.h"

#include <cstdio>
#include <utility>

namespace playden::stores {
namespace {

constexpr std::size_t kMaxEvents = 64;
constexpr char kExpiredMessage[] = "GOG sign-in expired. Sign in again.";
constexpr char kRestoreFailedMessage[] = "Playden couldn't read the saved GOG sign-in.";
constexpr char kSignOutFailedMessage[] = "Sign-out failed. Try again.";

const char* AnswerName(SignInAnswer::Kind kind) {
  switch (kind) {
    case SignInAnswer::Kind::kSignedIn: return "signed-in";
    case SignInAnswer::Kind::kFailed: return "failed";
    case SignInAnswer::Kind::kStale: return "stale";
    case SignInAnswer::Kind::kBusy: return "busy";
    case SignInAnswer::Kind::kClosing: return "closing";
    case SignInAnswer::Kind::kTimeout: return "timeout";
  }
  return "unknown";
}

const char* RestoreName(gog::RestoreOutcome::Kind kind) {
  switch (kind) {
    case gog::RestoreOutcome::Kind::kNone: return "signed-out";
    case gog::RestoreOutcome::Kind::kStorage: return "failure";
    case gog::RestoreOutcome::Kind::kSignedIn: return "signed-in";
    case gog::RestoreOutcome::Kind::kExpired: return "expired";
    case gog::RestoreOutcome::Kind::kOffline: return "offline";
    case gog::RestoreOutcome::Kind::kCanceled: return "canceled";
  }
  return "unknown";
}

std::string RequestLine(const char* step, std::uint64_t request, const char* result) {
  char line[96];
  std::snprintf(line, sizeof line, "%s request=%llu result=%s", step, static_cast<unsigned long long>(request),
                result);
  return line;
}

SignInAnswer Answer(SignInAnswer::Kind kind) {
  SignInAnswer answer;
  answer.kind = kind;
  return answer;
}

}  // namespace

GogAccount::Ticket::~Ticket() {
  platform::ZeroizeSecret(code);
  platform::ZeroizeSecret(answer.display_name);
}

GogAccount::GogAccount(gog::Session& session, gog::SessionStore& store, std::function<void(const std::string&)> log)
    : session_(session), store_(store), log_(std::move(log)) {
  session_.SetRevokedListener([this](std::uint64_t generation) { Revoked(generation); });
  worker_ = std::thread([this] { Run(); });
}

GogAccount::~GogAccount() { Shutdown(); }

void GogAccount::Log(const std::string& line) const {
  if (log_) log_(line);
}

void GogAccount::Shutdown() {
  // First, and without mutex_: clearing waits for a listener call in progress, which takes mutex_.
  session_.SetRevokedListener({});
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_ && !worker_.joinable()) return;
    stopping_ = true;
    if (restore_cancel_) restore_cancel_->store(true);
    if (running_cancel_) running_cancel_->store(true);
    // A sign-out asked for still clears the saved session, and a "Stay signed in" change still moves it; everything
    // else is dropped.
    std::deque<Job> kept;
    for (auto& job : pending_)
      if (job.operation == Operation::kSignOut || job.operation == Operation::kPersist) kept.push_back(std::move(job));
    pending_.swap(kept);
    busy_request_ = 0;
    wake_.notify_all();
    answered_.notify_all();
    // The waiters answer kClosing and leave before the account goes away.
    answered_.wait(lock, [this] { return waiters_ == 0; });
  }
  if (worker_.joinable()) worker_.join();
}

std::uint64_t GogAccount::RestoreSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return 0;
  if (restore_cancel_) restore_cancel_->store(true);
  restore_cancel_ = std::make_shared<std::atomic<bool>>(false);
  Job job;
  job.operation = Operation::kRestore;
  job.request = restore_request_ = ++next_request_;
  job.cancel = restore_cancel_;
  pending_.push_back(std::move(job));
  wake_.notify_all();
  return restore_request_;
}

std::uint64_t GogAccount::Reconnect() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return 0;
  if (restore_cancel_) restore_cancel_->store(true);
  restore_cancel_ = std::make_shared<std::atomic<bool>>(false);
  Job job;
  job.operation = Operation::kReconnect;
  job.request = restore_request_ = ++next_request_;
  job.cancel = restore_cancel_;
  pending_.push_back(std::move(job));
  wake_.notify_all();
  return restore_request_;
}

std::uint64_t GogAccount::BeginSignIn() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return 0;
  sign_in_epoch_ = session_.BeginSignIn();
  sign_in_request_ = ++next_request_;
  return sign_in_request_;
}

void GogAccount::Cancel(std::uint64_t request) {
  if (request == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (request == sign_in_request_) {
    session_.EndSignIn(sign_in_epoch_);
    sign_in_request_ = 0;
    sign_in_epoch_ = 0;
  }
  if (request == restore_request_) {
    if (restore_cancel_) restore_cancel_->store(true);
    restore_request_ = 0;
  }
}

void GogAccount::SignOut() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  // A restore still running must not answer after the sign-out.
  if (restore_cancel_) restore_cancel_->store(true);
  restore_request_ = 0;
  Job job;
  job.operation = Operation::kSignOut;
  pending_.push_back(std::move(job));
  wake_.notify_all();
}

std::vector<AccountEvent> GogAccount::PumpEvents(std::size_t max_events) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AccountEvent> result;
  while (!events_.empty() && result.size() < max_events) {
    result.push_back(std::move(events_.front()));
    events_.pop_front();
  }
  return result;
}

SignInAnswer GogAccount::SignInWithCode(std::string code, std::uint64_t request) {
  auto ticket = std::make_shared<Ticket>();
  ticket->code = code;
  platform::ZeroizeSecret(code);
  SignInAnswer answer;
  std::unique_lock<std::mutex> lock(mutex_);
  // Counted from here to the last line: Shutdown waits for every caller to leave before the account goes away.
  ++waiters_;
  if (stopping_) {
    answer = Answer(SignInAnswer::Kind::kClosing);
  } else if (request == 0 || request != sign_in_request_) {
    answer = Answer(SignInAnswer::Kind::kStale);
  } else if (request == busy_request_) {
    answer = Answer(SignInAnswer::Kind::kBusy);
  } else {
    busy_request_ = request;
    Job job;
    job.operation = Operation::kExchange;
    job.request = request;
    job.epoch = sign_in_epoch_;
    job.cancel = std::make_shared<std::atomic<bool>>(false);
    job.ticket = ticket;
    pending_.push_back(std::move(job));
    wake_.notify_all();
    answered_.wait_for(lock, kSignInWait, [&] { return ticket->done || stopping_; });
    if (ticket->done) answer = ticket->answer;
    else answer = Answer(stopping_ ? SignInAnswer::Kind::kClosing : SignInAnswer::Kind::kTimeout);
  }
  lock.unlock();
  Log(RequestLine("sign-in", request, AnswerName(answer.kind)));
  lock.lock();
  if (--waiters_ == 0) answered_.notify_all();
  return answer;
}

void GogAccount::SetPersist(bool persist, std::function<void(const gog::SaveResult&)> done) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  Job job;
  job.operation = Operation::kPersist;
  job.persist = persist;
  job.persisted = std::move(done);
  pending_.push_back(std::move(job));
  wake_.notify_all();
}

void GogAccount::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (pending_.empty()) return;
      job = std::move(pending_.front());
      pending_.pop_front();
      if (stopping_ && job.operation != Operation::kSignOut && job.operation != Operation::kPersist) continue;
      running_cancel_ = job.cancel;
    }
    switch (job.operation) {
      case Operation::kRestore: Restore(job); break;
      case Operation::kReconnect: ReconnectNow(job); break;
      case Operation::kExchange: Exchange(job); break;
      case Operation::kSignOut: SignOutNow(); break;
      case Operation::kPersist: {
        const auto saved = store_.SetPersist(job.persist);
        if (job.persisted) job.persisted(saved);
        break;
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_cancel_.reset();
  }
}

AccountEvent GogAccount::Signed(AccountEventKind kind, std::uint64_t request, const gog::Identity& identity) const {
  AccountEvent event;
  event.store = Store::kGog;
  event.kind = kind;
  event.request_id = request;
  event.generation = identity.generation;
  event.account_key = identity.account_key;
  event.display_name = identity.username;
  event.user_id = identity.user_id;
  return event;
}

void GogAccount::Emit(AccountEvent event) {
  events_.push_back(std::move(event));
  while (events_.size() > kMaxEvents) {
    platform::ZeroizeSecret(events_.front().user_id);
    events_.pop_front();
  }
}

void GogAccount::Restore(const Job& job) {
  const auto outcome = session_.Restore(*job.cancel);
  std::string line;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outcome.kind == gog::RestoreOutcome::Kind::kCanceled || job.cancel->load() ||
        job.request != restore_request_) {
      Log(RequestLine("restore", job.request, "canceled"));
      return;
    }
    restore_request_ = 0;
    AccountEvent event;
    event.store = Store::kGog;
    event.request_id = job.request;
    event.generation = outcome.generation;
    switch (outcome.kind) {
      case gog::RestoreOutcome::Kind::kSignedIn:
      case gog::RestoreOutcome::Kind::kOffline: {
        const auto identity = session_.identity();
        const bool offline = outcome.kind == gog::RestoreOutcome::Kind::kOffline;
        if (identity.account_key == 0 || identity.generation != outcome.generation) {
          // Revoked by another worker's refresh before this answer went out.
          event.kind = AccountEventKind::kExpired;
          event.generation = identity.generation;
          event.error = kExpiredMessage;
          line = RequestLine("restore", job.request, "expired");
          break;
        }
        event = Signed(offline ? AccountEventKind::kOffline : AccountEventKind::kSignedIn, job.request, identity);
        signed_in_generation_ = identity.generation;
        line = RequestLine("restore", job.request, RestoreName(outcome.kind));
        break;
      }
      case gog::RestoreOutcome::Kind::kNone:
        event.kind = AccountEventKind::kSignedOut;
        line = RequestLine("restore", job.request, RestoreName(outcome.kind));
        break;
      case gog::RestoreOutcome::Kind::kExpired:
        event.kind = AccountEventKind::kExpired;
        // The account's saved library is still shown while the player signs in again.
        event.account_key = outcome.account_key;
        event.error = kExpiredMessage;
        line = RequestLine("restore", job.request, RestoreName(outcome.kind));
        break;
      case gog::RestoreOutcome::Kind::kStorage:
        event.kind = AccountEventKind::kFailure;
        event.restoring = true;
        event.storage_full = outcome.storage_full;
        event.error = kRestoreFailedMessage;
        line = RequestLine("restore", job.request, RestoreName(outcome.kind));
        break;
      case gog::RestoreOutcome::Kind::kCanceled: return;
    }
    Emit(std::move(event));
  }
  Log(line);
}

void GogAccount::ReconnectNow(const Job& job) {
  auto token = session_.Token(*job.cancel);
  const auto status = token.status;
  const auto generation = token.generation;
  token.Wipe();
  std::string line;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status == gog::AccessToken::Status::kCanceled || job.cancel->load() || job.request != restore_request_) {
      Log(RequestLine("reconnect", job.request, "canceled"));
      return;
    }
    restore_request_ = 0;
    const auto identity = session_.identity();
    const bool usable = (status == gog::AccessToken::Status::kOk || status == gog::AccessToken::Status::kNetwork) &&
                        identity.account_key != 0 && identity.generation == generation;
    AccountEvent event;
    if (usable) {
      const bool online = status == gog::AccessToken::Status::kOk;
      event = Signed(online ? AccountEventKind::kSignedIn : AccountEventKind::kOffline, job.request, identity);
      signed_in_generation_ = identity.generation;
      line = RequestLine("reconnect", job.request, online ? "signed-in" : "offline");
    } else {
      // Revoked (the revocation's own event carries no request, so the controller waits for this one), or the session
      // ended meanwhile.
      event.store = Store::kGog;
      event.kind = AccountEventKind::kExpired;
      event.request_id = job.request;
      event.generation = identity.generation;
      event.error = kExpiredMessage;
      line = RequestLine("reconnect", job.request, "expired");
    }
    Emit(std::move(event));
  }
  Log(line);
}

void GogAccount::Exchange(const Job& job) {
  auto& ticket = *job.ticket;
  auto outcome = session_.Exchange(ticket.code, job.epoch, *job.cancel);
  platform::ZeroizeSecret(ticket.code);
  std::string line;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_request_ == job.request) busy_request_ = 0;
    SignInAnswer answer;
    switch (outcome.kind) {
      case gog::ExchangeOutcome::Kind::kSignedIn: {
        if (sign_in_request_ == job.request) {
          sign_in_request_ = 0;
          sign_in_epoch_ = 0;
        }
        const auto identity = session_.identity();
        if (identity.account_key == 0 || identity.generation != outcome.generation) {
          // Revoked by another worker's refresh before this answer went out.
          AccountEvent event;
          event.store = Store::kGog;
          event.kind = AccountEventKind::kExpired;
          event.request_id = job.request;
          event.generation = identity.generation;
          event.error = kExpiredMessage;
          Emit(std::move(event));
          answer = Answer(SignInAnswer::Kind::kFailed);
          break;
        }
        Emit(Signed(AccountEventKind::kSignedIn, job.request, identity));
        signed_in_generation_ = identity.generation;
        answer = Answer(SignInAnswer::Kind::kSignedIn);
        answer.display_name = outcome.username;
        break;
      }
      case gog::ExchangeOutcome::Kind::kStale: answer = Answer(SignInAnswer::Kind::kStale); break;
      case gog::ExchangeOutcome::Kind::kFailed:
        answer = Answer(SignInAnswer::Kind::kFailed);
        answer.rejected = outcome.rejected;
        break;
    }
    platform::ZeroizeSecret(outcome.username);
    line = RequestLine("exchange", job.request, AnswerName(answer.kind));
    ticket.answer = std::move(answer);
    ticket.done = true;
    answered_.notify_all();
  }
  Log(line);
}

void GogAccount::SignOutNow() {
  const auto cleared = session_.SignOut();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    signed_in_generation_ = 0;
    AccountEvent event;
    event.store = Store::kGog;
    event.request_id = 0;
    event.generation = session_.generation();
    if (cleared.ok) {
      event.kind = AccountEventKind::kSignedOut;
    } else {
      event.kind = AccountEventKind::kFailure;
      event.storage_full = cleared.storage_full;
      event.error = kSignOutFailedMessage;
    }
    Emit(std::move(event));
  }
  Log(RequestLine("sign-out", 0, cleared.ok ? "signed-out" : "failure"));
}

void GogAccount::Revoked(std::uint64_t generation) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != signed_in_generation_) return;
    signed_in_generation_ = 0;
    AccountEvent event;
    event.store = Store::kGog;
    event.kind = AccountEventKind::kExpired;
    event.request_id = 0;
    event.generation = session_.generation();
    event.error = kExpiredMessage;
    Emit(std::move(event));
  }
  Log(RequestLine("token", 0, "expired"));
}

}  // namespace playden::stores
