#include "update/release.h"

#include "steam/backend/json.h"
#include "update/zip_extract.h"

namespace playden::update {
namespace {

using Json = steam::json::Value;

const std::string* Text(const Json& object, std::string_view key) {
  const auto* value = object.Find(key);
  return value && value->is_string() ? &value->text() : nullptr;
}

bool SafePrefix(const std::string& prefix) {
  return !prefix.empty() && prefix.back() == '/' && SafeEntryName(prefix);
}

// A number of min_digits to max_digits digits at the front of text, taken off it.
bool TakeNumber(std::string_view* text, std::size_t min_digits, std::size_t max_digits, unsigned* out) {
  std::size_t digits = 0;
  unsigned value = 0;
  while (digits < text->size() && (*text)[digits] >= '0' && (*text)[digits] <= '9') {
    value = value * 10 + static_cast<unsigned>((*text)[digits] - '0');
    if (++digits > max_digits) return false;
  }
  if (digits < min_digits) return false;
  text->remove_prefix(digits);
  *out = value;
  return true;
}

bool TakeChar(std::string_view* text, char c) {
  if (text->empty() || text->front() != c) return false;
  text->remove_prefix(1);
  return true;
}

// The summary shown on the console: plain text with line breaks, no control characters (GitHub's
// "\r\n" would draw as "?"), at most one blank line in a row, and short enough for the dialog.
std::string Summary(std::string_view text) {
  constexpr std::size_t kMaxBytes = 600;
  std::string out;
  int newlines = 0;
  for (char c : text) {
    if (c == '\r') continue;
    if (c == '\n') {
      while (!out.empty() && out.back() == ' ') out.pop_back();
      if (!out.empty() && ++newlines <= 2) out += '\n';
      continue;
    }
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    if (c == ' ' && (out.empty() || out.back() == '\n')) continue;  // no indent
    newlines = 0;
    out += c;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
  if (out.size() > kMaxBytes) {
    std::size_t cut = kMaxBytes;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) --cut;  // a whole UTF-8 character
    if (const auto space = out.find_last_of(" \n", cut); space != std::string::npos && space > kMaxBytes / 2)
      cut = space;
    out.erase(cut);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    out += "\xE2\x80\xA6";
  }
  return out;
}

std::optional<Json> ParseObject(std::string_view json, std::string* error) {
  steam::json::Limits limits;
  limits.max_bytes = 2 << 20;
  limits.max_values = 1 << 16;
  auto value = steam::json::Parse(json, limits);
  if (!value || !value->is_object()) {
    if (error) *error = "not a JSON object";
    return std::nullopt;
  }
  return value;
}

}  // namespace

bool IsSha256(std::string_view text) {
  if (text.size() != 64) return false;
  for (char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool SafeFolderName(std::string_view name) {
  if (name.empty() || name.size() > 128 || name.front() == '.') return false;
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      return false;
  return true;
}

const ReleaseAsset* Release::Find(std::string_view name) const {
  for (const auto& asset : assets)
    if (asset.name == name) return &asset;
  return nullptr;
}

namespace {

std::optional<Release> ReleaseFrom(const Json* doc, std::string* error) {
  Release release;
  const auto* tag = Text(*doc, "tag_name");
  const auto version = tag ? ParseVersion(*tag) : std::nullopt;
  // Nightly builds are published elsewhere and are never offered as updates.
  if (!version || version->nightly()) {
    if (error) *error = "release tag is not a release version";
    return std::nullopt;
  }
  release.tag = *tag;
  if (const auto* page = Text(*doc, "html_url"); page && page->rfind("https://github.com/", 0) == 0 &&
                                                 page->size() < 512 && page->find_first_of(" \"<>") == std::string::npos)
    release.page = *page;
  release.version = *version;
  if (const auto* draft = doc->Find("draft"); draft && draft->is_bool()) release.draft = draft->boolean();
  if (const auto* pre = doc->Find("prerelease"); pre && pre->is_bool()) release.prerelease = pre->boolean();
  const auto* assets = doc->Find("assets");
  if (!assets || !assets->is_array()) {
    if (error) *error = "release has no assets";
    return std::nullopt;
  }
  for (const auto& item : assets->array()) {
    if (!item.is_object()) continue;
    const auto* name = Text(item, "name");
    const auto* url = Text(item, "browser_download_url");
    const auto* size = item.Find("size");
    if (!name || !url || !size || !size->AsUint64()) continue;
    ReleaseAsset asset{*name, *url, *size->AsUint64(), {}};
    if (const auto* digest = Text(item, "digest"); digest && digest->rfind("sha256:", 0) == 0 && IsSha256(digest->substr(7)))
      asset.sha256 = digest->substr(7);
    release.assets.push_back(std::move(asset));
  }
  return release;
}

}  // namespace

std::optional<Release> ParseRelease(std::string_view json, std::string* error) {
  const auto doc = ParseObject(json, error);
  if (!doc) return std::nullopt;
  return ReleaseFrom(&*doc, error);
}

std::optional<std::vector<Release>> ParseReleases(std::string_view json, std::string* error) {
  steam::json::Limits limits;
  limits.max_bytes = 4 << 20;
  limits.max_values = 1 << 18;
  const auto doc = steam::json::Parse(json, limits);
  if (!doc || !doc->is_array()) {
    if (error) *error = "not a JSON array";
    return std::nullopt;
  }
  std::vector<Release> releases;
  for (const auto& item : doc->array())
    if (item.is_object())
      if (auto release = ReleaseFrom(&item, nullptr)) releases.push_back(std::move(*release));  // a tag that isn't a version is skipped
  return releases;
}

std::optional<KitVersion> ParseKitVersion(std::string_view text) {
  KitVersion version;
  if (!TakeNumber(&text, 1, 6, &version.generation) || !TakeChar(&text, '.') ||
      !TakeNumber(&text, 1, 6, &version.revision))
    return std::nullopt;
  if (text.empty()) return version;
  constexpr std::string_view kDevelopment = "-dev.";
  if (text.substr(0, kDevelopment.size()) != kDevelopment) return std::nullopt;
  text.remove_prefix(kDevelopment.size());
  version.development = true;
  if (!TakeNumber(&text, 8, 8, &version.date) || !TakeChar(&text, '.') || !TakeNumber(&text, 1, 6, &version.build) ||
      !text.empty())
    return std::nullopt;
  return version;
}

std::optional<KitVersion> KitVersionFromArtifact(std::string_view artifact) {
  constexpr std::string_view kKit = "-kit-";
  const auto at = artifact.find(kKit);
  if (at == std::string_view::npos) return std::nullopt;
  return ParseKitVersion(artifact.substr(at + kKit.size()));
}

std::optional<UpdateManifest> ParseUpdateManifest(std::string_view json, std::string* error) {
  auto refuse = [&](const char* reason) -> std::optional<UpdateManifest> {
    if (error) *error = reason;
    return std::nullopt;
  };
  const auto doc = ParseObject(json, error);
  if (!doc) return std::nullopt;
  const auto* format = doc->Find("format");
  if (!format || format->AsUint64() != 1u) return refuse("unsupported update manifest format");
  const auto* app = doc->Find("app");
  const auto* kit = doc->Find("kit");
  if (!app || !app->is_object() || !kit || !kit->is_object()) return refuse("update manifest needs app and kit");
  UpdateManifest manifest;
  const auto* version = Text(*app, "version");
  const auto* min_from = Text(*app, "min_from");
  const auto* title = Text(*app, "title_id");
  const auto* app_asset = Text(*app, "asset");
  const auto* app_prefix = Text(*app, "prefix");
  if (!version || !ParseVersion(*version) || !min_from || !ParseVersion(*min_from) || !title || !app_asset ||
      !app_prefix)
    return refuse("update manifest app part is incomplete");
  manifest.app_version = *ParseVersion(*version);
  if (manifest.app_version.nightly()) return refuse("update manifest names a nightly build");
  manifest.min_from = *ParseVersion(*min_from);
  manifest.title_id = *title;
  manifest.app_asset = *app_asset;
  manifest.app_prefix = *app_prefix;
  if (!SafeFolderName(manifest.title_id) || !SafeFolderName(manifest.app_asset) || !SafePrefix(manifest.app_prefix))
    return refuse("update manifest app part is unsafe");
  const char* kit_fields[] = {"kit_id", "revision", "manifest_sha256", "asset", "prefix"};
  std::string* kit_values[] = {&manifest.kit.kit_id, &manifest.kit.revision, &manifest.kit.manifest_sha256,
                               &manifest.kit.asset, &manifest.kit.prefix};
  for (int i = 0; i < 5; ++i) {
    const auto* value = Text(*kit, kit_fields[i]);
    if (!value) return refuse("update manifest kit part is incomplete");
    *kit_values[i] = *value;
  }
  if (!SafeFolderName(manifest.kit.kit_id) || !SafeFolderName(manifest.kit.revision) ||
      !IsSha256(manifest.kit.manifest_sha256) ||
      !SafeFolderName(manifest.kit.asset) || !SafePrefix(manifest.kit.prefix))
    return refuse("update manifest kit part is unsafe");
  if (const auto* notes = Text(*doc, "notes")) manifest.notes = Summary(*notes);
  return manifest;
}

}  // namespace playden::update
