// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace playden::platform {

// Overwrites a secret's bytes before clearing it; std::string::clear() leaves them in memory.
inline void ZeroizeSecret(std::string& secret) {
  volatile char* bytes = secret.data();
  for (std::size_t i = 0; i < secret.size(); ++i) bytes[i] = 0;
  secret.clear();
}

// The same for a byte buffer, such as an HTTP body that held a token.
inline void ZeroizeSecret(std::vector<std::uint8_t>& secret) {
  volatile std::uint8_t* bytes = secret.data();
  for (std::size_t i = 0; i < secret.size(); ++i) bytes[i] = 0;
  secret.clear();
}

}  // namespace playden::platform
