#include "stores/local/import_store.h"

#include "platform/usb_mount.h"

#include "platform/random_id.h"
#include "steam/backend/json.h"
#include "stores/local/scanner.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

namespace playden::stores::local {
namespace {

using JsonValue = steam::json::Value;

constexpr std::size_t kMaxTitleBytes = 512;
constexpr std::size_t kMaxPathBytes = 1024;
constexpr std::size_t kMaxFailureBytes = 1024;
constexpr std::size_t kMaxVolumeBytes = 32;

ImportResult<void> Failure(ImportError error, std::string message) {
  return ImportResult<void>::Failure(error, std::move(message));
}
template <typename T>
ImportResult<T> FailureOf(ImportError error, std::string message) {
  return ImportResult<T>::Failure(error, std::move(message));
}

std::string Escape(const std::string& value) {
  std::string output;
  output.reserve(value.size() + 2);
  for (const unsigned char c : value) {
    switch (c) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (c < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          output += "\\u00";
          output.push_back(kHex[c >> 4]);
          output.push_back(kHex[c & 15]);
        } else {
          output.push_back(static_cast<char>(c));
        }
    }
  }
  return output;
}

void Field(std::string& output, const char* name, const std::string& value, bool comma = true) {
  output += '"';
  output += name;
  output += "\":\"";
  output += Escape(value);
  output += '"';
  if (comma) output += ',';
}

const JsonValue* Required(const JsonValue& object, const char* key, JsonValue::Type type) {
  const auto* value = object.Find(key);
  return value && value->type() == type ? value : nullptr;
}

bool ValidVolume(const std::string& volume) {
  return !volume.empty() && volume.size() <= kMaxVolumeBytes &&
         std::all_of(volume.begin(), volume.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
         });
}
bool ValidPath(const std::string& path) { return path.size() <= kMaxPathBytes && IsSafeRelativePath(path); }
bool ValidTitle(const std::string& title) {
  return !title.empty() && title.size() <= kMaxTitleBytes && title.find('\0') == std::string::npos;
}

std::optional<ImportState> ParseState(const std::string& key) {
  for (const auto state : {ImportState::kInPlace, ImportState::kCopyQueued, ImportState::kCopying, ImportState::kCopied})
    if (key == ImportStateKey(state)) return state;
  return std::nullopt;
}

std::optional<ImportEntry> ParseGame(const JsonValue& value) {
  if (!value.is_object()) return std::nullopt;
  const auto* id = Required(value, "id", JsonValue::Type::kString);
  const auto* title = Required(value, "title", JsonValue::Type::kString);
  const auto* volume = Required(value, "volume", JsonValue::Type::kString);
  const auto* folder = Required(value, "folder", JsonValue::Type::kString);
  const auto* program = Required(value, "program", JsonValue::Type::kString);
  const auto* state = Required(value, "state", JsonValue::Type::kString);
  const auto* added = Required(value, "added_at", JsonValue::Type::kNumber);
  if (!id || !title || !volume || !folder || !program || !state || !added) return std::nullopt;
  const auto parsed_state = ParseState(state->text());
  const auto added_at = added->AsUint64();
  if (!parsed_state || !added_at || !IsImportId(id->text()) || !ValidTitle(title->text()) ||
      !ValidVolume(volume->text()) || !ValidPath(folder->text()) || !ValidPath(program->text()))
    return std::nullopt;
  ImportEntry entry{id->text(), title->text(), volume->text(), folder->text(), program->text(), *parsed_state,
                    *added_at, {}};
  if (const auto* failure = value.Find("failure")) {
    if (!failure->is_string() || failure->text().size() > kMaxFailureBytes) return std::nullopt;
    entry.failure = failure->text();
  }
  return entry;
}

std::optional<RemovedImport> ParseRemoved(const JsonValue& value) {
  if (!value.is_object()) return std::nullopt;
  const auto* id = Required(value, "id", JsonValue::Type::kString);
  const auto* volume = Required(value, "volume", JsonValue::Type::kString);
  const auto* folder = Required(value, "folder", JsonValue::Type::kString);
  const auto* program = Required(value, "program", JsonValue::Type::kString);
  if (!id || !volume || !folder || !program || !IsImportId(id->text()) || !ValidVolume(volume->text()) ||
      !ValidPath(folder->text()) || !ValidPath(program->text()))
    return std::nullopt;
  return RemovedImport{id->text(), volume->text(), folder->text(), program->text()};
}

bool SameFolder(const std::string& volume_a, const std::string& folder_a, const std::string& volume_b,
                const std::string& folder_b) {
  return volume_a == volume_b && folder_a == folder_b;
}

// A removed game keeps its ID for its folder, on whichever mount its USB drive is now.
bool SameRemovedFolder(const std::string& volume_a, const std::string& folder_a, const std::string& volume_b,
                       const std::string& folder_b) {
  return SameFolder(volume_a, folder_a, volume_b, folder_b) ||
         (platform::IsUsbVolumeId(volume_a) && platform::IsUsbVolumeId(volume_b) && SameFolder(volume_a, folder_a, volume_a, folder_b));
}

}  // namespace

const char* ImportStateKey(ImportState state) {
  switch (state) {
    case ImportState::kInPlace: return "in_place";
    case ImportState::kCopyQueued: return "copy_queued";
    case ImportState::kCopying: return "copying";
    case ImportState::kCopied: return "copied";
  }
  return "in_place";
}

bool IsImportId(const std::string& id) {
  if (id.size() != 36) return false;
  for (std::size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> RandomImportId() { return playden::RandomUuid(); }

ImportStore::ImportStore(Storage& storage, std::string relative_path, IdSource ids, Clock clock)
    : storage_(storage), relative_path_(std::move(relative_path)), ids_(std::move(ids)), clock_(std::move(clock)) {
  if (!ids_) ids_ = RandomImportId;
  if (!clock_) clock_ = [] {
    const auto now = std::time(nullptr);
    return now > 0 ? static_cast<std::uint64_t>(now) : 0;
  };
}

ImportResult<void> ImportStore::Load() {
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_ = false;
  const auto read = storage_.ReadDurable(relative_path_, kMaxFileBytes);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) {
      games_.clear();
      removed_.clear();
      loaded_ = true;
      return ImportResult<void>::Success();
    }
    return Failure(read.error() == StorageError::kReadFailed ? ImportError::kBounds : ImportError::kIo,
                   "imports could not be read");
  }
  const std::string text(read.value().begin(), read.value().end());
  steam::json::Limits limits;
  limits.max_bytes = kMaxFileBytes;
  limits.max_depth = 4;
  limits.max_values = (kMaxGames + kMaxRemoved) * 24 + 16;
  limits.max_string_bytes = kMaxPathBytes;
  const auto root = steam::json::Parse(text, limits);
  if (!root || !root->is_object()) return Failure(ImportError::kMalformed, "imports are malformed");
  const auto* version = Required(*root, "version", JsonValue::Type::kNumber);
  const auto version_number = version ? version->AsUint64() : std::nullopt;
  if (!version_number) return Failure(ImportError::kMalformed, "imports version is malformed");
  if (*version_number != kVersion) return Failure(ImportError::kUnsupportedVersion, "imports version is unsupported");
  const auto* games = Required(*root, "games", JsonValue::Type::kArray);
  const auto* removed = Required(*root, "removed", JsonValue::Type::kArray);
  if (!games || !removed || games->array().size() > kMaxGames || removed->array().size() > kMaxRemoved)
    return Failure(ImportError::kMalformed, "imports entries are malformed");
  std::vector<ImportEntry> next_games;
  std::vector<RemovedImport> next_removed;
  const auto id_used = [&](const std::string& id) {
    return std::any_of(next_games.begin(), next_games.end(), [&](const auto& e) { return e.id == id; }) ||
           std::any_of(next_removed.begin(), next_removed.end(), [&](const auto& e) { return e.id == id; });
  };
  for (const auto& value : games->array()) {
    auto entry = ParseGame(value);
    if (!entry || id_used(entry->id) ||
        std::any_of(next_games.begin(), next_games.end(), [&](const auto& other) {
          return SameFolder(other.volume, other.folder, entry->volume, entry->folder);
        }))
      return Failure(ImportError::kMalformed, "an imported game is invalid");
    next_games.push_back(std::move(*entry));
  }
  for (const auto& value : removed->array()) {
    auto entry = ParseRemoved(value);
    if (!entry || id_used(entry->id)) return Failure(ImportError::kMalformed, "a removed imported game is invalid");
    next_removed.push_back(std::move(*entry));
  }
  games_ = std::move(next_games);
  removed_ = std::move(next_removed);
  loaded_ = true;
  return ImportResult<void>::Success();
}

ImportResult<void> ImportStore::Persist() {
  if (games_.size() > kMaxGames || removed_.size() > kMaxRemoved)
    return Failure(ImportError::kBounds, "imports exceed their entry limit");
  std::string output = "{\"version\":1,\"games\":[";
  for (std::size_t i = 0; i < games_.size(); ++i) {
    const auto& game = games_[i];
    if (i) output += ',';
    output += '{';
    Field(output, "id", game.id);
    Field(output, "title", game.title);
    Field(output, "volume", game.volume);
    Field(output, "folder", game.folder);
    Field(output, "program", game.program);
    Field(output, "state", ImportStateKey(game.state));
    if (!game.failure.empty()) Field(output, "failure", game.failure);
    output += "\"added_at\":" + std::to_string(game.added_at) + '}';
  }
  output += "],\"removed\":[";
  for (std::size_t i = 0; i < removed_.size(); ++i) {
    const auto& entry = removed_[i];
    if (i) output += ',';
    output += '{';
    Field(output, "id", entry.id);
    Field(output, "volume", entry.volume);
    Field(output, "folder", entry.folder);
    Field(output, "program", entry.program, false);
    output += '}';
  }
  output += "]}";
  if (output.size() > kMaxFileBytes) return Failure(ImportError::kBounds, "imports exceed their size limit");
  const auto written = storage_.WriteDurable(relative_path_, Bytes(output.begin(), output.end()));
  if (!written.ok()) {
    // The file may or may not hold the change: reload before anything else.
    loaded_ = false;
    return Failure(ImportError::kIo, "imports could not be written");
  }
  return ImportResult<void>::Success();
}

ImportResult<void> ImportStore::Save() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return Failure(ImportError::kNotLoaded, "imports are not loaded");
  return Persist();
}

bool ImportStore::loaded() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_;
}

std::vector<ImportEntry> ImportStore::Games() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_ ? games_ : std::vector<ImportEntry>{};
}

std::vector<RemovedImport> ImportStore::Removed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_ ? removed_ : std::vector<RemovedImport>{};
}

std::optional<ImportEntry> ImportStore::Find(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return std::nullopt;
  for (const auto& game : games_)
    if (game.id == id) return game;
  return std::nullopt;
}

std::optional<ImportEntry> ImportStore::FindByFolder(const std::string& volume, const std::string& folder) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return std::nullopt;
  for (const auto& game : games_)
    if (SameFolder(game.volume, game.folder, volume, folder)) return game;
  return std::nullopt;
}

std::optional<RemovedImport> ImportStore::FindRemoved(const std::string& volume, const std::string& folder) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return std::nullopt;
  for (const auto& entry : removed_)
    if (SameRemovedFolder(entry.volume, entry.folder, volume, folder)) return entry;
  return std::nullopt;
}

ImportEntry* ImportStore::Mutable(const std::string& id) {
  for (auto& game : games_)
    if (game.id == id) return &game;
  return nullptr;
}

void ImportStore::MoveToRemoved(std::size_t index) {
  const auto game = games_[index];
  games_.erase(games_.begin() + static_cast<std::ptrdiff_t>(index));
  removed_.erase(std::remove_if(removed_.begin(), removed_.end(),
                                [&](const auto& entry) {
                                  return entry.id == game.id ||
                                         SameRemovedFolder(entry.volume, entry.folder, game.volume, game.folder);
                                }),
                 removed_.end());
  removed_.push_back({game.id, game.volume, game.folder, game.program});
  while (removed_.size() > kMaxRemoved) removed_.erase(removed_.begin());
}

ImportResult<ImportEntry> ImportStore::SetVolume(const std::string& id, const std::string& volume) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return FailureOf<ImportEntry>(ImportError::kNotLoaded, "imports are not loaded");
  if (!ValidVolume(volume)) return FailureOf<ImportEntry>(ImportError::kInvalid, "the game's drive is invalid");
  auto* game = Mutable(id);
  if (!game) return FailureOf<ImportEntry>(ImportError::kNotFound, "the game is not in the library");
  if (game->volume == volume) return ImportResult<ImportEntry>::Success(*game);
  for (const auto& other : games_)
    if (other.id != id && SameFolder(other.volume, other.folder, volume, game->folder))
      return FailureOf<ImportEntry>(ImportError::kDuplicate, "this folder is already in the library");
  const auto previous = game->volume;
  game->volume = volume;
  const auto persisted = Persist();
  if (!persisted.ok()) {
    game->volume = previous;
    return FailureOf<ImportEntry>(persisted.error(), persisted.message());
  }
  return ImportResult<ImportEntry>::Success(*game);
}

ImportResult<ImportEntry> ImportStore::Add(const std::string& volume, const std::string& folder,
                                           const std::string& title, const std::string& program) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return FailureOf<ImportEntry>(ImportError::kNotLoaded, "imports are not loaded");
  if (!ValidVolume(volume) || !ValidPath(folder) || !ValidPath(program) || !ValidTitle(title))
    return FailureOf<ImportEntry>(ImportError::kInvalid, "the game's folder, program or title is invalid");
  for (const auto& game : games_)
    if (SameFolder(game.volume, game.folder, volume, folder))
      return FailureOf<ImportEntry>(ImportError::kDuplicate, "this folder is already in the library");
  if (games_.size() >= kMaxGames) return FailureOf<ImportEntry>(ImportError::kBounds, "too many imported games");
  const auto removed = std::find_if(removed_.begin(), removed_.end(), [&](const auto& entry) {
    return SameRemovedFolder(entry.volume, entry.folder, volume, folder);
  });
  std::string id;
  if (removed != removed_.end()) {
    id = removed->id;
  } else {
    for (int attempt = 0; attempt < 4 && id.empty(); ++attempt) {
      const auto made = ids_();
      if (!made || !IsImportId(*made)) break;
      const bool used = Mutable(*made) || std::any_of(removed_.begin(), removed_.end(),
                                                       [&](const auto& entry) { return entry.id == *made; });
      if (!used) id = *made;
    }
    if (id.empty()) return FailureOf<ImportEntry>(ImportError::kNoRandom, "no ID could be made for the game");
  }
  const auto previous_games = games_;
  const auto previous_removed = removed_;
  if (removed != removed_.end()) removed_.erase(removed);
  ImportEntry entry{id, title, volume, folder, program, ImportState::kInPlace, clock_(), {}};
  games_.push_back(entry);
  const auto persisted = Persist();
  if (!persisted.ok()) {
    games_ = previous_games;
    removed_ = previous_removed;
    return FailureOf<ImportEntry>(persisted.error(), persisted.message());
  }
  return ImportResult<ImportEntry>::Success(entry);
}

ImportResult<ImportEntry> ImportStore::SetProgram(const std::string& id, const std::string& program) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return FailureOf<ImportEntry>(ImportError::kNotLoaded, "imports are not loaded");
  if (!ValidPath(program)) return FailureOf<ImportEntry>(ImportError::kInvalid, "the program path is invalid");
  auto* game = Mutable(id);
  if (!game) return FailureOf<ImportEntry>(ImportError::kNotFound, "the game is not in the library");
  const auto previous = *game;
  game->program = program;
  const auto persisted = Persist();
  if (!persisted.ok()) {
    *game = previous;
    return FailureOf<ImportEntry>(persisted.error(), persisted.message());
  }
  return ImportResult<ImportEntry>::Success(*game);
}

ImportResult<ImportEntry> ImportStore::Transition(const std::string& id, ImportState from, ImportState to) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return FailureOf<ImportEntry>(ImportError::kNotLoaded, "imports are not loaded");
  auto* game = Mutable(id);
  if (!game) return FailureOf<ImportEntry>(ImportError::kNotFound, "the game is not in the library");
  if (game->state != from)
    return FailureOf<ImportEntry>(ImportError::kWrongState, std::string("the game is ") + ImportStateKey(game->state));
  const auto previous = *game;
  game->state = to;
  if (to == ImportState::kCopyQueued || to == ImportState::kCopied) game->failure.clear();
  const auto persisted = Persist();
  if (!persisted.ok()) {
    *game = previous;
    return FailureOf<ImportEntry>(persisted.error(), persisted.message());
  }
  return ImportResult<ImportEntry>::Success(*game);
}

ImportResult<ImportEntry> ImportStore::QueueCopy(const std::string& id) {
  return Transition(id, ImportState::kInPlace, ImportState::kCopyQueued);
}
ImportResult<ImportEntry> ImportStore::StartCopy(const std::string& id) {
  return Transition(id, ImportState::kCopyQueued, ImportState::kCopying);
}
ImportResult<ImportEntry> ImportStore::FinishCopy(const std::string& id) {
  return Transition(id, ImportState::kCopying, ImportState::kCopied);
}

ImportResult<ImportEntry> ImportStore::CancelCopy(const std::string& id, const std::string& failure) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return FailureOf<ImportEntry>(ImportError::kNotLoaded, "imports are not loaded");
  auto* found = Mutable(id);
  if (!found) return FailureOf<ImportEntry>(ImportError::kNotFound, "the game is not in the library");
  if (found->state != ImportState::kCopyQueued && found->state != ImportState::kCopying)
    return FailureOf<ImportEntry>(ImportError::kWrongState, std::string("the game is ") + ImportStateKey(found->state));
  const auto previous = games_;
  // The drive folder, and a drop-folder game's folder, are still there: the game plays in place again.
  found->state = ImportState::kInPlace;
  found->failure = failure.substr(0, kMaxFailureBytes);
  const auto kept = *found;
  const auto persisted = Persist();
  if (!persisted.ok()) {
    games_ = previous;
    return FailureOf<ImportEntry>(persisted.error(), persisted.message());
  }
  return ImportResult<ImportEntry>::Success(kept);
}

ImportResult<ImportEntry> ImportStore::NoteFailure(const std::string& id, const std::string& failure) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return FailureOf<ImportEntry>(ImportError::kNotLoaded, "imports are not loaded");
  auto* found = Mutable(id);
  if (!found) return FailureOf<ImportEntry>(ImportError::kNotFound, "the game is not in the library");
  const auto previous = *found;
  found->failure = failure.substr(0, kMaxFailureBytes);
  const auto kept = *found;
  const auto persisted = Persist();
  if (!persisted.ok()) {
    *found = previous;
    return FailureOf<ImportEntry>(persisted.error(), persisted.message());
  }
  return ImportResult<ImportEntry>::Success(kept);
}

ImportResult<void> ImportStore::Remove(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return Failure(ImportError::kNotLoaded, "imports are not loaded");
  const auto found = std::find_if(games_.begin(), games_.end(), [&](const auto& game) { return game.id == id; });
  if (found == games_.end()) return Failure(ImportError::kNotFound, "the game is not in the library");
  if (found->state == ImportState::kCopied)
    return Failure(ImportError::kWrongState, "uninstall the game's copy before removing it");
  if (found->state != ImportState::kInPlace)
    return Failure(ImportError::kWrongState, "cancel the game's copy before removing it");
  const auto previous_games = games_;
  const auto previous_removed = removed_;
  MoveToRemoved(static_cast<std::size_t>(found - games_.begin()));
  const auto persisted = Persist();
  if (!persisted.ok()) {
    games_ = previous_games;
    removed_ = previous_removed;
  }
  return persisted;
}

ImportResult<void> ImportStore::Uninstalled(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) return Failure(ImportError::kNotLoaded, "imports are not loaded");
  const auto found = std::find_if(games_.begin(), games_.end(), [&](const auto& game) { return game.id == id; });
  if (found == games_.end()) return Failure(ImportError::kNotFound, "the game is not in the library");
  if (found->state != ImportState::kCopied)
    return Failure(ImportError::kWrongState, std::string("the game is ") + ImportStateKey(found->state));
  const auto previous_games = games_;
  const auto previous_removed = removed_;
  MoveToRemoved(static_cast<std::size_t>(found - games_.begin()));
  const auto persisted = Persist();
  if (!persisted.ok()) {
    games_ = previous_games;
    removed_ = previous_removed;
  }
  return persisted;
}

}  // namespace playden::stores::local
