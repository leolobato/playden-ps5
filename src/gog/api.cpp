// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/api.h"

#include "gog/art_url.h"
#include "gog/client_config.h"
#include "gog/product_id.h"
#include "platform/secret.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <unordered_set>

namespace playden::gog {
namespace {

using steam::json::Value;

std::string_view BodyOf(const steam::HttpResponse& response) {
  return {reinterpret_cast<const char*>(response.body.data()), response.body.size()};
}

// A JSON number or a string holding a canonical ID (gamesdb sends IDs as strings, embed as numbers).
bool IdOf(const Value* value, std::uint64_t* id) {
  if (!value || !(value->is_number() || value->is_string())) return false;
  return CanonicalId(value->text(), id);
}

const Value* Object(const Value* parent, std::string_view key) {
  const Value* value = parent ? parent->Find(key) : nullptr;
  return value && value->is_object() ? value : nullptr;
}

const Value* Array(const Value* parent, std::string_view key) {
  const Value* value = parent ? parent->Find(key) : nullptr;
  return value && value->is_array() ? value : nullptr;
}

const std::string* String(const Value* parent, std::string_view key) {
  const Value* value = parent ? parent->Find(key) : nullptr;
  return value && value->is_string() ? &value->text() : nullptr;
}

// A localized map {"en-US": ..., "*": ...}: English first, then the default. A plain string is taken as it is.
const std::string* Localized(const Value* parent, std::string_view key) {
  const Value* value = parent ? parent->Find(key) : nullptr;
  if (!value) return nullptr;
  if (value->is_string()) return &value->text();
  if (!value->is_object()) return nullptr;
  for (const char* language : {"en-US", "*"})
    if (const auto* text = String(value, language)) return text;
  return nullptr;
}

bool Blank(const std::string& text) {
  for (const char c : text)
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
  return true;
}

bool UsableTitle(const std::string* title) {
  return title && !Blank(*title) && title->size() <= kMaxTitleBytes;
}

// At most max bytes, never ending inside a UTF-8 sequence.
std::string Cut(const std::string& text, std::size_t max) {
  if (text.size() <= max) return text;
  std::size_t end = max;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return text.substr(0, end);
}

std::string Art(const Value* game, std::string_view key, ArtKind kind) {
  const auto* format = String(Object(game, key), "url_format");
  return format ? ArtUrl(*format, kind) : std::string();
}

bool Windows(const Value* entry) {
  const Value* systems = Array(entry, "supported_operating_systems");
  if (!systems) return false;
  for (const auto& system : systems->array()) {
    if (system.is_string() && system.text() == "windows") return true;
    if (const auto* slug = system.is_object() ? String(&system, "slug") : nullptr; slug && *slug == "windows")
      return true;
  }
  return false;
}

ProductInfo Info(ProductInfo::Kind kind) {
  ProductInfo info;
  info.kind = kind;
  return info;
}

steam::HttpRequest Get(std::string url, std::size_t max_bytes) {
  steam::HttpRequest request;
  request.method = steam::HttpMethod::kGet;
  request.url = std::move(url);
  request.timeout_ms = kApiTimeoutMs;
  request.connect_timeout_ms = kApiConnectTimeoutMs;
  request.max_response_bytes = max_bytes;
  return request;
}

bool AsciiEqualFold(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// Overwrites every string of a parsed answer (keys too) before the tree is freed: a secure link's parameters are its
// signature. The tree is the caller's own, not const; its depth is capped by the parse limits.
void WipeTree(const Value& value) {
  platform::ZeroizeSecret(const_cast<std::string&>(value.text()));
  for (const auto& entry : value.array()) WipeTree(entry);
  for (const auto& [key, member] : value.object()) {
    platform::ZeroizeSecret(const_cast<std::string&>(key));
    WipeTree(member);
  }
}

std::string ContentUrl(std::string_view path) {
  std::string url = "https://";
  url += kContentSystemHost;
  url += path;
  return url;
}

ContentAnswer FetchContent(Transport& transport, std::string url, const std::atomic<bool>& cancel) {
  auto request = Get(std::move(url), kMaxContentAnswerBytes);
  request.headers.emplace_back("User-Agent", kUserAgent);
  ContentAnswer answer;
  if (!transport) return answer;
  auto response = transport(request, cancel);
  if (!response.ok()) {
    answer.canceled = response.error == steam::HttpError::kCanceled || cancel.load();
    return answer;
  }
  answer.answered = true;
  answer.status = response.status_code;
  answer.body.assign(BodyOf(response));
  return answer;
}

LinkResult::Kind LinkStatusKind(long status) {
  using Kind = LinkResult::Kind;
  if (status == 401) return Kind::kRejected;
  if (status == 403) return Kind::kForbidden;
  if (status == 429 || (status >= 400 && status <= 599)) return Kind::kNetwork;
  return Kind::kProtocol;
}

// A secure link: GET <content system><path> with "Authorization: Bearer <access token>", read for `generation`. The
// request's header, the answer's body and the parsed copy are wiped before it returns: they hold the token and the
// link's signature.
LinkResult FetchLink(Transport& transport, std::string_view access_token, const std::string& path, int generation,
                     const std::atomic<bool>& cancel) {
  LinkResult result;
  if (!transport) return result;
  auto request = Get(ContentUrl(path), kMaxContentAnswerBytes);
  std::string bearer;
  bearer.reserve(7 + access_token.size());
  bearer += "Bearer ";
  bearer += access_token;
  request.headers.reserve(2);
  request.headers.emplace_back("User-Agent", kUserAgent);
  request.headers.emplace_back("Authorization", std::string());
  request.headers.back().second.swap(bearer);
  auto response = transport(request, cancel);
  platform::ZeroizeSecret(request.headers.back().second);
  if (!response.ok()) {
    result.kind = response.error == steam::HttpError::kCanceled || cancel.load() ? LinkResult::Kind::kCanceled
                                                                                 : LinkResult::Kind::kNetwork;
  } else if (response.status_code == 200) {
    result = ParseLink(BodyOf(response), generation);
  } else {
    result.kind = LinkStatusKind(response.status_code);
  }
  result.status = response.ok() ? response.status_code : 0;
  platform::ZeroizeSecret(response.body);
  return result;
}

}  // namespace

OwnedResult ParseOwned(std::string_view body) {
  OwnedResult result;
  result.kind = OwnedResult::Kind::kProtocol;
  steam::json::Limits limits;
  limits.max_bytes = kMaxOwnedBytes;
  limits.max_values = kMaxOwnedValues;
  limits.max_depth = kMaxOwnedDepth;
  limits.max_string_bytes = 64 * 1024;
  const auto parsed = steam::json::Parse(body, limits);
  const Value* owned = parsed && parsed->is_object() ? Array(&*parsed, "owned") : nullptr;
  if (!owned) return result;
  std::unordered_set<std::uint64_t> seen;
  seen.reserve(owned->array().size());
  result.ids.reserve(owned->array().size());
  for (const auto& entry : owned->array()) {
    std::uint64_t id = 0;
    if (!entry.is_number() || !CanonicalId(entry.text(), &id)) continue;
    if (seen.insert(id).second) result.ids.push_back(id);
  }
  result.kind = OwnedResult::Kind::kOk;
  return result;
}

LibraryTagsPage ParseLibraryTagsPage(std::string_view body) {
  LibraryTagsPage page;
  steam::json::Limits limits;
  limits.max_bytes = kMaxTagPageBytes;
  limits.max_values = kMaxOwnedValues;
  limits.max_depth = 8;
  limits.max_string_bytes = 64 * 1024;
  const auto parsed = steam::json::Parse(body, limits);
  const Value* tags = parsed && parsed->is_object() ? Array(&*parsed, "tags") : nullptr;
  const Value* products = parsed && parsed->is_object() ? Array(&*parsed, "products") : nullptr;
  if (!tags || !products) return page;
  std::uint64_t number = 0;
  for (const auto& tag : tags->array()) {
    const std::string* id = tag.is_object() ? String(&tag, "id") : nullptr;
    const std::string* name = tag.is_object() ? String(&tag, "name") : nullptr;
    if (!id || !CanonicalId(*id, &number) || !name || name->empty() || name->size() > 1024) continue;
    page.tags.emplace_back(*id, *name);
  }
  for (const auto& product : products->array()) {
    std::uint64_t id = 0;
    const Value* value = product.is_object() ? product.Find("id") : nullptr;
    if (!value || !value->is_number() || !CanonicalId(value->text(), &id)) continue;
    std::vector<std::string> ids;
    if (const Value* list = Array(&product, "tags"))
      for (const auto& tag : list->array())
        if (tag.is_string() && CanonicalId(tag.text(), &number)) ids.push_back(tag.text());
    page.products.emplace_back(id, std::move(ids));
  }
  if (const Value* total = parsed->Find("totalPages"); total && total->is_number()) {
    std::uint64_t pages = 0;
    if (CanonicalId(total->text(), &pages)) page.total_pages = static_cast<std::uint32_t>(std::min<std::uint64_t>(pages, UINT32_MAX));
  }
  page.ok = true;
  return page;
}

TagsResult FetchLibraryTags(Transport& transport, std::string_view access_token, const std::atomic<bool>& cancel) {
  TagsResult result;
  if (!transport) return result;
  std::vector<std::pair<std::string, std::string>> names;  // GOG's order, from the first page
  std::vector<std::pair<std::uint64_t, std::vector<std::string>>> products;
  for (const bool hidden : {false, true}) {
    std::uint32_t total = 1;
    for (std::uint32_t number = 1; number <= total; ++number) {
      std::string url = "https://";
      url += kEmbedHost;
      url += "/account/getFilteredProducts?mediaType=1&page=" + std::to_string(number);
      if (hidden) url += "&hiddenFlag=1";
      auto request = Get(std::move(url), kMaxTagPageBytes);
      std::string bearer;
      bearer.reserve(7 + access_token.size());
      bearer += "Bearer ";
      bearer += access_token;
      request.headers.reserve(2);
      request.headers.emplace_back("User-Agent", kUserAgent);
      request.headers.emplace_back("Authorization", std::string());
      request.headers.back().second.swap(bearer);
      auto response = transport(request, cancel);
      platform::ZeroizeSecret(request.headers.back().second);
      result.status = response.ok() ? response.status_code : 0;
      if (!response.ok()) {
        result.kind = TagsResult::Kind::kNetwork;
        return result;
      }
      if (response.status_code != 200) {
        platform::ZeroizeSecret(response.body);
        result.kind = response.status_code == 401 ? TagsResult::Kind::kRejected
                      : response.status_code == 429 || response.status_code >= 400 ? TagsResult::Kind::kNetwork
                                                                                    : TagsResult::Kind::kProtocol;
        return result;
      }
      auto page = ParseLibraryTagsPage(BodyOf(response));
      platform::ZeroizeSecret(response.body);
      if (!page.ok) {
        result.kind = TagsResult::Kind::kProtocol;
        return result;
      }
      ++result.pages;
      if (names.empty()) names = std::move(page.tags);
      for (auto& product : page.products) products.push_back(std::move(product));
      // More pages than Playden reads: an incomplete list would drop tagged games, so the read fails (saved tags stay).
      if (page.total_pages > kMaxTagPages) {
        result.kind = TagsResult::Kind::kProtocol;
        return result;
      }
      total = std::max<std::uint32_t>(page.total_pages, 1);
    }
  }
  for (auto& [id, name] : names) {
    if (std::any_of(result.tags.begin(), result.tags.end(), [&](const LibraryTag& tag) { return tag.id == id; }))
      continue;
    LibraryTag tag;
    tag.id = id;
    tag.name = std::move(name);
    std::unordered_set<std::uint64_t> seen;
    for (const auto& [product, ids] : products)
      if (std::find(ids.begin(), ids.end(), tag.id) != ids.end() && seen.insert(product).second)
        tag.products.push_back(product);
    result.tags.push_back(std::move(tag));
  }
  result.kind = TagsResult::Kind::kOk;
  return result;
}

OwnedResult FetchOwned(Transport& transport, std::string_view access_token, const std::atomic<bool>& cancel) {
  std::string url = "https://";
  url += kEmbedHost;
  url += "/user/data/games";
  auto request = Get(std::move(url), kMaxOwnedBytes);
  // Reserved up front: a reallocation would leave a copy of the token behind in freed memory.
  std::string bearer;
  bearer.reserve(7 + access_token.size());
  bearer += "Bearer ";
  bearer += access_token;
  request.headers.reserve(2);
  request.headers.emplace_back("User-Agent", kUserAgent);
  request.headers.emplace_back("Authorization", std::string());
  request.headers.back().second.swap(bearer);
  auto response = transport ? transport(request, cancel) : steam::HttpResponse{steam::HttpError::kInvalidRequest};
  platform::ZeroizeSecret(request.headers.back().second);
  OwnedResult result;
  if (!response.ok()) {
    result.kind = OwnedResult::Kind::kNetwork;
  } else if (response.status_code == 200) {
    result = ParseOwned(BodyOf(response));
  } else if (response.status_code == 401) {
    result.kind = OwnedResult::Kind::kRejected;
  } else if (response.status_code == 429 || (response.status_code >= 400 && response.status_code <= 599)) {
    result.kind = OwnedResult::Kind::kNetwork;
  } else {
    result.kind = OwnedResult::Kind::kProtocol;
  }
  result.status = response.ok() ? response.status_code : 0;
  // The list names what the account owns: no copy stays behind.
  platform::ZeroizeSecret(response.body);
  return result;
}

bool UsableEtag(std::string_view etag) {
  if (etag.empty() || etag.size() > kMaxEtagBytes) return false;
  for (const char c : etag)
    if (c < 0x20 || c > 0x7E) return false;
  return true;
}

ProductResult FetchProduct(Transport& transport, std::uint64_t id, std::string_view etag,
                           const std::atomic<bool>& cancel) {
  std::string url = "https://";
  url += kGamesDbHost;
  url += "/platforms/gog/external_releases/";
  url += std::to_string(id);
  auto request = Get(std::move(url), kMaxProductBytes);
  request.headers.emplace_back("User-Agent", kUserAgent);
  if (UsableEtag(etag)) request.headers.emplace_back("If-None-Match", std::string(etag));
  ProductResult result;
  if (id == 0 || !transport) return result;
  auto response = transport(request, cancel);
  if (!response.ok()) return result;
  result.status = response.status_code;
  switch (response.status_code) {
    case 200:
      result.kind = ProductResult::Kind::kModified;
      result.body.assign(BodyOf(response));
      for (const auto& header : response.headers)
        if (AsciiEqualFold(header.first, "ETag")) {
          if (UsableEtag(header.second)) result.etag = header.second;
          break;
        }
      break;
    case 304: result.kind = ProductResult::Kind::kNotModified; break;
    case 404: result.kind = ProductResult::Kind::kMissing; break;
    default: break;
  }
  return result;
}

ProductInfo ParseProduct(std::string_view body, std::uint64_t id) {
  using Kind = ProductInfo::Kind;
  steam::json::Limits limits;
  limits.max_bytes = kMaxProductBytes;
  limits.max_values = kMaxProductValues;
  limits.max_depth = kMaxProductDepth;
  limits.max_string_bytes = kMaxProductBytes;
  const auto parsed = steam::json::Parse(body, limits);
  if (!parsed || !parsed->is_object() || id == 0) return Info(Kind::kInvalid);
  const Value* entry = &*parsed;
  std::uint64_t external = 0;
  if (!IdOf(entry->Find("external_id"), &external) || external != id) return Info(Kind::kInvalid);
  const std::string* type = String(entry, "type");
  const Value* game = Object(entry, "game");
  if (!type || !game) return Info(Kind::kInvalid);

  ProductInfo info;
  if (const Value* dlcs = Array(entry, "dlcs"))
    for (const auto& dlc : dlcs->array()) {
      std::uint64_t dlc_id = 0;
      if (dlc.is_object() && IdOf(dlc.Find("external_id"), &dlc_id)) info.dlc_ids.push_back(dlc_id);
    }
  const Value* visible = game->Find("visible_in_library");
  if ((*type != "game" && *type != "mod") || !visible || !visible->is_bool() || !visible->boolean() ||
      !Windows(entry)) {
    info.kind = Kind::kHidden;
    return info;
  }

  const std::string* title = Localized(entry, "title");
  if (!UsableTitle(title)) title = Localized(game, "title");
  if (!UsableTitle(title)) return Info(Kind::kInvalid);
  info.title = *title;
  const std::string* summary = Localized(entry, "summary");
  if (!summary || Blank(*summary)) summary = Localized(game, "summary");
  if (summary) info.summary = Cut(*summary, kMaxSummaryBytes);
  if (const Value* developers = Array(game, "developers"); developers && !developers->array().empty()) {
    const Value& first = developers->array().front();
    if (const auto* name = first.is_object() ? String(&first, "name") : nullptr; name && name->size() <= kMaxDeveloperBytes)
      info.developer = *name;
  }
  info.cover = Art(game, "vertical_cover", ArtKind::kCover);
  if (info.cover.empty()) info.cover = Art(game, "cover", ArtKind::kCover);
  info.hero = Art(game, "background", ArtKind::kHero);
  if (info.hero.empty()) info.hero = Art(game, "horizontal_artwork", ArtKind::kHero);
  info.kind = Kind::kGame;
  return info;
}

ContentAnswer FetchBuilds(Transport& transport, std::uint64_t product, const std::atomic<bool>& cancel) {
  if (product == 0) return {};
  return FetchContent(transport,
                      ContentUrl("/products/" + std::to_string(product) + "/os/windows/builds?generation=2&_version=2"),
                      cancel);
}

ContentAnswer FetchDependencyRepository(Transport& transport, const std::atomic<bool>& cancel) {
  return FetchContent(transport, ContentUrl("/dependencies/repository?generation=2"), cancel);
}

void LinkResult::WipeEndpoints(std::vector<CdnEndpoint>& endpoints) {
  for (auto& endpoint : endpoints) {
    platform::ZeroizeSecret(endpoint.url_format);
    platform::ZeroizeSecret(endpoint.url);
    for (auto& [key, value] : endpoint.parameters) platform::ZeroizeSecret(value);
    endpoint.parameters.clear();
  }
  endpoints.clear();
}

LinkResult ParseLink(std::string_view body, int generation) {
  LinkResult result;
  result.kind = LinkResult::Kind::kProtocol;
  steam::json::Limits limits;
  limits.max_bytes = kMaxContentAnswerBytes;
  limits.max_values = 4096;
  limits.max_depth = 8;
  auto parsed = steam::json::Parse(body, limits);
  if (!parsed) return result;
  const Value* urls = parsed->is_object() ? parsed->Find("urls") : nullptr;
  if (urls && urls->is_array()) {
    auto parsed_endpoints = ParseEndpoints(*urls);
    // OrderEndpoints erases the endpoints it drops from its own copy without wiping them: wipe those here first.
    for (auto& endpoint : parsed_endpoints) {
      if (std::find(endpoint.generations.begin(), endpoint.generations.end(), generation) != endpoint.generations.end())
        continue;
      std::vector<CdnEndpoint> dropped(1);
      dropped[0].url_format.swap(endpoint.url_format);
      dropped[0].url.swap(endpoint.url);
      dropped[0].parameters.swap(endpoint.parameters);
      LinkResult::WipeEndpoints(dropped);
    }
    result.endpoints = OrderEndpoints(parsed_endpoints, generation);
    LinkResult::WipeEndpoints(parsed_endpoints);
  }
  WipeTree(*parsed);
  if (result.endpoints.empty()) return result;
  for (const auto& endpoint : result.endpoints) {
    const auto found = endpoint.parameters.find("expires_at");
    if (found == endpoint.parameters.end()) continue;
    const std::string& text = found->second;
    if (text.empty() || text.size() > 18 || text.find_first_not_of("0123456789") != std::string::npos) continue;
    std::int64_t value = 0;
    for (const char c : text) value = value * 10 + (c - '0');
    if (result.expires_at == 0 || value < result.expires_at) result.expires_at = value;
  }
  result.kind = LinkResult::Kind::kOk;
  return result;
}

LinkResult FetchSecureLink(Transport& transport, std::string_view access_token, std::uint64_t product,
                           const std::atomic<bool>& cancel) {
  if (product == 0) return {};
  return FetchLink(transport, access_token,
                   "/products/" + std::to_string(product) + "/secure_link?_version=2&generation=2&path=/", 2, cancel);
}

LinkResult FetchSecureLinkV1(Transport& transport, std::string_view access_token, std::uint64_t product,
                             std::uint64_t timestamp, const std::atomic<bool>& cancel) {
  if (product == 0 || timestamp == 0) return {};
  return FetchLink(transport, access_token,
                   "/products/" + std::to_string(product) + "/secure_link?_version=2&type=depot&path=/windows/" +
                       std::to_string(timestamp) + "/",
                   1, cancel);
}

LinkResult FetchOpenLink(Transport& transport, const std::atomic<bool>& cancel) {
  LinkResult result;
  auto answer = FetchContent(transport, ContentUrl("/open_link?generation=2&_version=2&path=/dependencies/store/"), cancel);
  if (!answer.answered) {
    result.kind = answer.canceled ? LinkResult::Kind::kCanceled : LinkResult::Kind::kNetwork;
  } else if (answer.status == 200) {
    result = ParseLink(answer.body);
  } else {
    result.kind = LinkStatusKind(answer.status);
  }
  result.status = answer.status;
  return result;
}

std::string ArtUrl(std::string_view url_format, ArtKind kind) {
  static constexpr std::string_view kFormatSuffix = "{formatter}.{ext}?namespace=gamesdb";
  if (url_format.size() != kArtUrlPrefix.size() + kArtHashDigits + kFormatSuffix.size()) return {};
  if (url_format.substr(0, kArtUrlPrefix.size()) != kArtUrlPrefix) return {};
  if (url_format.substr(kArtUrlPrefix.size() + kArtHashDigits) != kFormatSuffix) return {};
  const auto hash = url_format.substr(kArtUrlPrefix.size(), kArtHashDigits);
  if (!IsArtHash(hash)) return {};
  std::string url;
  url.reserve(kArtUrlPrefix.size() + kArtHashDigits + kArtHeroFormatter.size() + kArtUrlSuffix.size());
  url += kArtUrlPrefix;
  url += hash;
  if (kind == ArtKind::kHero) url += kArtHeroFormatter;
  url += kArtUrlSuffix;
  return url;
}

}  // namespace playden::gog
