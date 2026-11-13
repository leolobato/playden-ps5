// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The number that names a GOG account's files (library-gog-<key>.cache, updates-gog-<key>.v2) and goes in
// stores::AccountEvent::account_key. It is a hash of the GOG user ID, so a file name in a log bundle or an FTP listing
// doesn't name the account. The user ID itself never goes into a file name, an event key or a log.

#include "platform/secret.h"
#include "runtime/artifact_hash.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace playden::gog {

// The first 8 bytes, big-endian, of SHA-256("playden-gog-account\n" + user_id). A hash of 0 becomes 1, so 0 always
// means "no account". 0 only when the hash can't be computed (it can't fail in practice); callers refuse that.
inline std::uint64_t AccountKey(std::string_view user_id) {
  static constexpr std::string_view kPrefix = "playden-gog-account\n";
  std::string input;
  input.reserve(kPrefix.size() + user_id.size());
  input += kPrefix;
  input += user_id;
  auto digest = runtime::HashArtifactBytes(input);
  platform::ZeroizeSecret(input);
  if (!digest || digest->size() < 16) return 0;
  std::uint64_t key = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    const char c = (*digest)[i];
    const int nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (nibble < 0) return 0;
    key = key << 4 | static_cast<std::uint64_t>(nibble);
  }
  return key == 0 ? 1 : key;
}

}  // namespace playden::gog
