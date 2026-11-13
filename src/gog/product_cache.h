// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What gamesdb said about each owned product, kept between refreshes: <user root>/gog/products/<product id>, 0600
// files in 0700 folders, with no link in any component, written durably. Its ETag lets the next refresh ask with
// If-None-Match, so an unchanged product costs a 304 and no parse. Nothing here logs.
//
// File: magic "PDGOGP\0\1", version u8 = 1, payload length u32 LE, CRC-32 of the payload u32 LE, then the payload:
// the ETag, a kind byte (1 game, 2 hidden), then the title, summary, developer, cover and hero, each string with a
// u32-LE length, and nothing after them.

#include "gog/api.h"
#include "platform/storage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace playden::gog {

struct CachedProduct {
  std::string etag;                                   // empty, or UsableEtag
  ProductInfo::Kind kind = ProductInfo::Kind::kHidden;  // kGame or kHidden; a kInvalid answer is never kept
  std::string title;
  std::string summary;
  std::string developer;
  std::string cover;  // empty, or IsArtUrl of a cover
  std::string hero;   // empty, or IsArtUrl of a hero
};

// The file's bytes, or empty when a field is out of range. Exposed for tests.
Bytes EncodeProduct(const CachedProduct& product);
// False for anything but exactly one well-formed product. Exposed for tests.
bool DecodeProduct(const Bytes& bytes, CachedProduct* product);

inline constexpr char kProductsFolderName[] = "products";
inline constexpr std::size_t kMaxProductFileBytes = 96 * 1024;

class ProductCache {
 public:
  // user_root: absolute, normalized, no link (/data/playden/user on the console). Its gog folder is shared with the
  // session store (gog/session_store.h).
  explicit ProductCache(std::string user_root);

  // The saved product; nothing when there is none, or it can't be read or decoded (it is then fetched again and
  // replaced). Never creates a folder.
  std::optional<CachedProduct> Load(std::uint64_t id) const;
  // Writes the product durably, creating gog/ and products/ when needed.
  bool Save(std::uint64_t id, const CachedProduct& product) const;
  // Deletes one product's file. Missing is success.
  bool Remove(std::uint64_t id) const;
  // Deletes every regular file in products/, temporary files included. A link or a folder there is never followed or
  // removed, and makes the result false. A missing folder is success, and nothing is created.
  bool Clear() const;

 private:
  std::string user_root_;
};

}  // namespace playden::gog
