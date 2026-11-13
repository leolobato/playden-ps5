// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GOG session for the whole process: it owns the refresh token, the access token and the account's identity, and
// every GOG worker (account, library, details, installs) asks it for an access token. There is no token copy per
// service.
//
// Generations: the number goes up at every sign-in, restore, sign-out and revocation. An exchange or a refresh notes
// the generation it started under, and its result is saved and used only if the generation is still the same. A late
// answer after Back, a sign-out or a newer sign-in is wiped and dropped.
//
// Log lines name the step and the outcome only ("refresh status=200 result=ok", "exchange status=400
// result=rejected", "restore result=offline"): never a URL, a token, a code, the username or the user ID.

#include "gog/http.h"
#include "gog/session_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace playden::gog {

// Refresh when fewer than this many seconds of the access token remain.
inline constexpr std::int64_t kRefreshMarginSeconds = 600;

struct Identity {
  std::string username;          // shown to the player, never logged
  std::string user_id;           // for log redaction only (the account worker), never logged or stored elsewhere
  std::uint64_t account_key = 0;  // AccountKey(user_id); 0 when signed out
  std::uint64_t generation = 0;   // the session's generation when this was read

  // Copied, never moved, so each copy wipes its own bytes (see StoredSession).
  Identity() = default;
  Identity(const Identity&) = default;
  Identity& operator=(const Identity&) = default;
  void Wipe();
  ~Identity() { Wipe(); }
};

struct RestoreOutcome {
  enum class Kind {
    kNone,      // nothing saved: signed out
    kStorage,   // the saved session can't be read (unsafe or corrupt); see SessionStore::Load
    kSignedIn,  // refreshed; the rotated refresh token was saved
    kExpired,   // GOG refused the refresh token: the saved session was cleared
    kOffline,   // no answer from GOG: the saved session is in use, and Token() refreshes later
    kCanceled,  // canceled, or a sign-in or sign-out happened meanwhile: nothing changed
  };
  Kind kind = Kind::kNone;
  bool storage_full = false;     // kStorage, or kSignedIn whose save failed with full storage
  std::uint64_t generation = 0;  // the session's generation when it returned
  std::uint64_t account_key = 0;  // kExpired: the account whose saved session GOG refused (its saved library stays)
};

struct ExchangeOutcome {
  enum class Kind {
    kSignedIn,  // saved (or kept in memory when the save failed) and in use
    kFailed,    // the code or the user call failed; the previous session is untouched
    kStale,     // the sign-in ended (Back, a newer screen) or the session changed meanwhile: dropped and wiped
  };
  Kind kind = Kind::kFailed;
  std::string username;          // kSignedIn only; never logged
  bool rejected = false;         // kFailed: GOG refused the code (sign in again), not a network or protocol error
  bool storage_full = false;     // kSignedIn: the save failed with full storage, so the session lives in memory only
  std::uint64_t generation = 0;  // the session's generation when it returned
};

struct AccessToken {
  enum class Status {
    kOk,
    kNetwork,    // no fresh token now (no answer, or GOG's answer was unusable); try again later
    kRevoked,    // GOG refused the refresh token: the session was cleared
    kSignedOut,  // no session, or it ended while the refresh ran
    kCanceled,   // the caller's cancel flag was set
  };
  Status status = Status::kSignedOut;
  std::string token;             // kOk only. The caller wipes it when done (Wipe, or the destructor).
  std::uint64_t generation = 0;  // the generation the token belongs to

  AccessToken() = default;
  AccessToken(const AccessToken&) = default;
  AccessToken& operator=(const AccessToken&) = default;
  void Wipe();
  ~AccessToken() { Wipe(); }
};

class Session {
 public:
  using Log = std::function<void(const std::string&)>;

  // store and transport are used from whichever thread calls in. HTTP calls are made one at a time (the transport
  // belongs to one HttpClient). now_seconds is a monotonic clock in seconds; log may be empty.
  Session(SessionStore& store, Transport transport, std::function<std::int64_t()> now_seconds, Log log = {});
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Loads the saved session and refreshes it (startup).
  RestoreOutcome Restore(const std::atomic<bool>& cancel);

  // A new sign-in for the open TV screen, replacing any other: the epoch its code must carry.
  std::uint64_t BeginSignIn();
  // Back on that screen: a code for this epoch, even one being exchanged, no longer signs in.
  void EndSignIn(std::uint64_t epoch);
  // Exchanges a sign-in code for tokens and fetches the username. The code is wiped.
  ExchangeOutcome Exchange(std::string code, std::uint64_t epoch, const std::atomic<bool>& cancel);

  // A usable access token, refreshing it when fewer than kRefreshMarginSeconds remain. One refresh runs at a time;
  // other callers wait for its result.
  AccessToken Token(const std::atomic<bool>& cancel);

  // An API refused this access token (401): the next Token() refreshes. Nothing changes when the session has moved on
  // (another generation) or already holds another token.
  void InvalidateAccessToken(std::uint64_t generation, std::string_view token);

  // Forgets the session in memory and on disk. The generation goes up even when the file can't be removed.
  SaveResult SignOut();

  Identity identity() const;
  std::uint64_t generation() const;
  bool signed_in() const;

  // Called once, outside the session's lock, with the generation that was revoked, when a refresh finds the session
  // revoked. Replacing or clearing the listener waits for a call in progress to return, so the listener must not
  // wait on whoever replaces it.
  void SetRevokedListener(std::function<void(std::uint64_t)> listener);

 private:
  class Lines;
  void WipeLocked();

  SessionStore& store_;
  Transport transport_;
  std::function<std::int64_t()> now_;
  Log log_;

  mutable std::mutex mutex_;
  std::condition_variable refreshed_;
  std::mutex io_mutex_;  // one HTTP call at a time; never held with mutex_

  bool signed_in_ = false;
  std::string refresh_token_;
  std::string access_token_;  // empty: refresh before use
  std::int64_t expires_at_ = 0;
  Identity identity_;
  std::uint64_t generation_ = 0;
  std::uint64_t next_epoch_ = 0;
  std::uint64_t epoch_ = 0;  // the open sign-in; 0 when none

  bool refreshing_ = false;
  std::uint64_t refresh_round_ = 0;  // goes up when a refresh ends
  AccessToken::Status last_refresh_ = AccessToken::Status::kOk;
  bool last_refresh_applied_ = false;  // the last refresh's result counted (not dropped or canceled)
  std::mutex listener_mutex_;  // held while the listener runs, and to replace it; never with mutex_
  std::function<void(std::uint64_t)> revoked_;  // under listener_mutex_
};

}  // namespace playden::gog
