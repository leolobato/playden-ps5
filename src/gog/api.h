// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GOG's library calls: the owned product IDs (embed.gog.com/user/data/games, GOG_PROTOCOL.md §2) and one product's
// gamesdb entry (gamesdb.gog.com/platforms/gog/external_releases/<id>, §3); and the content system's calls an install
// makes (builds, secure links, the dependency store, §4 and §6). Synchronous: call them on a worker.
// Nothing here logs; callers log the endpoint's name and counts, never a URL, a token, an ID or a title.

#include "gog/art_url.h"
#include "gog/content.h"
#include "gog/http.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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

// The player's library tags (#536), read-only, from embed.gog.com's account library listing:
// GET /account/getFilteredProducts?mediaType=1&page=N, then the same with &hiddenFlag=1 for the hidden products (left
// out of the first). Each page has the account's tags ([{"id":"<digits>","name":...}]) and its products with their
// tag IDs ("tags":["<digits>"]).
struct LibraryTagsPage {
  bool ok = false;
  std::uint32_t total_pages = 0;
  std::vector<std::pair<std::string, std::string>> tags;                         // id, name
  std::vector<std::pair<std::uint64_t, std::vector<std::string>>> products;      // product, its tag IDs
};
inline constexpr std::uint32_t kMaxTagPages = 50;  // a view: 5000 products at 100 a page
inline constexpr std::size_t kMaxTagPageBytes = 8 * 1024 * 1024;
// One page. A tag without a canonical ID or a name, or a product without a canonical ID, is skipped; anything but an
// object with "tags" and "products" arrays is not ok.
LibraryTagsPage ParseLibraryTagsPage(std::string_view body);
struct LibraryTag {
  std::string id;  // digits
  std::string name;
  std::vector<std::uint64_t> products;  // hidden ones too, each once
};
struct TagsResult {
  enum class Kind { kOk, kRejected, kNetwork, kProtocol };
  Kind kind = Kind::kNetwork;
  std::vector<LibraryTag> tags;  // kOk: in GOG's order
  long status = 0;                      // the last HTTP status; 0 when there was no answer
  std::size_t pages = 0;                // pages read, for the log
};
// Every page of both views, with "Authorization: Bearer <access token>" (wiped, as the answers, before it returns).
// A view of more than kMaxTagPages pages is kProtocol: never a partial list.
TagsResult FetchLibraryTags(Transport& transport, std::string_view access_token, const std::atomic<bool>& cancel);

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

// The content system's calls. None sends a User-Agent other than kUserAgent, and only FetchSecureLink and
// FetchSecureLinkV1 send a token.
inline constexpr std::size_t kMaxContentAnswerBytes = 1024 * 1024;

// A plain answer: whether one came, its status and its body (at most kMaxContentAnswerBytes).
struct ContentAnswer {
  bool answered = false;  // false: no answer (network, timeout, cancel); status and body are empty
  bool canceled = false;
  long status = 0;
  std::string body;
};

// GET https://content-system.gog.com/products/<id>/os/windows/builds?generation=2&_version=2, without Authorization.
// ChooseBuild reads the body.
ContentAnswer FetchBuilds(Transport& transport, std::uint64_t product, const std::atomic<bool>& cancel);

// GET https://content-system.gog.com/dependencies/repository?generation=2, unsigned. ParseDependencyRepository reads
// the body.
ContentAnswer FetchDependencyRepository(Transport& transport, const std::atomic<bool>& cancel);

struct LinkResult {
  enum class Kind {
    kOk,
    kRejected,   // 401: the access token is no longer good
    kForbidden,  // 403: the account doesn't own the product, or GOG refused the link
    kNetwork,    // no answer, 429, 5xx, another 4xx: try again later
    kProtocol,   // 200 without usable endpoints for the generation, or another status
    kCanceled,
  };
  Kind kind = Kind::kNetwork;
  long status = 0;
  // kOk: the endpoints for the generation, in the order to try them (OrderEndpoints). A secure link's parameters hold
  // its signature: never log them, and wipe them with WipeEndpoints when done.
  std::vector<CdnEndpoint> endpoints;
  // The earliest `expires_at` (Unix seconds) among the endpoints that state one (fastly); 0 when none does (gcore's
  // link has no expiry).
  std::int64_t expires_at = 0;

  LinkResult() = default;
  LinkResult(const LinkResult&) = default;
  LinkResult& operator=(const LinkResult&) = default;
  LinkResult(LinkResult&&) = default;
  LinkResult& operator=(LinkResult&&) = default;
  ~LinkResult() { WipeEndpoints(endpoints); }

  static void WipeEndpoints(std::vector<CdnEndpoint>& endpoints);
};

// Reads a secure link or open link answer ({"urls":[...]}): kOk with the endpoints that support `generation`, else
// kProtocol. For generation 1 that drops gcore, which lists only 2. The endpoints dropped are wiped too.
LinkResult ParseLink(std::string_view body, int generation = 2);

// GET https://content-system.gog.com/products/<pid>/secure_link?_version=2&generation=2&path=/ with
// "Authorization: Bearer <access token>". The request's header, the answer's body and the parsed copy are wiped before
// it returns: they hold the token and the link's signature.
LinkResult FetchSecureLink(Transport& transport, std::string_view access_token, std::uint64_t product,
                           const std::atomic<bool>& cancel);

// A gen 1 build's link: GET https://content-system.gog.com/products/<pid>/secure_link?_version=2&type=depot&path=
// /windows/<timestamp>/ (the build's legacy_build_id, BuildChoice::timestamp) with the same Bearer header. Its `path`
// names the build's folder; main.bin is that path plus "/main.bin" (FetchRange). The endpoints are those for
// generation 1 (fastly). 401 is kRejected, 403 kForbidden, the rest as FetchSecureLink, and the same buffers are wiped.
LinkResult FetchSecureLinkV1(Transport& transport, std::string_view access_token, std::uint64_t product,
                             std::uint64_t timestamp, const std::atomic<bool>& cancel);

// GET https://content-system.gog.com/open_link?generation=2&_version=2&path=/dependencies/store/, unsigned: the public
// dependency store's chunk bases. Their `url` is used as it is (FillUrl), not url_format.
LinkResult FetchOpenLink(Transport& transport, const std::atomic<bool>& cancel);

}  // namespace playden::gog
