// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/hash.h"

#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>

#include <cstdint>

namespace playden::gog {
namespace {

std::string Hex(const unsigned char* digest, std::size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string text(size * 2, '0');
  for (std::size_t i = 0; i < size; ++i) {
    text[i * 2] = kDigits[digest[i] >> 4];
    text[i * 2 + 1] = kDigits[digest[i] & 0x0f];
  }
  return text;
}

bool IsLowerHex(std::string_view text, std::size_t size) {
  if (text.size() != size) return false;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

}  // namespace

// Mbed TLS 3 calls only fail on a bad context, which these classes never pass, so their results are not checked.
struct Md5::State {
  mbedtls_md5_context context;
};

Md5::Md5() : state_(new State) {
  mbedtls_md5_init(&state_->context);
  mbedtls_md5_starts(&state_->context);
}

Md5::~Md5() { mbedtls_md5_free(&state_->context); }

void Md5::Update(const void* data, std::size_t size) {
  if (size) mbedtls_md5_update(&state_->context, static_cast<const unsigned char*>(data), size);
}

std::string Md5::FinalHex() {
  unsigned char digest[16] = {};
  mbedtls_md5_finish(&state_->context, digest);
  mbedtls_md5_starts(&state_->context);
  return Hex(digest, sizeof(digest));
}

struct Sha256::State {
  mbedtls_sha256_context context;
};

Sha256::Sha256() : state_(new State) {
  mbedtls_sha256_init(&state_->context);
  mbedtls_sha256_starts(&state_->context, 0);
}

Sha256::~Sha256() { mbedtls_sha256_free(&state_->context); }

void Sha256::Update(const void* data, std::size_t size) {
  if (size) mbedtls_sha256_update(&state_->context, static_cast<const unsigned char*>(data), size);
}

std::string Sha256::FinalHex() {
  unsigned char digest[32] = {};
  mbedtls_sha256_finish(&state_->context, digest);
  mbedtls_sha256_starts(&state_->context, 0);
  return Hex(digest, sizeof(digest));
}

bool IsMd5Hex(std::string_view text) { return IsLowerHex(text, 32); }
bool IsSha256Hex(std::string_view text) { return IsLowerHex(text, 64); }

}  // namespace playden::gog
