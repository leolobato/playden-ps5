// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/session_store.h"

#include "platform/directory_walk.h"
#include "platform/managed_directory.h"
#include "platform/ps5/title_allow_list.h"
#include "platform/secret.h"

#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <filesystem>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace playden::gog {
namespace {
namespace fs = std::filesystem;
using platform::DirectoryPolicy;
using platform::ManagedDirectory;
using platform::ZeroizeSecret;

constexpr std::uint8_t kMagic[8] = {'P', 'D', 'G', 'O', 'G', 'S', 0, 1};
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderBytes = sizeof(kMagic) + 1 + 4 + 4;
constexpr std::size_t kMaxTokenBytes = 4096;
constexpr std::size_t kMaxUserIdBytes = 20;
constexpr std::size_t kMaxUsernameBytes = 256;

// CRC-32 (IEEE 802.3, reflected, as zip and PNG use it).
std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

bool ValidToken(const std::string& token) {
  if (token.empty() || token.size() > kMaxTokenBytes) return false;
  for (const char c : token)
    if (c < '!' || c > '~') return false;
  return true;
}

// As GOG's answers give it (auth.cpp's rule): 1 to 20 digits, no leading zero.
bool ValidUserId(const std::string& id) {
  if (id.empty() || id.size() > kMaxUserIdBytes || id.front() == '0') return false;
  for (const char c : id)
    if (c < '0' || c > '9') return false;
  return true;
}

bool ValidUsername(const std::string& name) {
  if (name.empty() || name.size() > kMaxUsernameBytes) return false;
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) return false;
  }
  return true;
}

bool ValidSession(const StoredSession& session) {
  return ValidToken(session.refresh_token) && ValidUserId(session.user_id) && ValidUsername(session.username);
}

void Put16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void Put32(Bytes& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t Get32(const std::uint8_t* data) {
  return std::uint32_t{data[0]} | std::uint32_t{data[1]} << 8 | std::uint32_t{data[2]} << 16 |
         std::uint32_t{data[3]} << 24;
}

void PutString(Bytes& out, const std::string& text) {
  Put16(out, static_cast<std::uint16_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

// One u16-LE-length-prefixed string at *offset, within [0, end).
bool TakeString(const Bytes& bytes, std::size_t end, std::size_t* offset, std::string* out) {
  if (end - *offset < 2) return false;
  const std::size_t size = std::size_t{bytes[*offset]} | std::size_t{bytes[*offset + 1]} << 8;
  *offset += 2;
  if (end - *offset < size) return false;
  out->assign(reinterpret_cast<const char*>(bytes.data() + *offset), size);
  *offset += size;
  return true;
}

enum class FolderState { kOpen, kMissing, kUnsafe };

// <user root>/gog as a private folder. The user root itself is shared on the console (user/ is 01777, see
// runtime/self_heal.cpp), so it is opened as an approved tree: no link in any component, owner and mode as found.
// Missing: the user root or the gog folder is absent and create is false (or the user root is absent).
FolderState OpenFolder(const std::string& user_root, bool create, std::optional<ManagedDirectory>* out) {
  const fs::path root(user_root);
  if (user_root.empty() || user_root.find('\0') != std::string::npos || !root.is_absolute() ||
      root.lexically_normal() != root || !root.has_filename())
    return FolderState::kUnsafe;
  errno = 0;
  auto user = ManagedDirectory::Open(root, DirectoryPolicy::kApprovedTree);
  if (!user) return errno == ENOENT ? FolderState::kMissing : FolderState::kUnsafe;
  struct stat seen{};
  const bool existed = user->InspectChild(kSessionFolderName, &seen);
  if (!existed) {
    if (errno != ENOENT) return FolderState::kUnsafe;  // a link (ELOOP) or something other than a folder or file
    if (!create) return FolderState::kMissing;
  }
  // Open to FTP (#868); the session file in it is a secret (0600, platform/data_roots.h).
  auto folder = user->OpenChild(kSessionFolderName, DirectoryPolicy::kOpenTree, create, 0777);
  if (!folder) return FolderState::kUnsafe;
  if (!existed && (!folder->Sync() || !user->Sync())) return FolderState::kUnsafe;
  *out = std::move(folder);
  return FolderState::kOpen;
}

const char* ErrorKind(StorageError error) {
  switch (error) {
    case StorageError::kInvalidPath: return "invalid_path";
    case StorageError::kNotFound: return "not_found";
    case StorageError::kOpenFailed: return "open_failed";
    case StorageError::kReadFailed: return "read_failed";
    case StorageError::kWriteFailed: return "write_failed";
    case StorageError::kRenameFailed: return "rename_failed";
    case StorageError::kInvalidData: return "invalid_data";
    case StorageError::kPermissionFailed: return "permission_failed";
    case StorageError::kRemoveFailed: return "remove_failed";
    case StorageError::kSyncFailed: return "sync_failed";
    case StorageError::kNone: break;
  }
  return "failed";
}

SaveResult Ok() {
  SaveResult result;
  result.ok = true;
  return result;
}

SaveResult Failed(const char* reason) {
  SaveResult result;
  result.reason = reason;
  return result;
}

LoadResult Loaded(LoadStatus status, const char* reason = "") {
  LoadResult result;
  result.status = status;
  result.reason = reason;
  return result;
}

bool RemoveSessionFile(const std::string& user_root) {
  std::optional<ManagedDirectory> folder;
  switch (OpenFolder(user_root, false, &folder)) {
    case FolderState::kMissing: return true;
    case FolderState::kUnsafe: return false;
    case FolderState::kOpen: break;
  }
  struct stat info{};
  if (!folder->InspectChild(kSessionFileName, &info)) return errno == ENOENT;
  if (!S_ISREG(info.st_mode)) return false;
  return folder->RemoveRegularFile(kSessionFileName) && folder->Sync();
}

bool RemoveLeftoverFiles(const std::string& user_root) {
  std::optional<ManagedDirectory> folder;
  switch (OpenFolder(user_root, false, &folder)) {
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
  const std::string prefix = kSessionLeftoverPrefix;
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
    if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) names.push_back(name);
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

}  // namespace

void StoredSession::Wipe() {
  ZeroizeSecret(refresh_token);
  ZeroizeSecret(user_id);
  ZeroizeSecret(username);
}

Bytes EncodeSession(const StoredSession& session) {
  if (!ValidSession(session)) return {};
  const std::size_t payload_size =
      6 + session.refresh_token.size() + session.user_id.size() + session.username.size();
  Bytes out;
  // The exact size up front: no growth leaves a copy of the token behind in a freed buffer.
  out.reserve(kHeaderBytes + payload_size);
  out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
  out.push_back(kVersion);
  Put32(out, static_cast<std::uint32_t>(payload_size));
  Put32(out, 0);  // the checksum, filled in below
  PutString(out, session.refresh_token);
  PutString(out, session.user_id);
  PutString(out, session.username);
  const auto crc = Crc32(out.data() + kHeaderBytes, payload_size);
  for (int i = 0; i < 4; ++i) out[kHeaderBytes - 4 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
  return out;
}

bool DecodeSession(const Bytes& bytes, StoredSession* session) {
  if (!session || bytes.size() < kHeaderBytes || bytes.size() > kMaxSessionFileBytes) return false;
  for (std::size_t i = 0; i < sizeof(kMagic); ++i)
    if (bytes[i] != kMagic[i]) return false;
  if (bytes[sizeof(kMagic)] != kVersion) return false;
  const std::uint32_t payload_size = Get32(bytes.data() + sizeof(kMagic) + 1);
  const std::uint32_t crc = Get32(bytes.data() + sizeof(kMagic) + 5);
  if (payload_size != bytes.size() - kHeaderBytes) return false;  // no trailing bytes, no truncation
  if (Crc32(bytes.data() + kHeaderBytes, payload_size) != crc) return false;
  StoredSession decoded;
  std::size_t offset = kHeaderBytes;
  if (!TakeString(bytes, bytes.size(), &offset, &decoded.refresh_token) ||
      !TakeString(bytes, bytes.size(), &offset, &decoded.user_id) ||
      !TakeString(bytes, bytes.size(), &offset, &decoded.username) || offset != bytes.size() ||
      !ValidSession(decoded))
    return false;  // decoded wipes itself
  session->Wipe();
  *session = decoded;
  return true;
}

SessionStore::SessionStore(Options options)
    : user_root_(std::move(options.user_root)), persist_(options.persist), write_(std::move(options.write)) {
  if (!write_)
    write_ = [](const Storage& storage, const std::string& name, const Bytes& bytes) {
      return storage.WriteDurable(name, bytes);
    };
}

SessionStore::~SessionStore() = default;

bool SessionStore::persist() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return persist_;
}

bool SessionStore::RemoveLeftovers() {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto uid = ps5::HoldEffectiveUid();
  return RemoveLeftoversLocked();
}

bool SessionStore::RemoveLeftoversLocked() { return RemoveLeftoverFiles(user_root_); }

bool SessionStore::RemoveFileLocked() { return RemoveSessionFile(user_root_); }

LoadResult SessionStore::ReadLocked() {
  std::optional<ManagedDirectory> folder;
  switch (OpenFolder(user_root_, false, &folder)) {
    case FolderState::kMissing: return Loaded(LoadStatus::kNone);
    case FolderState::kUnsafe: return Loaded(LoadStatus::kUnavailable, "folder_unavailable");
    case FolderState::kOpen: break;
  }
  struct stat info{};
  if (!folder->InspectChild(kSessionFileName, &info))
    return errno == ENOENT ? Loaded(LoadStatus::kNone) : Loaded(LoadStatus::kUnavailable, "file_unsafe");
  // Only what this store writes: a regular file of this user, 0600. A link was refused above and is never followed.
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 07777) != 0600)
    return Loaded(LoadStatus::kUnavailable, "file_unsafe");
  if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > kMaxSessionFileBytes)
    return Loaded(LoadStatus::kCorrupt, "oversized");
  auto read = Storage(folder->path().string()).OpenTree().ReadDurable(kSessionFileName, kMaxSessionFileBytes);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) return Loaded(LoadStatus::kNone);
    return Loaded(LoadStatus::kUnavailable, ErrorKind(read.error()));
  }
  LoadResult result;
  const bool decoded = DecodeSession(read.value(), &result.session);
  ZeroizeSecret(read.value());
  if (!decoded) return Loaded(LoadStatus::kCorrupt, "corrupt");
  result.status = LoadStatus::kOk;
  return result;
}

LoadResult SessionStore::Load() {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto uid = ps5::HoldEffectiveUid();
  // Startup: what a failed write left beside the file goes first, whatever the setting.
  const bool leftovers = RemoveLeftoversLocked();
  if (!persist_) {
    // Deleted unread: the player asked for no token on disk.
    const bool removed = RemoveFileLocked();
    if (has_memory_) {
      LoadResult result;
      result.status = LoadStatus::kOk;
      result.session = memory_;
      return result;
    }
    if (!removed || !leftovers) return Loaded(LoadStatus::kUnavailable, "remove_failed");
    return Loaded(LoadStatus::kNone);
  }
  // A session saved earlier in this process is the newest sign-in.
  if (has_memory_) {
    LoadResult result;
    result.status = LoadStatus::kOk;
    result.session = memory_;
    return result;
  }
  auto result = ReadLocked();
  if (result.status == LoadStatus::kCorrupt) {
    // Removed, so the next start shows signed out instead of failing again.
    RemoveFileLocked();
    RemoveLeftoversLocked();
  } else if (result.status == LoadStatus::kOk) {
    memory_.Wipe();
    memory_ = result.session;
    has_memory_ = true;
  }
  return result;
}

SaveResult SessionStore::WriteLocked(const StoredSession& session) {
  std::optional<ManagedDirectory> folder;
  if (OpenFolder(user_root_, true, &folder) != FolderState::kOpen) return Failed("folder_unavailable");
  Bytes bytes = EncodeSession(session);
  if (bytes.empty()) return Failed("invalid_session");
  errno = 0;
  const auto written = write_(Storage(folder->path().string()).OpenTree(), kSessionFileName, bytes);
  const int error = errno;
  ZeroizeSecret(bytes);
  if (written.ok()) return Ok();
  SaveResult result = Failed(ErrorKind(written.error()));
  result.storage_full = error == ENOSPC
#ifdef EDQUOT
                        || error == EDQUOT
#endif
      ;
  folder.reset();
  // A failed sync can follow the rename, so the new file may already be in place: remove it and every temporary
  // file, and the next start shows signed out. The memory copy keeps serving this process.
  RemoveFileLocked();
  RemoveLeftoversLocked();
  return result;
}

SaveResult SessionStore::Save(const StoredSession& session) {
  if (!ValidSession(session)) return Failed("invalid_session");
  std::lock_guard<std::mutex> lock(mutex_);
  const auto uid = ps5::HoldEffectiveUid();
  memory_.Wipe();
  memory_ = session;
  has_memory_ = true;
  if (persist_) return WriteLocked(memory_);
  // Kept in memory only. A file from before goes, as at startup.
  const bool removed = RemoveFileLocked();
  const bool leftovers = RemoveLeftoversLocked();
  return removed && leftovers ? Ok() : Failed("remove_failed");
}

SaveResult SessionStore::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto uid = ps5::HoldEffectiveUid();
  memory_.Wipe();
  has_memory_ = false;
  const bool removed = RemoveFileLocked();
  const bool leftovers = RemoveLeftoversLocked();
  return removed && leftovers ? Ok() : Failed("remove_failed");
}

SaveResult SessionStore::SetPersist(bool persist) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto uid = ps5::HoldEffectiveUid();
  const bool was = persist_;
  persist_ = persist;
  if (persist) {
    if (was || !has_memory_) return Ok();
    return WriteLocked(memory_);
  }
  // The session keeps working until Playden closes: read it into memory first when it was never loaded.
  if (!has_memory_) {
    auto loaded = ReadLocked();
    if (loaded.status == LoadStatus::kOk) {
      memory_ = loaded.session;
      has_memory_ = true;
    }
  }
  const bool removed = RemoveFileLocked();
  const bool leftovers = RemoveLeftoversLocked();
  return removed && leftovers ? Ok() : Failed("remove_failed");
}

bool SessionStore::ForgetIfNotPersisted(const std::string& user_root) {
  Options options;
  options.user_root = user_root;
  options.persist = false;
  SessionStore store(std::move(options));
  return store.Clear().ok;
}

}  // namespace playden::gog
