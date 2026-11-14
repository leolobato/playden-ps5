// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/recipe_exports.h"

#include "library/game_ref.h"
#include "platform/managed_directory.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>

namespace playden {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;

constexpr std::string_view kPrefix = "recipe-";
constexpr std::string_view kSuffix = ".json";
constexpr std::string_view kTemporarySuffix = ".json.tmp";

bool StartsWith(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }
bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// The folder, made when missing. Folders and files beside the logs are open to FTP, whose root has no DAC bypass on /data.
std::optional<ManagedDirectory> OpenFolder(const std::filesystem::path& folder, bool create) {
  if (!folder.is_absolute() || folder.filename().empty()) return std::nullopt;
  if (auto opened = ManagedDirectory::Open(folder, DirectoryPolicy::kApprovedTree)) return opened;
  if (!create) return std::nullopt;
  auto parent = ManagedDirectory::Open(folder.parent_path(), DirectoryPolicy::kApprovedTree);
  if (!parent) return std::nullopt;
  auto made = parent->OpenChild(folder.filename().string(), DirectoryPolicy::kApprovedTree, true, 0777);
  if (made) parent->Sync();
  return made;
}

// The folder's regular files that keep is true for. Every entry is looked at, so other files never hide a recipe; at
// most 4096 are kept.
template <typename Keep>
std::vector<std::string> RegularFiles(const std::filesystem::path& folder, Keep keep) {
  std::vector<std::string> names;
  std::error_code error;
  for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end && names.size() < 4096; it.increment(error)) {
    std::error_code kind;
    auto name = it->path().filename().string();
    if (keep(name) && it->symlink_status(kind).type() == std::filesystem::file_type::regular) names.push_back(std::move(name));
  }
  return names;
}

bool WriteAll(int file, std::string_view text) {
  std::size_t done = 0;
  while (done < text.size()) {
    const auto count = ::write(file, text.data() + done, text.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    done += static_cast<std::size_t>(count);
  }
  return true;
}

}  // namespace

std::string ExportedRecipeFileName(const GameId& id, std::string_view game_name) {
  // NameKey gives "game" for a name it keeps nothing of: told apart from a game called "Game" by what it makes of
  // the name after a word of its own.
  const std::string probe = "a " + std::string(game_name);
  const bool named = NameKey(probe) != "a";
  const auto install_id = ToInstallId(id);
  return std::string(kPrefix) + (named ? NameKey(game_name) + "-" : std::string()) + install_id + std::string(kSuffix);
}

bool IsExportedRecipeOf(std::string_view file, const GameId& id) {
  if (!id.valid() || !StartsWith(file, kPrefix) || !EndsWith(file, kSuffix)) return false;
  const auto middle = file.substr(kPrefix.size(), file.size() - kPrefix.size() - kSuffix.size());
  const auto install_id = ToInstallId(id);
  // The install ID ends the name, after a NameKey and "-" or alone; the older name had the key alone.
  if (middle == install_id || middle == ToKey(id)) return true;
  if (middle.size() <= install_id.size() + 1 || !EndsWith(middle, "-" + install_id)) return false;
  const auto key = middle.substr(0, middle.size() - install_id.size() - 1);
  return IsStoreKey(kRecipeStoreImported, key);
}

bool IsLogsFolderRecipe(std::string_view file) {
  if (!StartsWith(file, kPrefix)) return false;
  std::string_view middle;
  if (EndsWith(file, kSuffix)) middle = file.substr(kPrefix.size(), file.size() - kPrefix.size() - kSuffix.size());
  else if (EndsWith(file, kTemporarySuffix))
    middle = file.substr(kPrefix.size(), file.size() - kPrefix.size() - kTemporarySuffix.size());
  else
    return false;
  const auto id = ParseKey(middle);
  return id && ToKey(*id) == middle;
}

std::optional<std::filesystem::path> WriteExportedRecipe(const std::filesystem::path& folder, const GameId& id,
                                                         std::string_view game_name, std::string_view text) {
  if (!id.valid()) return std::nullopt;
  const auto directory = OpenFolder(folder, true);
  if (!directory) return std::nullopt;
  const auto name = ExportedRecipeFileName(id, game_name);
  const auto temporary = name + ".playden-tmp-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                         std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  const int file = directory->CreateExclusiveFile(temporary, 0644);
  if (file < 0) return std::nullopt;
  bool written = WriteAll(file, text) && ::fsync(file) == 0;
  written = ::close(file) == 0 && written;
  if (!written || !directory->RenameChild(temporary, *directory, name, true)) {
    directory->RemoveRegularFile(temporary);
    return std::nullopt;
  }
  directory->Sync();
  // One export per game: the one before, under its older name or the game's name before a rename, goes.
  for (const auto& other : RegularFiles(directory->path(), [&](const std::string& file) { return file != name && IsExportedRecipeOf(file, id); }))
    directory->RemoveRegularFile(other);
  return directory->path() / name;
}

std::optional<std::string> FindExportedRecipe(const std::filesystem::path& folder, const GameId& id) {
  const auto directory = OpenFolder(folder, false);
  if (!directory) return std::nullopt;
  std::optional<std::string> newest;
  std::int64_t newest_time = 0;
  for (const auto& name : RegularFiles(directory->path(), [&](const std::string& file) { return IsExportedRecipeOf(file, id); })) {
    struct stat info {};
    if (!directory->InspectChild(name, &info) || !S_ISREG(info.st_mode)) continue;
    const auto changed = static_cast<std::int64_t>(info.st_mtime);
    if (!newest || changed > newest_time || (changed == newest_time && name < *newest)) {
      newest = name;
      newest_time = changed;
    }
  }
  return newest;
}

std::optional<std::string> ReadExportedRecipe(const std::filesystem::path& folder, const std::string& file) {
  const auto directory = OpenFolder(folder, false);
  if (!directory) return std::nullopt;
  const int fd = directory->OpenRegularFile(file);
  if (fd < 0) return std::nullopt;
  std::string text;
  char buffer[8192];
  bool ok = true;
  while (true) {
    const auto count = ::read(fd, buffer, sizeof buffer);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || text.size() + static_cast<std::size_t>(std::max<ssize_t>(count, 0)) > kMaxExportedRecipeBytes) ok = false;
    if (count <= 0 || !ok) break;
    text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  if (!ok) return std::nullopt;
  return text;
}

std::vector<std::string> MoveLogsFolderRecipes(const std::filesystem::path& logs, const std::filesystem::path& folder) {
  std::vector<std::string> lines;
  auto names = RegularFiles(logs, [](const std::string& file) { return IsLogsFolderRecipe(file); });
  if (names.empty()) return lines;
  std::sort(names.begin(), names.end());
  const auto from = ManagedDirectory::Open(logs, DirectoryPolicy::kApprovedTree);
  const auto to = from ? OpenFolder(folder, true) : std::nullopt;
  if (!from || !to) {
    lines.push_back("exported recipes: could not open " + (from ? folder : logs).string() + " errno=" + std::to_string(errno));
    return lines;
  }
  bool moved = false;
  for (const auto& name : names) {
    // A temporary file an export left behind is only removed.
    if (EndsWith(name, kTemporarySuffix)) {
      if (from->RemoveRegularFile(name)) lines.push_back("exported recipes: removed " + name + " from the logs folder");
      continue;
    }
    if (from->RenameChild(name, *to, name, false)) {
      moved = true;
      lines.push_back("exported recipes: moved " + name + " to " + to->path().string());
    } else {
      lines.push_back("exported recipes: left " + name + " in the logs folder errno=" + std::to_string(errno));
    }
  }
  if (moved) {
    from->Sync();
    to->Sync();
  }
  return lines;
}

}  // namespace playden
