// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/self_heal.h"
#include "platform/directory_walk.h"
#include "steam/backend/json.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace playden::runtime {
namespace fs = std::filesystem;
namespace {
using Log = std::function<void(const std::string&)>;

std::string Octal(mode_t mode) {
  char text[8];
  std::snprintf(text, sizeof(text), "%04o", unsigned(mode & 07777));
  return text;
}

void Note(const Log& log, const std::string& line) { if (log) log(line); }

// Takes away the bits of info's mode that `allowed` lacks, through fd. Only the owner's entries,
// and never a file with another link (someone else's file linked in).
void Tighten(int fd, const struct stat& info, mode_t allowed, const fs::path& path, const Log& log,
             SelfHealResult* result) {
  const mode_t current = info.st_mode & 07777;
  const mode_t wanted = current & allowed;
  if (wanted == current) return;
  if (info.st_uid != ::geteuid()) {
    ++result->foreign;
    Note(log, "self-heal left path=" + path.string() + " mode=" + Octal(current) + " uid=" + std::to_string(info.st_uid));
    return;
  }
  if (S_ISREG(info.st_mode) && info.st_nlink != 1) {
    ++result->foreign;
    Note(log, "self-heal left path=" + path.string() + " links=" + std::to_string(info.st_nlink));
    return;
  }
  if (::fchmod(fd, wanted) != 0) {
    ++result->failed;
    Note(log, "self-heal failed path=" + path.string() + " chmod=" + std::strerror(errno));
    return;
  }
  ++result->tightened;
  Note(log, "self-heal path=" + path.string() + " mode=" + Octal(current) + " -> " + Octal(wanted));
}

// The folder at path (missing: nothing to do) back to 0700 when `folder`, and the regular files
// directly in it back to 0600 when `files`. Subfolders are not entered. `traversable`: a folder
// at 0711 (a kit that lets others pass through bottles) is left as it is. `chmodded_files`: only
// files with an execute bit, which Playden never writes there but a recursive chmod leaves (a
// folder of private and ordinary files, like user/).
void TightenFolder(const fs::path& path, bool folder, bool files, const Log& log, SelfHealResult* result,
                   bool traversable = false, bool chmodded_files = false) {
  const int fd = platform::OpenDirectoryWalk(path);
  if (fd < 0) {
    if (errno != ENOENT) {
      ++result->failed;
      Note(log, "self-heal failed path=" + path.string() + " open=" + std::strerror(errno));
    }
    return;
  }
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode)) {
    ::close(fd);
    return;
  }
  const int listing_fd = files ? ::dup(fd) : -1;
  DIR* listing = listing_fd >= 0 ? ::fdopendir(listing_fd) : nullptr;
  if (files && !listing) {
    if (listing_fd >= 0) ::close(listing_fd);
    ++result->failed;
    Note(log, "self-heal failed path=" + path.string() + " list=" + std::strerror(errno));
  }
  while (listing) {
    errno = 0;
    const auto* entry = ::readdir(listing);
    if (!entry) break;
    const std::string name = entry->d_name;
    if (name == "." || name == ".." || entry->d_type == DT_DIR || entry->d_type == DT_LNK) continue;
    // O_NOFOLLOW: a link swapped in is refused, never followed.
    const int child = platform::OpenWalkChild(fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC,
                                              entry->d_fileno);
    if (child < 0) {
      if (errno != ELOOP && errno != ENOENT) {
        ++result->failed;
        Note(log, "self-heal failed path=" + (path / name).string() + " open=" + std::strerror(errno));
      }
      continue;
    }
    struct stat opened{};
    if (::fstat(child, &opened) == 0 && S_ISREG(opened.st_mode) && (!chmodded_files || (opened.st_mode & 0111)))
      Tighten(child, opened, 0600, path / name, log, result);
    ::close(child);
  }
  if (listing) ::closedir(listing);
  // The folder last: one that still looks open is looked at again, so an interrupted pass is finished later.
  if (folder && !(traversable && (info.st_mode & 07777) == 0711)) Tighten(fd, info, 0700, path, log, result);
  ::close(fd);
}

bool SafeRelative(const fs::path& path) {
  if (path.empty() || path.is_absolute()) return false;
  for (const auto& part : path)
    if (part.empty() || part == "." || part == "..") return false;
  return true;
}

std::optional<std::string> ReadManifest(const fs::path& path, std::size_t max_bytes) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string bytes;
  char buffer[65536];
  for (;;) {
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || bytes.size() + static_cast<std::size_t>(count) > max_bytes) { ::close(fd); return std::nullopt; }
    if (count == 0) break;
    bytes.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  return bytes;
}
}  // namespace

SelfHealResult TightenPrivateModes(const fs::path& data_root, const Log& log) {
  SelfHealResult result;
  const auto state = data_root / "runtime-state";
  TightenFolder(state, true, true, log, &result);
  TightenFolder(state / "requests", true, true, log, &result);
  // Retired title trees inside trash/ are 0755 on purpose: only the folder itself.
  TightenFolder(state / "trash", true, false, log, &result);
  // user/ is shared (01777); its private files hold the sign-in, beside ordinary 0666 caches.
  TightenFolder(data_root / "user", false, true, log, &result, false, true);
  TightenFolder(data_root / "user/game-settings-private", true, true, log, &result);
  TightenFolder(data_root / "art", true, false, log, &result);
  return result;
}

bool BottleTooOpen(const fs::path& data_root) {
  for (const auto* relative : {"", "library", ".playden-control"}) {
    struct stat info{};
    const auto path = *relative ? data_root / relative : data_root;
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) continue;
    const mode_t mode = info.st_mode & 07777;
    if (mode != 0700 && mode != 0711) return true;
  }
  return false;
}

SelfHealResult TightenBottleModes(const fs::path& data_root, const Log& log) {
  SelfHealResult result;
  // The folders the title's start check wants owner-only; drive_c and below are the game's. Deepest
  // first and the bottle itself last, and only when everything else went: until then the title's
  // start check refuses the bottle (no game starts from a half-repaired one), and BottleTooOpen
  // brings the next Prepare or Play back here.
  for (const auto* relative : {"title-metadata/sce_sys", "title-metadata", "logs/sessions", "logs", ".playden-control",
                               "library/profiles", "library/input"})
    TightenFolder(data_root / relative, true, true, log, &result);
  TightenFolder(data_root / "library/prefixes/game", true, true, log, &result, true);
  TightenFolder(data_root / "library/prefixes", true, false, log, &result, true);
  TightenFolder(data_root / "library", true, false, log, &result, true);
  if (result.failed == 0) TightenFolder(data_root, true, false, log, &result, true);
  return result;
}

SelfHealResult TightenKitModes(const fs::path& kit_root, const WritableFoldersAsRoot& as_root, const Log& log) {
  SelfHealResult result;
  constexpr std::size_t kMaxManifestBytes = 8 * 1024 * 1024, kMaxFiles = 32768;
  const auto bytes = ReadManifest(kit_root / "manifest.json", kMaxManifestBytes);
  if (!bytes) return result;  // no kit yet, or one admission will refuse anyway
  const auto manifest = steam::json::Parse(*bytes, {kMaxManifestBytes, 16, kMaxFiles * 8, 1024 * 1024});
  const auto* files = manifest && manifest->is_object() ? manifest->Find("files") : nullptr;
  if (!files || !files->is_array()) return result;
  struct Change { fs::path path; mode_t current, expected; };
  std::vector<Change> changes;
  for (const auto& entry : files->array()) {
    const auto* path_value = entry.is_object() ? entry.Find("path") : nullptr;
    const auto* mode_value = entry.is_object() ? entry.Find("mode") : nullptr;
    if (!path_value || !path_value->is_string() || !mode_value || !mode_value->is_string()) continue;
    const fs::path relative = path_value->text();
    const auto& mode_text = mode_value->text();
    if (!SafeRelative(relative) || mode_text.size() != 4 ||
        mode_text.find_first_not_of("01234567") != std::string::npos)
      continue;
    const mode_t expected = static_cast<mode_t>(std::stoul(mode_text, nullptr, 8));
    // A cheap look first: a kit with the right modes is not walked.
    struct stat seen{};
    const auto path = kit_root / relative;
    if (::lstat(path.c_str(), &seen) != 0 || !S_ISREG(seen.st_mode)) continue;
    const mode_t current = seen.st_mode & 07777;
    // Only extra bits are taken away; a file missing bits is the kit's problem, not a chmod's.
    if (current == expected || (expected & ~current) != 0) continue;
    changes.push_back({path, current, expected});
  }
  if (changes.empty()) return result;
  std::vector<bool> done(changes.size(), false);
  // One root section for the whole kit. The folder is opened again for each file: the by-path
  // walk reads the parent's listing, whose offset a second lookup through it would continue from.
  // The kit is root's when made by the upload tool or a download, the app user's when copied by
  // hand from an earlier layout; anything else is not the kit's.
  const uid_t app_user = ::geteuid();
  const bool ran = as_root && as_root([&] {
    for (std::size_t index = 0; index < changes.size(); ++index) {
      const auto& change = changes[index];
      const int parent = platform::OpenDirectoryWalk(change.path.parent_path());
      if (parent < 0) continue;
      const int fd = platform::OpenWalkChild(parent, change.path.filename().c_str(),
                                             O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      ::close(parent);
      if (fd < 0) continue;
      struct stat info{};
      // Still the file looked at: a regular file with one link and the mode seen.
      done[index] = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
                    (info.st_uid == 0 || info.st_uid == app_user) &&
                    (info.st_mode & 07777) == change.current && ::fchmod(fd, change.expected) == 0;
      ::close(fd);
    }
    return true;
  });
  // A whole kit can need it (thousands of files): one line, and only the first few failures by name.
  constexpr std::size_t kNamedFailures = 10;
  for (std::size_t index = 0; index < changes.size(); ++index) {
    const auto& change = changes[index];
    if (ran && done[index]) {
      ++result.tightened;
      continue;
    }
    if (++result.failed <= kNamedFailures)
      Note(log, "self-heal failed path=" + change.path.string() + " mode=" + Octal(change.current) +
                    " kit=" + Octal(change.expected) + (ran ? "" : " root=refused"));
  }
  Note(log, "self-heal kit=" + kit_root.string() + " files=" + std::to_string(result.tightened) +
                " failed=" + std::to_string(result.failed));
  return result;
}

}  // namespace playden::runtime
