// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/gog_relay.h"

#include "platform/secret.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace playden::web {
namespace {

constexpr std::size_t kTokenBytes = 16;
// Distinct senders remembered for the rate limit at once; a new one past this is refused until the window passes.
constexpr std::size_t kMaxClients = 256;

constexpr const char* kTooMany = "Too many tries. Wait a minute and try again.";
constexpr const char* kEnded = "This sign-in has ended. Choose Sign in to GOG on the PS5 again.";
constexpr const char* kOlder = "This page is from an older sign-in. Scan the code on the PS5 again.";
constexpr const char* kWrongShape = "That isn't the address GOG opened after sign-in.";
constexpr const char* kReused = "Playden is already signing in with this code.";
constexpr const char* kFailed = "That address didn't work. Sign in again and paste the new one.";
constexpr const char* kClosing = "Playden is closing.";
constexpr const char* kTimeout = "Playden is still working on it. Look at the PS5.";
constexpr const char* kNoRandom = "Playden can't start a new sign-in. Choose Sign in to GOG on the PS5 again.";

std::optional<std::string> UrandomToken() {
  unsigned char buffer[kTokenBytes];
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::size_t done = 0;
  while (done < sizeof buffer) {
    const auto count = ::read(fd, buffer + done, sizeof buffer - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ::close(fd);
      return std::nullopt;
    }
    done += static_cast<std::size_t>(count);
  }
  ::close(fd);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(kTokenBytes * 2);
  for (const unsigned char byte : buffer) {
    text.push_back(kHex[byte >> 4]);
    text.push_back(kHex[byte & 0x0f]);
  }
  volatile unsigned char* wipe = buffer;
  for (std::size_t i = 0; i < sizeof buffer; ++i) wipe[i] = 0;
  return text;
}

bool IsToken(const std::string& text) {
  if (text.size() != kTokenBytes * 2) return false;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// Takes as long for every token of the right length, wherever the first difference is.
bool EqualConstantTime(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  unsigned char difference = 0;
  for (std::size_t i = 0; i < left.size(); ++i) difference = static_cast<unsigned char>(difference | (left[i] ^ right[i]));
  return difference == 0;
}

std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += raw;
    } else if (c < 0x20 || c == 0x7f) {
      char code[8];
      std::snprintf(code, sizeof code, "\\u%04x", c);
      out += code;
    } else {
      out += raw;
    }
  }
  return out + "\"";
}

std::string Error(std::string_view message) { return "{\"error\":" + Quoted(message) + "}"; }

}  // namespace

GogRelay::GogRelay(GogRelayConfig config) : config_(std::move(config)) {}

std::chrono::steady_clock::time_point GogRelay::Now() const {
  return config_.now ? config_.now() : std::chrono::steady_clock::now();
}

std::optional<std::string> GogRelay::NewToken() const {
  auto token = config_.random_token ? config_.random_token() : UrandomToken();
  if (!token || !IsToken(*token)) {
    if (token) platform::ZeroizeSecret(*token);
    return std::nullopt;
  }
  return token;
}

std::optional<std::string> GogRelay::Open(std::uint64_t request) {
  auto token = NewToken();
  const auto now = Now();
  std::lock_guard<std::mutex> lock(mutex_);
  ++epoch_;
  ended_ = false;
  platform::ZeroizeSecret(token_);
  if (!token) {
    state_ = State::kClosed;
    request_ = 0;
    return std::nullopt;
  }
  state_ = State::kOpen;
  token_ = *token;
  expires_ = now + kTokenLife;
  request_ = request;
  return token;
}

void GogRelay::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++epoch_;
  ended_ = false;
  state_ = State::kClosed;
  platform::ZeroizeSecret(token_);
  request_ = 0;
}

std::optional<std::string> GogRelay::Token() const {
  const auto now = Now();
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::kOpen || now >= expires_) return std::nullopt;
  return token_;
}

std::uint64_t GogRelay::request() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return request_;
}

bool GogRelay::Expired() const {
  const auto now = Now();
  std::lock_guard<std::mutex> lock(mutex_);
  // A post after the expiry closed it but kept the request (Close, Open and a sign-in forget it).
  return (state_ == State::kOpen && now >= expires_) || (state_ == State::kClosed && request_ != 0 && !ended_);
}

bool GogRelay::Ended() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == State::kClosed && request_ != 0 && ended_;
}

void GogRelay::EndLocked() {
  ++epoch_;
  state_ = State::kClosed;
  platform::ZeroizeSecret(token_);
  ended_ = true;
}

bool GogRelay::Remint(std::chrono::steady_clock::time_point now) {
  auto token = NewToken();
  platform::ZeroizeSecret(token_);
  if (!token) {
    EndLocked();
    return false;
  }
  state_ = State::kOpen;
  token_ = std::move(*token);
  expires_ = now + kTokenLife;
  return true;
}

bool GogRelay::Allowed(const std::string& client, std::chrono::steady_clock::time_point now) {
  for (auto it = posts_.begin(); it != posts_.end();) {
    auto& times = it->second;
    while (!times.empty() && now - times.front() >= kRateWindow) times.pop_front();
    if (times.empty()) it = posts_.erase(it);
    else ++it;
  }
  auto found = posts_.find(client);
  if (found == posts_.end()) {
    if (posts_.size() >= kMaxClients) return false;
    found = posts_.emplace(client, std::deque<std::chrono::steady_clock::time_point>()).first;
  }
  if (found->second.size() >= kRateLimit) return false;
  found->second.push_back(now);
  return true;
}

RelayAnswer GogRelay::Answer(const char* word, int status, std::string body, std::optional<std::string> token) const {
  if (config_.log) config_.log(std::string("gog relay result=") + word);
  RelayAnswer answer;
  answer.status = status;
  answer.body = std::move(body);
  answer.token = std::move(token);
  return answer;
}

RelayAnswer GogRelay::Submit(const std::string& token, const std::string& paste, const std::string& client) {
  std::optional<std::string> code;
  std::uint64_t epoch = 0;
  std::uint64_t request = 0;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto now = Now();
    if (!Allowed(client, now)) {
      lock.unlock();
      return Answer("rate_limited", 429, Error(kTooMany));
    }
    if (state_ == State::kOpen && now >= expires_) {
      state_ = State::kClosed;
      platform::ZeroizeSecret(token_);
    }
    if (state_ == State::kClosed) {
      lock.unlock();
      return Answer("ended", 410, Error(kEnded));
    }
    const bool current = EqualConstantTime(token, token_);
    if (!current) {
      lock.unlock();
      return Answer("wrong_token", 403, Error(kOlder));
    }
    // The token was spent by a post whose exchange is still running.
    if (state_ == State::kExchanging) {
      lock.unlock();
      return Answer("reused", 409, Error(kReused));
    }
    if (config_.parse_code) code = config_.parse_code(paste);
    if (!code || code->empty()) {
      if (code) platform::ZeroizeSecret(*code);
      lock.unlock();
      return Answer("wrong_shape", 422, Error(kWrongShape));
    }
    // Spent at once: a second post with this token is answered 409 while this one is exchanged.
    state_ = State::kExchanging;
    epoch = epoch_;
    request = request_;
  }

  GogExchangeAnswer exchanged;
  exchanged.kind = GogExchangeAnswer::Kind::kClosing;
  if (config_.exchange) exchanged = config_.exchange(std::move(*code), request);
  platform::ZeroizeSecret(*code);
  code.reset();

  using Kind = GogExchangeAnswer::Kind;
  std::unique_lock<std::mutex> lock(mutex_);
  const bool same = epoch == epoch_;
  if (exchanged.kind == Kind::kSignedIn) {
    // Signed in, even when the TV already closed the relay on the sign-in event.
    if (same) {
      ++epoch_;
      state_ = State::kClosed;
      platform::ZeroizeSecret(token_);
      request_ = 0;
    }
    lock.unlock();
    return Answer("signed_in", 200, "{\"signed_in\":true,\"name\":" + Quoted(exchanged.name) + "}");
  }
  // Back or a newer sign-in screen meanwhile: this page's sign-in is over; the newer token stays.
  if (!same) {
    lock.unlock();
    return Answer("ended", 410, Error(kEnded));
  }
  const auto now = Now();
  switch (exchanged.kind) {
    case Kind::kFailed:
    case Kind::kBusy:
    case Kind::kTimeout: {
      // The same request takes another code: a new token, which the TV redraws and the page adopts.
      const char* word = exchanged.kind == Kind::kFailed ? "failed" : exchanged.kind == Kind::kBusy ? "busy" : "timeout";
      if (!Remint(now)) {
        lock.unlock();
        return Answer(word, 503, Error(kNoRandom));
      }
      const std::string fresh = token_;
      lock.unlock();
      const int status = exchanged.kind == Kind::kFailed ? 422 : exchanged.kind == Kind::kBusy ? 409 : 504;
      const char* message = exchanged.kind == Kind::kFailed ? kFailed : exchanged.kind == Kind::kBusy ? kReused : kTimeout;
      return Answer(word, status, "{\"error\":" + Quoted(message) + ",\"token\":" + Quoted(fresh) + "}", fresh);
    }
    case Kind::kClosing:
      EndLocked();
      lock.unlock();
      return Answer("closing", 503, Error(kClosing));
    case Kind::kStale:
    default:
      EndLocked();
      lock.unlock();
      return Answer("stale", 410, Error(kEnded));
  }
}

}  // namespace playden::web
