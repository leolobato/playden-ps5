// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A GOG product ID as text: canonical positive decimal that fits 64 bits (no sign, no leading zero, no fraction or
// exponent), the rule GameId's GOG values follow (library/store.h, ParseGogValue). Header-only, so every GOG source
// and test can use it without another library.

#include <cstdint>
#include <string_view>

namespace playden::gog {

inline bool CanonicalId(std::string_view text, std::uint64_t* id) {
  if (text.empty() || text.size() > 20 || text.front() == '0') return false;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *id = value;
  return true;
}

}  // namespace playden::gog
