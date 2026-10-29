#include "library/organization.h"

#include "library/store.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace playden {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{{'P', 'L', 'D', 'O', 'R', 'G', 0, 0}};
constexpr std::uint16_t kVersion1 = 1;
constexpr std::uint16_t kVersion2 = 2;
constexpr char kPathV2[] = "organization.v2";
constexpr std::size_t kMaximumSnapshotBytes = 1024 * 1024;
constexpr std::size_t kMaximumGameIds = 10000;
constexpr std::size_t kMaximumCollections = 256;
constexpr std::size_t kMaximumCollectionIdBytes = 128;
constexpr std::size_t kMaximumCollectionNameBytes = 128;
constexpr std::size_t kMaximumNoteBytes = 4096;
constexpr std::size_t kMaximumQueryBytes = 256;
constexpr std::size_t kMaximumScopeBytes = 128;
constexpr std::size_t kMaximumSortBytes = 32;
constexpr std::size_t kMaximumFilterBytes = 32;
constexpr std::size_t kMaximumKeyBytes = 64;

std::string PathV1(std::uint64_t account_id) {
  return "organization-" + std::to_string(account_id) + ".v1";
}

template <typename T>
Result<T> InvalidValue(const char* reason) {
  return Result<T>::Failure(StorageError::kInvalidData,
                            std::string("organization data is invalid: ") + reason);
}

Result<void> Invalid(const char* reason) {
  return InvalidValue<void>(reason);
}

bool ValidUtf8(const std::string& value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0) return false;
    if (first < 0x80) {
      ++index;
      continue;
    }
    std::size_t count = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 2;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 3;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 4;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (count > value.size() - index) return false;
    for (std::size_t offset = 1; offset < count; ++offset) {
      const auto continuation = static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    if ((count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
      return false;
    }
    index += count;
  }
  return true;
}

bool ValidText(const std::string& value, std::size_t maximum, bool allow_empty) {
  return (allow_empty || !value.empty()) && value.size() <= maximum && ValidUtf8(value);
}

bool ValidCollectionId(const std::string& value) {
  if (value.empty() || value.size() > kMaximumCollectionIdBytes) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
  });
}

bool CanonicalCollectionName(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  return first == 0 && value.find_last_not_of(" \t\r\n") == value.size() - 1;
}

bool BuiltInScope(const std::string& value) {
  return value == "all" || value == "favorites" || value == "hidden";
}

// The Library rails of installed and imported games: scopes, never collection IDs.
bool RailScope(const std::string& value) { return value == "installed" || value == "imported"; }

bool ReservedCollectionId(const std::string& value) {
  return BuiltInScope(value) || RailScope(value) || value == "new-collection";
}

bool ValidSort(const std::string& value) {
  return value == "name" || value == "recent-played" || value == "recent-added" ||
         value == "playtime";
}

// Version 1 knows only Playable; version 2 also keeps demos and family-shared games hidden (they show by default).
bool ValidFilter(const std::string& value, std::uint16_t version) {
  return value == "compat-playable" || (version != kVersion1 && (value == "hide-demos" || value == "hide-family"));
}

// Version 1 stores Steam app IDs only; version 2 any valid game.
bool AddGameId(std::set<GameId>& all_ids, const GameId& id, std::uint16_t version) {
  if (version == kVersion1 ? !id.steam_app() : !id.valid() || ToKey(id).size() > kMaximumKeyBytes)
    return false;
  all_ids.insert(id);
  return all_ids.size() <= kMaximumGameIds;
}

bool Validate(const OrganizationSnapshot& snapshot, std::uint16_t version) {
  if (snapshot.schema_version != version ||
      snapshot.collections.size() > kMaximumCollections ||
      !ValidText(snapshot.query, kMaximumQueryBytes, true) ||
      !ValidText(snapshot.scope, kMaximumScopeBytes, false) ||
      !ValidText(snapshot.sort, kMaximumSortBytes, false) ||
      !ValidSort(snapshot.sort)) {
    return false;
  }
  if (snapshot.selected_game_id.valid() && version == kVersion2 &&
      ToKey(snapshot.selected_game_id).size() > kMaximumKeyBytes) {
    return false;
  }

  std::set<GameId> game_ids;
  for (const GameId& id : snapshot.favorites) if (!AddGameId(game_ids, id, version)) return false;
  for (const GameId& id : snapshot.hidden) if (!AddGameId(game_ids, id, version)) return false;
  // Selection is one scalar, independent of the annotation/member ID budget.

  std::set<std::string> collection_ids;
  std::set<std::string> collection_names;
  for (const auto& collection : snapshot.collections) {
    if (!ValidCollectionId(collection.id) || ReservedCollectionId(collection.id) ||
        !collection_ids.insert(collection.id).second ||
        !ValidText(collection.name, kMaximumCollectionNameBytes, false) ||
        !CanonicalCollectionName(collection.name) ||
        !collection_names.insert(collection.name).second ||
        collection.members.size() > kMaximumGameIds) {
      return false;
    }
    for (const GameId& id : collection.members) if (!AddGameId(game_ids, id, version)) return false;
  }
  if (!BuiltInScope(snapshot.scope) && !RailScope(snapshot.scope) && !collection_ids.count(snapshot.scope))
    return false;

  if (snapshot.ratings.size() > kMaximumGameIds || snapshot.notes.size() > kMaximumGameIds)
    return false;
  for (const auto& rating : snapshot.ratings) {
    if (!AddGameId(game_ids, rating.first, version) ||
        static_cast<std::uint8_t>(rating.second) >
            static_cast<std::uint8_t>(CompatibilityRating::kBroken)) {
      return false;
    }
  }
  for (const auto& note : snapshot.notes) {
    if (!AddGameId(game_ids, note.first, version) ||
        !ValidText(note.second, kMaximumNoteBytes, true)) {
      return false;
    }
  }
  if (snapshot.filters.size() > (version == kVersion1 ? 1u : 3u)) return false;
  for (const auto& filter : snapshot.filters) {
    if (!ValidText(filter, kMaximumFilterBytes, false) || !ValidFilter(filter, version)) return false;
  }
  return true;
}

std::uint32_t Checksum(const Bytes& bytes, std::size_t size) {
  std::uint32_t value = 2166136261u;
  for (std::size_t index = 0; index < size; ++index) {
    value ^= bytes[index];
    value *= 16777619u;
  }
  return value;
}

class Writer {
 public:
  bool U8(std::uint8_t value) { return Append(&value, 1); }
  bool U16(std::uint16_t value) { return Integer(value, 2); }
  bool U32(std::uint32_t value) { return Integer(value, 4); }
  bool U64(std::uint64_t value) { return Integer(value, 8); }
  bool String(const std::string& value) {
    return value.size() <= std::numeric_limits<std::uint32_t>::max() &&
           U32(static_cast<std::uint32_t>(value.size())) &&
           Append(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  }
  bool Raw(const std::uint8_t* bytes, std::size_t size) { return Append(bytes, size); }
  Bytes Take() { return std::move(bytes_); }

 private:
  bool Append(const std::uint8_t* bytes, std::size_t size) {
    constexpr std::size_t kChecksumBytes = 4;
    if (size > kMaximumSnapshotBytes - kChecksumBytes - bytes_.size()) return false;
    bytes_.insert(bytes_.end(), bytes, bytes + size);
    return true;
  }
  bool Integer(std::uint64_t value, unsigned size) {
    std::uint8_t encoded[8]{};
    for (unsigned index = 0; index < size; ++index) {
      encoded[index] = static_cast<std::uint8_t>(value);
      value >>= 8;
    }
    return Append(encoded, size);
  }
  Bytes bytes_;
};

class Reader {
 public:
  Reader(const Bytes& bytes, std::size_t offset, std::size_t limit)
      : bytes_(bytes), offset_(offset), limit_(limit) {}
  bool U8(std::uint8_t& value) { return Integer(value, 1); }
  bool U16(std::uint16_t& value) { return Integer(value, 2); }
  bool U32(std::uint32_t& value) { return Integer(value, 4); }
  bool U64(std::uint64_t& value) { return Integer(value, 8); }
  bool String(std::string& value, std::size_t maximum) {
    std::uint32_t size = 0;
    if (!U32(size) || size > maximum || size > Remaining()) return false;
    value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }
  std::size_t offset() const { return offset_; }
  std::size_t limit() const { return limit_; }

 private:
  std::size_t Remaining() const {
    return offset_ <= limit_ ? limit_ - offset_ : 0;
  }
  template <typename Value>
  bool Integer(Value& value, unsigned size) {
    if (size > Remaining()) return false;
    value = 0;
    for (unsigned index = 0; index < size; ++index)
      value |= static_cast<Value>(bytes_[offset_++]) << (8 * index);
    return true;
  }
  const Bytes& bytes_;
  std::size_t offset_;
  std::size_t limit_;
};

using Loaded = Result<std::optional<OrganizationSnapshot>>;

Loaded InvalidLoad(const char* reason) {
  return InvalidValue<std::optional<OrganizationSnapshot>>(reason);
}

void AppendChecksum(Bytes& encoded) {
  const std::uint32_t checksum = Checksum(encoded, encoded.size());
  for (unsigned shift = 0; shift < 32; shift += 8)
    encoded.push_back(static_cast<std::uint8_t>(checksum >> shift));
}

// Header, magic and checksum; returns the payload reader positioned after the magic.
bool CheckFrame(const Bytes& bytes, std::size_t fixed_bytes) {
  if (bytes.size() < fixed_bytes || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    return false;
  const std::size_t payload_limit = bytes.size() - 4;
  Reader checksum_reader(bytes, payload_limit, bytes.size());
  std::uint32_t stored_checksum = 0;
  return checksum_reader.U32(stored_checksum) && stored_checksum == Checksum(bytes, payload_limit);
}

// ---- Version 1: u32 Steam app IDs, account embedded. ----

bool WriteIdsV1(Writer& writer, const std::set<GameId>& ids) {
  if (!writer.U32(static_cast<std::uint32_t>(ids.size()))) return false;
  for (const GameId& id : ids) if (!writer.U32(*id.steam_app())) return false;
  return true;
}

bool ReadIdsV1(Reader& reader, std::set<GameId>& ids) {
  std::uint32_t count = 0;
  if (!reader.U32(count) || count > kMaximumGameIds) return false;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint32_t id = 0;
    if (!reader.U32(id) || id == 0 || !ids.insert(GameId::Steam(id)).second) return false;
  }
  return true;
}

bool ValidV1(const OrganizationSnapshot& snapshot) {
  auto v1 = snapshot;
  v1.schema_version = kVersion1;
  if (!Validate(v1, kVersion1)) return false;
  // Header, all count/string prefixes, and trailing checksum.
  std::size_t bytes = 62 + snapshot.query.size() + snapshot.scope.size() + snapshot.sort.size();
  bytes += 4 * (snapshot.favorites.size() + snapshot.hidden.size());
  bytes += 5 * snapshot.ratings.size();
  for (const auto& [id, note] : snapshot.notes) { (void)id; bytes += 8 + note.size(); }
  for (const auto& collection : snapshot.collections)
    bytes += 13 + collection.id.size() + collection.name.size() + 4 * collection.members.size();
  for (const auto& filter : snapshot.filters) bytes += 4 + filter.size();
  return bytes <= kMaximumSnapshotBytes;
}

Loaded DecodeV1(const Bytes& bytes, std::uint64_t account_id) {
  constexpr std::size_t kFixedBytes = kMagic.size() + 2 + 8 + 4 + 4;
  if (bytes.size() < kFixedBytes || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    return InvalidLoad("header");
  if (!CheckFrame(bytes, kFixedBytes)) return InvalidLoad("checksum");

  Reader reader(bytes, kMagic.size(), bytes.size() - 4);
  std::uint16_t version = 0;
  std::uint64_t embedded_account = 0;
  std::uint32_t selected_game_id = 0;
  OrganizationSnapshot snapshot;
  if (!reader.U16(version) || version != kVersion1 ||
      !reader.U64(embedded_account) || embedded_account != account_id ||
      !reader.U32(selected_game_id) ||
      !ReadIdsV1(reader, snapshot.favorites) || !ReadIdsV1(reader, snapshot.hidden)) {
    return InvalidLoad("identity or fields");
  }
  snapshot.schema_version = version;
  snapshot.selected_game_id = GameId::Steam(selected_game_id);

  std::uint32_t collection_count = 0;
  if (!reader.U32(collection_count) || collection_count > kMaximumCollections)
    return InvalidLoad("collection count");
  snapshot.collections.reserve(collection_count);
  for (std::uint32_t index = 0; index < collection_count; ++index) {
    OrganizationCollection collection;
    std::uint8_t pinned = 0;
    if (!reader.String(collection.id, kMaximumCollectionIdBytes) ||
        !reader.String(collection.name, kMaximumCollectionNameBytes) ||
        !reader.U8(pinned) || pinned > 1 || !ReadIdsV1(reader, collection.members)) {
      return InvalidLoad("collection");
    }
    collection.pinned = pinned != 0;
    snapshot.collections.push_back(std::move(collection));
  }

  std::uint32_t rating_count = 0;
  if (!reader.U32(rating_count) || rating_count > kMaximumGameIds) return InvalidLoad("rating count");
  for (std::uint32_t index = 0; index < rating_count; ++index) {
    std::uint32_t id = 0;
    std::uint8_t rating = 0;
    if (!reader.U32(id) || id == 0 || !reader.U8(rating) ||
        rating > static_cast<std::uint8_t>(CompatibilityRating::kBroken) ||
        !snapshot.ratings.emplace(GameId::Steam(id), static_cast<CompatibilityRating>(rating)).second) {
      return InvalidLoad("rating");
    }
  }

  std::uint32_t note_count = 0;
  if (!reader.U32(note_count) || note_count > kMaximumGameIds) return InvalidLoad("note count");
  for (std::uint32_t index = 0; index < note_count; ++index) {
    std::uint32_t id = 0;
    std::string note;
    if (!reader.U32(id) || id == 0 || !reader.String(note, kMaximumNoteBytes) ||
        !snapshot.notes.emplace(GameId::Steam(id), std::move(note)).second) {
      return InvalidLoad("note");
    }
  }

  std::uint32_t filter_count = 0;
  if (!reader.String(snapshot.query, kMaximumQueryBytes) ||
      !reader.String(snapshot.scope, kMaximumScopeBytes) ||
      !reader.String(snapshot.sort, kMaximumSortBytes) ||
      !reader.U32(filter_count) || filter_count > 1) {
    return InvalidLoad("view state");
  }
  for (std::uint32_t index = 0; index < filter_count; ++index) {
    std::string filter;
    if (!reader.String(filter, kMaximumFilterBytes) || !snapshot.filters.insert(std::move(filter)).second)
      return InvalidLoad("filter");
  }
  if (reader.offset() != reader.limit() || !Validate(snapshot, kVersion1))
    return InvalidLoad("trailing or semantic data");
  return Loaded::Success(std::move(snapshot));
}

// ---- Version 2: games as `ToKey` strings, no account. ----

bool WriteKeys(Writer& writer, const std::set<GameId>& ids) {
  if (!writer.U32(static_cast<std::uint32_t>(ids.size()))) return false;
  for (const GameId& id : ids) if (!writer.String(ToKey(id))) return false;
  return true;
}

bool ReadKey(Reader& reader, GameId& id) {
  std::string key;
  if (!reader.String(key, kMaximumKeyBytes)) return false;
  const auto parsed = ParseKey(key);
  if (!parsed || !parsed->valid()) return false;
  id = *parsed;
  return true;
}

bool ReadKeys(Reader& reader, std::set<GameId>& ids) {
  std::uint32_t count = 0;
  if (!reader.U32(count) || count > kMaximumGameIds) return false;
  for (std::uint32_t index = 0; index < count; ++index) {
    GameId id;
    if (!ReadKey(reader, id) || !ids.insert(std::move(id)).second) return false;
  }
  return true;
}

std::size_t KeyBytes(const GameId& id) { return 4 + ToKey(id).size(); }

std::size_t KeysBytes(const std::set<GameId>& ids) {
  std::size_t bytes = 4;
  for (const GameId& id : ids) bytes += KeyBytes(id);
  return bytes;
}

std::size_t EncodedBytesV2(const OrganizationSnapshot& snapshot) {
  // Magic, version, selection, every count/string prefix, and trailing checksum.
  std::size_t bytes = kMagic.size() + 2 + 4 + 4;
  if (snapshot.selected_game_id.valid()) bytes += ToKey(snapshot.selected_game_id).size();
  bytes += KeysBytes(snapshot.favorites) + KeysBytes(snapshot.hidden) + 4;
  for (const auto& collection : snapshot.collections)
    bytes += 4 + collection.id.size() + 4 + collection.name.size() + 1 + KeysBytes(collection.members);
  bytes += 4;
  for (const auto& [id, rating] : snapshot.ratings) { (void)rating; bytes += KeyBytes(id) + 1; }
  bytes += 4;
  for (const auto& [id, note] : snapshot.notes) bytes += KeyBytes(id) + 4 + note.size();
  bytes += 12 + snapshot.query.size() + snapshot.scope.size() + snapshot.sort.size() + 4;
  for (const auto& filter : snapshot.filters) bytes += 4 + filter.size();
  return bytes;
}

Loaded DecodeV2(const Bytes& bytes) {
  constexpr std::size_t kFixedBytes = kMagic.size() + 2 + 4 + 4;
  if (bytes.size() < kFixedBytes || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    return InvalidLoad("header");
  if (!CheckFrame(bytes, kFixedBytes)) return InvalidLoad("checksum");

  Reader reader(bytes, kMagic.size(), bytes.size() - 4);
  std::uint16_t version = 0;
  std::string selected;
  OrganizationSnapshot snapshot;
  if (!reader.U16(version) || version != kVersion2 || !reader.String(selected, kMaximumKeyBytes))
    return InvalidLoad("version or selection");
  snapshot.schema_version = version;
  if (!selected.empty()) {
    const auto parsed = ParseKey(selected);
    if (!parsed || !parsed->valid()) return InvalidLoad("selection");
    snapshot.selected_game_id = *parsed;
  }
  if (!ReadKeys(reader, snapshot.favorites) || !ReadKeys(reader, snapshot.hidden))
    return InvalidLoad("favorites or hidden");

  std::uint32_t collection_count = 0;
  if (!reader.U32(collection_count) || collection_count > kMaximumCollections)
    return InvalidLoad("collection count");
  snapshot.collections.reserve(collection_count);
  for (std::uint32_t index = 0; index < collection_count; ++index) {
    OrganizationCollection collection;
    std::uint8_t pinned = 0;
    if (!reader.String(collection.id, kMaximumCollectionIdBytes) ||
        !reader.String(collection.name, kMaximumCollectionNameBytes) ||
        !reader.U8(pinned) || pinned > 1 || !ReadKeys(reader, collection.members)) {
      return InvalidLoad("collection");
    }
    collection.pinned = pinned != 0;
    snapshot.collections.push_back(std::move(collection));
  }

  std::uint32_t rating_count = 0;
  if (!reader.U32(rating_count) || rating_count > kMaximumGameIds) return InvalidLoad("rating count");
  for (std::uint32_t index = 0; index < rating_count; ++index) {
    GameId id;
    std::uint8_t rating = 0;
    if (!ReadKey(reader, id) || !reader.U8(rating) ||
        rating > static_cast<std::uint8_t>(CompatibilityRating::kBroken) ||
        !snapshot.ratings.emplace(std::move(id), static_cast<CompatibilityRating>(rating)).second) {
      return InvalidLoad("rating");
    }
  }

  std::uint32_t note_count = 0;
  if (!reader.U32(note_count) || note_count > kMaximumGameIds) return InvalidLoad("note count");
  for (std::uint32_t index = 0; index < note_count; ++index) {
    GameId id;
    std::string note;
    if (!ReadKey(reader, id) || !reader.String(note, kMaximumNoteBytes) ||
        !snapshot.notes.emplace(std::move(id), std::move(note)).second) {
      return InvalidLoad("note");
    }
  }

  std::uint32_t filter_count = 0;
  if (!reader.String(snapshot.query, kMaximumQueryBytes) ||
      !reader.String(snapshot.scope, kMaximumScopeBytes) ||
      !reader.String(snapshot.sort, kMaximumSortBytes) ||
      !reader.U32(filter_count) || filter_count > 3) {
    return InvalidLoad("view state");
  }
  for (std::uint32_t index = 0; index < filter_count; ++index) {
    std::string filter;
    if (!reader.String(filter, kMaximumFilterBytes) || !snapshot.filters.insert(std::move(filter)).second)
      return InvalidLoad("filter");
  }
  if (reader.offset() != reader.limit() || !Validate(snapshot, kVersion2))
    return InvalidLoad("trailing or semantic data");
  return Loaded::Success(std::move(snapshot));
}

// ---- Migration: every account's `.v1` into one device-wide snapshot. ----

// `organization-<decimal account>.v1`, account non-zero.
std::optional<std::uint64_t> LegacyAccount(const std::string& name) {
  constexpr std::string_view kPrefix = "organization-";
  constexpr std::string_view kSuffix = ".v1";
  if (name.size() <= kPrefix.size() + kSuffix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0 ||
      name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
    return std::nullopt;
  }
  const std::string digits = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
  if (digits.front() == '0') return std::nullopt;
  std::uint64_t value = 0;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9') return std::nullopt;
    const auto next = static_cast<std::uint64_t>(digit - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - next) / 10) return std::nullopt;
    value = value * 10 + next;
  }
  return value == 0 ? std::nullopt : std::optional<std::uint64_t>(value);
}

std::string FreeCollectionId(const std::vector<OrganizationCollection>& collections) {
  for (std::uint64_t serial = 1;; ++serial) {
    const auto candidate = "collection-" + std::to_string(serial);
    if (std::none_of(collections.begin(), collections.end(),
                     [&](const auto& value) { return value.id == candidate; }))
      return candidate;
  }
}

// `newest_first` is ordered by modification time, newest first.
OrganizationSnapshot Merge(const std::vector<OrganizationSnapshot>& newest_first) {
  OrganizationSnapshot merged = newest_first.front();
  merged.schema_version = kVersion2;
  for (std::size_t index = 1; index < newest_first.size(); ++index) {
    const auto& older = newest_first[index];
    merged.favorites.insert(older.favorites.begin(), older.favorites.end());
    merged.hidden.insert(older.hidden.begin(), older.hidden.end());
    for (const auto& collection : older.collections) {
      const auto same = std::find_if(merged.collections.begin(), merged.collections.end(),
                                     [&](const auto& value) { return value.name == collection.name; });
      if (same != merged.collections.end()) {
        same->members.insert(collection.members.begin(), collection.members.end());
        same->pinned = same->pinned || collection.pinned;
        continue;
      }
      auto added = collection;
      if (std::any_of(merged.collections.begin(), merged.collections.end(),
                      [&](const auto& value) { return value.id == added.id; }))
        added.id = FreeCollectionId(merged.collections);
      merged.collections.push_back(std::move(added));
    }
    // emplace keeps the newer file's value.
    for (const auto& rating : older.ratings) merged.ratings.emplace(rating);
    for (const auto& note : older.notes) merged.notes.emplace(note);
  }
  return merged;
}

}  // namespace

bool operator==(const OrganizationCollection& left,
                const OrganizationCollection& right) {
  return left.id == right.id && left.name == right.name &&
         left.pinned == right.pinned && left.members == right.members;
}

bool operator==(const OrganizationSnapshot& left,
                const OrganizationSnapshot& right) {
  return left.schema_version == right.schema_version &&
         left.favorites == right.favorites && left.hidden == right.hidden &&
         left.collections == right.collections && left.ratings == right.ratings &&
         left.notes == right.notes && left.query == right.query &&
         left.scope == right.scope && left.sort == right.sort &&
         left.filters == right.filters &&
         left.selected_game_id == right.selected_game_id;
}

bool OrganizationSnapshotIsValid(const OrganizationSnapshot& snapshot) {
  return Validate(snapshot, kVersion2) && EncodedBytesV2(snapshot) <= kMaximumSnapshotBytes;
}

Result<void> OrganizationStore::Save(std::uint64_t scope, const OrganizationSnapshot& snapshot) const {
  if (scope != kDeviceOrganization) return Invalid("organization scope");
  if (!OrganizationSnapshotIsValid(snapshot)) return Invalid("snapshot fields or encoded size");
  const auto ready = storage_.EnsureRootDirectory();
  if (!ready.ok()) return ready;

  Writer writer;
  const GameId& selected = snapshot.selected_game_id;
  bool okay = writer.Raw(kMagic.data(), kMagic.size()) && writer.U16(kVersion2) &&
              writer.String(selected.valid() ? ToKey(selected) : std::string()) &&
              WriteKeys(writer, snapshot.favorites) && WriteKeys(writer, snapshot.hidden) &&
              writer.U32(static_cast<std::uint32_t>(snapshot.collections.size()));
  for (const auto& collection : snapshot.collections) {
    okay = okay && writer.String(collection.id) && writer.String(collection.name) &&
           writer.U8(collection.pinned ? 1 : 0) && WriteKeys(writer, collection.members);
  }
  okay = okay && writer.U32(static_cast<std::uint32_t>(snapshot.ratings.size()));
  for (const auto& rating : snapshot.ratings)
    okay = okay && writer.String(ToKey(rating.first)) && writer.U8(static_cast<std::uint8_t>(rating.second));
  okay = okay && writer.U32(static_cast<std::uint32_t>(snapshot.notes.size()));
  for (const auto& note : snapshot.notes)
    okay = okay && writer.String(ToKey(note.first)) && writer.String(note.second);
  okay = okay && writer.String(snapshot.query) && writer.String(snapshot.scope) &&
         writer.String(snapshot.sort) &&
         writer.U32(static_cast<std::uint32_t>(snapshot.filters.size()));
  for (const auto& filter : snapshot.filters) okay = okay && writer.String(filter);
  if (!okay) return Invalid("snapshot exceeds one MiB limit");

  Bytes encoded = writer.Take();
  AppendChecksum(encoded);
  return storage_.WriteAtomicPrivate(kPathV2, encoded);
}

Result<std::optional<OrganizationSnapshot>> OrganizationStore::Load(std::uint64_t scope) const {
  if (scope != kDeviceOrganization) return InvalidLoad("organization scope");
  const auto ready = storage_.EnsureRootDirectory();
  if (!ready.ok()) return Loaded::Failure(ready.error(), ready.message());
  const auto loaded = storage_.Read(kPathV2, kMaximumSnapshotBytes);
  if (loaded.ok()) return DecodeV2(loaded.value());
  if (loaded.error() != StorageError::kNotFound) return Loaded::Failure(loaded.error(), loaded.message());

  // First load on this device: unite every account's version 1 file.
  const auto listed = storage_.ListFiles();
  if (!listed.ok()) return Loaded::Failure(listed.error(), listed.message());
  struct Legacy {
    std::int64_t modified_ns;
    std::string name;
    OrganizationSnapshot snapshot;
  };
  std::vector<Legacy> legacy;
  for (const auto& file : listed.value()) {
    const auto account = LegacyAccount(file.name);
    if (!account) continue;
    auto snapshot = LoadV1(*account);
    // A damaged or unreadable account file has nothing to give; the others still merge.
    if (!snapshot.ok() || !snapshot.value()) continue;
    legacy.push_back({file.modified_ns, file.name, std::move(*snapshot.value())});
  }
  if (legacy.empty()) return Loaded::Success(std::nullopt);
  std::sort(legacy.begin(), legacy.end(), [](const Legacy& left, const Legacy& right) {
    if (left.modified_ns != right.modified_ns) return left.modified_ns > right.modified_ns;
    return left.name < right.name;
  });
  std::vector<OrganizationSnapshot> newest_first;
  newest_first.reserve(legacy.size());
  for (auto& item : legacy) newest_first.push_back(std::move(item.snapshot));
  auto merged = Merge(newest_first);
  if (!OrganizationSnapshotIsValid(merged)) {
    // The union is past a limit: keep the most recently used account's organization whole.
    merged = std::move(newest_first.front());
    merged.schema_version = kVersion2;
  }
  // Written now so the merge happens once; a failed write merges again on the next start.
  (void)Save(scope, merged);
  return Loaded::Success(std::move(merged));
}

Result<void> OrganizationStore::Clear(std::uint64_t scope) const {
  if (scope != kDeviceOrganization) return Invalid("organization scope");
  const auto final_result = storage_.Remove(kPathV2);
  const auto temporary_result = storage_.Remove(std::string(kPathV2) + ".tmp");
  if (!final_result.ok()) return final_result;
  if (!temporary_result.ok()) return temporary_result;
  return Result<void>::Success();
}

Result<void> OrganizationStore::SaveV1(std::uint64_t account_id,
                                       const OrganizationSnapshot& snapshot) const {
  if (account_id == 0) return Invalid("account identifier");
  if (!ValidV1(snapshot)) return Invalid("snapshot fields or encoded size");
  const auto ready = storage_.EnsureRootDirectory();
  if (!ready.ok()) return ready;

  Writer writer;
  bool okay = writer.Raw(kMagic.data(), kMagic.size()) && writer.U16(kVersion1) &&
              writer.U64(account_id) && writer.U32(snapshot.selected_game_id.steam_app().value_or(0)) &&
              WriteIdsV1(writer, snapshot.favorites) && WriteIdsV1(writer, snapshot.hidden) &&
              writer.U32(static_cast<std::uint32_t>(snapshot.collections.size()));
  for (const auto& collection : snapshot.collections) {
    okay = okay && writer.String(collection.id) && writer.String(collection.name) &&
           writer.U8(collection.pinned ? 1 : 0) && WriteIdsV1(writer, collection.members);
  }
  okay = okay && writer.U32(static_cast<std::uint32_t>(snapshot.ratings.size()));
  for (const auto& rating : snapshot.ratings) {
    okay = okay && writer.U32(*rating.first.steam_app()) &&
           writer.U8(static_cast<std::uint8_t>(rating.second));
  }
  okay = okay && writer.U32(static_cast<std::uint32_t>(snapshot.notes.size()));
  for (const auto& note : snapshot.notes)
    okay = okay && writer.U32(*note.first.steam_app()) && writer.String(note.second);
  okay = okay && writer.String(snapshot.query) && writer.String(snapshot.scope) &&
         writer.String(snapshot.sort) &&
         writer.U32(static_cast<std::uint32_t>(snapshot.filters.size()));
  for (const auto& filter : snapshot.filters) okay = okay && writer.String(filter);
  if (!okay) return Invalid("snapshot exceeds one MiB limit");

  Bytes encoded = writer.Take();
  AppendChecksum(encoded);
  return storage_.WriteAtomicPrivate(PathV1(account_id), encoded);
}

Result<std::optional<OrganizationSnapshot>> OrganizationStore::LoadV1(std::uint64_t account_id) const {
  if (account_id == 0) return InvalidLoad("account identifier");
  const auto ready = storage_.EnsureRootDirectory();
  if (!ready.ok()) return Loaded::Failure(ready.error(), ready.message());
  const auto loaded = storage_.Read(PathV1(account_id), kMaximumSnapshotBytes);
  if (!loaded.ok()) {
    if (loaded.error() == StorageError::kNotFound) return Loaded::Success(std::nullopt);
    return Loaded::Failure(loaded.error(), loaded.message());
  }
  return DecodeV1(loaded.value(), account_id);
}

}  // namespace playden
