// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/collections_cache.h"

#include <algorithm>
#include <unordered_set>

namespace playden {
namespace {
constexpr std::size_t kMaximumFileBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumGameValue = 48;
const Bytes kMagic = {'P', 'D', 'C', 'O', 'L', 0, 1, 0};

std::uint32_t Checksum(const Bytes& bytes, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
void Number(Bytes& out, std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    out.push_back(static_cast<std::uint8_t>(value));
    value >>= 8;
  }
}
void String(Bytes& out, const std::string& value) {
  Number(out, value.size(), 4);
  out.insert(out.end(), value.begin(), value.end());
}
struct Reader {
  const Bytes& bytes;
  std::size_t offset = 0, limit = 0;
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
    const auto count = Number(4);
    if (!okay || count > maximum || offset > limit || count > limit - offset) {
      okay = false;
      return {};
    }
    std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
    offset += count;
    return value;
  }
};
Result<void> Invalid(const char* message) { return Result<void>::Failure(StorageError::kInvalidData, message); }
bool ValidName(const std::string& name) { return !name.empty() && name.size() <= CollectionsCache::kMaximumNameBytes; }
}  // namespace

Result<void> CollectionsCache::Save(std::uint64_t account, const std::vector<LibraryCollection>& collections) const {
  if (!account) return Invalid("Invalid collections account identifier.");
  if (collections.size() > kMaximumCollections) return Invalid("Too many collections to save.");
  Bytes encoded = kMagic;
  Number(encoded, account, 8);
  Number(encoded, collections.size(), 4);
  std::unordered_set<std::string> ids;
  std::size_t members = 0;
  for (const auto& collection : collections) {
    if (!ValidLibraryCollectionId(collection.id) || !ids.insert(collection.id).second || !ValidName(collection.name))
      return Invalid("Invalid or duplicate collection.");
    members += collection.members.size();
    if (members > kMaximumMembers) return Invalid("Too many collection members to save.");
    String(encoded, collection.id);
    String(encoded, collection.name);
    Number(encoded, collection.members.size(), 4);
    for (const auto& id : collection.members) {
      if (!id.valid() || id.value.size() > kMaximumGameValue) return Invalid("Invalid collection member.");
      Number(encoded, static_cast<std::uint8_t>(id.store), 1);
      String(encoded, id.value);
    }
    if (encoded.size() > kMaximumFileBytes - 4) return Invalid("Collections exceed their storage limit.");
  }
  Number(encoded, Checksum(encoded, encoded.size()), 4);
  return storage_.WriteAtomic(Path(account), encoded);
}

Result<std::vector<LibraryCollection>> CollectionsCache::Load(std::uint64_t account) const {
  using Loaded = Result<std::vector<LibraryCollection>>;
  const auto invalid = [] {
    return Loaded::Failure(StorageError::kInvalidData, "Saved collections are damaged or belong to another account.");
  };
  if (!account) return invalid();
  const auto read = storage_.Read(Path(account), kMaximumFileBytes);
  if (!read.ok()) return Loaded::Failure(read.error(), read.message());
  const auto& bytes = read.value();
  if (bytes.size() < kMagic.size() + 4 || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return invalid();
  Reader tail{bytes, bytes.size() - 4, bytes.size()};
  if (tail.Number(4) != Checksum(bytes, bytes.size() - 4)) return invalid();
  Reader reader{bytes, kMagic.size(), bytes.size() - 4};
  if (reader.Number(8) != account) return invalid();
  const auto count = reader.Number(4);
  if (!reader.okay || count > kMaximumCollections) return invalid();
  std::vector<LibraryCollection> collections;
  std::unordered_set<std::string> ids;
  std::size_t members = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    LibraryCollection collection;
    collection.id = reader.String(64);
    collection.name = reader.String(kMaximumNameBytes);
    const auto member_count = reader.Number(4);
    if (!reader.okay || !ValidLibraryCollectionId(collection.id) || !ids.insert(collection.id).second ||
        !ValidName(collection.name) || member_count > kMaximumMembers - members)
      return invalid();
    members += member_count;
    for (std::uint64_t m = 0; m < member_count; ++m) {
      GameId id;
      // An unknown store byte makes an invalid GameId, which is refused.
      id.store = static_cast<Store>(reader.Number(1));
      id.value = reader.String(kMaximumGameValue);
      if (!reader.okay || !id.valid()) return invalid();
      collection.members.push_back(std::move(id));
    }
    collections.push_back(std::move(collection));
  }
  if (reader.offset != reader.limit) return invalid();
  return Loaded::Success(std::move(collections));
}

Result<void> CollectionsCache::Clear(std::uint64_t account) const {
  if (!account) return Invalid("Invalid collections account identifier.");
  const auto path = Path(account);
  const auto final_result = storage_.Remove(path);
  const auto temporary_result = storage_.Remove(path + ".tmp");
  if (!final_result.ok()) return final_result;
  return temporary_result;
}

}  // namespace playden
