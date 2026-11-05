// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/recipe_files.h"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {
namespace fs = std::filesystem;

constexpr off_t kMaxRecipeFile = 1024 * 1024;

std::optional<std::string> ReadRegular(const fs::path& path) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return std::nullopt;
  struct stat info {};
  std::string bytes;
  bool ok = ::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= kMaxRecipeFile;
  if (ok) {
    bytes.resize(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
      const auto count = ::read(file, bytes.data() + done, bytes.size() - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) ok = false;
      else done += static_cast<std::size_t>(count);
    }
  }
  ::close(file);
  if (!ok) return std::nullopt;
  return bytes;
}

// Game files are open to FTP, whatever drive they are on, and the title can't set its umask.
constexpr mode_t kFolderMode = 0777;
constexpr mode_t kFileMode = 0666;

enum class Folders { kReady, kInTheWay, kFailed };

Folders MakeFoldersTo(const fs::path& root, const std::string& relative) {
  auto folder = root;
  for (const auto& part : fs::path(relative).parent_path()) {
    folder /= part;
    struct stat info {};
    if (::lstat(folder.c_str(), &info) == 0) {
      if (!S_ISDIR(info.st_mode)) return Folders::kInTheWay;
      continue;
    }
    if (errno != ENOENT) return Folders::kFailed;
    if (::mkdir(folder.c_str(), kFolderMode) != 0 || ::chmod(folder.c_str(), kFolderMode) != 0) return Folders::kFailed;
  }
  return Folders::kReady;
}

bool WriteAtomic(const fs::path& path, std::string_view bytes) {
  const auto temporary = path.parent_path() / (".playden-" + path.filename().string() + ".tmp");
  ::unlink(temporary.c_str());
  const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kFileMode);
  if (file < 0) return false;
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < bytes.size()) {
    const auto count = ::write(file, bytes.data() + done, bytes.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) ok = false;
    else done += static_cast<std::size_t>(count);
  }
  ok = ok && ::fchmod(file, kFileMode) == 0 && ::fsync(file) == 0;
  ok = ::close(file) == 0 && ok;
  if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  return true;
}
}  // namespace

RecipeFilesResult ApplyRecipeFiles(const std::string& game_root, const std::string& source_dir,
                                   const std::vector<std::string>& paths) {
  RecipeFilesResult result;
  const fs::path root(game_root);
  for (const auto& relative : paths) {
    const auto path = root / relative;
    struct stat info {};
    if (::lstat(path.c_str(), &info) == 0) {
      // Playden's own from an earlier start needs nothing; anything else is the game's.
      const auto current = S_ISREG(info.st_mode) ? ReadRegular(path) : std::nullopt;
      const auto source = ReadRegular(fs::path(source_dir) / relative);
      if (!current || !source || *current != *source) result.skipped.push_back(relative + ": the game has its own");
      continue;
    }
    const auto source = ReadRegular(fs::path(source_dir) / relative);
    if (!source) {
      result.skipped.push_back(relative + ": Playden has no copy");
      continue;
    }
    const auto folders = MakeFoldersTo(root, relative);
    if (folders == Folders::kInTheWay) {
      result.skipped.push_back(relative + ": something else is in the way of its folder");
      continue;
    }
    if (folders == Folders::kFailed || !WriteAtomic(path, *source)) {
      result.ok = false;
      result.message = "Playden could not put a file this game needs at " + relative + ".";
      return result;
    }
    result.changed.push_back(relative);
  }
  return result;
}

}  // namespace playden::runtime
