#include "runtime/game_install.h"

#include "steam/backend/json.h"
#include "steam/backend/app_info.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <unordered_set>

namespace playden::runtime {
namespace {
// Text builder with the stream operators this file uses. iostreams need locale
// support that the console's libc does not provide.
struct TextOutput {
  std::string text;
  TextOutput& operator<<(char value) { text += value; return *this; }
  TextOutput& operator<<(const char* value) { text += value; return *this; }
  TextOutput& operator<<(const std::string& value) { text += value; return *this; }
  TextOutput& operator<<(std::uint32_t value) { text += std::to_string(value); return *this; }
  TextOutput& operator<<(std::uint64_t value) { text += std::to_string(value); return *this; }
  const std::string& str() const { return text; }
};

using JsonValue = steam::json::Value;

constexpr std::size_t kMaxRecords = 128;
constexpr std::size_t kMaxArtifacts = 32;
constexpr std::size_t kMaxIdBytes = 128;
constexpr std::size_t kMaxPathBytes = 1024;
constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxLaunchArgumentsBytes = 4096;

RepositoryResult<void> Failure(RepositoryError error, std::string message) {
  return RepositoryResult<void>::Failure(error, std::move(message));
}

bool IsSimpleId(const std::string& value, std::size_t limit = kMaxIdBytes) {
  if (value.empty() || value.size() > limit) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '+';
  });
}

// game-launch.json v1 identifiers share the native descriptor's 64-byte grammar.
bool IsDescriptorId(const std::string& value) {
  const auto alphanumeric = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  return !value.empty() && value.size() <= 64 && alphanumeric(value.front()) &&
         std::all_of(value.begin(), value.end(), [&](unsigned char c) {
           return alphanumeric(c) || c == '-' || c == '_';
         });
}

bool IsAbsoluteNormalizedPath(const std::string& path) {
  if (path.empty() || path.size() > kMaxPathBytes || path.front() != '/' ||
      path.find('\0') != std::string::npos) return false;
  std::size_t begin = 1;
  while (begin <= path.size()) {
    const auto end = path.find('/', begin);
    const auto segment = path.substr(begin, end == std::string::npos ? path.size() - begin
                                                                    : end - begin);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return true;
}

bool IsRelativeLaunchPath(const std::string& path) {
  const auto normalized = steam::RelativeGamePath(path);
  return path.size() <= kMaxPathBytes && normalized && *normalized == path;
}

bool NormalizeLaunchPaths(RuntimeInstall& record) {
  const auto executable = steam::RelativeGamePath(record.launch_path);
  const auto working = steam::RelativeGamePath(record.launch_working_directory, true);
  if (!executable || !working) return false;
  record.launch_path = *executable;
  record.launch_working_directory = *working == "." ? "" : *working;
  record.launch_in_install_root = record.launch_in_install_root && record.launch_working_directory.empty();
  return true;
}

bool IsBoundedText(const std::string& value, std::size_t limit, bool allow_empty) {
  if ((!allow_empty && value.empty()) || value.size() > limit) return false;
  return std::none_of(value.begin(), value.end(), [](unsigned char c) {
    return c < 0x20 || c == 0x7f;
  });
}

bool IsSha256(const std::string& value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

const char* StateName(PreparationState state) {
  switch (state) {
    case PreparationState::kDownloaded: return "downloaded";
    case PreparationState::kPreparing: return "preparing";
    case PreparationState::kRegistering: return "registering";
    case PreparationState::kReady: return "ready";
    case PreparationState::kFailed: return "failed";
    case PreparationState::kUnavailable: return "unavailable";
    case PreparationState::kRemoving: return "removing";
  }
  return "";
}

std::optional<PreparationState> ParseState(const JsonValue& value) {
  if (!value.is_string()) return std::nullopt;
  if (value.text() == "downloaded") return PreparationState::kDownloaded;
  if (value.text() == "preparing") return PreparationState::kPreparing;
  if (value.text() == "registering") return PreparationState::kRegistering;
  if (value.text() == "ready") return PreparationState::kReady;
  if (value.text() == "failed") return PreparationState::kFailed;
  if (value.text() == "unavailable") return PreparationState::kUnavailable;
  if (value.text() == "removing") return PreparationState::kRemoving;
  return std::nullopt;
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

void StringField(TextOutput& output, const char* name, const std::string& value,
                 bool comma = true) {
  output << '\"' << name << "\":\"" << Escape(value) << '\"';
  if (comma) output << ',';
}

const JsonValue* Required(const JsonValue& object, const char* key, JsonValue::Type type) {
  const auto* value = object.Find(key);
  return value && value->type() == type ? value : nullptr;
}

// A Steam game's number is its app ID; others take one from the allocator's range.
bool ValidNumber(const RuntimeInstall& record) {
  if (record.runtime_number == 0) return false;
  if (record.game == playden::GameId{}) return true;  // not carried (see RuntimeInstall::game)
  if (!record.game.valid()) return false;
  if (const auto app = record.game.steam_app()) return *app == record.runtime_number;
  return record.runtime_number >= kFirstRuntimeNumber && record.runtime_number <= kLastRuntimeNumber;
}

// "store" on disk: only stores other than Steam are written, and only Local is read for now.
std::optional<playden::Store> ParseStore(const std::string& key) { return playden::ParseStoreKey(key); }

// Both copies name the game, or one doesn't carry it (see RuntimeInstall::game).
bool SameGame(const playden::GameId& a, const playden::GameId& b) {
  return a == b || a == playden::GameId{} || b == playden::GameId{};
}

// A default GameId in a request stands for the Steam game its number names, as on disk.
playden::GameId RequestGame(const playden::GameId& game, std::uint32_t runtime_number) {
  return game == playden::GameId{} && runtime_number ? playden::GameId::Steam(runtime_number) : game;
}

bool ValidRecord(const RuntimeInstall& record) {
  if (record.schema_version != 1 || !IsDescriptorId(record.install_id) ||
      !IsManagedTitle(record.title_id) || !ValidNumber(record) ||
      record.data_root != "/data/playden/games/" + record.install_id ||
      !IsDescriptorId(record.default_profile_id) || !IsDescriptorId(record.runtime_bundle_id) ||
      !IsDescriptorId(record.recipe_id) || !IsDescriptorId(record.recipe_version) ||
      record.exit_policy != "exit-title" || !IsSimpleId(record.launch_entry_id) ||
      !IsRelativeLaunchPath(record.launch_path) ||
      !IsBoundedText(record.launch_arguments, kMaxLaunchArgumentsBytes, true) ||
      (!record.launch_working_directory.empty() && !IsRelativeLaunchPath(record.launch_working_directory)) ||
      (record.launch_in_install_root && !record.launch_working_directory.empty()) ||
      !IsAbsoluteNormalizedPath(record.install_path) || !IsSimpleId(record.volume_id) ||
      record.artifacts.size() > kMaxArtifacts) return false;
  for (const auto& artifact : record.artifacts)
    if (!IsSimpleId(artifact.artifact_id) || !IsSimpleId(artifact.identity) ||
        !IsSha256(artifact.sha256)) return false;
  if (record.last_failure &&
      (!IsSimpleId(record.last_failure->code) || record.last_failure->message.empty() ||
       record.last_failure->message.size() > kMaxMessageBytes)) return false;
  if (record.journal &&
      (!IsSimpleId(record.journal->operation_id) || !IsSimpleId(record.journal->operation) ||
       !IsSimpleId(record.journal->step))) return false;
  if (!record.moved_from.empty() &&
      (!IsAbsoluteNormalizedPath(record.moved_from) || record.moved_from == record.install_path)) return false;
  return true;
}

std::optional<RuntimeInstall> ParseRecord(const JsonValue& value, RepositoryError& error) {
  if (!value.is_object()) return std::nullopt;
  const auto* schema = Required(value, "schema_version", JsonValue::Type::kNumber);
  if (!schema || !schema->AsUint64()) return std::nullopt;
  if (*schema->AsUint64() != 1) { error = RepositoryError::kUnsupportedSchema; return std::nullopt; }
  const auto* install_id = Required(value, "install_id", JsonValue::Type::kString);
  const auto* title_id = Required(value, "title_id", JsonValue::Type::kString);
  const auto* app_id = Required(value, "app_id", JsonValue::Type::kNumber);
  const auto* data_root = Required(value, "data_root", JsonValue::Type::kString);
  const auto* profile = Required(value, "default_profile_id", JsonValue::Type::kString);
  const auto* bundle = Required(value, "runtime_bundle_id", JsonValue::Type::kString);
  const auto* recipe = Required(value, "recipe_id", JsonValue::Type::kString);
  const auto* recipe_version = Required(value, "recipe_version", JsonValue::Type::kString);
  const auto* revision = Required(value, "settings_revision", JsonValue::Type::kNumber);
  const auto* exit_policy = Required(value, "exit_policy", JsonValue::Type::kString);
  const auto* launch_entry_id = Required(value, "launch_entry_id", JsonValue::Type::kString);
  const auto* launch = Required(value, "launch_path", JsonValue::Type::kString);
  const auto* launch_arguments = Required(value, "launch_arguments", JsonValue::Type::kString);
  const auto* launch_working_directory =
      Required(value, "launch_working_directory", JsonValue::Type::kString);
  const auto* install_path = Required(value, "install_path", JsonValue::Type::kString);
  const auto* volume = Required(value, "volume_id", JsonValue::Type::kString);
  const auto* state_value = Required(value, "state", JsonValue::Type::kString);
  const auto* artifacts = Required(value, "artifacts", JsonValue::Type::kArray);
  const auto* failure = value.Find("last_failure");
  const auto* journal = value.Find("journal");
  // Added later: records without it start the program in its own folder, as before.
  const auto* in_install_root = value.Find("launch_in_install_root");
  if (in_install_root && !in_install_root->is_bool()) return std::nullopt;
  // Added for games from other stores; absent means Steam.
  const auto* store = value.Find("store");
  const auto* game = value.Find("game");
  if ((store == nullptr) != (game == nullptr) || (store && (!store->is_string() || !game->is_string())))
    return std::nullopt;
  if (!install_id || !title_id || !app_id || !data_root || !profile || !bundle || !recipe ||
      !recipe_version || !revision || !exit_policy || !launch_entry_id || !launch ||
      !launch_arguments || !launch_working_directory || !install_path || !volume ||
      !state_value || !artifacts || !failure || !journal) return std::nullopt;
  const auto parsed_app = app_id->AsUint64();
  const auto parsed_revision = revision->AsUint64();
  const auto parsed_state = ParseState(*state_value);
  if (!parsed_app || *parsed_app > std::numeric_limits<std::uint32_t>::max() ||
      !parsed_revision || !parsed_state || artifacts->array().size() > kMaxArtifacts) return std::nullopt;

  RuntimeInstall record;
  record.install_id = install_id->text(); record.title_id = title_id->text();
  record.runtime_number = static_cast<std::uint32_t>(*parsed_app); record.data_root = data_root->text();
  if (store) {
    const auto parsed_store = ParseStore(store->text());
    if (!parsed_store) return std::nullopt;
    record.game = playden::GameId{*parsed_store, game->text()};
  } else {
    record.game = playden::GameId::Steam(record.runtime_number);
  }
  record.default_profile_id = profile->text(); record.runtime_bundle_id = bundle->text();
  record.recipe_id = recipe->text(); record.recipe_version = recipe_version->text();
  record.settings_revision = *parsed_revision; record.exit_policy = exit_policy->text();
  record.launch_entry_id = launch_entry_id->text(); record.launch_path = launch->text();
  record.launch_arguments = launch_arguments->text();
  record.launch_working_directory = launch_working_directory->text();
  record.launch_in_install_root = in_install_root && in_install_root->boolean();
  record.install_path = install_path->text();
  record.volume_id = volume->text(); record.state = *parsed_state;
  for (const auto& entry : artifacts->array()) {
    const auto* id = Required(entry, "artifact_id", JsonValue::Type::kString);
    const auto* identity = Required(entry, "identity", JsonValue::Type::kString);
    const auto* hash = Required(entry, "sha256", JsonValue::Type::kString);
    if (!id || !identity || !hash) return std::nullopt;
    record.artifacts.push_back({id->text(), identity->text(), hash->text()});
  }
  if (!failure->is_null()) {
    const auto* code = Required(*failure, "code", JsonValue::Type::kString);
    const auto* message = Required(*failure, "message", JsonValue::Type::kString);
    if (!code || !message) return std::nullopt;
    record.last_failure = RuntimeFailure{code->text(), message->text()};
  }
  if (!journal->is_null()) {
    const auto* operation_id = Required(*journal, "operation_id", JsonValue::Type::kString);
    const auto* operation = Required(*journal, "operation", JsonValue::Type::kString);
    const auto* step = Required(*journal, "step", JsonValue::Type::kString);
    if (!operation_id || !operation || !step) return std::nullopt;
    record.journal = PreparationJournal{operation_id->text(), operation->text(), step->text()};
  }
  if (const auto* moved = value.Find("moved_from")) {
    if (!moved->is_string() || moved->text().empty()) return std::nullopt;
    record.moved_from = moved->text();
  }
  if (!ValidRecord(record)) return std::nullopt;
  return record;
}

void WriteRecord(TextOutput& output, const RuntimeInstall& record) {
  output << '{' << "\"schema_version\":1,";
  StringField(output, "install_id", record.install_id); StringField(output, "title_id", record.title_id);
  output << "\"app_id\":" << record.runtime_number << ',';
  if (record.game.store != playden::Store::kSteam) {
    StringField(output, "store", std::string(Traits(record.game.store).key));
    StringField(output, "game", record.game.value);
  } StringField(output, "data_root", record.data_root);
  StringField(output, "default_profile_id", record.default_profile_id);
  StringField(output, "runtime_bundle_id", record.runtime_bundle_id);
  StringField(output, "recipe_id", record.recipe_id); StringField(output, "recipe_version", record.recipe_version);
  output << "\"settings_revision\":" << record.settings_revision << ',';
  StringField(output, "exit_policy", record.exit_policy);
  StringField(output, "launch_entry_id", record.launch_entry_id);
  StringField(output, "launch_path", record.launch_path);
  StringField(output, "launch_arguments", record.launch_arguments);
  StringField(output, "launch_working_directory", record.launch_working_directory);
  if (record.launch_in_install_root) output << "\"launch_in_install_root\":true,";
  StringField(output, "install_path", record.install_path); StringField(output, "volume_id", record.volume_id);
  StringField(output, "state", StateName(record.state)); output << "\"artifacts\":[";
  for (std::size_t i = 0; i < record.artifacts.size(); ++i) {
    if (i) output << ','; output << '{';
    StringField(output, "artifact_id", record.artifacts[i].artifact_id);
    StringField(output, "identity", record.artifacts[i].identity);
    StringField(output, "sha256", record.artifacts[i].sha256, false); output << '}';
  }
  output << "],\"last_failure\":";
  if (record.last_failure) { output << '{'; StringField(output, "code", record.last_failure->code);
    StringField(output, "message", record.last_failure->message, false); output << '}'; }
  else output << "null";
  output << ",\"journal\":";
  if (record.journal) { output << '{'; StringField(output, "operation_id", record.journal->operation_id);
    StringField(output, "operation", record.journal->operation);
    StringField(output, "step", record.journal->step, false); output << '}'; }
  else output << "null";
  if (!record.moved_from.empty()) { output << ','; StringField(output, "moved_from", record.moved_from, false); }
  output << '}';
}

}  // namespace

bool IsManagedTitle(const std::string& title) {
  if (title.size() != 9 || title.compare(0, 6, "FAKE78") != 0) return false;
  return std::all_of(title.begin() + 6, title.end(), [](unsigned char c) {
    return std::isdigit(c);
  });
}

bool ValidateRuntimeInstallRecord(const RuntimeInstall& record) { return ValidRecord(record); }

bool SameRuntimeInstallIdentity(const RuntimeInstall& a, const RuntimeInstall& b) {
  // Only the fields that pick which install this is and where its effects land: the
  // title and data folders, and the game folder (its dxvk.conf and the descriptor's
  // game root). Everything else is fixed at reservation or, like the runtime
  // artifacts after a kit respin, may change while a settings journal is pending;
  // comparing it stranded such journals (#5). A different runtime or recipe needs
  // the game prepared again, which starts a new journal.
  return ValidateRuntimeInstallRecord(a) && ValidateRuntimeInstallRecord(b) &&
      a.schema_version == b.schema_version && a.install_id == b.install_id && a.title_id == b.title_id &&
      a.runtime_number == b.runtime_number && SameGame(a.game, b.game) && a.data_root == b.data_root && a.install_path == b.install_path;
}

std::optional<std::string> RebaseInstallPath(const std::string& path, std::string from, std::string to) {
  while (from.size() > 1 && from.back() == '/') from.pop_back();
  while (to.size() > 1 && to.back() == '/') to.pop_back();
  if (from.empty() || to.empty() || path.compare(0, from.size(), from) != 0) return std::nullopt;
  if (path.size() == from.size()) return to;
  if (path[from.size()] != '/') return std::nullopt;
  return to + path.substr(from.size());
}

RuntimeInstallRepository::RuntimeInstallRepository(Storage& storage, std::string relative_path,
                                                   NativeAvailabilityQuery native_availability)
    : storage_(storage), relative_path_(std::move(relative_path)),
      native_availability_(std::move(native_availability)) {}

RepositoryResult<void> RuntimeInstallRepository::Load() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto read = storage_.ReadDurable(relative_path_, kMaxFileBytes);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) {
      installs_.clear(); reservations_.clear(); allocated_.clear();
      next_runtime_number_ = kFirstRuntimeNumber; write_next_runtime_number_ = false; loaded_ = true;
      return RepositoryResult<void>::Success();
    }
    loaded_ = false;
    return Failure(read.error() == StorageError::kReadFailed ? RepositoryError::kBounds
                                                             : RepositoryError::kIo,
                   "runtime install registry could not be read");
  }
  const std::string text(read.value().begin(), read.value().end());
  steam::json::Limits limits;
  limits.max_bytes = kMaxFileBytes; limits.max_depth = 8;
  limits.max_values = kMaxRecords * (128 + kMaxArtifacts * 8);
  limits.max_string_bytes = kMaxLaunchArgumentsBytes;
  const auto root = steam::json::Parse(text, limits);
  if (!root || !root->is_object()) { loaded_ = false; return Failure(RepositoryError::kMalformed, "runtime install registry is malformed"); }
  const auto* schema = Required(*root, "schema_version", JsonValue::Type::kNumber);
  const auto schema_number = schema ? schema->AsUint64() : std::nullopt;
  if (!schema_number) { loaded_ = false; return Failure(RepositoryError::kMalformed, "runtime install registry schema is malformed"); }
  if (*schema_number != kSchemaVersion) { loaded_ = false; return Failure(RepositoryError::kUnsupportedSchema, "runtime install registry schema is unsupported"); }
  const auto* reservations = Required(*root, "reservations", JsonValue::Type::kArray);
  const auto* installs = Required(*root, "installs", JsonValue::Type::kArray);
  if (!reservations || !installs || reservations->array().size() > kMaxRecords || installs->array().size() > kMaxRecords) {
    loaded_ = false; return Failure(RepositoryError::kMalformed, "runtime install registry entries are malformed");
  }
  // Written once a game from another store was given a number; absent: none was.
  std::uint32_t next_number = kFirstRuntimeNumber;
  const auto* next_value = root->Find("next_runtime_number");
  if (next_value) {
    const auto parsed = next_value->AsUint64();
    if (!parsed || *parsed < kFirstRuntimeNumber || *parsed > std::numeric_limits<std::uint32_t>::max()) {
      loaded_ = false; return Failure(RepositoryError::kMalformed, "runtime number allocator is malformed");
    }
    next_number = static_cast<std::uint32_t>(*parsed);
  }
  std::unordered_map<std::string, std::string> next_reservations;
  for (const auto& value : reservations->array()) {
    const auto* install = Required(value, "install_id", JsonValue::Type::kString);
    const auto* title = Required(value, "title_id", JsonValue::Type::kString);
    if (!install || !title || !IsSimpleId(install->text()) || !IsManagedTitle(title->text()) ||
        !next_reservations.emplace(title->text(), install->text()).second) {
      loaded_ = false; return Failure(RepositoryError::kMalformed, "runtime title reservations are malformed");
    }
  }
  std::unordered_map<std::string, RuntimeInstall> next_installs;
  RepositoryError parse_error = RepositoryError::kMalformed;
  for (const auto& value : installs->array()) {
    auto record = ParseRecord(value, parse_error);
    if (!record) { loaded_ = false; return Failure(parse_error, "runtime install record is invalid"); }
    if (!next_installs.emplace(record->install_id, *record).second ||
        next_reservations.find(record->title_id) == next_reservations.end() ||
        next_reservations[record->title_id] != record->install_id) {
      loaded_ = false; return Failure(RepositoryError::kMalformed, "runtime install identities conflict");
    }
  }
  if (next_installs.size() != next_reservations.size()) {
    loaded_ = false; return Failure(RepositoryError::kMalformed, "runtime title reservation has no install record");
  }
  // One number, one game: logs, journals and launch records name games by number.
  std::unordered_map<std::uint32_t, playden::GameId> numbers;
  for (const auto& entry : next_installs) {
    const auto& record = entry.second;
    const auto known = numbers.emplace(record.runtime_number, record.game);
    if (!known.second && known.first->second != record.game) {
      loaded_ = false; return Failure(RepositoryError::kConflict, "two games share runtime number " +
                                      std::to_string(record.runtime_number));
    }
    // A number at or past the allocator's next (a file edited by hand) moves it on.
    if (record.game.store != playden::Store::kSteam && record.runtime_number >= next_number)
      next_number = record.runtime_number + 1;
  }
  installs_ = std::move(next_installs); reservations_ = std::move(next_reservations);
  allocated_.clear();
  next_runtime_number_ = next_number; write_next_runtime_number_ = next_value != nullptr;
  loaded_ = true;
  return RepositoryResult<void>::Success();
}

RepositoryResult<void> RuntimeInstallRepository::Persist() {
  if (installs_.size() > kMaxRecords || reservations_.size() > kMaxRecords)
    return Failure(RepositoryError::kBounds, "runtime install registry exceeds its record limit");
  std::vector<std::string> install_ids;
  install_ids.reserve(installs_.size());
  for (const auto& entry : installs_) install_ids.push_back(entry.first);
  std::sort(install_ids.begin(), install_ids.end());
  std::vector<std::string> titles;
  titles.reserve(reservations_.size());
  for (const auto& entry : reservations_) titles.push_back(entry.first);
  std::sort(titles.begin(), titles.end());
  TextOutput output;
  output << "{\"schema_version\":1,\"reservations\":[";
  for (std::size_t i = 0; i < titles.size(); ++i) {
    if (i) output << ',';
    output << '{'; StringField(output, "install_id", reservations_.at(titles[i]));
    StringField(output, "title_id", titles[i], false); output << '}';
  }
  output << "],\"installs\":[";
  for (std::size_t i = 0; i < install_ids.size(); ++i) {
    if (i) output << ','; WriteRecord(output, installs_.at(install_ids[i]));
  }
  output << ']';
  if (write_next_runtime_number_) output << ",\"next_runtime_number\":" << next_runtime_number_;
  output << '}';
  const std::string text = output.str();
  if (text.size() > kMaxFileBytes) return Failure(RepositoryError::kBounds, "runtime install registry exceeds its size limit");
  const auto written = storage_.WriteDurable(relative_path_, Bytes(text.begin(), text.end()));
  if (!written.ok()) loaded_ = false;
  return written.ok() ? RepositoryResult<void>::Success()
                      : Failure(RepositoryError::kIo, "runtime install registry could not be written");
}

RepositoryResult<void> RuntimeInstallRepository::Save() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return Failure(RepositoryError::kNotLoaded, "runtime install registry is not loaded");
  return Persist();
}

std::vector<RuntimeInstall> RuntimeInstallRepository::Snapshot() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<RuntimeInstall> result;
  if (!loaded_) return result;
  result.reserve(installs_.size());
  for (const auto& entry : installs_) result.push_back(entry.second);
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.install_id < right.install_id;
  });
  return result;
}

std::optional<RuntimeInstall> RuntimeInstallRepository::Find(const std::string& install_id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return std::nullopt;
  const auto found = installs_.find(install_id);
  return found == installs_.end() ? std::nullopt : std::optional<RuntimeInstall>(found->second);
}

RepositoryResult<RuntimeInstall> RuntimeInstallRepository::Reserve(const ReserveRequest& request) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return RepositoryResult<RuntimeInstall>::Failure(RepositoryError::kNotLoaded, "runtime install registry is not loaded");
  const auto game = RequestGame(request.game, request.runtime_number);
  const auto existing = installs_.find(request.install_id);
  if (existing != installs_.end()) {
    if (existing->second.runtime_number != request.runtime_number || existing->second.game != game)
      return RepositoryResult<RuntimeInstall>::Failure(RepositoryError::kConflict, "install identity belongs to another app");
    return RepositoryResult<RuntimeInstall>::Success(existing->second);
  }
  for (const auto& entry : installs_)
    if (entry.second.runtime_number == request.runtime_number && entry.second.game != game)
      return RepositoryResult<RuntimeInstall>::Failure(RepositoryError::kConflict,
          "runtime number belongs to another game");
  for (const auto& entry : allocated_)
    if (entry.second == request.runtime_number && entry.first != game)
      return RepositoryResult<RuntimeInstall>::Failure(RepositoryError::kConflict,
          "runtime number belongs to another game");
  if (installs_.size() >= kMaxRecords)
    return RepositoryResult<RuntimeInstall>::Failure(RepositoryError::kBounds,
        "runtime install registry has reached its record limit");
  RuntimeInstall candidate;
  candidate.install_id = request.install_id; candidate.runtime_number = request.runtime_number;
  candidate.game = game;
  candidate.data_root = "/data/playden/games/" + request.install_id;
  candidate.default_profile_id = request.default_profile_id;
  candidate.runtime_bundle_id = request.runtime_bundle_id; candidate.recipe_id = request.recipe_id;
  candidate.recipe_version = request.recipe_version; candidate.settings_revision = request.settings_revision;
  candidate.launch_entry_id = request.launch_entry_id; candidate.launch_path = request.launch_path;
  candidate.launch_arguments = request.launch_arguments;
  candidate.launch_working_directory = request.launch_working_directory;
  candidate.launch_in_install_root = request.launch_in_install_root;
  candidate.install_path = request.install_path;
  candidate.volume_id = request.volume_id;
  if (!NormalizeLaunchPaths(candidate))
    return RepositoryResult<RuntimeInstall>::Failure(RepositoryError::kInvalidRecord,
        "launch paths must remain within the game install");
  std::unordered_set<std::string> seen;
  for (const auto& title : request.kit_title_variants) {
    if (!IsManagedTitle(title) || !seen.insert(title).second || reservations_.count(title)) continue;
    const auto available = native_availability_ ? native_availability_(title) : NativeTitleAvailability{};
    if (available.inventory == TitleAvailability::kFree && available.source_path == TitleAvailability::kFree) {
      candidate.title_id = title; break;
    }
  }
  if (candidate.title_id.empty())
    return RepositoryResult<RuntimeInstall>::Failure(RepositoryError::kTitleVariantsExhausted, "no supported native title variant is available");
  if (!ValidRecord(candidate))
    return RepositoryResult<RuntimeInstall>::Failure(RepositoryError::kInvalidRecord, "runtime install request is invalid");
  installs_.emplace(candidate.install_id, candidate);
  reservations_.emplace(candidate.title_id, candidate.install_id);
  const auto persisted = Persist();
  if (!persisted.ok()) {
    reservations_.erase(candidate.title_id); installs_.erase(candidate.install_id);
    return RepositoryResult<RuntimeInstall>::Failure(persisted.error(), persisted.message());
  }
  return RepositoryResult<RuntimeInstall>::Success(candidate);
}

RepositoryResult<RuntimeInstall> RuntimeInstallRepository::Adopt(const LegacyInstall& legacy) {
  ReserveRequest request;
  request.install_id = legacy.install_id; request.runtime_number = legacy.runtime_number;
  request.game = legacy.game;
  request.default_profile_id = legacy.default_profile_id;
  request.runtime_bundle_id = legacy.runtime_bundle_id; request.recipe_id = legacy.recipe_id;
  request.recipe_version = legacy.recipe_version; request.launch_entry_id = legacy.launch_entry_id;
  request.launch_path = legacy.launch_path; request.launch_arguments = legacy.launch_arguments;
  request.launch_working_directory = legacy.launch_working_directory;
  request.install_path = legacy.install_path; request.volume_id = legacy.volume_id;
  request.kit_title_variants = legacy.kit_title_variants;
  return Reserve(request);
}

RepositoryResult<void> RuntimeInstallRepository::Update(const RuntimeInstall& record) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return Failure(RepositoryError::kNotLoaded, "runtime install registry is not loaded");
  const auto found = installs_.find(record.install_id);
  if (found == installs_.end()) return Failure(RepositoryError::kNotFound, "runtime install does not exist");
  auto normalized = record;
  if (!NormalizeLaunchPaths(normalized) || !ValidRecord(normalized))
    return Failure(RepositoryError::kInvalidRecord, "runtime install record is invalid");
  const auto& current = found->second;
  if (normalized.game == playden::GameId{}) normalized.game = current.game;
  if (record.title_id != current.title_id || record.runtime_number != current.runtime_number ||
      normalized.game != current.game || record.data_root != current.data_root || record.schema_version != current.schema_version)
    return Failure(RepositoryError::kConflict, "runtime install identity is immutable");
  const auto previous = current;
  found->second = std::move(normalized);
  const auto persisted = Persist();
  if (!persisted.ok()) { found->second = previous; return persisted; }
  return RepositoryResult<void>::Success();
}

RepositoryResult<void> RuntimeInstallRepository::UpdateAll(const std::vector<RuntimeInstall>& records) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return Failure(RepositoryError::kNotLoaded, "runtime install registry is not loaded");
  std::unordered_map<std::string, RuntimeInstall> next = installs_;
  for (const auto& record : records) {
    const auto found = next.find(record.install_id);
    if (found == next.end()) return Failure(RepositoryError::kNotFound, "runtime install does not exist");
    auto normalized = record;
    if (!NormalizeLaunchPaths(normalized) || !ValidRecord(normalized))
      return Failure(RepositoryError::kInvalidRecord, "runtime install record is invalid");
    const auto& current = found->second;
    if (normalized.game == playden::GameId{}) normalized.game = current.game;
    if (record.title_id != current.title_id || record.runtime_number != current.runtime_number ||
        normalized.game != current.game || record.data_root != current.data_root || record.schema_version != current.schema_version)
      return Failure(RepositoryError::kConflict, "runtime install identity is immutable");
    found->second = std::move(normalized);
  }
  auto previous = std::move(installs_);
  installs_ = std::move(next);
  const auto persisted = Persist();
  if (!persisted.ok()) { installs_ = std::move(previous); return persisted; }
  return RepositoryResult<void>::Success();
}

RepositoryResult<void> RuntimeInstallRepository::EraseAfterUnregister(const std::string& install_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return Failure(RepositoryError::kNotLoaded, "runtime install registry is not loaded");
  const auto found = installs_.find(install_id);
  if (found == installs_.end()) return Failure(RepositoryError::kNotFound, "runtime install does not exist");
  return EraseAfterUnregister(install_id, native_availability_ ? native_availability_(found->second.title_id)
                                                               : NativeTitleAvailability{});
}

RepositoryResult<void> RuntimeInstallRepository::EraseAfterUnregister(const std::string& install_id,
                                                                      const NativeTitleAvailability& availability) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return Failure(RepositoryError::kNotLoaded, "runtime install registry is not loaded");
  const auto found = installs_.find(install_id);
  if (found == installs_.end()) return Failure(RepositoryError::kNotFound, "runtime install does not exist");
  if (found->second.state != PreparationState::kRemoving)
    return Failure(RepositoryError::kRemovalIncomplete, "runtime install is not awaiting removal");
  if (availability.inventory != TitleAvailability::kFree ||
      availability.source_path != TitleAvailability::kFree)
    return Failure(RepositoryError::kRemovalIncomplete, "native unregister or source cleanup is incomplete");
  const auto record = found->second;
  installs_.erase(found); reservations_.erase(record.title_id);
  const auto persisted = Persist();
  if (!persisted.ok()) {
    installs_.emplace(record.install_id, record); reservations_.emplace(record.title_id, record.install_id);
    return persisted;
  }
  // Installed again in this session, the game gets its number back.
  if (record.game.store != playden::Store::kSteam) allocated_.emplace(record.game, record.runtime_number);
  return RepositoryResult<void>::Success();
}

RepositoryResult<std::uint32_t> RuntimeInstallRepository::RuntimeNumberFor(const playden::GameId& game) {
  using Result = RepositoryResult<std::uint32_t>;
  if (const auto app = game.steam_app()) return Result::Success(*app);
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return Result::Failure(RepositoryError::kNotLoaded, "runtime install registry is not loaded");
  if (!game.valid()) return Result::Failure(RepositoryError::kInvalidRecord, "the game has no valid identity");
  for (const auto& entry : installs_)
    if (entry.second.game == game) return Result::Success(entry.second.runtime_number);
  if (const auto found = allocated_.find(game); found != allocated_.end()) return Result::Success(found->second);
  std::unordered_set<std::uint32_t> used;
  for (const auto& entry : installs_) used.insert(entry.second.runtime_number);
  for (const auto& entry : allocated_) used.insert(entry.second);
  auto number = next_runtime_number_;
  while (number <= kLastRuntimeNumber && used.count(number)) ++number;
  if (number < kFirstRuntimeNumber || number > kLastRuntimeNumber)
    return Result::Failure(RepositoryError::kBounds, "no runtime number is left for another game");
  const auto previous_next = next_runtime_number_;
  const auto previous_write = write_next_runtime_number_;
  next_runtime_number_ = number + 1;  // at most 0xFFFFFFFF: the range is used up
  write_next_runtime_number_ = true;
  const auto persisted = Persist();
  if (!persisted.ok()) {
    next_runtime_number_ = previous_next; write_next_runtime_number_ = previous_write;
    return Result::Failure(persisted.error(), persisted.message());
  }
  allocated_.emplace(game, number);
  return Result::Success(number);
}

RepositoryResult<void> RuntimeInstallRepository::RememberNumber(const playden::GameId& game, std::uint32_t runtime_number) {
  if (game.steam_app()) return RepositoryResult<void>::Success();
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!loaded_) return Failure(RepositoryError::kNotLoaded, "runtime install registry is not loaded");
  if (!game.valid() || runtime_number < kFirstRuntimeNumber || runtime_number > kLastRuntimeNumber)
    return Failure(RepositoryError::kInvalidRecord, "the game or its runtime number is invalid");
  for (const auto& entry : installs_) {
    const bool same_game = entry.second.game == game, same_number = entry.second.runtime_number == runtime_number;
    if (same_game != same_number) return Failure(RepositoryError::kConflict, "runtime number belongs to another game");
    if (same_game) return RepositoryResult<void>::Success();
  }
  for (const auto& entry : allocated_) {
    const bool same_game = entry.first == game, same_number = entry.second == runtime_number;
    if (same_game != same_number) return Failure(RepositoryError::kConflict, "runtime number belongs to another game");
    if (same_game) return RepositoryResult<void>::Success();
  }
  allocated_.emplace(game, runtime_number);
  return RepositoryResult<void>::Success();
}

}  // namespace playden::runtime
