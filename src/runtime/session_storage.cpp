#include "runtime/session_storage.h"

#include "platform/managed_directory.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace playden::runtime {
namespace fs = std::filesystem;
namespace json = playden::steam::json;
using platform::DirectoryPolicy;
using platform::ManagedDirectory;

namespace {

constexpr std::string_view kCanonicalDataRoot = "/data";
constexpr std::size_t kMaxIdentityBytes = 128;
constexpr std::size_t kMaxMessageBytes = 4096;

SessionEnumeration SourceFailure(std::string code, std::string message) {
  return SessionEnumeration::Failure(std::move(code), std::move(message));
}

bool Identifier(std::string_view value, std::size_t maximum = 64) {
  if (value.empty() || value.size() > maximum) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
           byte == '.';
  });
}

bool SafeRelative(std::string_view value) {
  if (value.empty() || value.size() > 512 || value.front() == '/' ||
      value.back() == '/' || value.find('\\') != std::string_view::npos ||
      value.find('\0') != std::string_view::npos)
    return false;
  std::size_t start = 0;
  while (start < value.size()) {
    const auto slash = value.find('/', start);
    const auto part = value.substr(
        start, (slash == std::string_view::npos ? value.size() : slash) - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

bool LowerHex(std::string_view value, std::size_t length) {
  return value.size() == length &&
         std::all_of(value.begin(), value.end(), [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

bool CanonicalUuid(std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-')
    return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto byte = static_cast<unsigned char>(value[index]);
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return false;
  }
  // Match pw_title_session_id_valid: callers may supply any canonical
  // lowercase dashed hex ID; generation alone chooses RFC v4 bits.
  return true;
}

std::optional<std::string> ReadFile(const ManagedDirectory &directory,
                                    const std::string &name,
                                    std::size_t maximum) {
  if (!directory.Validate()) return std::nullopt;
  const int descriptor = directory.OpenRegularFile(name, true, 0600);
  if (descriptor < 0) return std::nullopt;
  struct stat info {};
  bool valid = ::fstat(descriptor, &info) == 0 && info.st_size >= 0 &&
               static_cast<std::uint64_t>(info.st_size) <= maximum;
  std::string bytes;
  std::array<char, 1024> buffer{};
  while (valid) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || static_cast<std::size_t>(count) > maximum - bytes.size()) {
      valid = false;
      break;
    }
    if (count == 0) break;
    bytes.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (::close(descriptor) != 0) valid = false;
  if (!directory.Validate()) valid = false;
  if (!valid) return std::nullopt;
  return bytes;
}

std::optional<std::map<std::string, std::string>> ParseMarker(
    std::string_view bytes) {
  if (bytes.empty() || bytes.size() >
                           FilesystemReconcileSessionSource::kMaxMarkerBytes ||
      bytes.back() != '\n')
    return std::nullopt;
  std::map<std::string, std::string> values;
  std::size_t start = 0;
  while (start < bytes.size()) {
    const auto end = bytes.find('\n', start);
    if (end == std::string_view::npos || end == start) return std::nullopt;
    const auto equal = bytes.find('=', start);
    if (equal == std::string_view::npos || equal == start || equal >= end)
      return std::nullopt;
    std::string key(bytes.substr(start, equal - start));
    std::string value(bytes.substr(equal + 1, end - equal - 1));
    if (key.size() > 32 || value.size() > 512 ||
        !values.emplace(std::move(key), std::move(value)).second)
      return std::nullopt;
    start = end + 1;
  }
  return values;
}

bool MarkerMatches(const ManagedDirectory &bottle,
                   const RuntimeInstall &record) {
  const auto bytes = ReadFile(bottle, ".playden-template-owner",
                              FilesystemReconcileSessionSource::kMaxMarkerBytes);
  if (!bytes) return false;
  const auto marker = ParseMarker(*bytes);
  static const std::set<std::string> keys = {
      "schema",          "kind",           "install_id",
      "title_id",        "kit_id",         "files_sha256",
      "runtime_bundle_id", "runtime_revision", "runtime_path",
      "variant_id",      "template_path",  "recipe_id",
      "recipe_version"};
  if (!marker || marker->size() != keys.size()) return false;
  for (const auto &key : keys)
    if (!marker->count(key)) return false;
  const auto &get = [&](const char *key) -> const std::string & {
    return marker->at(key);
  };
  return get("schema") == "1" && get("kind") == "template" &&
         get("install_id") == record.install_id &&
         get("title_id") == record.title_id &&
         get("runtime_bundle_id") == record.runtime_bundle_id &&
         get("recipe_id") == record.recipe_id &&
         get("recipe_version") == record.recipe_version &&
         Identifier(get("install_id")) && Identifier(get("title_id"), 9) &&
         Identifier(get("kit_id"), 128) && LowerHex(get("files_sha256"), 64) &&
         Identifier(get("runtime_bundle_id")) &&
         Identifier(get("runtime_revision"), 128) &&
         SafeRelative(get("runtime_path")) && Identifier(get("variant_id"), 128) &&
         SafeRelative(get("template_path")) && Identifier(get("recipe_id")) &&
         Identifier(get("recipe_version"));
}

const json::Value *Required(const json::Value &object, const char *key,
                            json::Value::Type type) {
  const auto *value = object.Find(key);
  return value && value->type() == type ? value : nullptr;
}

std::optional<int> ParseInt(const json::Value &value) {
  if (!value.is_number() || value.text().empty()) return std::nullopt;
  int result = 0;
  const auto parsed = std::from_chars(value.text().data(),
                                      value.text().data() + value.text().size(),
                                      result);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != value.text().data() + value.text().size() ||
      std::to_string(result) != value.text())
    return std::nullopt;
  return result;
}

std::optional<WrapperSession> ParseSession(std::string_view bytes,
                                           const std::string &filename_id) {
  auto root = json::Parse(bytes, {bytes.size(), 3, 32, 512});
  if (!root || !root->is_object()) return std::nullopt;
  const bool has_exit = root->Find("exit_code") != nullptr;
  if (root->object().size() != (has_exit ? 9U : 8U)) return std::nullopt;
  using Type = json::Value::Type;
  const auto *schema = Required(*root, "schema_version", Type::kNumber);
  const auto *session_id = Required(*root, "session_id", Type::kString);
  const auto *install_id = Required(*root, "install_id", Type::kString);
  const auto *title_id = Required(*root, "title_id", Type::kString);
  const auto *origin = Required(*root, "origin", Type::kString);
  const auto *revision = Required(*root, "settings_revision", Type::kNumber);
  const auto *runtime = Required(*root, "runtime_bundle_id", Type::kString);
  const auto *phase = Required(*root, "phase", Type::kString);
  if (!schema || schema->AsUint64() != 1 || !session_id || !install_id ||
      !title_id || !origin || !revision || !runtime || !phase ||
      !CanonicalUuid(session_id->text()) || session_id->text() != filename_id ||
      !Identifier(install_id->text()) || title_id->text().size() != 9 ||
      !Identifier(title_id->text(), 9) ||
      !Identifier(runtime->text()) || !revision->AsUint64())
    return std::nullopt;

  WrapperSession result;
  result.session_id = session_id->text();
  result.install_id = install_id->text();
  result.title_id = title_id->text();
  result.runtime_bundle_id = runtime->text();
  result.settings_revision = *revision->AsUint64();
  if (origin->text() == "home")
    result.origin = SessionOrigin::kHome;
  else if (origin->text() == "playden")
    result.origin = SessionOrigin::kPlayden;
  else
    return std::nullopt;
  if (phase->text() == "starting")
    result.phase = WrapperSessionPhase::kStarting;
  else if (phase->text() == "running")
    result.phase = WrapperSessionPhase::kRunning;
  else if (phase->text() == "failed")
    result.phase = WrapperSessionPhase::kFailed;
  else if (phase->text() == "exited")
    result.phase = WrapperSessionPhase::kExited;
  else
    return std::nullopt;
  if (has_exit) {
    const auto *exit = Required(*root, "exit_code", Type::kNumber);
    if (!exit) return std::nullopt;
    result.exit_code = ParseInt(*exit);
    if (!result.exit_code) return std::nullopt;
  }
  if ((result.phase == WrapperSessionPhase::kStarting ||
       result.phase == WrapperSessionPhase::kRunning) &&
      result.exit_code)
    return std::nullopt;
  return result;
}

bool BoundToRecord(const WrapperSession &session, const RuntimeInstall &record) {
  return session.install_id == record.install_id &&
         session.title_id == record.title_id;
}

std::optional<ManagedDirectory> Child(ManagedDirectory directory,
                                      std::initializer_list<const char *> parts,
                                      DirectoryPolicy policy = DirectoryPolicy::kPrivate) {
  for (const auto *part : parts) {
    auto child = directory.OpenChild(part, policy);
    if (!child) return std::nullopt;
    directory = std::move(*child);
  }
  return directory;
}

std::optional<std::vector<std::string>> ListSessionFiles(
    const ManagedDirectory &directory, std::size_t limit) {
  if (!limit || limit > SessionReconciler::kMaxSessionsPerInstall ||
      !directory.Validate())
    return std::nullopt;
  const int duplicate = ::dup(directory.fd());
  if (duplicate < 0) return std::nullopt;
  DIR *stream = ::fdopendir(duplicate);
  if (!stream) {
    ::close(duplicate);
    return std::nullopt;
  }
  std::vector<std::string> names;
  std::size_t entries = 0;
  bool valid = true;
  errno = 0;
  while (const auto *entry = ::readdir(stream)) {
    const std::string name(entry->d_name);
    if (name == "." || name == "..") continue;
    if (++entries > FilesystemReconcileSessionSource::kMaxDirectoryEntries) {
      valid = false;
      break;
    }
    constexpr std::string_view suffix = ".json";
    if (name.size() != 36 + suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0 ||
        !CanonicalUuid(std::string_view(name).substr(0, 36))) {
      valid = false; // Includes pending .tmp/.lock evidence: never drop it.
      break;
    }
    names.push_back(name);
    if (names.size() > limit) {
      valid = false;
      break;
    }
    errno = 0;
  }
  if (errno != 0) valid = false;
  if (::closedir(stream) != 0) valid = false;
  if (!directory.Validate()) valid = false;
  if (!valid) return std::nullopt;
  std::sort(names.begin(), names.end());
  return names;
}

std::string Escape(std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size());
  for (const unsigned char byte : value) {
    switch (byte) {
    case '"': result += "\\\""; break;
    case '\\': result += "\\\\"; break;
    case '\b': result += "\\b"; break;
    case '\f': result += "\\f"; break;
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    default:
      if (byte < 0x20) {
        result += "\\u00";
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
      } else {
        result.push_back(static_cast<char>(byte));
      }
    }
  }
  return result;
}

const char *OriginName(SessionOrigin origin) {
  switch (origin) {
  case SessionOrigin::kHome: return "home";
  case SessionOrigin::kPlayden: return "playden";
  }
  return nullptr;
}
const char *SessionPhaseName(WrapperSessionPhase phase) {
  switch (phase) {
  case WrapperSessionPhase::kStarting: return "starting";
  case WrapperSessionPhase::kRunning: return "running";
  case WrapperSessionPhase::kFailed: return "failed";
  case WrapperSessionPhase::kExited: return "exited";
  }
  return nullptr;
}
const char *ReconcilePhaseName(ReconcilePhase phase) {
  switch (phase) {
  case ReconcilePhase::kNeedsRecoveryConfirmation: return "needs-recovery-confirmation";
  case ReconcilePhase::kDispatchPending: return "dispatch-pending";
  case ReconcilePhase::kCloudRunning: return "cloud-running";
  case ReconcilePhase::kCompletionPending: return "completion-pending";
  case ReconcilePhase::kCompleted: return "completed";
  case ReconcilePhase::kFailed: return "failed";
  }
  return nullptr;
}

std::optional<ReconcilePhase> ParseReconcilePhase(std::string_view value) {
  if (value == "needs-recovery-confirmation") return ReconcilePhase::kNeedsRecoveryConfirmation;
  if (value == "dispatch-pending") return ReconcilePhase::kDispatchPending;
  if (value == "cloud-running") return ReconcilePhase::kCloudRunning;
  if (value == "completion-pending") return ReconcilePhase::kCompletionPending;
  if (value == "completed") return ReconcilePhase::kCompleted;
  if (value == "failed") return ReconcilePhase::kFailed;
  return std::nullopt;
}

bool ValidJournalEntry(const ReconcileEntry &entry) {
  return entry.schema_version == 1 && entry.session.schema_version == 1 &&
         CanonicalUuid(entry.session.session_id) &&
         Identifier(entry.session.install_id) &&
         entry.session.title_id.size() == 9 &&
         Identifier(entry.session.title_id, 9) &&
         Identifier(entry.session.runtime_bundle_id) &&
         !entry.account_token.empty() && entry.account_token.size() <= kMaxIdentityBytes &&
         entry.operation_id.size() <= kMaxIdentityBytes &&
         entry.code.size() <= kMaxIdentityBytes &&
         entry.message.size() <= kMaxMessageBytes &&
         ((entry.phase != ReconcilePhase::kCloudRunning &&
           entry.phase != ReconcilePhase::kCompletionPending &&
           entry.phase != ReconcilePhase::kCompleted) ||
          !entry.operation_id.empty()) &&
         !((entry.session.phase == WrapperSessionPhase::kStarting ||
            entry.session.phase == WrapperSessionPhase::kRunning) &&
           entry.session.exit_code);
}

std::optional<std::string> SerializeJournal(const ReconcileEntry &entry) {
  if (!ValidJournalEntry(entry)) return std::nullopt;
  const auto *session_phase = SessionPhaseName(entry.session.phase);
  const auto *origin = OriginName(entry.session.origin);
  const auto *reconcile_phase = ReconcilePhaseName(entry.phase);
  if (!session_phase || !origin || !reconcile_phase) return std::nullopt;
  std::string bytes = "{\"schema_version\":1,\"session\":{\"schema_version\":1,";
  bytes += "\"session_id\":\"" + Escape(entry.session.session_id);
  bytes += "\",\"install_id\":\"" + Escape(entry.session.install_id);
  bytes += "\",\"title_id\":\"" + Escape(entry.session.title_id);
  bytes += "\",\"origin\":\"" + std::string(origin);
  bytes += "\",\"settings_revision\":" + std::to_string(entry.session.settings_revision);
  bytes += ",\"runtime_bundle_id\":\"" + Escape(entry.session.runtime_bundle_id);
  bytes += "\",\"phase\":\"" + std::string(session_phase) + "\"";
  if (entry.session.exit_code)
    bytes += ",\"exit_code\":" + std::to_string(*entry.session.exit_code);
  bytes += "},\"account_token\":\"" + Escape(entry.account_token);
  bytes += "\",\"phase\":\"" + std::string(reconcile_phase);
  bytes += "\",\"operation_id\":\"" + Escape(entry.operation_id);
  bytes += "\",\"code\":\"" + Escape(entry.code);
  bytes += "\",\"message\":\"" + Escape(entry.message) + "\"}\n";
  if (bytes.size() > DurableReconcileJournal::kMaxJournalBytes)
    return std::nullopt;
  return bytes;
}

std::optional<ReconcileEntry> ParseJournal(std::string_view bytes,
                                           const std::string &expected_id) {
  auto root = json::Parse(bytes, {DurableReconcileJournal::kMaxJournalBytes,
                                  4, 64, kMaxMessageBytes});
  if (!root || !root->is_object() || root->object().size() != 7) return std::nullopt;
  using Type = json::Value::Type;
  const auto *schema = Required(*root, "schema_version", Type::kNumber);
  const auto *session_value = Required(*root, "session", Type::kObject);
  const auto *account = Required(*root, "account_token", Type::kString);
  const auto *phase = Required(*root, "phase", Type::kString);
  const auto *operation = Required(*root, "operation_id", Type::kString);
  const auto *code = Required(*root, "code", Type::kString);
  const auto *message = Required(*root, "message", Type::kString);
  if (!schema || schema->AsUint64() != 1 || !session_value || !account ||
      !phase || !operation || !code || !message)
    return std::nullopt;
  std::string session_bytes;
  // Parse the nested object directly by rebuilding only its strict known schema.
  const auto *session_schema = Required(*session_value, "schema_version", Type::kNumber);
  const auto *session_id = Required(*session_value, "session_id", Type::kString);
  const auto *install = Required(*session_value, "install_id", Type::kString);
  const auto *title = Required(*session_value, "title_id", Type::kString);
  const auto *origin = Required(*session_value, "origin", Type::kString);
  const auto *revision = Required(*session_value, "settings_revision", Type::kNumber);
  const auto *runtime = Required(*session_value, "runtime_bundle_id", Type::kString);
  const auto *session_phase = Required(*session_value, "phase", Type::kString);
  const bool has_exit = session_value->Find("exit_code") != nullptr;
  if (session_value->object().size() != (has_exit ? 9U : 8U) ||
      !session_schema || session_schema->AsUint64() != 1 || !session_id ||
      !install || !title || !origin || !revision || !runtime || !session_phase)
    return std::nullopt;
  session_bytes = "{\"schema_version\":1,\"session_id\":\"" + Escape(session_id->text());
  session_bytes += "\",\"install_id\":\"" + Escape(install->text());
  session_bytes += "\",\"title_id\":\"" + Escape(title->text());
  session_bytes += "\",\"origin\":\"" + Escape(origin->text());
  session_bytes += "\",\"settings_revision\":" + revision->text();
  session_bytes += ",\"runtime_bundle_id\":\"" + Escape(runtime->text());
  session_bytes += "\",\"phase\":\"" + Escape(session_phase->text()) + "\"";
  if (has_exit) {
    const auto *exit = Required(*session_value, "exit_code", Type::kNumber);
    if (!exit) return std::nullopt;
    session_bytes += ",\"exit_code\":" + exit->text();
  }
  session_bytes += '}';
  auto session = ParseSession(session_bytes, expected_id);
  auto parsed_phase = ParseReconcilePhase(phase->text());
  if (!session || !parsed_phase || session->session_id != expected_id)
    return std::nullopt;
  ReconcileEntry entry;
  entry.session = std::move(*session);
  entry.account_token = account->text();
  entry.phase = *parsed_phase;
  entry.operation_id = operation->text();
  entry.code = code->text();
  entry.message = message->text();
  return ValidJournalEntry(entry) ? std::optional<ReconcileEntry>(std::move(entry))
                                  : std::nullopt;
}

} // namespace

FilesystemReconcileSessionSource::FilesystemReconcileSessionSource()
    : host_data_root_(kCanonicalDataRoot) {}

FilesystemReconcileSessionSource::FilesystemReconcileSessionSource(
    HostDataRootForTests mapping)
    : host_data_root_(std::move(mapping.path)) {}

std::optional<ManagedDirectory> FilesystemReconcileSessionSource::OpenSessions(
    const RuntimeInstall &record, SessionEnumeration *failure) {
  if (!Identifier(record.install_id) || record.title_id.size() != 9 ||
      !Identifier(record.title_id, 9) ||
      !Identifier(record.runtime_bundle_id) || !Identifier(record.recipe_id) ||
      !Identifier(record.recipe_version))
    {
    *failure = SourceFailure("session-record-invalid",
                         "runtime install identity is invalid");
    return std::nullopt;
  }
  const fs::path expected = fs::path(kCanonicalDataRoot) / "playden/games" /
                            record.install_id;
  if (record.data_root != expected.generic_string() ||
      host_data_root_.empty() || !host_data_root_.is_absolute())
    {
    *failure = SourceFailure("session-root-invalid",
                         "session root is not the canonical managed game root");
    return std::nullopt;
  }
  const auto mapped = host_data_root_ / "playden/games" / record.install_id;
  // The data root, library, prefixes and the bottle may be 0711 (bottle-profile-shared).
  auto game = ManagedDirectory::Open(mapped, DirectoryPolicy::kPrivateTraversable);
  if (!game)
    {
    *failure = SourceFailure("session-root-unavailable",
                         "managed game root is unavailable or unsafe");
    return std::nullopt;
  }
  auto marker_root = ManagedDirectory::Open(mapped, DirectoryPolicy::kPrivateTraversable);
  if (!marker_root)
    {
    *failure = SourceFailure("session-root-unavailable",
                         "managed game root changed during ownership admission");
    return std::nullopt;
  }
  auto bottle = Child(std::move(*marker_root),
                      {"library", "prefixes", "game"}, DirectoryPolicy::kPrivateTraversable);
  if (!bottle || !MarkerMatches(*bottle, record))
    {
    *failure = SourceFailure("session-owner-invalid",
                         "managed bottle ownership marker is invalid");
    return std::nullopt;
  }
  // The title shell makes logs/sessions 0700, or 0777 with publication-open-modes (#868, FTP can delete them): any
  // mode of Playden's user is read, links never.
  auto logs = ManagedDirectory::Open(mapped / "logs", DirectoryPolicy::kOpenTree);
  auto sessions = logs ? logs->OpenChild("sessions", DirectoryPolicy::kOpenTree) : std::nullopt;
  if (!sessions || !game->Validate()) {
    *failure = SourceFailure("session-evidence-unavailable",
                         "wrapper session evidence is unavailable or unsafe");
    return std::nullopt;
  }
  return sessions;
}

SessionEnumeration FilesystemReconcileSessionSource::Enumerate(
    const RuntimeInstall &record, std::size_t limit) {
  SessionEnumeration failure;
  auto sessions = OpenSessions(record, &failure);
  if (!sessions) return failure;
  const auto names = ListSessionFiles(*sessions, limit);
  if (!names)
    return SourceFailure(
        "session-enumeration-pending",
        "wrapper session listing is unsafe, over limit, or contains pending evidence");
  std::vector<WrapperSession> result;
  result.reserve(names->size());
  for (const auto &name : *names) {
    const auto bytes = ReadFile(*sessions, name, kMaxSessionBytes);
    if (!bytes)
      return SourceFailure("session-read-failed",
                           "wrapper session record is unreadable, oversized, or unsafe");
    const auto id = name.substr(0, 36);
    auto session = ParseSession(*bytes, id);
    if (!session || !BoundToRecord(*session, record))
      return SourceFailure("session-identity-invalid",
                           "wrapper session syntax or persisted identity is invalid");
    result.push_back(std::move(*session));
  }
  return SessionEnumeration::Success(std::move(result));
}

SessionEnumeration FilesystemReconcileSessionSource::Read(
    const RuntimeInstall &record, const std::string &session_id) {
  if (!CanonicalUuid(session_id))
    return SourceFailure("session-id-invalid", "wrapper session id is invalid");
  SessionEnumeration failure;
  auto sessions = OpenSessions(record, &failure);
  if (!sessions) return failure;
  // An absent or unreadable record is not evidence yet.
  const auto bytes = ReadFile(*sessions, session_id + ".json", kMaxSessionBytes);
  if (!bytes) return SessionEnumeration::Success({});
  auto session = ParseSession(*bytes, session_id);
  if (!session || !BoundToRecord(*session, record))
    return SourceFailure("session-identity-invalid",
                         "wrapper session syntax or persisted identity is invalid");
  return SessionEnumeration::Success({std::move(*session)});
}

namespace {

// A record's modification time when it names this install and reached running or exited.
std::optional<std::int64_t> PlayedAt(const ManagedDirectory &sessions,
                                     const RuntimeInstall &record,
                                     const std::string &name,
                                     const struct stat &info) {
  const auto bytes = ReadFile(sessions, name, FilesystemReconcileSessionSource::kMaxSessionBytes);
  if (!bytes) return std::nullopt;
  const auto session = ParseSession(*bytes, name.substr(0, 36));
  if (!session || !BoundToRecord(*session, record)) return std::nullopt;
  if (session->phase != WrapperSessionPhase::kRunning &&
      session->phase != WrapperSessionPhase::kExited)
    return std::nullopt;
  if (info.st_mtime <= 0) return std::nullopt;
  return static_cast<std::int64_t>(info.st_mtime);
}

bool SessionRecordName(const std::string &name) {
  constexpr std::string_view suffix = ".json";
  return name.size() == 36 + suffix.size() &&
         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
         CanonicalUuid(std::string_view(name).substr(0, 36));
}

} // namespace

std::optional<std::int64_t> FilesystemReconcileSessionSource::LastPlayed(
    const RuntimeInstall &record, std::int64_t not_after) {
  SessionEnumeration failure;
  auto sessions = OpenSessions(record, &failure);
  if (!sessions) return std::nullopt;
  // By absolute path: a PS5 title can't open relative to a directory descriptor.
  DIR *stream = ::opendir(sessions->path().c_str());
  if (!stream) return std::nullopt;
  std::vector<std::pair<std::int64_t, std::string>> records;
  std::size_t entries = 0;
  while (const auto *entry = ::readdir(stream)) {
    if (++entries > kMaxPlayedEntries) break;
    const std::string name(entry->d_name);
    if (!SessionRecordName(name)) continue;
    struct stat info {};
    if (!sessions->InspectChild(name, &info) || !S_ISREG(info.st_mode) || info.st_mtime > not_after) continue;
    records.emplace_back(static_cast<std::int64_t>(info.st_mtime), name);
  }
  ::closedir(stream);
  // Newest first; one that never ran (starting, failed) or doesn't read gives way to the next.
  std::sort(records.begin(), records.end(), std::greater<>());
  for (const auto &[modified, name] : records) {
    struct stat info {};
    if (!sessions->InspectChild(name, &info) || !S_ISREG(info.st_mode)) continue;
    if (const auto played = PlayedAt(*sessions, record, name, info)) return played;
  }
  return std::nullopt;
}

std::optional<std::int64_t> FilesystemReconcileSessionSource::SessionPlayed(
    const RuntimeInstall &record, const std::string &session_id) {
  if (!CanonicalUuid(session_id)) return std::nullopt;
  SessionEnumeration failure;
  auto sessions = OpenSessions(record, &failure);
  if (!sessions) return std::nullopt;
  const auto name = session_id + ".json";
  struct stat info {};
  if (!sessions->InspectChild(name, &info) || !S_ISREG(info.st_mode)) return std::nullopt;
  return PlayedAt(*sessions, record, name, info);
}

DurableReconcileJournal::DurableReconcileJournal(Storage &storage)
    : storage_(storage) {}

ReconcileJournalLoad DurableReconcileJournal::Load(
    const std::string &session_id) {
  if (!CanonicalUuid(session_id))
    return ReconcileJournalLoad::Failure("journal-session-invalid",
                                         "journal session id is invalid");
  const auto loaded = storage_.ReadDurable(session_id + ".json", kMaxJournalBytes);
  if (!loaded.ok()) {
    if (loaded.error() == StorageError::kNotFound)
      return ReconcileJournalLoad::Success(std::nullopt);
    return ReconcileJournalLoad::Failure("journal-read-failed", loaded.message());
  }
  const std::string bytes(loaded.value().begin(), loaded.value().end());
  auto entry = ParseJournal(bytes, session_id);
  if (!entry)
    return ReconcileJournalLoad::Failure("journal-invalid",
                                         "reconciliation journal is malformed or unsupported");
  return ReconcileJournalLoad::Success(std::move(entry));
}

ReconcileJournalResult DurableReconcileJournal::Save(
    const ReconcileEntry &entry) {
  const auto bytes = SerializeJournal(entry);
  if (!bytes || !ParseJournal(*bytes, entry.session.session_id))
    return ReconcileJournalResult::Failure("journal-invalid",
                                           "reconciliation journal entry is invalid");
  const Bytes payload(bytes->begin(), bytes->end());
  const auto saved = storage_.WriteDurable(entry.session.session_id + ".json",
                                           payload);
  if (!saved.ok())
    return ReconcileJournalResult::Failure("journal-write-failed",
                                           saved.message());
  return ReconcileJournalResult::Success();
}

} // namespace playden::runtime
