// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/pairing.h"

#include "platform/random_id.h"

#include <algorithm>
#include <chrono>

namespace playden::web {
namespace {

std::string RandomHexDigits(const std::function<std::optional<std::string>()>& source, std::size_t digits) {
  std::string out;
  while (out.size() < digits) {
    const auto text = source();
    if (!text) return {};
    for (const char c : *text)
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) out += c;
    if (text->empty()) return {};
  }
  out.resize(digits);
  return out;
}

bool EqualConstantTime(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  unsigned char difference = 0;
  for (std::size_t i = 0; i < left.size(); ++i)
    difference = static_cast<unsigned char>(difference | (left[i] ^ right[i]));
  return difference == 0;
}

}  // namespace

Pairing::Pairing(Options options) : options_(std::move(options)) {
  if (!options_.random_hex) options_.random_hex = [] { return playden::RandomUuid(); };
}

std::int64_t Pairing::Now() const {
  if (options_.now) return options_.now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Pairing::Expire(std::int64_t now) {
  if (!code_.empty() && now >= code_expires_) code_.clear();
  if (locked_until_ != 0 && now >= locked_until_) {
    locked_until_ = 0;
    wrong_tries_ = 0;
  }
}

bool Pairing::Start(bool replace, std::int64_t* wait) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = Now();
  Expire(now);
  if (wait) *wait = 0;
  if (locked_until_ != 0) {
    if (wait) *wait = locked_until_ - now;
    return false;
  }
  if (!code_.empty() && !replace) return true;
  // 12 hex digits are 48 bits: the bias of taking them modulo a million is far too small to matter.
  const auto hex = RandomHexDigits(options_.random_hex, 12);
  if (hex.empty()) return false;
  std::uint64_t value = 0;
  for (const char c : hex) value = value * 16 + static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  std::string code = std::to_string(value % 1000000);
  code.insert(0, 6 - code.size(), '0');
  code_ = std::move(code);
  code_expires_ = now + options_.code_lifetime_ms;
  return true;
}

void Pairing::Cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  code_.clear();
}

Pairing::ConfirmResult Pairing::Confirm(const std::string& code, std::string* token) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = Now();
  Expire(now);
  if (locked_until_ != 0) return ConfirmResult::kLocked;
  if (code_.empty()) return ConfirmResult::kNoCode;
  if (!EqualConstantTime(code, code_)) {
    if (++wrong_tries_ >= options_.max_wrong_tries) {
      locked_until_ = now + options_.lockout_ms;
      code_.clear();
      return ConfirmResult::kLocked;
    }
    return ConfirmResult::kWrong;
  }
  auto minted = RandomHexDigits(options_.random_hex, 64);
  if (minted.empty()) return ConfirmResult::kFailed;
  code_.clear();
  wrong_tries_ = 0;
  if (tokens_.size() >= options_.max_tokens) tokens_.erase(tokens_.begin());
  tokens_.push_back(minted);
  if (token) *token = std::move(minted);
  return ConfirmResult::kPaired;
}

bool Pairing::Paired(const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool found = false;
  for (const auto& known : tokens_) found = EqualConstantTime(token, known) || found;
  return found;
}

std::string Pairing::ActiveCode() {
  std::lock_guard<std::mutex> lock(mutex_);
  Expire(Now());
  return code_;
}

}  // namespace playden::web
