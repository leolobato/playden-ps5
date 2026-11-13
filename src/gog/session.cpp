// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/session.h"

#include "gog/account_key.h"
#include "gog/auth.h"
#include "gog/user.h"
#include "platform/secret.h"

#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace playden::gog {
namespace {

using platform::ZeroizeSecret;

const char* TokenKindName(TokenResult::Kind kind) {
  switch (kind) {
    case TokenResult::Kind::kOk: return "ok";
    case TokenResult::Kind::kRejected: return "rejected";
    case TokenResult::Kind::kNetwork: return "network";
    case TokenResult::Kind::kProtocol: return "protocol";
  }
  return "unknown";
}

const char* UserKindName(UserResult::Kind kind) {
  switch (kind) {
    case UserResult::Kind::kOk: return "ok";
    case UserResult::Kind::kRejected: return "rejected";
    case UserResult::Kind::kNetwork: return "network";
    case UserResult::Kind::kProtocol: return "protocol";
  }
  return "unknown";
}

std::string Line(const char* step, long status, const char* result) {
  char line[96];
  std::snprintf(line, sizeof line, "%s status=%ld result=%s", step, status, result);
  return line;
}

std::string Line(const char* step, const char* result) {
  char line[96];
  std::snprintf(line, sizeof line, "%s result=%s", step, result);
  return line;
}

// The reason strings come from SessionStore: short kinds such as "write_failed", never a path or account data.
std::string SaveFailedLine(const SaveResult& saved) {
  return "session save failed reason=" + (saved.reason.empty() ? std::string("unknown") : saved.reason);
}

}  // namespace

// Log lines gathered under the lock and written once it is released: declared before the lock, destroyed after it.
class Session::Lines {
 public:
  explicit Lines(const Log& log) : log_(log) {}
  ~Lines() {
    if (!log_) return;
    for (const auto& line : lines_) log_(line);
  }
  void Add(std::string line) { lines_.push_back(std::move(line)); }

 private:
  const Log& log_;
  std::vector<std::string> lines_;
};

void Identity::Wipe() {
  ZeroizeSecret(username);
  ZeroizeSecret(user_id);
  account_key = 0;
  generation = 0;
}

void AccessToken::Wipe() { ZeroizeSecret(token); }

Session::Session(SessionStore& store, Transport transport, std::function<std::int64_t()> now_seconds, Log log)
    : store_(store), transport_(std::move(transport)), now_(std::move(now_seconds)), log_(std::move(log)) {
  if (!now_)
    now_ = [] {
      return static_cast<std::int64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count());
    };
  if (!transport_)
    transport_ = [](const steam::HttpRequest&, const std::atomic<bool>&) {
      steam::HttpResponse response;
      response.error = steam::HttpError::kInitializationFailed;
      return response;
    };
}

Session::~Session() {
  std::lock_guard<std::mutex> lock(mutex_);
  WipeLocked();
}

void Session::WipeLocked() {
  signed_in_ = false;
  ZeroizeSecret(refresh_token_);
  ZeroizeSecret(access_token_);
  expires_at_ = 0;
  identity_.Wipe();
}

RestoreOutcome Session::Restore(const std::atomic<bool>& cancel) {
  Lines lines(log_);
  RestoreOutcome outcome;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    generation = generation_;
  }
  auto loaded = store_.Load();
  switch (loaded.status) {
    case LoadStatus::kNone:
      lines.Add(Line("restore", "none"));
      outcome.kind = RestoreOutcome::Kind::kNone;
      outcome.generation = generation;
      return outcome;
    case LoadStatus::kCorrupt:
    case LoadStatus::kUnavailable:
      lines.Add(std::string("restore result=storage reason=") +
                (loaded.reason.empty() ? std::string("unknown") : loaded.reason));
      outcome.kind = RestoreOutcome::Kind::kStorage;
      outcome.storage_full = loaded.storage_full;
      outcome.generation = generation;
      return outcome;
    case LoadStatus::kOk: break;
  }
  const std::uint64_t key = AccountKey(loaded.session.user_id);
  if (key == 0) {
    lines.Add(std::string("restore result=storage reason=account_key"));
    outcome.kind = RestoreOutcome::Kind::kStorage;
    outcome.generation = generation;
    return outcome;
  }

  TokenResult refreshed;
  {
    std::lock_guard<std::mutex> io(io_mutex_);
    refreshed = gog::Refresh(transport_, loaded.session.refresh_token, cancel);
  }
  lines.Add(Line("refresh", refreshed.status, TokenKindName(refreshed.kind)));

  std::lock_guard<std::mutex> lock(mutex_);
  outcome.generation = generation_;
  // A sign-in, sign-out or another restore happened meanwhile, or the caller gave up: this answer is not wanted.
  if (generation != generation_ || cancel.load()) {
    if (generation == generation_ && refreshed.kind == TokenResult::Kind::kOk) {
      // Canceled, but GOG may have rotated the refresh token: keep the saved session usable for the next start.
      StoredSession rotated = loaded.session;
      ZeroizeSecret(rotated.refresh_token);
      rotated.refresh_token = refreshed.tokens.refresh_token;
      const auto saved = store_.Save(rotated);
      if (!saved.ok) lines.Add(SaveFailedLine(saved));
    }
    refreshed.tokens.Wipe();
    lines.Add(Line("restore", "dropped"));
    outcome.kind = RestoreOutcome::Kind::kCanceled;
    return outcome;
  }
  switch (refreshed.kind) {
    case TokenResult::Kind::kRejected: {
      if (signed_in_) {
        // Restored over a session already in use (it is the one saved): that one is gone too.
        ++generation_;
        WipeLocked();
        outcome.generation = generation_;
      }
      const auto cleared = store_.Clear();
      if (!cleared.ok) lines.Add("session clear failed reason=" + cleared.reason);
      lines.Add(Line("restore", "expired"));
      outcome.kind = RestoreOutcome::Kind::kExpired;
      outcome.account_key = key;
      return outcome;
    }
    case TokenResult::Kind::kNetwork:
    case TokenResult::Kind::kProtocol:
      // Kept as saved: Token() refreshes when a worker needs a token.
      WipeLocked();
      refresh_token_ = loaded.session.refresh_token;
      identity_.username = loaded.session.username;
      identity_.user_id = loaded.session.user_id;
      identity_.account_key = key;
      signed_in_ = true;
      outcome.generation = ++generation_;
      lines.Add(Line("restore", "offline"));
      outcome.kind = RestoreOutcome::Kind::kOffline;
      return outcome;
    case TokenResult::Kind::kOk: break;
  }
  StoredSession rotated = loaded.session;
  ZeroizeSecret(rotated.refresh_token);
  rotated.refresh_token = refreshed.tokens.refresh_token;
  const auto saved = store_.Save(rotated);
  if (!saved.ok) {
    lines.Add(SaveFailedLine(saved));
    outcome.storage_full = saved.storage_full;
  }
  WipeLocked();
  refresh_token_ = refreshed.tokens.refresh_token;
  access_token_ = refreshed.tokens.access_token;
  expires_at_ = now_() + refreshed.tokens.expires_in;
  identity_.username = loaded.session.username;
  identity_.user_id = loaded.session.user_id;
  identity_.account_key = key;
  signed_in_ = true;
  refreshed.tokens.Wipe();
  outcome.generation = ++generation_;
  lines.Add(Line("restore", "signed-in"));
  outcome.kind = RestoreOutcome::Kind::kSignedIn;
  return outcome;
}

std::uint64_t Session::BeginSignIn() {
  std::lock_guard<std::mutex> lock(mutex_);
  epoch_ = ++next_epoch_;
  return epoch_;
}

void Session::EndSignIn(std::uint64_t epoch) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (epoch != 0 && epoch == epoch_) epoch_ = 0;
}

ExchangeOutcome Session::Exchange(std::string code, std::uint64_t epoch, const std::atomic<bool>& cancel) {
  Lines lines(log_);
  ExchangeOutcome outcome;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    outcome.generation = generation_;
    // Not the open sign-in: the code is not spent.
    if (epoch == 0 || epoch != epoch_) {
      ZeroizeSecret(code);
      lines.Add(Line("exchange", "stale"));
      outcome.kind = ExchangeOutcome::Kind::kStale;
      return outcome;
    }
    generation = generation_;
  }

  TokenResult tokens;
  UserResult user;
  {
    std::lock_guard<std::mutex> io(io_mutex_);
    tokens = gog::Exchange(transport_, code, cancel);
    ZeroizeSecret(code);  // moved-from or copied: either way its bytes go
    lines.Add(Line("exchange", tokens.status, TokenKindName(tokens.kind)));
    if (tokens.kind == TokenResult::Kind::kOk) {
      user = FetchUser(transport_, tokens.tokens.access_token, cancel);
      lines.Add(Line("user", user.status, UserKindName(user.kind)));
    }
  }
  if (tokens.kind != TokenResult::Kind::kOk || user.kind != UserResult::Kind::kOk) {
    tokens.tokens.Wipe();
    ZeroizeSecret(user.username);
    outcome.rejected = tokens.kind == TokenResult::Kind::kRejected;
    outcome.kind = ExchangeOutcome::Kind::kFailed;
    return outcome;
  }
  const std::uint64_t key = AccountKey(tokens.tokens.user_id);

  std::lock_guard<std::mutex> lock(mutex_);
  outcome.generation = generation_;
  if (epoch != epoch_ || generation != generation_ || key == 0) {
    tokens.tokens.Wipe();
    ZeroizeSecret(user.username);
    lines.Add(Line("exchange", key == 0 ? "account_key" : "dropped"));
    outcome.kind = key == 0 ? ExchangeOutcome::Kind::kFailed : ExchangeOutcome::Kind::kStale;
    return outcome;
  }
  StoredSession stored;
  stored.refresh_token = tokens.tokens.refresh_token;
  stored.user_id = tokens.tokens.user_id;
  stored.username = user.username;
  const auto saved = store_.Save(stored);
  if (!saved.ok) {
    lines.Add(SaveFailedLine(saved));
    outcome.storage_full = saved.storage_full;
  }
  WipeLocked();
  refresh_token_ = tokens.tokens.refresh_token;
  access_token_ = tokens.tokens.access_token;
  expires_at_ = now_() + tokens.tokens.expires_in;
  identity_.username = user.username;
  identity_.user_id = tokens.tokens.user_id;
  identity_.account_key = key;
  signed_in_ = true;
  tokens.tokens.Wipe();
  epoch_ = 0;
  outcome.generation = ++generation_;
  outcome.username = user.username;
  ZeroizeSecret(user.username);
  refreshed_.notify_all();
  outcome.kind = ExchangeOutcome::Kind::kSignedIn;
  return outcome;
}

AccessToken Session::Token(const std::atomic<bool>& cancel) {
  using Status = AccessToken::Status;
  Lines lines(log_);
  AccessToken result;
  bool revoked = false;
  std::uint64_t revoked_generation = 0;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      result.generation = generation_;
      if (cancel.load()) {
        result.status = Status::kCanceled;
        break;
      }
      if (!signed_in_) {
        result.status = Status::kSignedOut;
        break;
      }
      if (!access_token_.empty() && expires_at_ - now_() >= kRefreshMarginSeconds) {
        result.status = Status::kOk;
        result.token = access_token_;
        break;
      }
      if (refreshing_) {
        // Another caller is refreshing: wait for its result, watching this caller's own cancel flag.
        const std::uint64_t round = refresh_round_;
        const std::uint64_t generation = generation_;
        while (refreshing_ && round == refresh_round_ && generation == generation_ && !cancel.load())
          refreshed_.wait_for(lock, std::chrono::milliseconds(50));
        // Its network failure (or revocation) is this caller's answer too; anything else (success, a dropped or
        // canceled refresh, a new generation) is looked at again from the top.
        if (round != refresh_round_ && generation == generation_ && last_refresh_applied_) {
          if (last_refresh_ == Status::kNetwork) {
            result.status = Status::kNetwork;
            break;
          }
          // Its new token, even when GOG gave it less than the margin: no second refresh for the same token.
          if (last_refresh_ == Status::kOk && !access_token_.empty()) {
            result.status = Status::kOk;
            result.token = access_token_;
            break;
          }
        }
        continue;
      }

      // This caller refreshes.
      refreshing_ = true;
      const std::uint64_t generation = generation_;
      std::string refresh = refresh_token_;
      lock.unlock();
      TokenResult refreshed;
      {
        std::lock_guard<std::mutex> io(io_mutex_);
        refreshed = gog::Refresh(transport_, refresh, cancel);
      }
      ZeroizeSecret(refresh);
      lines.Add(Line("refresh", refreshed.status, TokenKindName(refreshed.kind)));
      lock.lock();
      refreshing_ = false;
      ++refresh_round_;
      last_refresh_applied_ = false;
      refreshed_.notify_all();

      if (generation != generation_) {
        // Signed out, revoked or replaced while it ran: never saved, never used.
        refreshed.tokens.Wipe();
        lines.Add(Line("refresh", "dropped"));
        result.status = Status::kSignedOut;
        break;
      }
      if (refreshed.kind == TokenResult::Kind::kOk) {
        StoredSession stored;
        stored.refresh_token = refreshed.tokens.refresh_token;
        stored.user_id = identity_.user_id;
        stored.username = identity_.username;
        const auto saved = store_.Save(stored);
        if (!saved.ok) lines.Add(SaveFailedLine(saved));
        ZeroizeSecret(refresh_token_);
        ZeroizeSecret(access_token_);
        refresh_token_ = refreshed.tokens.refresh_token;
        access_token_ = refreshed.tokens.access_token;
        expires_at_ = now_() + refreshed.tokens.expires_in;
        refreshed.tokens.Wipe();
        last_refresh_ = Status::kOk;
        last_refresh_applied_ = true;
        // Handed out even when GOG gave it less than the margin: refreshing again would not get a longer one.
        result.status = Status::kOk;
        result.token = access_token_;
        break;
      }
      if (refreshed.kind == TokenResult::Kind::kRejected) {
        const auto cleared = store_.Clear();
        if (!cleared.ok) lines.Add("session clear failed reason=" + cleared.reason);
        revoked_generation = generation_;
        ++generation_;
        WipeLocked();
        last_refresh_ = Status::kRevoked;
        last_refresh_applied_ = true;
        revoked = true;
        refreshed_.notify_all();
        result.generation = generation_;
        result.status = Status::kRevoked;
        break;
      }
      // No answer, or one that can't be used: the session stays as it is.
      refreshed.tokens.Wipe();
      if (cancel.load()) {
        result.status = Status::kCanceled;
        break;
      }
      last_refresh_ = Status::kNetwork;
      last_refresh_applied_ = true;
      result.status = Status::kNetwork;
      break;
    }
  }
  if (revoked) {
    std::lock_guard<std::mutex> listener(listener_mutex_);
    if (revoked_) revoked_(revoked_generation);
  }
  return result;
}

void Session::InvalidateAccessToken(std::uint64_t generation, std::string_view token) {
  Lines lines(log_);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!signed_in_ || generation != generation_ || access_token_.empty() || access_token_ != token) return;
  ZeroizeSecret(access_token_);
  expires_at_ = 0;
  lines.Add(Line("access token", "refused"));
}

SaveResult Session::SignOut() {
  Lines lines(log_);
  std::lock_guard<std::mutex> lock(mutex_);
  ++generation_;
  WipeLocked();
  refreshed_.notify_all();
  auto cleared = store_.Clear();
  lines.Add(cleared.ok ? Line("sign-out", "ok") : "sign-out result=failed reason=" + cleared.reason);
  return cleared;
}

Identity Session::identity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Identity identity = identity_;
  identity.generation = generation_;
  return identity;
}

std::uint64_t Session::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

bool Session::signed_in() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return signed_in_;
}

void Session::SetRevokedListener(std::function<void(std::uint64_t)> listener) {
  std::lock_guard<std::mutex> lock(listener_mutex_);
  revoked_ = std::move(listener);
}

}  // namespace playden::gog
