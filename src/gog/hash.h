// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Incremental MD5 and SHA-256 over Mbed TLS, for GOG's chunk and file hashes (lowercase hex, as manifests write them).

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace playden::gog {

class Md5 {
 public:
  Md5();
  ~Md5();
  Md5(const Md5&) = delete;
  Md5& operator=(const Md5&) = delete;

  void Update(const void* data, std::size_t size);
  // 32 lowercase hex characters; the object starts over for the next input.
  std::string FinalHex();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void Update(const void* data, std::size_t size);
  // 64 lowercase hex characters; the object starts over for the next input.
  std::string FinalHex();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// 32 lowercase hex characters: an MD5 as GOG writes one (chunk and file hashes, manifest IDs).
bool IsMd5Hex(std::string_view text);
// 64 lowercase hex characters.
bool IsSha256Hex(std::string_view text);

}  // namespace playden::gog
