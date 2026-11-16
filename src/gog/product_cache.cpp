// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/product_cache.h"

#include "gog/art_url.h"
#include "gog/session_store.h"
#include "platform/directory_walk.h"
#include "platform/managed_directory.h"
#include "platform/ps5/title_allow_list.h"

#include <cerrno>
#include <dirent.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace playden::gog {
namespace {
namespace fs = std::filesystem;
using platform::DirectoryPolicy;
using platform::ManagedDirectory;

constexpr std::uint8_t kMagic[8] = {'P', 'D', 'G', 'O', 'G', 'P', 0, 1};
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderBytes = sizeof(kMagic) + 1 + 4 + 4;
constexpr std::uint8_t kGameByte = 1;
constexpr std::uint8_t kHiddenByte = 2;

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

void Put32(Bytes& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t Get32(const std::uint8_t* data) {
  return std::uint32_t{data[0]} | std::uint32_t{data[1]} << 8 | std::uint32_t{data[2]} << 16 |
         std::uint32_t{data[3]} << 24;
}

void PutString(Bytes& out, const std::string& text) {
  Put32(out, static_cast<std::uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

bool TakeString(const Bytes& bytes, std::size_t* offset, std::size_t max, std::string* out) {
  if (bytes.size() - *offset < 4) return false;
  const std::size_t size = Get32(bytes.data() + *offset);
  *offset += 4;
  if (size > max || bytes.size() - *offset < size) return false;
  out->assign(reinterpret_cast<const char*>(bytes.data() + *offset), size);
  *offset += size;
  return true;
}

bool Blank(const std::string& text) {
  for (const char c : text)
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
  return true;
}

bool Valid(const CachedProduct& product) {
  if (!product.etag.empty() && !UsableEtag(product.etag)) return false;
  if (product.kind != ProductInfo::Kind::kGame && product.kind != ProductInfo::Kind::kHidden) return false;
  if (product.kind == ProductInfo::Kind::kGame && Blank(product.title)) return false;
  return product.title.size() <= kMaxTitleBytes && product.summary.size() <= kMaxSummaryBytes &&
         product.developer.size() <= kMaxDeveloperBytes && (product.cover.empty() || IsArtUrl(product.cover, ArtKind::kCover)) &&
         (product.hero.empty() || IsArtUrl(product.hero, ArtKind::kHero));
}

std::string Name(std::uint64_t id) { return std::to_string(id); }

// <user root>/gog/products as a private folder; the user root itself is shared on the console, so it is opened as an
// approved tree (as the session store does). Nothing when a folder is missing and create is false, or unsafe.
enum class FolderState { kOpen, kMissing, kUnsafe };

FolderState OpenProducts(const std::string& user_root, bool create, std::optional<ManagedDirectory>* out) {
  const fs::path root(user_root);
  if (user_root.empty() || user_root.find('\0') != std::string::npos || !root.is_absolute() ||
      root.lexically_normal() != root || !root.has_filename())
    return FolderState::kUnsafe;
  errno = 0;
  auto folder = ManagedDirectory::Open(root, DirectoryPolicy::kApprovedTree);
  if (!folder) return errno == ENOENT ? FolderState::kMissing : FolderState::kUnsafe;
  for (const char* name : {kSessionFolderName, kProductsFolderName}) {
    struct stat seen{};
    const bool existed = folder->InspectChild(name, &seen);
    if (!existed) {
      if (errno != ENOENT) return FolderState::kUnsafe;  // a link (ELOOP), or not a folder or file
      if (!create) return FolderState::kMissing;
    }
    auto child = folder->OpenChild(name, DirectoryPolicy::kOpenTree, create, 0777);  // open to FTP (#868)
    if (!child) return FolderState::kUnsafe;
    if (!existed && (!child->Sync() || !folder->Sync())) return FolderState::kUnsafe;
    folder = std::move(child);
  }
  *out = std::move(folder);
  return FolderState::kOpen;
}

}  // namespace

Bytes EncodeProduct(const CachedProduct& product) {
  if (!Valid(product)) return {};
  const std::size_t payload_size = 1 + 6 * 4 + product.etag.size() + product.title.size() + product.summary.size() +
                                   product.developer.size() + product.cover.size() + product.hero.size();
  Bytes out;
  out.reserve(kHeaderBytes + payload_size);
  out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
  out.push_back(kVersion);
  Put32(out, static_cast<std::uint32_t>(payload_size));
  Put32(out, 0);  // the checksum, filled in below
  PutString(out, product.etag);
  out.push_back(product.kind == ProductInfo::Kind::kGame ? kGameByte : kHiddenByte);
  PutString(out, product.title);
  PutString(out, product.summary);
  PutString(out, product.developer);
  PutString(out, product.cover);
  PutString(out, product.hero);
  const auto crc = Crc32(out.data() + kHeaderBytes, payload_size);
  for (int i = 0; i < 4; ++i) out[kHeaderBytes - 4 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
  return out;
}

bool DecodeProduct(const Bytes& bytes, CachedProduct* product) {
  if (!product || bytes.size() < kHeaderBytes || bytes.size() > kMaxProductFileBytes) return false;
  for (std::size_t i = 0; i < sizeof(kMagic); ++i)
    if (bytes[i] != kMagic[i]) return false;
  if (bytes[sizeof(kMagic)] != kVersion) return false;
  const std::uint32_t payload_size = Get32(bytes.data() + sizeof(kMagic) + 1);
  const std::uint32_t crc = Get32(bytes.data() + sizeof(kMagic) + 5);
  if (payload_size != bytes.size() - kHeaderBytes) return false;
  if (Crc32(bytes.data() + kHeaderBytes, payload_size) != crc) return false;
  CachedProduct decoded;
  std::size_t offset = kHeaderBytes;
  if (!TakeString(bytes, &offset, kMaxEtagBytes, &decoded.etag) || offset >= bytes.size()) return false;
  const std::uint8_t kind = bytes[offset++];
  if (kind != kGameByte && kind != kHiddenByte) return false;
  decoded.kind = kind == kGameByte ? ProductInfo::Kind::kGame : ProductInfo::Kind::kHidden;
  if (!TakeString(bytes, &offset, kMaxTitleBytes, &decoded.title) ||
      !TakeString(bytes, &offset, kMaxSummaryBytes, &decoded.summary) ||
      !TakeString(bytes, &offset, kMaxDeveloperBytes, &decoded.developer) ||
      !TakeString(bytes, &offset, 1024, &decoded.cover) || !TakeString(bytes, &offset, 1024, &decoded.hero) ||
      offset != bytes.size() || !Valid(decoded))
    return false;
  *product = std::move(decoded);
  return true;
}

ProductCache::ProductCache(std::string user_root) : user_root_(std::move(user_root)) {}

std::optional<CachedProduct> ProductCache::Load(std::uint64_t id) const {
  if (id == 0) return std::nullopt;
  const auto uid = ps5::HoldEffectiveUid();
  std::optional<ManagedDirectory> folder;
  if (OpenProducts(user_root_, false, &folder) != FolderState::kOpen) return std::nullopt;
  // A regular file of this user at any mode (written 0666, 0600 before #868). A link is refused and
  // never followed.
  const int fd = folder->OpenRegularFile(Name(id), true);
  if (fd < 0) return std::nullopt;
  struct stat info{};
  bool valid = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
               info.st_uid == ::geteuid() &&
               static_cast<std::uint64_t>(info.st_size) <= kMaxProductFileBytes;
  Bytes bytes;
  std::uint8_t buffer[4096];
  while (valid) {
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || static_cast<std::size_t>(count) > kMaxProductFileBytes - bytes.size()) {
      valid = false;
      break;
    }
    if (count == 0) break;
    bytes.insert(bytes.end(), buffer, buffer + count);
  }
  ::close(fd);
  CachedProduct product;
  if (!valid || !DecodeProduct(bytes, &product)) return std::nullopt;
  return product;
}

bool ProductCache::Save(std::uint64_t id, const CachedProduct& product) const {
  if (id == 0) return false;
  const Bytes bytes = EncodeProduct(product);
  if (bytes.empty()) return false;
  const auto uid = ps5::HoldEffectiveUid();
  std::optional<ManagedDirectory> folder;
  if (OpenProducts(user_root_, true, &folder) != FolderState::kOpen) return false;
  return Storage(folder->path().string()).OpenTree().WriteDurable(Name(id), bytes).ok();
}

bool ProductCache::Remove(std::uint64_t id) const {
  if (id == 0) return false;
  const auto uid = ps5::HoldEffectiveUid();
  std::optional<ManagedDirectory> folder;
  switch (OpenProducts(user_root_, false, &folder)) {
    case FolderState::kMissing: return true;
    case FolderState::kUnsafe: return false;
    case FolderState::kOpen: break;
  }
  struct stat info{};
  if (!folder->InspectChild(Name(id), &info)) return errno == ENOENT;
  if (!S_ISREG(info.st_mode)) return false;
  return folder->RemoveRegularFile(Name(id)) && folder->Sync();
}

bool ProductCache::Clear() const {
  const auto uid = ps5::HoldEffectiveUid();
  std::optional<ManagedDirectory> folder;
  switch (OpenProducts(user_root_, false, &folder)) {
    case FolderState::kMissing: return true;
    case FolderState::kUnsafe: return false;
    case FolderState::kOpen: break;
  }
  // A fresh descriptor by absolute path for the listing (no descriptor-relative open on the console), checked to be
  // the folder just validated.
  const int walk = platform::OpenDirectoryWalk(folder->path());
  if (walk < 0) return false;
  struct stat walked{}, held{};
  if (::fstat(walk, &walked) != 0 || ::fstat(folder->fd(), &held) != 0 || walked.st_dev != held.st_dev ||
      walked.st_ino != held.st_ino) {
    ::close(walk);
    return false;
  }
  DIR* listing = ::fdopendir(walk);
  if (!listing) {
    ::close(walk);
    return false;
  }
  std::vector<std::string> names;
  bool ok = true;
  for (;;) {
    errno = 0;
    const dirent* entry = ::readdir(listing);
    if (!entry) {
      if (errno != 0) ok = false;
      break;
    }
    const std::string name = entry->d_name;
    if (name != "." && name != "..") names.push_back(name);
  }
  ::closedir(listing);
  bool removed = false;
  for (const auto& name : names) {
    struct stat info{};
    if (!folder->InspectChild(name, &info)) {
      if (errno != ENOENT) ok = false;  // a link (ELOOP) is never followed or removed
      continue;
    }
    if (!S_ISREG(info.st_mode) || !folder->RemoveRegularFile(name)) {
      ok = false;
      continue;
    }
    removed = true;
  }
  if (removed && !folder->Sync()) ok = false;
  return ok;
}

}  // namespace playden::gog
