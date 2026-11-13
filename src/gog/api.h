// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GOG's library calls: the owned product IDs (embed.gog.com/user/data/games, GOG_PROTOCOL.md §2) and one product's
// gamesdb entry (gamesdb.gog.com/platforms/gog/external_releases/<id>, §3). Synchronous: call them on a worker.
// Nothing here logs; callers log the endpoint's name and counts, never a URL, a token, an ID or a title.

#include "gog/art_url.h"
#include "gog/http.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playden::gog {

inline constexpr std::uint32_t kApiTimeoutMs = 20'000;
inline constexpr std::uint32_t kApiConnectTimeoutMs = 10'000;

// The owned list holds every product of the account, DLC included: thousands of IDs for a large account, far more than
// steam::json's default 4096 values.
inline constexpr std::size_t kMaxOwnedBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxOwnedValues = 500'000;
inline constexpr std::size_t kMaxOwnedDepth = 4;

// One gamesdb entry: screenshots, videos, releases on other stores and localized maps, about 10-20 KiB in practice.
inline constexpr std::size_t kMaxProductBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kMaxProductValues = 65'536;
inline constexpr std::size_t kMaxProductDepth = 16;

// The library cache's limits (library/cache.cpp): a field past them would make the whole saved library unwritable.
inline constexpr std::size_t kMaxTitleBytes = 4096;
inline constexpr std::size_t kMaxSummaryBytes = 64 * 1024;
inline constexpr std::size_t kMaxDeveloperBytes = 4096;
// An ETag is sent back as a header: at most this many printable ASCII bytes, else it is not kept.
inline constexpr std::size_t kMaxEtagBytes = 256;

struct OwnedResult {
  enum class Kind {
    kOk,
    kRejected,  // 401: the access token is no longer good
    kNetwork,   // try again later: no answer, 429, 5xx, another 4xx
    kProtocol,  // 200 that isn't {"owned":[...]}, or another status
  };
  Kind kind = Kind::kNetwork;
  std::vector<std::uint64_t> ids;  // kOk: owned order, duplicates removed
  long status = 0;                 // the HTTP status; 0 when there was no answer
};

// {"owned":[<ids>]}. An entry that isn't a canonical positive integer that fits 64 bits is skipped; a repeated ID is
// kept once, where it first appears. Anything but an object with an "owned" array is kProtocol.
OwnedResult ParseOwned(std::string_view body);

// GET https://embed.gog.com/user/data/games with "Authorization: Bearer <access token>". The request's header and the
// response body are wiped before it returns.
OwnedResult FetchOwned(Transport& transport, std::string_view access_token, const std::atomic<bool>& cancel);

struct ProductResult {
  enum class Kind {
    kModified,     // 200: body and etag (empty when GOG sent none, or one Playden won't send back)
    kNotModified,  // 304 to If-None-Match: the cached entry is still current
    kMissing,      // 404: gamesdb has no entry for the product
    kFailed,       // no answer, another status: keep what was cached and try again later
  };
  Kind kind = Kind::kFailed;
  std::string body;
  std::string etag;
  long status = 0;
};

// Whether an ETag can be stored and sent back: 1-256 bytes of printable ASCII (no CR or LF), as received (gamesdb's
// are weak, W/"...").
bool UsableEtag(std::string_view etag);

// GET https://gamesdb.gog.com/platforms/gog/external_releases/<id>, with If-None-Match when etag is usable. No
// Authorization: gamesdb is public.
ProductResult FetchProduct(Transport& transport, std::uint64_t id, std::string_view etag,
                           const std::atomic<bool>& cancel);

struct ProductInfo {
  enum class Kind {
    kGame,     // shown in the Library
    kHidden,   // a real entry Playden doesn't list: DLC, spam, not visible in the library, or no Windows build
    kInvalid,  // malformed, or the entry of another product (gamesdb answers nonsense IDs too)
  };
  Kind kind = Kind::kInvalid;
  std::string title;      // kGame: not blank, at most kMaxTitleBytes
  std::string summary;    // at most kMaxSummaryBytes (cut at a character boundary)
  std::string developer;  // the first developer, or empty
  std::string cover;      // the cover-shaped ArtUrl of the vertical cover, else of the cover; empty when neither fits
  std::string hero;       // the hero-shaped ArtUrl of the background, else of the horizontal artwork
  std::vector<std::uint64_t> dlc_ids;  // dlcs[].external_id, for installs later; not shown
};

// Reads one gamesdb entry for product id. Localized maps take "en-US", then "*". Ownership never comes from here.
ProductInfo ParseProduct(std::string_view body, std::uint64_t id);

// gamesdb's url_format, exactly https://images.gog.com/<64 lowercase hex>{formatter}.{ext}?namespace=gamesdb, as the
// address Playden fetches (gog/art_url.h): https://images.gog.com/<hex>.jpg?namespace=gamesdb for a cover, with
// _bg_crop_1920x655 before .jpg for a hero. Anything else gives "".
std::string ArtUrl(std::string_view url_format, ArtKind kind = ArtKind::kCover);

}  // namespace playden::gog
