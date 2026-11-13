// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/api.h"

#include "gog/art_url.h"
#include "gog/client_config.h"
#include "platform/secret.h"
#include "steam/backend/json.h"

#include <unordered_set>

namespace playden::gog {
namespace {

using steam::json::Value;

std::string_view BodyOf(const steam::HttpResponse& response) {
  return {reinterpret_cast<const char*>(response.body.data()), response.body.size()};
}

// A canonical positive decimal that fits 64 bits: no sign, no leading zero, no fraction or exponent.
bool CanonicalId(std::string_view text, std::uint64_t* id) {
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
