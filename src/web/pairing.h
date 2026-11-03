// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace playden::web {

// Pairing a browser with Playden: the browser asks, the TV shows a 6-digit code, the player types it into the
// browser, and the browser gets a token for this Playden session. Tokens live in memory only.
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
    std::size_t max_tokens = 4;
  };
  enum class ConfirmResult { kPaired, kWrong, kNoCode, kLocked, kFailed };

  explicit Pairing(Options options);
  // Shows a code on the TV: the current one, or a new one when asked. False with the lockout's remaining
  // milliseconds in *wait, or when no random source is available (*wait 0).
  bool Start(bool replace, std::int64_t* wait);
  // Ends the code without pairing (the player dismissed it on the TV).
  void Cancel();
  ConfirmResult Confirm(const std::string& code, std::string* token);
  bool Paired(const std::string& token);
  // The code the TV should show now; empty when none.
  std::string ActiveCode();

 private:
  std::int64_t Now() const;
  void Expire(std::int64_t now);
  Options options_;
  std::mutex mutex_;
  std::string code_;
  std::int64_t code_expires_ = 0;
  // Wrong tries count across codes, or asking for a new code would give unlimited guesses.
  int wrong_tries_ = 0;
  std::int64_t locked_until_ = 0;
  std::vector<std::string> tokens_;
};

}  // namespace playden::web
