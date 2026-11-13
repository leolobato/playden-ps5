// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace playden::web {

// Pairing a browser with Playden: the browser asks, the TV shows a 6-digit code, the player types it into the
// browser, and the browser gets a token. Playden keeps only a hash of each token. A browser paired for this session
// is forgotten when Playden closes; a remembered one is kept in a file under user data, so it stays paired across
// restarts until it goes unused for remember_seconds.
class Pairing {
 public:
  struct Options {
    // Milliseconds of a steady clock. Empty: the system's.
    std::function<std::int64_t()> now;
    // Fresh random hex digits, or nothing. Empty: /dev/urandom through RandomUuid.
    std::function<std::optional<std::string>()> random_hex;
    std::int64_t code_lifetime_ms = 120'000;
    std::int64_t lockout_ms = 30'000;
    int max_wrong_tries = 5;
    // Paired browsers, remembered or not. Pairing one more forgets the one used least recently.
    std::size_t max_tokens = 4;
    // Seconds since 1970, for when a browser was last used. Empty: the system clock.
    std::function<std::int64_t()> wall_now;
    // The private folder that keeps remembered browsers (absolute; its parent must exist). Empty: none is kept.
    std::string store_root;
    // A remembered browser unused this long has to pair again. Each use starts it over.
    std::int64_t remember_seconds = 90 * 86'400;
    // How old the stored last use may get before it is written again, so a browser's requests don't each write.
    std::int64_t touch_seconds = 86'400;
  };
  enum class ConfirmResult { kPaired, kWrong, kNoCode, kLocked, kFailed };
  enum class Browser { kNone, kSession, kRemembered };
  // kBusy: the file lock is held, try again soon; kFailed: the write failed, try again later.
  enum class StoreWork { kDone, kBusy, kFailed };

  explicit Pairing(Options options);
  // Shows a code on the TV: the current one, or a new one when asked. False with the lockout's remaining
  // milliseconds in *wait, or when no random source is available (*wait 0).
  bool Start(bool replace, std::int64_t* wait);
  // Ends the code without pairing (the player dismissed it on the TV).
  void Cancel();
  // remember: keep the browser across restarts (only when a store_root is set).
  ConfirmResult Confirm(const std::string& code, bool remember, std::string* token);
  // Whether a token is paired, and how; counts as a use of that browser.
  Browser Check(const std::string& token);
  bool Paired(const std::string& token) { return Check(token) != Browser::kNone; }
  // The code the TV should show now; empty when none.
  std::string ActiveCode();
  // Paired browsers now, remembered or not. No file access.
  std::size_t Count();
  // Every browser has to pair again, this session's too.
  void ForgetAll();
  // Called (without locks held) when the store has work: something to load or write.
  void SetStoreChanged(std::function<void()> changed);

  // Reads the store once and writes it when it changed, on one worker thread only. hold, when set, keeps root from
  // being raised (ps5::TryHoldEffectiveUid): nothing back means the file work waits (kBusy). *log gets a line for
  // web.log when something was read, written or refused.
  // A write that is still owed after kBusy or kFailed happens on the next call.
  StoreWork DoStoreWork(const std::function<std::shared_ptr<void>()>& hold, std::string* log);

 private:
  struct Entry {
    std::string hash;  // SHA-256 of the token, hex
    bool remembered = false;
    std::int64_t last_used = 0;  // seconds since 1970
    std::int64_t stored = 0;     // remembered: the last use the file has
  };
  std::int64_t Now() const;
  std::int64_t WallNow() const;
  void Expire(std::int64_t now);
  bool Expired(const Entry& entry, std::int64_t wall) const;
  void MarkDirty();
  bool StoreWanted() const;
  std::string Serialize() const;
  Options options_;
  std::mutex mutex_;
  std::string code_;
  std::int64_t code_expires_ = 0;
  // Wrong tries count across codes, or asking for a new code would give unlimited guesses.
  int wrong_tries_ = 0;
  std::int64_t locked_until_ = 0;
  std::vector<Entry> entries_;
  std::function<void()> store_changed_;
  bool loaded_ = false;          // the store was read (or there is none)
  bool forgot_unloaded_ = false;  // Forget all came before the store was read: what it holds is forgotten too
  bool dirty_ = false;           // the store doesn't have what entries_ remembers
  std::uint64_t changes_ = 0;    // counts changes to what the store should hold
  bool folder_ready_ = false;
  bool store_disabled_ = false;  // its folder couldn't be prepared: nothing is read or written this session
};

}  // namespace playden::web
