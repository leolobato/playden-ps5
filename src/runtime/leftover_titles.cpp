// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/leftover_titles.h"

#include "platform/directory_walk.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <unordered_set>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {
namespace fs = std::filesystem;
namespace json = steam::json;

bool SameDirectory(const struct stat& first, const struct stat& second) {
  return S_ISDIR(second.st_mode) && first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

// Takes ownership of fd. Refuse incomplete trees rather than show a misleading
// size or follow a link. The bound also keeps malformed trees off small stacks.
bool DirectoryBytes(int fd, const fs::path& path, std::uint64_t& bytes,
                    std::size_t& entries, unsigned depth = 0) {
  DIR* directory = ::fdopendir(fd);
  if (!directory) { ::close(fd); return false; }
  bool ok = depth <= 32;
  while (ok) {
    errno = 0;
    const auto entry = ::readdir(directory);
    if (!entry) { ok = errno == 0; break; }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (++entries > 100000) { ok = false; break; }
    struct stat listed{};
    if (::lstat((path / name).c_str(), &listed) != 0) { ok = false; break; }
    if (S_ISLNK(listed.st_mode)) continue;
    if (!S_ISDIR(listed.st_mode) && !S_ISREG(listed.st_mode)) continue;
    const int child = platform::OpenWalkChild(fd, name.c_str(),
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
        (S_ISDIR(listed.st_mode) ? O_DIRECTORY : 0), entry->d_ino);
    struct stat opened{};
    if (child < 0 || ::fstat(child, &opened) != 0 ||
        opened.st_dev != listed.st_dev || opened.st_ino != listed.st_ino) {
      if (child >= 0) ::close(child);
      ok = false; break;
    }
    if (S_ISDIR(opened.st_mode)) {
      ok = DirectoryBytes(child, path / name, bytes, entries, depth + 1);
    } else {
      ::close(child);
      if (!S_ISREG(opened.st_mode) || opened.st_size < 0 ||
          static_cast<std::uint64_t>(opened.st_size) > std::numeric_limits<std::uint64_t>::max() - bytes) {
        ok = false; break;
      }
      bytes += static_cast<std::uint64_t>(opened.st_size);
    }
  }
  ::closedir(directory);
  return ok;
}

std::string Name(const json::Value* value) {
  if (!value || !value->is_object()) return {};
  const auto name = value->Find("titleName");
  if (!name || !name->is_string() || name->text().size() > 512) return {};
  for (const unsigned char c : name->text()) if (c < 0x20 || c == 0x7f) return {};
  return name->text();
}

std::string TitleName(int title_fd) {
  const int system = platform::OpenWalkChild(title_fd, "sce_sys", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (system < 0) return {};
  const int fd = platform::OpenWalkChild(system, "param.json", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  ::close(system);
  if (fd < 0) return {};
  struct stat info{};
  std::string bytes;
  bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 && info.st_size <= 64 * 1024;
  if (ok) {
    bytes.resize(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { ok = false; break; }
      offset += static_cast<std::size_t>(count);
    }
  }
  ::close(fd);
  const auto parsed = ok ? json::Parse(bytes, {64 * 1024, 8, 4096, 4096}) : std::nullopt;
  if (!parsed) return {};
  auto name = Name(&*parsed);
  if (!name.empty()) return name;
  const auto localized = parsed->Find("localizedParameters");
  if (!localized || !localized->is_object()) return {};
  const auto language = localized->Find("defaultLanguage");
  if (language && language->is_string()) name = Name(localized->Find(language->text()));
  if (name.empty()) name = Name(localized->Find("en-US"));
  if (name.empty()) for (const auto& entry : localized->object()) {
    name = Name(&entry.second);
    if (!name.empty()) break;
  }
  return name;
}
}  // namespace

std::optional<std::vector<LeftoverTitleCandidate>> ListLeftoverTitleCandidates(
    const fs::path& homebrew_root, uid_t effective_uid,
    const std::unordered_set<std::string>& excluded_title_ids) {
  const int root = platform::OpenDirectoryWalk(homebrew_root);
  if (root < 0) return std::nullopt;
  DIR* directory = ::fdopendir(root);
  if (!directory) { ::close(root); return std::nullopt; }
  std::vector<LeftoverTitleCandidate> candidates;
  bool ok = true;
  while (true) {
    errno = 0;
    const auto entry = ::readdir(directory);
    if (!entry) { ok = errno == 0; break; }
    const std::string id = entry->d_name;
    if (!IsManagedTitle(id) || excluded_title_ids.count(id)) continue;
    struct stat listed{};
    if (::lstat((homebrew_root / id).c_str(), &listed) != 0 ||
        !S_ISDIR(listed.st_mode) || listed.st_uid != effective_uid) continue;
    const int fd = platform::OpenWalkChild(root, id.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,
                                         entry->d_ino);
    struct stat opened{};
    if (fd < 0 || ::fstat(fd, &opened) != 0 || !SameDirectory(listed, opened) || opened.st_uid != effective_uid) {
      if (fd >= 0) ::close(fd);
      continue;
    }
    // The by-path walk may enumerate a held directory to verify a child inode.
    // Keep metadata's enumeration separate from the size walk's file position.
    const int metadata_fd = platform::OpenWalkChild(root, id.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, entry->d_ino);
    struct stat metadata_info{};
    std::string name;
    if (metadata_fd >= 0) {
      if (::fstat(metadata_fd, &metadata_info) == 0 && SameDirectory(opened, metadata_info))
        name = TitleName(metadata_fd);
      ::close(metadata_fd);
    }
    LeftoverTitleCandidate candidate{id, true, false, opened.st_uid, false, std::move(name), 0,
        static_cast<std::uint64_t>(opened.st_dev), static_cast<std::uint64_t>(opened.st_ino)};
    std::size_t entries = 0;
    candidate.readable = DirectoryBytes(fd, homebrew_root / id, candidate.bytes, entries);
    candidates.push_back(std::move(candidate));
  }
  ::closedir(directory);
  return ok ? std::optional<std::vector<LeftoverTitleCandidate>>(std::move(candidates)) : std::nullopt;
}

std::vector<LeftoverTitle> FindLeftoverTitles(
    const fs::path& homebrew_root, bool repository_loaded, const std::vector<RuntimeInstall>& records,
    const LeftoverTitleActivity& activity, uid_t effective_uid, const LeftoverTitleLister& lister) {
  if (!repository_loaded || !lister || !activity) return {};
  std::unordered_set<std::string> reserved;
  for (const auto& record : records) reserved.insert(record.title_id);
  const auto candidates = lister(homebrew_root, effective_uid, reserved);
  if (!candidates) return {};
  std::vector<LeftoverTitle> leftovers;
  for (const auto& candidate : *candidates) {
    if (!IsManagedTitle(candidate.title_id) || !candidate.is_directory || candidate.is_symlink ||
        candidate.owner != effective_uid || !candidate.readable || reserved.count(candidate.title_id) ||
        activity(candidate.title_id) != NativeActivity::kStopped) continue;
    leftovers.push_back({candidate.title_id, homebrew_root / candidate.title_id, candidate.name, candidate.bytes,
                         candidate.device, candidate.inode});
    reserved.insert(candidate.title_id);
  }
  std::sort(leftovers.begin(), leftovers.end(), [](const auto& first, const auto& second) {
    return first.title_id < second.title_id;
  });
  return leftovers;
}
}  // namespace playden::runtime
