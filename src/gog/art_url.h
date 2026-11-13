// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The two shapes of GOG art address Playden asks for, made from gamesdb's url_format with "{ext}" set to jpg:
//
// - a cover, "{formatter}" removed: https://images.gog.com/<64 lowercase hex>.jpg?namespace=gamesdb. GOG's covers are
//   small (342x482 seen);
// - a hero, "{formatter}" set to _bg_crop_1920x655: https://images.gog.com/<hex>_bg_crop_1920x655.jpg?namespace=gamesdb.
//   The original backgrounds are 2560x670, more than the artwork decoder takes (kMaxArtworkDimension, 1920), and the
//   game page's hero is 1920 wide.
//
// gog::ArtUrl makes them, the library keeps them, and the artwork backend fetches nothing else for a GOG game.
// Header-only and without std::regex (the console has no locale), so the UI and the artwork cache can check them
// without linking the GOG client.

#include <cstddef>
#include <string_view>

namespace playden::gog {

enum class ArtKind { kCover, kHero };

inline constexpr std::string_view kArtUrlPrefix = "https://images.gog.com/";
inline constexpr std::string_view kArtUrlSuffix = ".jpg?namespace=gamesdb";
inline constexpr std::string_view kArtHeroFormatter = "_bg_crop_1920x655";
inline constexpr std::size_t kArtHashDigits = 64;

inline bool IsArtHash(std::string_view hash) {
  if (hash.size() != kArtHashDigits) return false;
  for (const char c : hash)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// Exactly ^https://images\.gog\.com/[0-9a-f]{64}(_bg_crop_1920x655)?\.jpg\?namespace=gamesdb$. hash and kind, when
// given, receive the 64 digits and which of the two shapes it is.
inline bool IsArtUrl(std::string_view url, std::string_view* hash = nullptr, ArtKind* kind = nullptr) {
  const std::size_t cover_size = kArtUrlPrefix.size() + kArtHashDigits + kArtUrlSuffix.size();
  const bool hero = url.size() == cover_size + kArtHeroFormatter.size();
  if (url.size() != cover_size && !hero) return false;
  if (url.substr(0, kArtUrlPrefix.size()) != kArtUrlPrefix) return false;
  const auto digits = url.substr(kArtUrlPrefix.size(), kArtHashDigits);
  if (!IsArtHash(digits)) return false;
  auto rest = url.substr(kArtUrlPrefix.size() + kArtHashDigits);
  if (hero) {
    if (rest.substr(0, kArtHeroFormatter.size()) != kArtHeroFormatter) return false;
    rest = rest.substr(kArtHeroFormatter.size());
  }
  if (rest != kArtUrlSuffix) return false;
  if (hash) *hash = digits;
  if (kind) *kind = hero ? ArtKind::kHero : ArtKind::kCover;
  return true;
}

// Whether url is exactly the given shape.
inline bool IsArtUrl(std::string_view url, ArtKind expected) {
  ArtKind kind = ArtKind::kCover;
  return IsArtUrl(url, nullptr, &kind) && kind == expected;
}

}  // namespace playden::gog
