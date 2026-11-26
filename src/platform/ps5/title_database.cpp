#include "platform/ps5/title_database.h"
#include "platform/ps5/title_runtime.h"
#include "sqlite3.h"
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sys/stat.h>
namespace playden::ps5 {
namespace {
// SQLite is built without internal threading. All app reader connections and
// initialization run under this one nonblocking gate, including native callers.
std::mutex reader_mutex;
std::optional<std::string> Text(sqlite3_stmt* statement, int column, int limit) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT) return std::nullopt;
  const int length = sqlite3_column_bytes(statement, column);
  const auto* bytes = sqlite3_column_text(statement, column);
  if (!bytes || length <= 0 || length > limit || std::memchr(bytes, 0, length)) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(bytes), length);
}
}
TitleDatabaseObservation ReadTitleDatabase(const std::string& path, const std::string& title) {
  std::unique_lock<std::mutex> lock(reader_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return {};
  if (!ValidTitleId(title) || path.empty() || path.find('\0') != std::string::npos ||
      !std::filesystem::path(path).is_absolute()) return {};
  struct stat before{};
  if (lstat(path.c_str(), &before) || !S_ISREG(before.st_mode) || before.st_size < 16 ||
      before.st_size > 64 * 1024 * 1024) return {};
  sqlite3* database = nullptr;
  const int opened = sqlite3_open_v2(path.c_str(), &database,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
  if (opened != SQLITE_OK) { if (database) sqlite3_close(database); return {}; }
  // No waiting, temporary files, writable handle, extension loading or cache.
  sqlite3_busy_timeout(database, 0);
  sqlite3_limit(database, SQLITE_LIMIT_LENGTH, 65536);
  sqlite3_limit(database, SQLITE_LIMIT_SQL_LENGTH, 4096);
  unsigned progress = 0;
  sqlite3_progress_handler(database, 1000, [](void* count) -> int {
    return ++*static_cast<unsigned*>(count) > 1000;
  }, &progress);
  sqlite3_stmt* statement = nullptr;
  TitleDatabaseObservation result;
  constexpr const char* query = "SELECT titleId,contentId,metaDataPath FROM tbl_contentinfo "
      "WHERE titleId = ?1 COLLATE NOCASE LIMIT 2";
  if (sqlite3_prepare_v2(database, query, -1, &statement, nullptr) == SQLITE_OK &&
      sqlite3_stmt_readonly(statement) && sqlite3_bind_text(statement, 1, title.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK) {
    const auto first = sqlite3_step(statement);
    if (first == SQLITE_DONE) result.inventory = runtime::TitleAvailability::kFree;
    if (first == SQLITE_ROW) {
      result.inventory = runtime::TitleAvailability::kOccupied;
      auto id = Text(statement, 0, 9), content = Text(statement, 1, 128), metadata = Text(statement, 2, 512);
      if (id && *id == title && content && metadata && std::filesystem::path(*metadata).is_absolute() &&
          std::filesystem::path(*metadata).lexically_normal().string() == *metadata)
        result.identity = InstalledTitleIdentity{*id, *content, *metadata};
      else
        result.reason = !id ? "title-id-unreadable" : *id != title ? "title-id-case" : !content ? "content-id-unreadable" :
            !metadata ? "metadata-path-unreadable" : "metadata-path-not-normal";
      const auto next = sqlite3_step(statement);
      // Duplicate/case aliases remain occupied.
      if (next == SQLITE_ROW) { result.identity.reset(); result.reason = "duplicate-rows"; }
      else if (next != SQLITE_DONE) result = {}; // An incomplete observation is unknown.
    }
  }
  if (statement && sqlite3_finalize(statement) != SQLITE_OK) result = {};
  if (sqlite3_close(database) != SQLITE_OK) result = {};
  struct stat after{};
  if (lstat(path.c_str(), &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
      !S_ISREG(after.st_mode)) return {};
  return result;
}
TitleDatabaseObservation ReadNativeTitleDatabase(const std::string& title) {
#if defined(__PROSPERO__)
  return ReadTitleDatabase("/system_data/priv/mms/app.db", title);
#else
  (void)title; return {};
#endif
}
}
