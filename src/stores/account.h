#pragma once
#include "library/store.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace playden::stores {

enum class AccountEventKind { kSignedIn, kOffline, kSignedOut, kExpired, kFailure };

// What a store's account worker reports to the controller (Steam's are steam::SteamEvent instead).
struct AccountEvent {
  Store store = Store::kSteam;
  AccountEventKind kind = AccountEventKind::kFailure;
  // The request it answers (RestoreSession's, or one of the store's own sign-in calls); 0 for an event the
  // account raises itself: the answer to SignOut, or a session that expired while in use.
  std::uint64_t request_id = 0;
  // The account worker's own session generation, for the log.
  std::uint64_t generation = 0;
  // Names the account's files (a hash), never the store's user ID; 0 when signed out. kExpired: the account whose
  // saved library may still be shown, or 0.
  std::uint64_t account_key = 0;
  std::string display_name;        // shown in Settings, never logged
  std::string user_id;             // only for log redaction; never reaches the model or a log
  std::string error;               // player-facing, no identifiers
  bool restoring = false;          // kFailure: the saved sign-in could not be restored
  bool storage_full = false;       // kFailure: console storage is full
};

// One store's account: it restores the saved sign-in at startup and forgets it on sign-out. How a
// player signs in is the store's own (Steam: QR code or account name and password, see SteamAccount).
// Results come back on the store's worker and are read by the controller.
class StoreAccount {
 public:
  virtual ~StoreAccount() = default;
  virtual Store store() const = 0;
  // Starts restoring the saved sign-in; the request it answers, 0 when refused.
  virtual std::uint64_t RestoreSession() = 0;
  virtual void Cancel(std::uint64_t request) = 0;
  // For a store whose sign-in is answered elsewhere (GOG's phone relay): opens a sign-in and returns the request the
  // answer will carry. 0 when unsupported (Steam signs in through SteamAccount's own calls).
  virtual std::uint64_t BeginSignIn() { return 0; }
  // A session restored offline: asks the store again. The request is answered kSignedIn once the store answers,
  // kOffline while it still doesn't, or kExpired. 0 when the store has no such call (Steam restores again instead).
  virtual std::uint64_t Reconnect() { return 0; }
  // Forgets the saved sign-in.
  virtual void SignOut() = 0;
  // Stops the store's worker, answering whoever waits on it; a sign-out already asked for still runs. The controller
  // calls it for every store but Steam at its Shutdown, before web access's workers are joined. Safe to call twice.
  virtual void Shutdown() {}
  // The results so far, at most max_events, oldest first. Steam's account doesn't use this: its events are
  // Steam's own, read through SteamAccount::Pump.
  virtual std::vector<AccountEvent> PumpEvents(std::size_t max_events) {
    (void)max_events;
    return {};
  }
};

}  // namespace playden::stores
