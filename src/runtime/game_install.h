#pragma once

#include "library/store.h"
#include "platform/storage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace playden::runtime {

enum class PreparationState {
  kDownloaded,
  kPreparing,
  kRegistering,
  kReady,
  kFailed,
  kUnavailable,
  kRemoving,
};

enum class TitleAvailability { kFree, kOccupied, kUnknown };

struct NativeTitleAvailability {
  TitleAvailability inventory = TitleAvailability::kUnknown;
  TitleAvailability source_path = TitleAvailability::kUnknown;
};

using NativeAvailabilityQuery =
    std::function<NativeTitleAvailability(const std::string& title_id)>;

struct RuntimeArtifactIdentity {
  std::string artifact_id;
  std::string identity;
  std::string sha256;
};

struct RuntimeFailure {
  std::string code;
  std::string message;
};

struct PreparationJournal {
  std::string operation_id;
  std::string operation;
  std::string step;
};

struct RuntimeInstall {
  std::uint32_t schema_version = 1;
  std::string install_id;
  std::string title_id;
  // The runtime's number for the game ("app_id" on disk, in game-launch.json and the journals): a
  // Steam game's app ID, or a number from the repository's allocator for other stores.
  std::uint32_t runtime_number = 0;
  // The game itself. Stored as "store"/"game" only when not Steam; absent means Steam with the
  // value runtime_number. The repository's records always carry it; a default GameId means a copy
  // that doesn't (an install record read back from a publication journal, whose layout has only
  // the number): comparisons then rely on the number, which the repository keeps to one game.
  GameId game;
  std::string data_root;
  std::string default_profile_id;
  std::string runtime_bundle_id;
  std::string recipe_id;
  std::string recipe_version;
  std::uint64_t settings_revision = 0;
  std::string exit_policy = "exit-title";

  std::string launch_entry_id;
  std::string launch_path;
  std::string launch_arguments;
  std::string launch_working_directory;
  // With no working directory: the program starts in the install root, not its own folder.
  bool launch_in_install_root = false;
  std::string install_path;
  std::string volume_id;
  PreparationState state = PreparationState::kDownloaded;
  std::vector<RuntimeArtifactIdentity> artifacts;
  std::optional<RuntimeFailure> last_failure;
  std::optional<PreparationJournal> journal;
  // Set when the game folder moved with the games root (the 0.6 layout): its earlier
  // install_path, which the committed publication still names. The game needs a
  // republication against install_path; a committed publication clears it. Stored only
  // when set, so older readers (which ignore unknown keys) are unaffected.
  std::string moved_from;
};

// The title range reserved for Playden games; excludes the launcher itself.
bool IsManagedTitle(const std::string& title);

// Shared OWNER record validation; this grants no filesystem ownership or readiness.
bool ValidateRuntimeInstallRecord(const RuntimeInstall& record);
// Same install: install, title, runtime number and game, data root and game folder. Runtime
// artifacts, recipe and launch fields are not compared (a kit respin may change
// them). State, revision, journal and failure are checked separately.
bool SameRuntimeInstallIdentity(const RuntimeInstall& first, const RuntimeInstall& second);
// path with its leading `from` replaced by `to`, when path is from or inside it (whole
// path components: /a/games-old is not inside /a/games); nothing otherwise.
std::optional<std::string> RebaseInstallPath(const std::string& path, std::string from, std::string to);

struct ReserveRequest {
  std::string install_id;
  std::uint32_t runtime_number = 0;
  GameId game;  // Steam: GameId::Steam(runtime_number)
  std::string default_profile_id;
  std::string runtime_bundle_id;
  std::string recipe_id;
  std::string recipe_version;
  std::uint64_t settings_revision = 0;
  std::string launch_entry_id;
  std::string launch_path;
  std::string launch_arguments;
  std::string launch_working_directory;
  bool launch_in_install_root = false;
  std::string install_path;
  std::string volume_id;
  std::vector<std::string> kit_title_variants;
};

struct LegacyInstall {
  std::string install_id;
  std::uint32_t runtime_number = 0;
  GameId game;
  std::string install_path;
  std::string volume_id;
  std::string launch_entry_id;
  std::string launch_path;
  std::string launch_arguments;
  std::string launch_working_directory;
  std::string default_profile_id;
  std::string runtime_bundle_id;
  std::string recipe_id;
  std::string recipe_version;
  std::vector<std::string> kit_title_variants;
};

enum class RepositoryError {
  kNone,
  kNotLoaded,
  kIo,
  kMalformed,
  kUnsupportedSchema,
  kBounds,
  kInvalidRecord,
  kConflict,
  kNotFound,
  kTitleVariantsExhausted,
  kRemovalIncomplete,
};

template <typename T>
class RepositoryResult {
 public:
  static RepositoryResult Success(T value) {
    return RepositoryResult(std::move(value), RepositoryError::kNone, {});
  }
  static RepositoryResult Failure(RepositoryError error, std::string message) {
    return RepositoryResult(std::nullopt, error, std::move(message));
  }
  bool ok() const { return value_.has_value(); }
  const T& value() const { return value_.value(); }
  T& value() { return value_.value(); }
  RepositoryError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  RepositoryResult(T value, RepositoryError error, std::string message)
      : value_(std::move(value)), error_(error), message_(std::move(message)) {}
  RepositoryResult(std::nullopt_t, RepositoryError error, std::string message)
      : value_(std::nullopt), error_(error), message_(std::move(message)) {}
  std::optional<T> value_;
  RepositoryError error_;
  std::string message_;
};

template <>
class RepositoryResult<void> {
 public:
  static RepositoryResult Success() {
    return RepositoryResult(true, RepositoryError::kNone, {});
  }
  static RepositoryResult Failure(RepositoryError error, std::string message) {
    return RepositoryResult(false, error, std::move(message));
  }
  bool ok() const { return ok_; }
  RepositoryError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  RepositoryResult(bool ok, RepositoryError error, std::string message)
      : ok_(ok), error_(error), message_(std::move(message)) {}
  bool ok_;
  RepositoryError error_;
  std::string message_;
};

// Runtime numbers of games from stores other than Steam: far above any Steam app ID.
inline constexpr std::uint32_t kFirstRuntimeNumber = 0xF0000000u;
inline constexpr std::uint32_t kLastRuntimeNumber = 0xFFFFFFFEu;

// Every public member locks the repository, so the application thread may ask for a runtime
// number (RuntimeNumberFor) while the runtime worker uses it.
class RuntimeInstallRepository {
 public:
  static constexpr std::uint32_t kSchemaVersion = 1;
  static constexpr std::size_t kMaxFileBytes = 256 * 1024;

  RuntimeInstallRepository(Storage& storage, std::string relative_path,
                           NativeAvailabilityQuery native_availability);

  RepositoryResult<void> Load();
  RepositoryResult<void> Save();
  // False before Load, and after a write whose outcome is unknown until the registry is read again.
  bool Loaded() const { return loaded_; }
  std::vector<RuntimeInstall> Snapshot() const;
  std::optional<RuntimeInstall> Find(const std::string& install_id) const;
  RepositoryResult<RuntimeInstall> Reserve(const ReserveRequest& request);
  RepositoryResult<RuntimeInstall> Adopt(const LegacyInstall& legacy);
  RepositoryResult<void> Update(const RuntimeInstall& record);
  // Several updates in one durable write: all of them or none.
  RepositoryResult<void> UpdateAll(const std::vector<RuntimeInstall>& records);
  RepositoryResult<void> EraseAfterUnregister(const std::string& install_id);
  // Same, with the caller's fresh observation instead of the availability query:
  // removal confirms with the console inventory and the source folder while
  // ShadowMount's cache is stale. Both must be free; nothing else is relaxed.
  RepositoryResult<void> EraseAfterUnregister(const std::string& install_id,
                                              const NativeTitleAvailability& confirmed);
  // The runtime number for a game: Steam, its app ID; others, the number of its install record,
  // or the one given to it earlier in this session (RememberNumber included: a kept bottle's), or
  // a new one (persisted as next_runtime_number before it is returned). Numbers are unique across
  // the records and never handed out twice. Fails when the registry is not loaded, the game is
  // invalid, the range is used up or the write fails.
  RepositoryResult<std::uint32_t> RuntimeNumberFor(const GameId& game);
  // A number a game already has outside the records (a kept bottle's marker names it): this
  // session's RuntimeNumberFor gives it back to that game and never to another. Refused (kConflict)
  // when the number is another game's, or the game has another one; Steam games need none.
  RepositoryResult<void> RememberNumber(const GameId& game, std::uint32_t runtime_number);

 private:
  RepositoryResult<void> Persist();

  Storage& storage_;
  std::string relative_path_;
  NativeAvailabilityQuery native_availability_;
  bool loaded_ = false;
  std::unordered_map<std::string, RuntimeInstall> installs_;
  std::unordered_map<std::string, std::string> reservations_;
  // The allocator: the next number to try, and whether installs.json carries it (it did when
  // loaded, or a number was allocated), so a Steam-only registry is written as before.
  std::uint32_t next_runtime_number_ = kFirstRuntimeNumber;
  bool write_next_runtime_number_ = false;
  // Numbers allocated in this session for games with no install record yet.
  std::unordered_map<GameId, std::uint32_t> allocated_;
  // Recursive: Adopt and EraseAfterUnregister call other public members.
  mutable std::recursive_mutex mutex_;
};

}  // namespace playden::runtime
