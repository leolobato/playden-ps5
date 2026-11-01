#include "steam/store_browse_service.h"

#include "platform/storage.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace playden::steam {
namespace {

constexpr const char* kAssetBase = "https://shared.akamai.steamstatic.com/store_item_assets/";
constexpr std::size_t kMaximumTags = 20;
constexpr std::size_t kMaximumShots = 4;
constexpr std::size_t kMaximumTagBytes = 64;
constexpr std::size_t kMaximumUrlBytes = 512;
constexpr std::size_t kMaximumDescriptionBytes = 4096;
constexpr std::size_t kMaximumGenres = 8;
constexpr std::size_t kMaximumGenreBytes = 64;
constexpr std::size_t kMaxBrowseBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxCachedApps = 20000;
constexpr const char* kBrowsePath = "store-browse.cache";
const Bytes kBrowseMagic = {'P', 'D', 'N', 'S', 'T', 'B', 0, 3};

bool ValidTag(std::string_view tag) {
  return !tag.empty() && tag.size() <= kMaximumTagBytes && tag.find('\0') == std::string_view::npos;
}

bool ValidUrl(std::string_view url) {
  return url.size() <= kMaximumUrlBytes && url.rfind("https://", 0) == 0 && url.find('\0') == std::string_view::npos;
}

bool ValidGenre(std::string_view genre) {
  return !genre.empty() && genre.size() <= kMaximumGenreBytes && genre.find('\0') == std::string_view::npos;
}

bool ValidController(std::string_view support) {
  return support.empty() || support == "full" || support == "partial";
}

std::string TrimmedDescription(std::string text) {
  if (text.find('\0') != std::string::npos) return {};
  if (text.size() > kMaximumDescriptionBytes) text.resize(kMaximumDescriptionBytes);
  return text;
}

std::string EncodeQuery(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '_' || c == '.' || c == '~';
    if (plain) out.push_back(static_cast<char>(c));
    else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

// `format` is Steam's asset path with ${FILENAME}. A filename that already starts at steam/ is used whole.
std::string ResolveAsset(std::string_view format, std::string_view filename) {
  if (filename.empty() || filename.find('\0') != std::string_view::npos) return {};
  std::string path;
  if (filename.rfind("steam/", 0) == 0) path = std::string(filename);
  else {
    constexpr std::string_view kSlot = "${FILENAME}";
    const auto slot = format.find(kSlot);
    if (slot == std::string_view::npos) return {};
    path.reserve(format.size() + filename.size());
    path.append(format.substr(0, slot));
    path.append(filename);
    path.append(format.substr(slot + kSlot.size()));
  }
  if (path.rfind("https://", 0) == 0) return ValidUrl(path) ? path : std::string();
  path.insert(0, kAssetBase);
  return ValidUrl(path) ? path : std::string();
}

const json::Value* ObjectField(const json::Value& value, std::string_view key) {
  const auto* field = value.Find(key);
  return field && field->is_object() ? field : nullptr;
}

const json::Value* ArrayField(const json::Value& value, std::string_view key) {
  const auto* field = value.Find(key);
  return field && field->is_array() ? field : nullptr;
}

std::string TrailerFile(const json::Value& trailer) {
  const auto* clips = ArrayField(trailer, "microtrailer");
  if (!clips) return {};
  std::string webm;
  for (const auto& clip : clips->array()) {
    const auto* name = clip.Find("filename");
    const auto* type = clip.Find("type");
    if (!name || !name->is_string()) continue;
    if (type && type->is_string() && type->text() == "video/mp4") return name->text();
    if (webm.empty()) webm = name->text();
  }
  return webm;
}

void AppendNumber(Bytes& out, std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    out.push_back(static_cast<std::uint8_t>(value));
    value >>= 8;
  }
}

void AppendString(Bytes& out, const std::string& value) {
  AppendNumber(out, value.size(), 2);
  out.insert(out.end(), value.begin(), value.end());
}

std::uint32_t Checksum(const Bytes& bytes, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

struct Reader {
  const Bytes& bytes;
  std::size_t offset = 0;
  std::size_t limit = 0;
  bool okay = true;
  std::uint64_t Number(unsigned count) {
    if (!okay || offset > limit || count > limit - offset) {
      okay = false;
      return 0;
    }
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(bytes[offset++]) << (i * 8);
    return value;
  }
  std::string String(std::size_t maximum) {
    const auto size = Number(2);
    if (!okay || size > maximum || offset > limit || size > limit - offset) {
      okay = false;
      return {};
    }
    std::string value(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(size));
    offset += static_cast<std::size_t>(size);
    return value;
  }
};

struct BrowseFile {
  std::map<std::uint32_t, std::string> tags;
  std::uint64_t tags_at = 0;
  std::map<std::uint32_t, StoreBrowseItem> apps;
};

bool BrowseMagic(const Bytes& bytes, unsigned* version) {
  if (bytes.size() < kBrowseMagic.size()) return false;
  if (!std::equal(kBrowseMagic.begin(), kBrowseMagic.end() - 1, bytes.begin())) return false;
  if (bytes[7] != 1 && bytes[7] != 2 && bytes[7] != 3) return false;
  *version = bytes[7];
  return true;
}

BrowseFile LoadBrowse(const Storage& storage) {
  const auto read = storage.Read(kBrowsePath, kMaxBrowseBytes);
  if (!read.ok()) return {};
  const Bytes& bytes = read.value();
  unsigned version = 0;
  if (bytes.size() < kBrowseMagic.size() + 8 + 4 + 4 + 4 || !BrowseMagic(bytes, &version)) return {};
  const std::size_t body = bytes.size() - 4;
  Reader crc{bytes, body, bytes.size()};
  if (static_cast<std::uint32_t>(crc.Number(4)) != Checksum(bytes, body)) return {};
  Reader reader{bytes, kBrowseMagic.size(), body};
  BrowseFile file;
  file.tags_at = reader.Number(8);
  const auto tag_count = reader.Number(4);
  if (!reader.okay || tag_count > 10000) return {};
  for (std::uint64_t i = 0; i < tag_count; ++i) {
    const auto id = static_cast<std::uint32_t>(reader.Number(4));
    auto name = reader.String(kMaximumTagBytes);
    if (!reader.okay || !id || !ValidTag(name)) return {};
    file.tags.emplace(id, std::move(name));
  }
  const auto app_count = reader.Number(4);
  if (!reader.okay || app_count > kMaxCachedApps) return {};
  for (std::uint64_t i = 0; i < app_count; ++i) {
    StoreBrowseItem item;
    item.app = static_cast<std::uint32_t>(reader.Number(4));
    item.fetched_at = reader.Number(8);
    item.tags_known = reader.Number(1) != 0;
    const auto tags = reader.Number(1);
    if (!reader.okay || !item.app || tags > kMaximumTags) return {};
    for (std::uint64_t tag = 0; tag < tags; ++tag) {
      auto name = reader.String(kMaximumTagBytes);
      if (!reader.okay || !ValidTag(name)) return {};
      item.tags.push_back(std::move(name));
    }
    // Records saved when a game kept only 12 tags are fetched again, once, so the rest can be stored.
    if (version < 3 && item.tags.size() >= 12) item.fetched_at = 0;
    const auto shots = reader.Number(1);
    if (!reader.okay || shots > kMaximumShots) return {};
    for (std::uint64_t shot = 0; shot < shots; ++shot) {
      auto url = reader.String(kMaximumUrlBytes);
      if (!reader.okay || !ValidUrl(url)) return {};
      item.screenshots.push_back(std::move(url));
    }
    item.trailer_thumbnail = reader.String(kMaximumUrlBytes);
    item.trailer_url = reader.String(kMaximumUrlBytes);
    if (!reader.okay) return {};
    if (!item.trailer_thumbnail.empty() && !ValidUrl(item.trailer_thumbnail)) return {};
    if (!item.trailer_url.empty() && !ValidUrl(item.trailer_url)) return {};
    if (version >= 2) {
      item.description_known = reader.Number(1) != 0;
      item.description = reader.String(kMaximumDescriptionBytes);
      item.controller_support = reader.String(16);
      item.genres_known = reader.Number(1) != 0;
      const auto genres = reader.Number(1);
      if (!reader.okay || genres > kMaximumGenres || !ValidController(item.controller_support)) return {};
      if (item.description.find('\0') != std::string::npos) return {};
      for (std::uint64_t genre = 0; genre < genres; ++genre) {
        auto name = reader.String(kMaximumGenreBytes);
        if (!reader.okay || !ValidGenre(name)) return {};
        item.genres.push_back(std::move(name));
      }
    }
    file.apps.emplace(item.app, std::move(item));
  }
  if (!reader.okay || reader.offset != body) return {};
  return file;
}

bool SaveBrowse(const Storage& storage, const BrowseFile& file) {
  if (file.apps.size() > kMaxCachedApps || file.tags.size() > 10000) return false;
  Bytes out = kBrowseMagic;
  AppendNumber(out, file.tags_at, 8);
  AppendNumber(out, file.tags.size(), 4);
  for (const auto& [id, name] : file.tags) {
    if (!id || !ValidTag(name)) return false;
    AppendNumber(out, id, 4);
    AppendString(out, name);
  }
  AppendNumber(out, file.apps.size(), 4);
  for (const auto& [id, item] : file.apps) {
    if (id != item.app || item.tags.size() > kMaximumTags || item.screenshots.size() > kMaximumShots) return false;
    const std::size_t entry = 4 + 8 + 1 + 1 + 1 + item.trailer_thumbnail.size() + item.trailer_url.size() + 4;
    if (out.size() + entry + 4 > kMaxBrowseBytes) return false;
    AppendNumber(out, item.app, 4);
    AppendNumber(out, item.fetched_at, 8);
    AppendNumber(out, item.tags_known ? 1u : 0u, 1);
    AppendNumber(out, item.tags.size(), 1);
    for (const auto& tag : item.tags) {
      if (!ValidTag(tag)) return false;
      AppendString(out, tag);
    }
    AppendNumber(out, item.screenshots.size(), 1);
    for (const auto& url : item.screenshots) {
      if (!ValidUrl(url)) return false;
      AppendString(out, url);
    }
    if ((!item.trailer_thumbnail.empty() && !ValidUrl(item.trailer_thumbnail)) ||
        (!item.trailer_url.empty() && !ValidUrl(item.trailer_url)))
      return false;
    if (item.description.size() > kMaximumDescriptionBytes || item.description.find('\0') != std::string::npos ||
        !ValidController(item.controller_support) || item.genres.size() > kMaximumGenres)
      return false;
    AppendString(out, item.trailer_thumbnail);
    AppendString(out, item.trailer_url);
    AppendNumber(out, item.description_known ? 1u : 0u, 1);
    AppendString(out, item.description);
    AppendString(out, item.controller_support);
    AppendNumber(out, item.genres_known ? 1u : 0u, 1);
    AppendNumber(out, item.genres.size(), 1);
    for (const auto& genre : item.genres) {
      if (!ValidGenre(genre)) return false;
      AppendString(out, genre);
    }
  }
  if (out.size() + 4 > kMaxBrowseBytes) return false;
  AppendNumber(out, Checksum(out, out.size()), 4);
  return storage.WriteAtomic(kBrowsePath, out).ok();
}

}  // namespace

std::string StoreTagListUrl() {
  return "https://api.steampowered.com/IStoreService/GetTagList/v1/?language=english";
}

std::string StoreBrowseItemsUrl(const std::vector<std::uint32_t>& apps) {
  std::string json = "{\"ids\":[";
  for (std::size_t i = 0; i < apps.size(); ++i) {
    if (i) json += ',';
    json += "{\"appid\":";
    json += std::to_string(apps[i]);
    json += '}';
  }
  json +=
      "],\"context\":{\"language\":\"english\",\"country_code\":\"US\",\"steam_realm\":1},"
      "\"data_request\":{\"include_assets\":true,\"include_screenshots\":true,\"include_trailers\":true,"
      "\"include_tag_count\":20,\"include_basic_info\":true}}";
  return "https://api.steampowered.com/IStoreBrowseService/GetItems/v1/?input_json=" + EncodeQuery(json);
}

std::map<std::uint32_t, std::string> ParseStoreTagList(std::string_view body) {
  const auto root = json::Parse(body, {2 * 1024 * 1024, 8, 20000, 256});
  if (!root || !root->is_object()) return {};
  const auto* response = ObjectField(*root, "response");
  if (!response) return {};
  const auto* tags = ArrayField(*response, "tags");
  if (!tags) return {};
  std::map<std::uint32_t, std::string> names;
  for (const auto& tag : tags->array()) {
    const auto id = tag.Find("tagid");
    const auto name = tag.Find("name");
    if (!id || !name || !name->is_string()) continue;
    const auto tagid = id->AsUint64();
    if (!tagid || *tagid > 0xffffffffu || !ValidTag(name->text())) continue;
    names.emplace(static_cast<std::uint32_t>(*tagid), name->text());
  }
  return names;
}

std::optional<std::vector<StoreBrowseItem>> ParseStoreBrowseItems(std::string_view body,
                                                                  const std::map<std::uint32_t, std::string>& tags) {
  const auto root = json::Parse(body, {kStoreBrowseResponseBytes, 12, 200000, 64 * 1024});
  if (!root || !root->is_object()) return std::nullopt;
  const auto* response = ObjectField(*root, "response");
  if (!response) return std::nullopt;
  const auto* items = ArrayField(*response, "store_items");
  if (!items) return std::nullopt;
  std::vector<StoreBrowseItem> parsed;
  for (const auto& item : items->array()) {
    const auto* app = item.Find("appid");
    const auto appid = app ? app->AsUint64() : std::nullopt;
    if (!appid || *appid == 0 || *appid > 0xffffffffu) continue;
    StoreBrowseItem parsed_item;
    parsed_item.app = static_cast<std::uint32_t>(*appid);
    const auto* listed = ArrayField(item, "tags");
    parsed_item.tags_known = tags.empty() ? false : true;
    if (listed && parsed_item.tags_known) {
      for (const auto& tag : listed->array()) {
        if (parsed_item.tags.size() >= kMaximumTags) break;
        const auto* id = tag.Find("tagid");
        const auto tagid = id ? id->AsUint64() : std::nullopt;
        if (!tagid) continue;
        const auto found = tags.find(static_cast<std::uint32_t>(*tagid));
        if (found == tags.end() || !ValidTag(found->second)) continue;
        if (std::find(parsed_item.tags.begin(), parsed_item.tags.end(), found->second) == parsed_item.tags.end())
          parsed_item.tags.push_back(found->second);
      }
    }
    if (const auto* screenshots = ObjectField(item, "screenshots")) {
      const auto* frames = ArrayField(*screenshots, "all_ages_screenshots");
      if (!frames) frames = ArrayField(*screenshots, "mature_content_screenshots");
      if (frames) {
        for (const auto& frame : frames->array()) {
          if (parsed_item.screenshots.size() >= kMaximumShots) break;
          const auto* filename = frame.Find("filename");
          if (!filename || !filename->is_string()) continue;
          auto url = ResolveAsset({}, filename->text());
          if (!url.empty()) parsed_item.screenshots.push_back(std::move(url));
        }
      }
    }
    const json::Value* trailer = nullptr;
    if (const auto* trailers = ObjectField(item, "trailers")) {
      if (const auto* highlights = ArrayField(*trailers, "highlights"); highlights && !highlights->array().empty())
        trailer = &highlights->array().front();
      else if (const auto* others = ArrayField(*trailers, "other_trailers"); others && !others->array().empty())
        trailer = &others->array().front();
    }
    if (trailer) {
      const auto* format = trailer->Find("trailer_url_format");
      const std::string_view pattern = format && format->is_string() ? std::string_view(format->text()) : std::string_view();
      if (const auto* thumb = trailer->Find("screenshot_medium"); thumb && thumb->is_string())
        parsed_item.trailer_thumbnail = ResolveAsset(pattern, thumb->text());
      parsed_item.trailer_url = ResolveAsset(pattern, TrailerFile(*trailer));
    }
    if (const auto* basic = ObjectField(item, "basic_info")) {
      parsed_item.description_known = true;
      if (const auto* text = basic->Find("short_description"); text && text->is_string())
        parsed_item.description = TrimmedDescription(text->text());
    }
    if (const auto* categories = ObjectField(item, "categories")) {
      if (const auto* ids = ArrayField(*categories, "controller_categoryids")) {
        bool full = false;
        bool partial = false;
        for (const auto& id : ids->array()) {
          const auto value = id.AsUint64();
          if (!value) continue;
          if (*value == 28) full = true;
          else if (*value == 18) partial = true;
        }
        if (full) parsed_item.controller_support = "full";
        else if (partial) parsed_item.controller_support = "partial";
      }
    }
    parsed.push_back(std::move(parsed_item));
  }
  return parsed;
}

StoreBrowseService::StoreBrowseService(Fetch fetch, std::chrono::milliseconds pause)
    : fetch_(std::move(fetch)), pause_(pause) {
  worker_ = std::thread([this] { Run(); });
}

StoreBrowseService::~StoreBrowseService() { Shutdown(); }

void StoreBrowseService::SetStorage(const Storage* storage) {
  std::lock_guard<std::mutex> lock(mutex_);
  storage_ = storage;
}

void StoreBrowseService::SetNow(std::function<std::uint64_t()> now) {
  std::lock_guard<std::mutex> lock(mutex_);
  now_ = std::move(now);
}

std::uint64_t StoreBrowseService::Now() const {
  if (now_) return now_();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}

bool StoreBrowseService::Current(const StoreBrowseItem& item, std::uint64_t now) const {
  return item.fetched_at != 0 && now >= item.fetched_at && now - item.fetched_at < kStoreBrowseTtlSeconds;
}

bool StoreBrowseService::Fresh(const StoreBrowseItem& item, std::uint64_t now) const {
  return Current(item, now) && item.tags_known;
}

void StoreBrowseService::EnsureLoaded() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (cache_loaded_) return;
  const Storage* storage = storage_;
  if (!storage) {
    cache_loaded_ = true;
    return;
  }
  lock.unlock();
  auto loaded = LoadBrowse(*storage);
  lock.lock();
  if (cache_loaded_ || stopping_) return;
  cache_loaded_ = true;
  tag_names_ = std::move(loaded.tags);
  tag_names_at_ = loaded.tags_at;
  cached_ = std::move(loaded.apps);
}

void StoreBrowseService::SaveLocked(std::unique_lock<std::mutex>& lock) {
  const Storage* storage = storage_;
  if (!storage) return;
  BrowseFile file;
  file.tags = tag_names_;
  file.tags_at = tag_names_at_;
  file.apps = cached_;
  lock.unlock();
  SaveBrowse(*storage, file);
  lock.lock();
}

std::optional<StoreBrowseItem> StoreBrowseService::Cached(GameId id) {
  if (!id.steam_app()) return std::nullopt;
  EnsureLoaded();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = cached_.find(static_cast<std::uint32_t>(*id.steam_app()));
  if (found == cached_.end() || !Current(found->second, Now())) return std::nullopt;
  return found->second;
}

void StoreBrowseService::RememberDetails(GameId id, std::string description, std::vector<std::string> genres,
                                         std::string controller) {
  if (!id.steam_app()) return;
  description = TrimmedDescription(std::move(description));
  if (!ValidController(controller)) controller.clear();
  std::vector<std::string> kept;
  for (auto& genre : genres) {
    if (kept.size() >= kMaximumGenres || !ValidGenre(genre)) continue;
    kept.push_back(std::move(genre));
  }
  EnsureLoaded();
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopping_) return;
  const auto app = static_cast<std::uint32_t>(*id.steam_app());
  auto& item = cached_[app];
  item.app = app;
  if (item.fetched_at == 0) item.fetched_at = Now();
  item.genres = std::move(kept);
  item.genres_known = true;
  if (!item.description_known) {
    item.description = std::move(description);
    item.description_known = true;
  }
  if (item.controller_support.empty() && !controller.empty()) item.controller_support = std::move(controller);
  SaveLocked(lock);
}

void StoreBrowseService::Enqueue(GameId id, bool first) {
  if (!id.steam_app()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  const auto now = Now();
  if (const auto found = cached_.find(static_cast<std::uint32_t>(*id.steam_app())); found != cached_.end() && Fresh(found->second, now)) {
    if (!requested_.insert(id).second) return;
    results_.push_back(found->second);
    wake_.notify_one();
    return;
  }
  if (!first && pending_.size() >= kMaxPending && !requested_.count(id)) return;
  const auto queued = std::find(pending_.begin(), pending_.end(), id);
  if (queued != pending_.end()) {
    if (!first || queued == pending_.begin()) return;
    pending_.erase(queued);
    pending_.push_front(id);
    wake_.notify_one();
    return;
  }
  if (requested_.count(id)) return;
  if (pending_.size() >= kMaxPending) {
    requested_.erase(pending_.back());
    pending_.pop_back();
  }
  requested_.insert(id);
  if (first) pending_.push_front(id);
  else pending_.push_back(id);
  wake_.notify_one();
}

void StoreBrowseService::Request(GameId id) { Enqueue(id, false); }

void StoreBrowseService::RequestFirst(GameId id) { Enqueue(id, true); }

std::vector<StoreBrowseItem> StoreBrowseService::Pump(std::size_t maximum) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoreBrowseItem> out;
  while (!results_.empty() && out.size() < maximum) {
    out.push_back(std::move(results_.front()));
    results_.pop_front();
  }
  return out;
}

void StoreBrowseService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cancel_.store(true);
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void StoreBrowseService::FetchItems(const std::vector<std::uint32_t>& batch,
                                    const std::map<std::uint32_t, std::string>& tag_names) {
  if (batch.empty() || cancel_.load()) return;
  std::optional<std::vector<StoreBrowseItem>> parsed;
  if (fetch_) {
    if (auto body = fetch_(StoreBrowseItemsUrl(batch), cancel_)) parsed = ParseStoreBrowseItems(*body, tag_names);
  }
  if (!parsed) {
    if (batch.size() == 1) {
      std::lock_guard<std::mutex> lock(mutex_);
      requested_.erase(GameId::Steam(batch[0]));
      return;
    }
    const auto mid = batch.size() / 2;
    FetchItems(std::vector<std::uint32_t>(batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(mid)), tag_names);
    FetchItems(std::vector<std::uint32_t>(batch.begin() + static_cast<std::ptrdiff_t>(mid), batch.end()), tag_names);
    return;
  }
  const auto now = Now();
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  std::map<std::uint32_t, StoreBrowseItem> by_app;
  for (auto& item : *parsed) by_app.emplace(item.app, std::move(item));
  for (const auto app : batch) {
    StoreBrowseItem incoming;
    if (const auto found = by_app.find(app); found != by_app.end()) incoming = std::move(found->second);
    else {
      incoming.app = app;
      incoming.tags_known = true;
    }
    incoming.fetched_at = now;
    if (const auto old = cached_.find(app); old != cached_.end()) {
      if (!incoming.description_known) {
        incoming.description = old->second.description;
        incoming.description_known = old->second.description_known;
      }
      if (incoming.controller_support.empty()) incoming.controller_support = old->second.controller_support;
      if (!incoming.genres_known) {
        incoming.genres = old->second.genres;
        incoming.genres_known = old->second.genres_known;
      }
    }
    if (incoming.tags_known) cached_[app] = incoming;
    results_.push_back(std::move(incoming));
  }
}

void StoreBrowseService::Run() {
  for (;;) {
    std::vector<std::uint32_t> batch;
    std::map<std::uint32_t, std::string> tag_names;
    bool tags_fresh = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      lock.unlock();
      EnsureLoaded();
      lock.lock();
      if (stopping_) return;
      const auto now = Now();
      while (!pending_.empty() && batch.size() < kBatch) {
        const auto id = pending_.front();
        pending_.pop_front();
        const auto app = static_cast<std::uint32_t>(*id.steam_app());
        const auto found = cached_.find(app);
        if (found != cached_.end() && Fresh(found->second, now)) results_.push_back(found->second);
        else batch.push_back(app);
      }
      tag_names = tag_names_;
      tags_fresh = tag_names_at_ != 0 && now >= tag_names_at_ && now - tag_names_at_ < kStoreBrowseTtlSeconds;
    }
    if (batch.empty()) continue;
    if (!tags_fresh && fetch_) {
      if (auto body = fetch_(StoreTagListUrl(), cancel_)) {
        auto parsed = ParseStoreTagList(*body);
        if (!parsed.empty()) {
          std::lock_guard<std::mutex> lock(mutex_);
          tag_names_ = parsed;
          tag_names_at_ = Now();
          tag_names = tag_names_;
          tags_fresh = true;
        }
      }
    }
    FetchItems(batch, tag_names);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stopping_) return;
      if (tags_fresh) SaveLocked(lock);
    }
    if (pause_.count() > 0 && !stopping_) {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, pause_, [this] { return stopping_; });
    }
  }
}

}  // namespace playden::steam
