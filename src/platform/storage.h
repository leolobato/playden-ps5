#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace playden {

using Bytes = std::vector<std::uint8_t>;

enum class StorageError {
  kNone,
  kInvalidPath,
  kNotFound,
  kOpenFailed,
  kReadFailed,
  kWriteFailed,
  kRenameFailed,
  kInvalidData,
  kPermissionFailed,
  kRemoveFailed,
  kSyncFailed,
};

template <typename T>
class Result {
 public:
  static Result Success(T value) {
    return Result(std::move(value), StorageError::kNone, std::string());
  }

  static Result Failure(StorageError error, std::string message) {
    return Result(std::nullopt, error, std::move(message));
  }

  bool ok() const { return value_.has_value(); }
  const T& value() const { return value_.value(); }
  T& value() { return value_.value(); }
  StorageError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  Result(T value, StorageError error, std::string message)
      : value_(std::move(value)), error_(error), message_(std::move(message)) {}

  Result(std::nullopt_t, StorageError error, std::string message)
      : value_(std::nullopt), error_(error), message_(std::move(message)) {}

  std::optional<T> value_;
  StorageError error_;
  std::string message_;
};

template <>
class Result<void> {
 public:
  static Result Success() {
    return Result(true, StorageError::kNone, std::string());
  }

  static Result Failure(StorageError error, std::string message) {
    return Result(false, error, std::move(message));
  }

  bool ok() const { return ok_; }
  StorageError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  Result(bool ok, StorageError error, std::string message)
      : ok_(ok), error_(error), message_(std::move(message)) {}

  bool ok_;
  StorageError error_;
  std::string message_;
};

struct StoredFile {
  std::string name;
  std::int64_t modified_ns = 0;  // modification time, nanoseconds since the epoch
};

class Storage {
 public:
  // The platform supplies an app-owned root. Relative path validation does not
  // defend against symlinks created by another writer inside that root.
  explicit Storage(std::string root);

  // Idempotent worker-side setup of the app-owned root; parent must exist.
  Result<void> EnsureRootDirectory() const;

  // Reads are bounded so a damaged file cannot consume unbounded memory.
  static constexpr std::size_t kMaxReadBytes = 1024 * 1024;
  Result<Bytes> Read(const std::string& relative_path,
                     std::size_t max_bytes = kMaxReadBytes) const;

  // A single app writer is assumed. The temporary file is synced before the
  // rename and the folder after it (best effort), so a crash or power loss
  // leaves the old contents or the new ones, not an empty file. The file is
  // 0666 where the filesystem keeps modes, so FTP can read and replace it.
  Result<void> WriteAtomic(const std::string& relative_path,
                           const Bytes& bytes) const;

  // WriteAtomic with the effective UID held (ps5::HoldEffectiveUid), as WriteAtomicPrivate does: for Playden's
  // own data that used to be owner-only and is open now (#868). Not for a caller that holds other locks.
  Result<void> WriteAtomicOpen(const std::string& relative_path, const Bytes& bytes) const;

  // Creates the temporary file with owner-only permissions before writing any
  // bytes. Platforms without a supported permission primitive report failure.
  Result<void> WriteAtomicPrivate(const std::string& relative_path,
                                  const Bytes& bytes) const;

  // Narrow journal operations: existing root/parents must be owned 0700 (any mode after
  // OpenTree) and
  // have no symlink components. Write creates an exclusive 0600 file, syncs it,
  // closes, renames, then syncs the directory. Failure can follow publication;
  // callers must stop and reload/reconcile before further effects. Failed
  // temporary files are retained. Read accepts owned legacy 0644 or new 0600
  // regular files and syncs the file/directory before accepting recovery.
  Result<void> WriteDurable(const std::string& relative_path, const Bytes& bytes) const;
  Result<Bytes> ReadDurable(const std::string& relative_path,
                            std::size_t max_bytes = kMaxReadBytes) const;
  // Same durability/private-root contract, with checked absent destination.
  // Cooperating writers must be serialized; this is not a kernel atomic
  // no-replace primitive against an adversarial concurrent writer.
  Result<void> WriteDurableExclusive(const std::string& relative_path,
                                      const Bytes& bytes) const;
  // Removes only an owned regular file and syncs its parent. Missing also syncs
  // the parent. A sync failure may follow removal and requires reconciliation.
  Result<void> RemoveDurable(const std::string& relative_path) const;

  // Regular files directly in the root (no links, no folders), in no particular
  // order. A missing root lists nothing.
  Result<std::vector<StoredFile>> ListFiles() const;

  // Missing files are already removed and therefore succeed.
  Result<void> Remove(const std::string& relative_path) const;

  // Journal operations also admit a 0711 root (others may pass through, never
  // list): a bottle whose way to its user profile is open. Parents stay 0700.
  Storage& AllowTraversableRoot() { traversable_root_ = true; return *this; }
  // Journal operations on Playden's open data (#868): the root and parents are the app user's at
  // any mode (DirectoryPolicy::kOpenTree), and files are written 0666, or 0600 for a secret
  // (platform/data_roots.h), so the console owner can delete them over FTP.
  Storage& OpenTree() { open_tree_ = true; return *this; }

 private:
  Result<std::string> Resolve(const std::string& relative_path) const;
  Result<void> WriteAtomicImpl(const std::string& relative_path,
                               const Bytes& bytes, bool private_file, bool* retry = nullptr) const;

  Result<void> WriteDurableImpl(const std::string& relative_path,
                                 const Bytes& bytes, bool replace_existing) const;

  std::string root_;
  bool traversable_root_ = false;
  bool open_tree_ = false;
};

}  // namespace playden
