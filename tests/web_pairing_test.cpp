// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/pairing.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace playden::web;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

int main() {
  std::int64_t now = 1000;
  int sequence = 0;
  Pairing::Options options;
  options.now = [&] { return now; };
  options.random_hex = [&]() -> std::optional<std::string> {
    char text[33];
    std::snprintf(text, sizeof text, "%08x%024x", ++sequence * 7919, sequence);
    return std::string(text);
  };
  Pairing pairing(options);
  std::int64_t wait = 0;
  std::string token;

  // Nothing to confirm yet, and no code on the TV.
  CHECK(pairing.Confirm("000000", &token) == Pairing::ConfirmResult::kNoCode);
  CHECK(pairing.ActiveCode().empty());
  CHECK(!pairing.Paired("") && !pairing.Paired("x"));

  // A code: six digits; asking again keeps it unless a new one is wanted.
  CHECK(pairing.Start(false, &wait));
  const auto code = pairing.ActiveCode();
  CHECK(code.size() == 6 && code.find_first_not_of("0123456789") == std::string::npos);
  CHECK(pairing.Start(false, &wait) && pairing.ActiveCode() == code);
  CHECK(pairing.Start(true, &wait) && pairing.ActiveCode() != code);

  // Right: a token that stays valid, and the code is used up.
  const auto second = pairing.ActiveCode();
  CHECK(pairing.Confirm(second, &token) == Pairing::ConfirmResult::kPaired);
  CHECK(token.size() == 64 && pairing.Paired(token));
  CHECK(pairing.ActiveCode().empty());
  CHECK(pairing.Confirm(second, nullptr) == Pairing::ConfirmResult::kNoCode);

  // Expiry.
  CHECK(pairing.Start(false, &wait));
  now += options.code_lifetime_ms;
  CHECK(pairing.ActiveCode().empty());
  CHECK(pairing.Paired(token));

  // Wrong tries count across new codes: asking for another doesn't give more guesses.
  const auto wrong_of = [](const std::string& right) { return right == "000000" ? std::string("000001") : std::string("000000"); };
  for (int i = 0; i < options.max_wrong_tries - 1; ++i) {
    CHECK(pairing.Start(true, &wait));
    CHECK(pairing.Confirm(wrong_of(pairing.ActiveCode()), nullptr) == Pairing::ConfirmResult::kWrong);
  }
  CHECK(pairing.Start(true, &wait));
  const auto last = pairing.ActiveCode();
  CHECK(pairing.Confirm(wrong_of(last), nullptr) == Pairing::ConfirmResult::kLocked);
  CHECK(pairing.ActiveCode().empty());
  CHECK(!pairing.Start(true, &wait) && wait == options.lockout_ms);
  CHECK(pairing.Confirm(last, nullptr) == Pairing::ConfirmResult::kLocked);
  now += options.lockout_ms;
  CHECK(pairing.Start(false, &wait));

  // Cancel from the TV.
  pairing.Cancel();
  CHECK(pairing.ActiveCode().empty());

  // At most a few browsers: the oldest goes first.
  std::string first_token;
  for (std::size_t i = 0; i <= options.max_tokens; ++i) {
    CHECK(pairing.Start(true, &wait));
    std::string minted;
    CHECK(pairing.Confirm(pairing.ActiveCode(), &minted) == Pairing::ConfirmResult::kPaired);
    if (i == 0) first_token = minted;
    CHECK(pairing.Paired(minted));
  }
  CHECK(!pairing.Paired(first_token));

  // No random source: no code, and no wait to report.
  Pairing::Options broken = options;
  broken.random_hex = []() -> std::optional<std::string> { return std::nullopt; };
  Pairing none(broken);
  CHECK(!none.Start(false, &wait) && wait == 0);
  std::puts("web_pairing: ok");
}
