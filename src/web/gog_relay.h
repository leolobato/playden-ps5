// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace playden::web {

// What the GOG account answered for one code (it mirrors stores::SignInAnswer; web/ doesn't depend on stores/).
struct GogExchangeAnswer {
  enum class Kind {
    kSignedIn,  // name is the GOG username
    kFailed,    // GOG refused the code or didn't answer; the sign-in stays open for another code
    kStale,     // the request is not the open sign-in any more
    kBusy,      // a code for this request is being exchanged already
    kClosing,   // Playden is closing
    kTimeout,   // no answer within the wait; the exchange may still finish
  };
  Kind kind = Kind::kFailed;
  std::string name;
};

struct GogRelayConfig {
  // The sign-in code in what the player pasted (gog::CodeFromPaste), or nothing for the wrong shape.
  std::function<std::optional<std::string>(std::string_view paste)> parse_code;
  // Exchanges the code for the TV's sign-in request on the account worker and waits for the answer
  // (GogAccount::SignInWithCode). Called on a web server worker, never with the relay's lock held.
  std::function<GogExchangeAnswer(std::string code, std::uint64_t request)> exchange;
  // 32 lower-case hex digits (128 bits), or nothing. Empty: 16 bytes from /dev/urandom.
  std::function<std::optional<std::string>()> random_token;
  // Empty: the steady clock.
  std::function<std::chrono::steady_clock::time_point()> now;
  // Outcome lines ("gog relay result=<word>"); never a token, a paste or a code.
  std::function<void(const std::string& line)> log;
};

// One answer to the phone page: the status, a JSON body, and the token that replaces the spent one, if any.
struct RelayAnswer {
  int status = 0;
  std::string body;
  std::optional<std::string> token;
};

// The GOG sign-in relay: the TV opens it with a one-time token that it shows in a QR code, and the phone page posts
// the address GOG opened after sign-in, with that token, to POST /api/gog/sign-in. Open, Close, Token and request
// run on the main thread; Submit runs on a web server worker. The token is never logged.
class GogRelay {
 public:
  static constexpr std::chrono::minutes kTokenLife{10};
  static constexpr std::chrono::seconds kRateWindow{60};
  static constexpr std::size_t kRateLimit = 10;  // posts per client address in kRateWindow

  explicit GogRelay(GogRelayConfig config);
  GogRelay(const GogRelay&) = delete;
  GogRelay& operator=(const GogRelay&) = delete;

  // Starts taking a code for the TV's sign-in request: a new token, valid for kTokenLife, replaces any earlier one.
  // Nothing when no randomness is available (the relay is then closed).
  std::optional<std::string> Open(std::uint64_t request);
  // Back, or signed in: no token is valid any more, and an exchange still running ends with 410 unless it signed in.
  void Close();
  // The token the QR code carries: while open and not expired. It changes after a failed exchange.
  std::optional<std::string> Token() const;
  std::uint64_t request() const;
  // The token's time ran out with nobody signing in: the TV's sign-in must start again for a new one. False while a
  // code is being exchanged, and once closed by Back or a sign-in.
  bool Expired() const;
  // A post's exchange ended the sign-in on the relay's side with no account event to follow: GOG's session changed
  // meanwhile (stale), Playden is closing, or no randomness was left for a new token. The TV's sign-in must start
  // again. Kept, with the request, until Open or Close.
  bool Ended() const;

  // One post from the phone page; client is the sender's numeric address. May wait for the exchange.
  RelayAnswer Submit(const std::string& token, const std::string& paste, const std::string& client);

 private:
  enum class State { kClosed, kOpen, kExchanging };
  std::chrono::steady_clock::time_point Now() const;
  std::optional<std::string> NewToken() const;
  // Puts a new token in place for the same request (lock held); false when no randomness is available.
  bool Remint(std::chrono::steady_clock::time_point now);
  // Closes the relay for good on the relay's side, keeping the request for Ended (lock held).
  void EndLocked();
  bool Allowed(const std::string& client, std::chrono::steady_clock::time_point now);
  RelayAnswer Answer(const char* word, int status, std::string body, std::optional<std::string> token = std::nullopt) const;

  GogRelayConfig config_;
  mutable std::mutex mutex_;
  State state_ = State::kClosed;
  std::string token_;
  std::chrono::steady_clock::time_point expires_{};
  std::uint64_t request_ = 0;
  std::uint64_t epoch_ = 0;  // every Open and Close: an exchange's late result checks it
  bool ended_ = false;       // closed by EndLocked: Ended, not Expired
  std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> posts_;
};

}  // namespace playden::web
