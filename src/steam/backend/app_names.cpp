// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/app_names.h"

#include <algorithm>
#include <vector>

namespace playden::steam {
namespace {

constexpr const char* kPath = "app-names.cache";
// Version 2 added each entry's dlc_for. Version 1 files still load, without their kOther entries.
const Bytes kMagicV1 = {'P', 'D', 'N', 'A', 'M', 'E', 0, 1};
const Bytes kMagic = {'P', 'D', 'N', 'A', 'M', 'E', 0, 2};
constexpr std::size_t kMaxName = 1024;
constexpr std::size_t kMaxIcon = 64;
// Games and demos fill at most kMaxNamedBytes; the other apps' entries are fixed-size and fit beside them.
constexpr std::size_t kMaxNamedBytes = 4 * 1024 * 1024;
constexpr std::size_t kOtherEntryBytes = 4 + 1 + 8 + 2 + 2 + 4;
constexpr std::size_t kMaxBytes = kMaxNamedBytes + kMaxOtherApps * kOtherEntryBytes;

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
    const auto count = Number(2);
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

}  // namespace

AppNames LoadAppNames(const Storage& storage) {
  const auto read = storage.Read(kPath, kMaxBytes);
  if (!read.ok()) return {};
  const Bytes& bytes = read.value();
  if (bytes.size() < kMagic.size() + 8) return {};
  const bool v1 = std::equal(kMagicV1.begin(), kMagicV1.end(), bytes.begin());
  if (!v1 && !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return {};
  const std::size_t body = bytes.size() - 4;
  Reader crc{bytes, body, bytes.size()};
  if (static_cast<std::uint32_t>(crc.Number(4)) != Checksum(bytes, body)) return {};
  Reader reader{bytes, kMagic.size(), body};
  const auto count = reader.Number(4);
  if (!reader.okay || count > kMaxAppNames + kMaxOtherApps) return {};
  AppNames names;
  names.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto id = static_cast<std::uint32_t>(reader.Number(4));
    AppName name;
    const auto kind = reader.Number(1);
    name.learned_at = reader.Number(8);
    name.name = reader.String(kMaxName);
    name.icon = reader.String(kMaxIcon);
    if (!v1) name.dlc_for = static_cast<std::uint32_t>(reader.Number(4));
    if (!reader.okay || !id || kind > 2) return {};
    name.kind = static_cast<AppName::Kind>(kind);
    if (name.kind != AppName::Kind::kOther && name.name.empty()) return {};
    if (v1 && name.kind == AppName::Kind::kOther) continue;
    names[id] = std::move(name);
  }
  if (reader.offset != body) return {};
  return names;
}

bool SaveAppNames(const Storage& storage, const AppNames& names) {
  using Entry = std::pair<std::uint32_t, const AppName*>;
  std::vector<Entry> named, others;
  for (const auto& [id, name] : names) {
    if (!id || name.name.size() > kMaxName || name.icon.size() > kMaxIcon) continue;
    if (name.kind == AppName::Kind::kOther) others.emplace_back(id, &name);
    else if (!name.name.empty()) named.emplace_back(id, &name);
  }
  // The newest stay, as many as Load accepts: kMaxAppNames games and demos in kMaxNamedBytes, and
  // kMaxOtherApps other apps without a name or icon.
  const auto newest = [](const Entry& a, const Entry& b) { return a.second->learned_at > b.second->learned_at; };
  std::sort(named.begin(), named.end(), newest);
  std::size_t bytes = kMagic.size() + 4 + 4;
  std::size_t kept = 0;
  for (; kept < named.size() && kept < kMaxAppNames; ++kept) {
    const auto& name = *named[kept].second;
    const std::size_t size = 4 + 1 + 8 + 2 + name.name.size() + 2 + name.icon.size() + 4;
    if (bytes + size > kMaxNamedBytes) break;
    bytes += size;
  }
  named.resize(kept);
  std::sort(others.begin(), others.end(), newest);
  if (others.size() > kMaxOtherApps) others.resize(kMaxOtherApps);
  std::vector<Entry> entries = std::move(named);
  entries.insert(entries.end(), others.begin(), others.end());
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  Bytes out = kMagic;
  Number(out, entries.size(), 4);
  for (const auto& [id, name] : entries) {
    Number(out, id, 4);
    Number(out, static_cast<std::uint8_t>(name->kind), 1);
    Number(out, name->learned_at, 8);
    // Another app keeps no name or icon, so it takes kOtherEntryBytes.
    const bool other = name->kind == AppName::Kind::kOther;
    Number(out, other ? 0 : name->name.size(), 2);
    if (!other) out.insert(out.end(), name->name.begin(), name->name.end());
    Number(out, other ? 0 : name->icon.size(), 2);
    if (!other) out.insert(out.end(), name->icon.begin(), name->icon.end());
    Number(out, name->dlc_for, 4);
  }
  Number(out, Checksum(out, out.size()), 4);
  return storage.WriteAtomic(kPath, out).ok();
}

}  // namespace playden::steam
