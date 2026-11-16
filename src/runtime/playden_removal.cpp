// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/playden_removal.h"
#include "platform/data_roots.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace fs = std::filesystem;
namespace roots = platform::data_roots;
namespace {
using Log = std::function<void(const std::string&)>;
constexpr int kMaxDepth = 64;

fs::path Under(const fs::path& prefix, const fs::path& console) {
  return prefix.empty() ? console : prefix / console.relative_path();
}

// The names in a folder, listed through a descriptor opened without following a link and checked to be the
// folder lstat saw: a link in a folder's place lists nothing (it is then deleted as a link).
std::vector<std::string> Names(const fs::path& folder) {
  std::vector<std::string> names;
  struct stat seen{}, opened{};
  if (::lstat(folder.c_str(), &seen) != 0 || !S_ISDIR(seen.st_mode)) return names;
  const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return names;
  if (::fstat(fd, &opened) != 0 || opened.st_dev != seen.st_dev || opened.st_ino != seen.st_ino) {
    ::close(fd);
    return names;
  }
  DIR* listing = ::fdopendir(fd);
  if (!listing) { ::close(fd); return names; }
  while (const auto* entry = ::readdir(listing)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") names.push_back(name);
  }
  ::closedir(listing);
  return names;
}

// Deletes path and everything under it. A link is unlinked, never followed. The folder's names are read
// whole before any is removed (a by-path listing must not change under it). False with *reason on the
// first entry that stays.
bool RemoveTree(const fs::path& path, int depth, std::string* reason) {
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) return true;
    *reason = path.string() + ": " + std::strerror(errno);
    return false;
  }
  if (S_ISDIR(info.st_mode)) {
    if (depth > kMaxDepth) { *reason = path.string() + ": too deep"; return false; }
    bool ok = true;
    for (const auto& name : Names(path)) ok = RemoveTree(path / name, depth + 1, reason) && ok;
    if (::rmdir(path.c_str()) != 0 && errno != ENOENT) {
      if (ok) *reason = path.string() + ": " + std::strerror(errno);
      return false;
    }
    return ok;
  }
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    *reason = path.string() + ": " + std::strerror(errno);
    return false;
  }
  return true;
}

bool IsFolder(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool EmptyFolder(const fs::path& path) { return IsFolder(path) && Names(path).empty(); }

void Note(const Log& log, const std::string& line) { if (log) log(line); }

const char* StepName(RemovalStep step) { return step == RemovalStep::kFiles ? "files" : "titles"; }
}  // namespace

PlaydenRemovalPaths ConsoleRemovalPaths(const fs::path& prefix) {
  PlaydenRemovalPaths paths;
  paths.data_root = Under(prefix, roots::kPlaydenRoot);
  paths.games_files_root = Under(prefix, roots::kGamesFilesRoot);
  paths.import_root = Under(prefix, roots::kImportRoot);
  paths.app_root = Under(prefix, fs::path(roots::kHomebrewRoot) / roots::kAppTitleId);
  for (const auto& mount : roots::DriveMounts()) paths.drive_mounts.push_back(Under(prefix, mount));
  return paths;
}

bool RemovalStarted(const fs::path& data_root) {
  struct stat info{};
  return ::lstat((data_root / roots::kUninstallJournal).c_str(), &info) == 0;
}

std::optional<RemovalJournal> LoadRemovalJournal(const fs::path& data_root) {
  const auto path = data_root / roots::kUninstallJournal;
  // Anything but a whole journal of the files step (missing bytes, another kind of entry, a link) is taken as a
  // removal still at its titles, keeping saves: that step removes the titles first and keeps every bottle, so
  // doubt never deletes a save or a bottle a title still uses.
  const RemovalJournal doubtful{RemovalStep::kTitles, true};
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT ? std::nullopt : std::optional<RemovalJournal>(doubtful);
  if (!S_ISREG(info.st_mode) || info.st_size > 255) return doubtful;
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return doubtful;
  char buffer[256] = {};
  const auto count = ::read(fd, buffer, sizeof(buffer) - 1);
  ::close(fd);
  const std::string text = count > 0 ? std::string(buffer, static_cast<std::size_t>(count)) : std::string();
  const bool keep = text.find("\"keep_saves\":true") != std::string::npos;
  const bool drop = text.find("\"keep_saves\":false") != std::string::npos;
  const bool titles = text.find("\"step\":\"titles\"") != std::string::npos;
  const bool files = text.find("\"step\":\"files\"") != std::string::npos;
  if (text.rfind("{\"schema\":1,", 0) != 0 || keep == drop || titles == files) return doubtful;
  return RemovalJournal{files ? RemovalStep::kFiles : RemovalStep::kTitles, keep};
}

bool SaveRemovalJournal(const fs::path& data_root, const RemovalJournal& journal) {
  const auto path = data_root / roots::kUninstallJournal;
  const auto temporary = data_root / (std::string(roots::kUninstallJournal) + ".tmp");
  const std::string text = std::string("{\"schema\":1,\"step\":\"") + StepName(journal.step) +
                           "\",\"keep_saves\":" + (journal.keep_saves ? "true" : "false") + "}\n";
  ::unlink(temporary.c_str());
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
  if (fd < 0) return false;
  (void)::fchmod(fd, 0666);
  const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!written || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  if (const int folder = ::open(data_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); folder >= 0) {
    ::fsync(folder);
    ::close(folder);
  }
  return true;
}

PlaydenRemovalResult RemovePlaydenFiles(const PlaydenRemovalPaths& paths, bool keep_saves, const RemovalAsRoot& as_root,
                                        const Log& log) {
  PlaydenRemovalResult result;
  // Playden's own folder must be a folder, not a link to somewhere else: nothing is done through one.
  {
    struct stat info{};
    if (::lstat(paths.data_root.c_str(), &info) == 0 && !S_ISDIR(info.st_mode)) {
      result.failed.push_back(paths.data_root.string() + ": not a folder");
      Note(log, "playden removal refused: " + paths.data_root.string() + " is not a folder");
      return result;
    }
  }
  const auto remove = [&](const fs::path& path) {
    std::string reason;
    if (RemoveTree(path, 0, &reason)) return true;
    result.failed.push_back(reason);
    Note(log, "playden removal failed " + reason);
    return false;
  };
  // Root's folders, and anything else that refused as the app user (FTP leaves root-owned files).
  const auto remove_as_root = [&](const fs::path& path) {
    std::string reason;
    if (RemoveTree(path, 0, &reason)) return true;
    bool removed = false;
    if (as_root) as_root([&] { reason.clear(); removed = RemoveTree(path, 0, &reason); return true; });
    if (removed) return true;
    result.failed.push_back(reason);
    Note(log, "playden removal failed " + reason);
    return false;
  };
  const auto keep_import = [&](const fs::path& import) {
    if (!IsFolder(import)) return;
    if (EmptyFolder(import)) { remove_as_root(import); return; }
    result.kept.push_back(import.string());
    Note(log, "playden removal keeps " + import.string() + ": it holds files of yours");
  };
  // 1. Game files: on console storage, then on each drive (a drive that isn't there can't be cleaned).
  remove_as_root(paths.games_files_root);
  for (const auto& mount : paths.drive_mounts) {
    const auto folder = mount / roots::kDriveFolder;
    if (!IsFolder(folder)) continue;
    for (const auto& name : Names(folder)) {
      if (name == "import") keep_import(folder / name);
      else remove_as_root(folder / name);
    }
    if (EmptyFolder(folder)) remove_as_root(folder);
  }
  // 2. The drop folder for imports: the player's files.
  keep_import(paths.import_root);
  // 3. Root's folders under /data/playden: the runtime kit, the title rule, updates.
  for (const char* name : {roots::kKits, roots::kTitleRule, roots::kSelfUpdate}) remove_as_root(paths.data_root / name);
  // 4. The rest of /data/playden: the journal stays until the end, and so do the kept saves.
  for (const auto& name : Names(paths.data_root)) {
    if (name == roots::kUninstallJournal) continue;
    if (keep_saves && name == roots::kGames) {
      const auto games = paths.data_root / name;
      for (const auto& bottle : Names(games)) {
        const bool kept = bottle.size() > 5 && bottle.compare(bottle.size() - 5, 5, ".kept") == 0;
        if (kept) {
          result.kept.push_back((games / bottle).string());
          continue;
        }
        remove_as_root(games / bottle);
      }
      continue;
    }
    remove_as_root(paths.data_root / name);
  }
  // 5. What the app folder held before the 0.6 layout. The app itself stays: the player deletes its tile.
  for (const char* name : {"playden-data", "logs"}) remove_as_root(paths.app_root / name);
  result.complete = result.failed.empty();
  // 6. Last: the journal, then /data/playden itself when nothing was kept in it.
  if (result.complete) {
    remove_as_root(paths.data_root / roots::kUninstallJournal);
    if (EmptyFolder(paths.data_root)) remove_as_root(paths.data_root);
    result.complete = result.failed.empty();
  }
  Note(log, "playden removal files complete=" + std::to_string(result.complete) +
                " kept=" + std::to_string(result.kept.size()) + " failed=" + std::to_string(result.failed.size()));
  return result;
}

}  // namespace playden::runtime
