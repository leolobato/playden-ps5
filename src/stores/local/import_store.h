#pragma once
// imports.json: the Windows games the player added from a drive or the drop folder, and the ones
// they removed (so adding the same folder again brings back its ID, bottle and settings).
//
// A game is named by `volume` (an install volume ID: internal, m2, extended, usb0 to usb7) and
// `folder`, relative to that volume's mount (scanner.h ImportRoots): "playden/import/Delores" on
// usb0 is /mnt/usb0/playden/import/Delores; the drop folder's games are volume "internal",
// folder "playden-import/<name>" (/data/playden-import/<name>). The store never touches the game
// files; callers check the folder and program with the scanner before adding.
#include "platform/storage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace playden::stores::local {

enum class ImportState {
  kInPlace,     // plays from the drive folder
  kCopyQueued,  // a copy into Playden waits; still plays from the drive folder if it did
  kCopying,
  kCopied,      // plays from <games_root>/local-<id>
};
const char* ImportStateKey(ImportState state);

struct ImportEntry {
  std::string id;  // lower-case random UUID
  std::string title;
  std::string volume;
  std::string folder;
  std::string program;  // relative to the folder, '/' separators
  ImportState state = ImportState::kInPlace;
  std::uint64_t added_at = 0;  // seconds since the epoch
  std::string failure;  // the last copy's failure (or what it left behind), empty when none
};

struct RemovedImport {
  std::string id;
  std::string volume;
  std::string folder;
  std::string program;
};

enum class ImportError {
  kNone,
  kNotLoaded,
  kIo,
  kMalformed,
  kUnsupportedVersion,
  kBounds,
  kInvalid,     // a field the store refuses (volume, folder, program, title)
  kDuplicate,   // the folder is already in the library
  kNotFound,
  kWrongState,  // the transition isn't in the state table
  kNoRandom,    // no ID could be made
};

template <typename T>
class ImportResult {
 public:
  static ImportResult Success(T value) { return ImportResult(std::move(value), ImportError::kNone, {}); }
  static ImportResult Failure(ImportError error, std::string message) {
    return ImportResult(std::nullopt, error, std::move(message));
  }
  bool ok() const { return value_.has_value(); }
  const T& value() const { return *value_; }
  ImportError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  ImportResult(std::optional<T> value, ImportError error, std::string message)
      : value_(std::move(value)), error_(error), message_(std::move(message)) {}
  std::optional<T> value_;
  ImportError error_;
  std::string message_;
};

template <>
class ImportResult<void> {
 public:
  static ImportResult Success() { return ImportResult(ImportError::kNone, {}); }
  static ImportResult Failure(ImportError error, std::string message) { return ImportResult(error, std::move(message)); }
  bool ok() const { return error_ == ImportError::kNone; }
  ImportError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  ImportResult(ImportError error, std::string message) : error_(error), message_(std::move(message)) {}
  ImportError error_;
  std::string message_;
};

// Lower-case canonical UUID text (8-4-4-4-12 hex digits).
bool IsImportId(const std::string& id);
// A random (version 4) UUID from /dev/urandom, or nothing (platform/random_id.h).
std::optional<std::string> RandomImportId();

class ImportStore {
 public:
  static constexpr std::uint32_t kVersion = 1;
  static constexpr std::size_t kMaxFileBytes = 256 * 1024;
  static constexpr std::size_t kMaxGames = 256;
  static constexpr std::size_t kMaxRemoved = 256;  // the oldest removed entry goes first
  static constexpr const char* kDropFolderVolume = "internal";

  using IdSource = std::function<std::optional<std::string>()>;
  using Clock = std::function<std::uint64_t()>;

  // `storage` is the private runtime state folder (the one holding installs.json); writes are
  // durable private writes, which hold the effective-UID lock root raises use. Empty `ids` and
  // `clock` use RandomImportId and the system time.
  explicit ImportStore(Storage& storage, std::string relative_path = "imports.json", IdSource ids = {},
                       Clock clock = {});

  // A missing file loads empty. A damaged or newer file fails and leaves the store unloaded: every
  // change is then refused (kNotLoaded), so the file is never replaced by an empty one.
  ImportResult<void> Load();
  ImportResult<void> Save();
  bool loaded() const;

  std::vector<ImportEntry> Games() const;  // in the order they were added
  std::vector<RemovedImport> Removed() const;
  std::optional<ImportEntry> Find(const std::string& id) const;
  std::optional<ImportEntry> FindByFolder(const std::string& volume, const std::string& folder) const;
  std::optional<RemovedImport> FindRemoved(const std::string& volume, const std::string& folder) const;

  // Adds a folder, playing in place. A folder on the removed list gets its old ID back; one already
  // in the library is refused (kDuplicate).
  ImportResult<ImportEntry> Add(const std::string& volume, const std::string& folder, const std::string& title,
                                const std::string& program);
  // The program Game settings chose (a program check precedes it).
  ImportResult<ImportEntry> SetProgram(const std::string& id, const std::string& program);

  // State table. Every call refuses (kWrongState) a move the table doesn't have, and writes the file.
  // in_place -> copy_queued (clears the last failure).
  ImportResult<ImportEntry> QueueCopy(const std::string& id);
  // copy_queued -> copying.
  ImportResult<ImportEntry> StartCopy(const std::string& id);
  // copying -> copied.
  ImportResult<ImportEntry> FinishCopy(const std::string& id);
  // copy_queued/copying -> in_place, keeping `failure` (empty for a plain cancel). A drop-folder
  // game too: its folder stays, and it plays in place again.
  ImportResult<ImportEntry> CancelCopy(const std::string& id, const std::string& failure = {});
  // Keeps a sentence on an entry without changing its state (a copied drop-folder game whose
  // drop-folder files could not be deleted).
  ImportResult<ImportEntry> NoteFailure(const std::string& id, const std::string& failure);
  // in_place -> removed list (its files stay). A copied game must be uninstalled first; a copy in
  // progress must be cancelled first.
  ImportResult<void> Remove(const std::string& id);
  // copied -> removed list, once its files in Playden are deleted (the bottle stays like any kept data).
  ImportResult<void> Uninstalled(const std::string& id);

 private:
  ImportResult<void> Persist();
  ImportEntry* Mutable(const std::string& id);
  void MoveToRemoved(std::size_t index);
  ImportResult<ImportEntry> Transition(const std::string& id, ImportState from, ImportState to);

  Storage& storage_;
  std::string relative_path_;
  IdSource ids_;
  Clock clock_;
  mutable std::mutex mutex_;
  bool loaded_ = false;
  std::vector<ImportEntry> games_;
  std::vector<RemovedImport> removed_;
};

}  // namespace playden::stores::local
