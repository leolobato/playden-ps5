// Linux port: glibc spells the stat timestamps st_*tim, BSD (and the PS5) st_*timespec.
#if defined(__linux__) && !defined(st_mtimespec)
#define st_mtimespec st_mtim
#define st_atimespec st_atim
#define st_ctimespec st_ctim
#endif
#include "runtime/publication_backup.h"
#include "runtime/artifact_hash.h"
#include "platform/managed_directory.h"

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {
using platform::ManagedDirectory;
using platform::DirectoryPolicy;
bool Hex(std::string_view text) {
  for (char ch : text) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  return true;
}
bool Transaction(const std::string& text) {
  if (text.size() != 36) return false;
  bool nonzero = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; }
    else { if (!Hex(std::string_view(text).substr(i, 1))) return false; nonzero |= text[i] != '0'; }
  }
  return nonzero;
}
std::string Filename(const std::string& transaction) { return "registry-backup-" + transaction + ".v1"; }
bool PrivateFile(const struct stat& value) {
  return S_ISREG(value.st_mode) && value.st_uid == geteuid() && value.st_nlink == 1 &&
         (value.st_mode & 07777) == 0600 && value.st_size >= 0;
}
bool Same(const struct stat& a, const struct stat& b) {
  bool same = a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
      a.st_mode == b.st_mode && a.st_uid == b.st_uid && a.st_nlink == b.st_nlink &&
      a.st_mtime == b.st_mtime && a.st_ctime == b.st_ctime;
#if defined(__APPLE__)
  return same && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
  return same && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
}
PublicationBackupLoad Failure(PublicationRecoveryState state, const char* message) {
  return {state, {}, message};
}
Result<RegistryBackupBinding> CreateFailure(StorageError error, const char* message) {
  return Result<RegistryBackupBinding>::Failure(error, message);
}
}

PublicationBackupLoad LoadPublicationRegistryBackup(
    const std::filesystem::path& control, const std::string& transaction,
    const RegistryBackupBinding& binding) {
  if (!Transaction(transaction) || binding.filename != Filename(transaction) ||
      (binding.source_mode != 0600 && binding.source_mode != 0644) ||
      binding.size > kMaxPublicationRegistryBackupBytes || binding.sha256.size() != 64 || !Hex(binding.sha256))
    return Failure(PublicationRecoveryState::kCorrupt, "Invalid frozen registry backup binding.");
  auto directory = ManagedDirectory::Open(control, DirectoryPolicy::kPrivate);
  if (!directory) return Failure(PublicationRecoveryState::kUnavailable, "Private control directory is unavailable.");
  struct stat before{}, opened{}, after{}, named{};
  if (!directory->InspectChild(binding.filename, &before)) {
    const int error = errno;
    if (error == ENOENT) return Failure(PublicationRecoveryState::kAbsent, "Referenced backup is missing.");
    return Failure(error == EINVAL || error == ELOOP ? PublicationRecoveryState::kCorrupt : PublicationRecoveryState::kUnavailable,
                   "Cannot inspect the referenced backup.");
  }
  if (!PrivateFile(before) || static_cast<std::uint64_t>(before.st_size) != binding.size)
    return Failure(PublicationRecoveryState::kCorrupt, "Backup ownership, permissions, type or size changed.");
  const int fd = directory->OpenRegularFile(binding.filename, true, 0600);
  if (fd < 0) return Failure(PublicationRecoveryState::kUnavailable, "Cannot open the private backup.");
  bool valid = fstat(fd, &opened) == 0 && Same(before, opened);
  std::string bytes;
  if (valid) bytes.resize(static_cast<std::size_t>(binding.size));
  std::size_t offset = 0;
  while (valid && offset < bytes.size()) {
    const auto count = read(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { valid = false; break; }
    offset += static_cast<std::size_t>(count);
  }
  char extra{}; ssize_t count = -1;
  if (valid) do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
  valid = valid && count == 0 && fstat(fd, &after) == 0 && Same(opened, after) &&
      directory->InspectChild(binding.filename, &named) && Same(opened, named);
  const bool synced = valid && fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  if (!valid || !synced || !closed || !directory->Sync())
    return Failure(PublicationRecoveryState::kUnavailable, "Backup read, identity or durability could not be confirmed.");
  const auto digest = HashArtifactBytes(bytes);
  if (!digest) return Failure(PublicationRecoveryState::kUnavailable, "Backup hashing is unavailable.");
  if (*digest != binding.sha256)
    return Failure(PublicationRecoveryState::kCorrupt, "Referenced backup digest changed.");
  return {PublicationRecoveryState::kValid, std::move(bytes), {}};
}

Result<RegistryBackupBinding> CreatePublicationRegistryBackup(
    const std::filesystem::path& control, const std::string& transaction,
    std::string_view bytes, unsigned source_mode) {
  if (!Transaction(transaction) || (source_mode != 0600 && source_mode != 0644) ||
      bytes.size() > kMaxPublicationRegistryBackupBytes)
    return CreateFailure(StorageError::kInvalidData, "Invalid registry backup transaction, mode or size.");
  const auto hash = HashArtifactBytes(bytes);
  if (!hash) return CreateFailure(StorageError::kInvalidData, "Registry backup hashing is unavailable.");
  auto directory = ManagedDirectory::Open(control, DirectoryPolicy::kPrivate);
  if (!directory) return CreateFailure(StorageError::kOpenFailed, "Private control directory is unavailable.");
  RegistryBackupBinding binding{Filename(transaction), source_mode, bytes.size(), *hash};
  const int fd = directory->CreateExclusiveFile(binding.filename, 0600);
  if (fd < 0) return CreateFailure(StorageError::kOpenFailed, "Backup collision or creation failure; existing file preserved.");
  bool valid = true; std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { valid = false; break; }
    offset += static_cast<std::size_t>(count);
  }
  const bool synced = valid && fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  if (!valid || !synced || !closed || !directory->Sync())
    return CreateFailure(StorageError::kSyncFailed, "Backup write or durability failed; file retained for reconciliation.");
  auto recovered = LoadPublicationRegistryBackup(control, transaction, binding);
  if (recovered.state != PublicationRecoveryState::kValid || recovered.bytes != bytes)
    return CreateFailure(StorageError::kInvalidData, "Backup readback failed; file retained for reconciliation.");
  return Result<RegistryBackupBinding>::Success(std::move(binding));
}
}  // namespace playden::runtime
