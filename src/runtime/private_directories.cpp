#include "runtime/private_directories.h"
#include "platform/managed_directory.h"
#include "platform/directory_walk.h"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace playden::runtime {
namespace fs = std::filesystem;
namespace {
using platform::DirectoryPolicy;
using platform::ManagedDirectory;
bool SafeRelative(const fs::path& path) {
  if (path.empty() || path.is_absolute()) return false;
  for (const auto& part : path)
    if (part.empty() || part == "." || part == "..") return false;
  return true;
}
bool Traversable(const PrivateTraversal& traversal, const fs::path& path) {
  for (const auto& folder : traversal.folders)
    if (folder == path) return true;
  return false;
}
DirectoryPolicy PolicyFor(const PrivateTraversal& traversal, const fs::path& path) {
  return Traversable(traversal, path) ? DirectoryPolicy::kPrivateTraversable : DirectoryPolicy::kPrivate;
}
}
bool IsPrivateDirectory(const fs::path& directory, bool traversable) {
  return ManagedDirectory::Open(directory, traversable ? DirectoryPolicy::kPrivateTraversable
                                                       : DirectoryPolicy::kPrivate).has_value();
}
bool IsPrivateDirectoryTree(const fs::path& private_root, const fs::path& directory,
                            const PrivateTraversal& traversal) {
  const auto relative = directory.lexically_relative(private_root);
  if (directory != private_root && !SafeRelative(relative)) return false;
  auto current = ManagedDirectory::Open(private_root, PolicyFor(traversal, private_root));
  if (!current) return false;
  if (directory != private_root) {
    fs::path path = private_root;
    for (const auto& part : relative) {
      path /= part;
      auto next = current->OpenChild(part.string(), PolicyFor(traversal, path));
      if (!next) return false;
      current = std::move(next);
    }
  }
  return current->Validate();
}
bool EnsurePrivateDirectories(const fs::path& boundary, const fs::path& private_root,
                              const fs::path& directory, std::string* error,
                              const PrivateTraversal& traversal) {
  if (error) error->clear();
  const auto fail = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  const auto relative = directory.lexically_relative(boundary);
  const auto private_relative = private_root.lexically_relative(boundary);
  const auto within = directory.lexically_relative(private_root);
  if (traversal.created_mode != 0700 && traversal.created_mode != 0711)
    return fail("traversal folders must be created 0700 or 0711");
  if (!SafeRelative(relative) || !SafeRelative(private_relative) ||
      (directory != private_root && !SafeRelative(within)))
    return fail("private directory is outside its trusted boundary");
  auto current = ManagedDirectory::Open(boundary, DirectoryPolicy::kApprovedTree);
  if (!current) return fail("cannot open directory boundary: " + std::string(strerror(errno)));
  fs::path path = boundary;
  bool in_private = false;
  for (const auto& part : relative) {
    path /= part;
    if (path == private_root) in_private = true;
    // Shared parents it creates above the private root are 01777, so FTP and other
    // apps can write beside them; existing ones keep their mode.
    const auto policy = PolicyFor(traversal, path);
    auto next = !in_private ? current->OpenChild(part.string(), DirectoryPolicy::kApprovedTree, true, 01777)
                : policy == DirectoryPolicy::kPrivateTraversable
                    ? current->OpenChild(part.string(), policy, true, traversal.created_mode)
                    : current->OpenChild(part.string(), policy, true);
    if (!next)
      return fail("cannot open/create private directory (wrong owner or permissions): " + path.string() + ": " + strerror(errno));
    // Flush the directory and its parent entry before journaled preparation.
    // Failure may follow creation; callers retain their retry/recovery record.
    if (!next->Sync() || !current->Sync())
      return fail("cannot sync private directory: " + path.string() + ": " + strerror(errno));
    current = std::move(next);
  }
  return current->Validate();
}
namespace {
#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
constexpr bool kDescriptorRelative = false;  // as ManagedDirectory: no *at calls on the console
#else
constexpr bool kDescriptorRelative = true;
#endif
bool SameEntry(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
}
// A child opened without following a link: relative to its folder where *at calls exist, else
// by path, checked to be the entry lstat saw, in a folder its path still names. -1 with
// *stop: the folder's path names another folder now, so nothing more is opened through it.
int OpenEntry(int folder, const struct stat& folder_info, const fs::path& folder_path, const char* name, bool by_path,
              int extra_flags, bool* stop) {
  const int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | extra_flags;
  *stop = false;
  if (!by_path) return ::openat(folder, name, flags);
  struct stat parent{}, before{}, opened{};
  if (::lstat(folder_path.c_str(), &parent) != 0 || !SameEntry(parent, folder_info)) {
    *stop = true;
    errno = ESTALE;
    return -1;
  }
  const auto path = folder_path / name;
  if (::lstat(path.c_str(), &before) != 0) return -1;
  if (S_ISLNK(before.st_mode)) { errno = ELOOP; return -1; }
  const int fd = ::open(path.c_str(), flags);
  if (fd >= 0 && (::fstat(fd, &opened) != 0 || !SameEntry(opened, before))) {
    ::close(fd);
    errno = ESTALE;
    return -1;
  }
  return fd;
}
constexpr int kSharedTreeDepth = 64;
constexpr std::size_t kSharedTreeEntries = 200000;
struct Walk {
  fs::path root;
  const SharedTreeOptions& options;
  std::size_t entries = 0;
  SharedTreeResult* result;
};
// Takes fd. Folders of other owners are not entered. False: a folder could not be listed, or
// the walk found a folder's path changed under it.
bool ShareFolder(int fd, const fs::path& path, int depth, Walk& walk) {
  auto* result = walk.result;
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode)) { ::close(fd); return false; }
  if (info.st_uid != ::geteuid()) { ::close(fd); ++result->skipped; return true; }
  const bool private_folder = IsPrivateProfilePath(path.lexically_relative(walk.root), walk.options.private_paths);
  const mode_t folder_mode = private_folder ? 0700 : 0777;
  if ((info.st_mode & 07777) != folder_mode) {
    if (::fchmod(fd, folder_mode) != 0) { ::close(fd); ++result->skipped; return true; }
    ++result->changed;
  }
  const int listing_fd = ::dup(fd);
  DIR* listing = listing_fd >= 0 ? ::fdopendir(listing_fd) : nullptr;
  if (!listing) {
    if (listing_fd >= 0) ::close(listing_fd);
    ::close(fd);
    return false;
  }
  bool ok = true;
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(listing);
    if (!entry) { ok = errno == 0; break; }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (++walk.entries > kSharedTreeEntries) { result->truncated = true; break; }
    if (entry->d_type == DT_LNK) continue;
    if (walk.options.before_open) walk.options.before_open(path / name);
    // O_NOFOLLOW: a link swapped in is refused, never followed.
    const int child = platform::OpenWalkChild(fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC,
                                              entry->d_fileno);
    if (child < 0) {
      if (errno == ENOSYS || errno == EINVAL) { ok = false; break; }
      if (errno != ELOOP && errno != EMLINK) ++result->skipped;
      continue;
    }
    struct stat opened{};
    if (::fstat(child, &opened) != 0) { ::close(child); ++result->skipped; continue; }
    if (S_ISDIR(opened.st_mode)) {
      if (depth + 1 > kSharedTreeDepth) { ::close(child); result->truncated = true; break; }
      if (!ShareFolder(child, path / name, depth + 1, walk)) { ok = false; break; }
      if (result->truncated) break;
      continue;
    }
    // A file with another link is someone else's too (a private file linked in): left alone.
    if (S_ISREG(opened.st_mode) && opened.st_nlink != 1) {
      ++result->linked;
    } else if (S_ISREG(opened.st_mode) && opened.st_uid == ::geteuid()) {
      const mode_t wanted = IsPrivateProfilePath((path / name).lexically_relative(walk.root), walk.options.private_paths)
                                ? 0600 : 0666 | (opened.st_mode & 0111);
      if ((opened.st_mode & 07777) != wanted) {
        if (::fchmod(child, wanted) == 0) ++result->changed;
        else ++result->skipped;
      }
    }
    ::close(child);
  }
  ::closedir(listing);
  ::close(fd);
  return ok;
}
}  // namespace
bool DescriptorRelativeOpens() { return kDescriptorRelative; }
int OpenFolderNoFollow(const fs::path& path) {
  struct stat before{}, opened{};
  if (::lstat(path.c_str(), &before) != 0) return -1;
  if (S_ISLNK(before.st_mode)) { errno = ELOOP; return -1; }
  if (!S_ISDIR(before.st_mode)) { errno = ENOTDIR; return -1; }
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd >= 0 && (::fstat(fd, &opened) != 0 || !SameEntry(opened, before))) {
    ::close(fd);
    errno = ESTALE;
    return -1;
  }
  return fd;
}
int OpenChildFolderNoFollow(int parent_fd, const fs::path& parent_path, const std::string& name, bool by_path) {
  struct stat parent{};
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) { errno = EINVAL; return -1; }
  if (::fstat(parent_fd, &parent) != 0) return -1;
  bool stop = false;
  const int fd = OpenEntry(parent_fd, parent, parent_path, name.c_str(), by_path, O_DIRECTORY, &stop);
  struct stat opened{};
  // O_DIRECTORY|O_NOFOLLOW on a link may say ENOTDIR (macOS): it is still a link.
  if (fd < 0 && errno == ENOTDIR && !by_path && ::fstatat(parent_fd, name.c_str(), &opened, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISLNK(opened.st_mode))
    errno = ELOOP;
  if (fd >= 0 && (::fstat(fd, &opened) != 0 || !S_ISDIR(opened.st_mode))) {
    ::close(fd);
    errno = ENOTDIR;
    return -1;
  }
  return fd;
}
bool SetTraversableMode(const fs::path& folder, mode_t mode) {
  if (mode != 0700 && mode != 0711) { errno = EINVAL; return false; }
  const int fd = platform::OpenDirectoryWalk(folder);
  if (fd < 0) return errno == ENOENT;
  struct stat info{};
  bool ok = ::fstat(fd, &info) == 0;
  const auto current = info.st_mode & 07777;
  if (ok && info.st_uid == ::geteuid() && (current == 0700 || current == 0711) && current != mode)
    ok = ::fchmod(fd, mode) == 0 && ::fsync(fd) == 0;
  const int error = errno;
  ::close(fd);
  errno = error;
  return ok;
}
const std::vector<fs::path>& PrivateProfilePaths() {
  // Valve's Steam client (Steam features = Online) saves its sign-in, a token, in AppData/Local/Steam
  // (local.vdf). Add each other credential file or dedicated credential subtree here when its writer is introduced.
  static const std::vector<fs::path> paths{"AppData/Local/Steam"};
  return paths;
}
bool IsPrivateProfilePath(const fs::path& relative, const std::vector<fs::path>& private_paths) {
  for (const auto& path : private_paths) {
    const auto within = relative.lexically_relative(path);
    if (within == "." || SafeRelative(within)) return true;
  }
  return false;
}
SharedTreeResult OpenTreeToEveryone(const fs::path& root, const SharedTreeOptions& options) {
  SharedTreeResult result;
  for (const auto& path : options.private_paths)
    if (!SafeRelative(path)) { errno = EINVAL; return result; }
  const int fd = platform::OpenDirectoryWalk(root);
  if (fd < 0) { result.ok = errno == ENOENT; return result; }
  Walk walk{root, options, 0, &result};
  result.ok = ShareFolder(fd, root, 0, walk);
  return result;
}
}
