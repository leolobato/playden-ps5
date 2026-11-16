#include "platform/managed_directory.h"

#include <cerrno>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <string>
#include <utility>

#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
#define PLAYDEN_MANAGED_NATIVE 1
#else
#define PLAYDEN_MANAGED_NATIVE 0
#endif

namespace playden::platform {
namespace fs = std::filesystem;
namespace {
constexpr int kDirectoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

bool Refuse(int error) { errno = error; return false; }

bool SameIdentity(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
         a.st_uid == b.st_uid && a.st_gid == b.st_gid &&
         (a.st_mode & (S_IFMT | 07777)) == (b.st_mode & (S_IFMT | 07777));
}

bool PrivateFamily(DirectoryPolicy policy) {
  return policy == DirectoryPolicy::kPrivate || policy == DirectoryPolicy::kPrivateTraversable;
}

// Policies checked again at every Validate: private modes, or the open tree's owner.
bool Checked(DirectoryPolicy policy) { return PrivateFamily(policy) || policy == DirectoryPolicy::kOpenTree; }

// An open folder: the app user's, any mode (#868).
bool OpenFolder(const struct stat& info, uid_t uid) { return S_ISDIR(info.st_mode) && info.st_uid == uid; }

// kPrivateTraversable also admits 0711: others may enter, but not list or write.
bool Private(const struct stat& info, uid_t uid, DirectoryPolicy policy = DirectoryPolicy::kPrivate) {
  const auto mode = info.st_mode & 07777;
  return S_ISDIR(info.st_mode) && info.st_uid == uid &&
         (mode == 0700 || (policy == DirectoryPolicy::kPrivateTraversable && mode == 0711));
}

bool Admitted(const struct stat& info, uid_t uid, DirectoryPolicy policy) {
  if (policy == DirectoryPolicy::kOpenTree) return OpenFolder(info, uid);
  return !PrivateFamily(policy) || Private(info, uid, policy);
}

bool Ordinary(const struct stat& info) {
  if (S_ISLNK(info.st_mode)) return Refuse(ELOOP);
  return S_ISREG(info.st_mode) || S_ISDIR(info.st_mode) || Refuse(EINVAL);
}

bool AbsolutePath(const fs::path& path) {
  const auto& text = path.native();
  if (text.empty() || text.front() != '/' || text.find('\0') != std::string::npos ||
      text.find('\\') != std::string::npos) return Refuse(EINVAL);
  if (text.size() >= PATH_MAX) return Refuse(ENAMETOOLONG);
  if (text == "/") return true;
  for (std::size_t start = 1; start <= text.size();) {
    const auto slash = text.find('/', start);
    const auto part = text.substr(start, slash == std::string::npos ? slash : slash - start);
    if (part.empty() || part == "." || part == "..") return Refuse(EINVAL);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

void ClosePreservingError(int fd) {
  const int saved = errno;
  if (fd >= 0) ::close(fd);
  errno = saved;
}
}

DirectoryBackend ManagedDirectory::DefaultBackend() {
#if PLAYDEN_MANAGED_NATIVE
  return DirectoryBackend::kAbsolutePath;
#else
  return DirectoryBackend::kDescriptorRelative;
#endif
}

ManagedDirectory::ManagedDirectory(ManagedDirectory&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), root_(std::move(other.root_)),
      path_(std::move(other.path_)), policy_(other.policy_), backend_(other.backend_),
      uid_(other.uid_), observations_(std::move(other.observations_)) {}

ManagedDirectory& ManagedDirectory::operator=(ManagedDirectory&& other) noexcept {
  if (this != &other) {
    ClosePreservingError(fd_);
    fd_ = std::exchange(other.fd_, -1);
    root_ = std::move(other.root_);
    path_ = std::move(other.path_);
    policy_ = other.policy_;
    backend_ = other.backend_;
    uid_ = other.uid_;
    observations_ = std::move(other.observations_);
  }
  return *this;
}

ManagedDirectory::~ManagedDirectory() { ClosePreservingError(fd_); }

std::optional<ManagedDirectory> ManagedDirectory::Open(
    const fs::path& path, DirectoryPolicy policy, DirectoryBackend backend) {
  if (!AbsolutePath(path)) return std::nullopt;
#if PLAYDEN_MANAGED_NATIVE
  if (backend == DirectoryBackend::kDescriptorRelative) {
    errno = ENOTSUP;
    return std::nullopt;
  }
#endif
  ManagedDirectory result;
  result.root_ = result.path_ = path;
  result.uid_ = ::geteuid();
  result.policy_ = policy;
  result.backend_ = backend;
  fs::path current = "/";
  const auto observe = [&](const fs::path& absolute, const fs::path& name) {
    struct stat before{}, after{};
    if (::lstat(absolute.c_str(), &before) != 0) return false;
    if (!S_ISDIR(before.st_mode)) return Refuse(S_ISLNK(before.st_mode) ? ELOOP : ENOTDIR);
    int next;
#if !PLAYDEN_MANAGED_NATIVE
    if (backend == DirectoryBackend::kDescriptorRelative && result.fd_ >= 0)
      next = ::openat(result.fd_, name.c_str(), kDirectoryFlags);
    else
#else
    (void)name;
#endif
      next = ::open(absolute.c_str(), kDirectoryFlags);
    if (next < 0) return false;
    const auto check = Checked(policy) && absolute == path ? policy : DirectoryPolicy::kApprovedTree;
    bool valid = ::fstat(next, &after) == 0;
    if (valid && !SameIdentity(before, after)) valid = Refuse(ESTALE);
    if (valid && !Admitted(after, result.uid_, check)) valid = Refuse(EACCES);
    if (!valid) { ClosePreservingError(next); return false; }
    ClosePreservingError(result.fd_);
    result.fd_ = next;
    result.observations_.push_back({absolute, after, check});
    return true;
  };
  if (!observe(current, current)) return std::nullopt;
  for (const auto& part : path.relative_path()) {
    current /= part;
    if (!observe(current, part)) return std::nullopt;
  }
  if (!result.Validate()) return std::nullopt;
  return result;
}

bool ManagedDirectory::Validate() const {
  if (fd_ < 0 || observations_.empty()) return Refuse(EBADF);
  if (::geteuid() != uid_) return Refuse(EACCES);
  struct stat descriptor{};
  if (::fstat(fd_, &descriptor) != 0) return false;
  if (!SameIdentity(descriptor, observations_.back().identity)) return Refuse(ESTALE);
  for (const auto& observation : observations_) {
    struct stat current{};
    if (::lstat(observation.path.c_str(), &current) != 0) return false;
    if (!SameIdentity(observation.identity, current)) return Refuse(ESTALE);
    if (!S_ISDIR(current.st_mode)) return Refuse(ENOTDIR);
    if (!Admitted(current, uid_, observation.check)) return Refuse(EACCES);
  }
  return true;
}

bool ManagedDirectory::ValidName(const std::string& name) const {
  if (name.empty() || name == "." || name == ".." ||
      name.find_first_of("/\\") != std::string::npos || name.find('\0') != std::string::npos)
    return Refuse(EINVAL);
  if (name.size() > NAME_MAX || path_.native().size() + 1 + name.size() >= PATH_MAX)
    return Refuse(ENAMETOOLONG);
  return true;
}

int ManagedDirectory::OpenFile(const std::string& name, int flags, mode_t mode) const {
#if !PLAYDEN_MANAGED_NATIVE
  if (backend_ == DirectoryBackend::kDescriptorRelative)
    return ::openat(fd_, name.c_str(), flags, mode);
#endif
  return ::open((path_ / name).c_str(), flags, mode);
}

bool ManagedDirectory::Inspect(const std::string& name, struct stat* result) const {
#if !PLAYDEN_MANAGED_NATIVE
  if (backend_ == DirectoryBackend::kDescriptorRelative)
    return ::fstatat(fd_, name.c_str(), result, AT_SYMLINK_NOFOLLOW) == 0;
#endif
  return ::lstat((path_ / name).c_str(), result) == 0;
}

bool ManagedDirectory::InspectChild(const std::string& name, struct stat* result) const {
  if (!result) return Refuse(EINVAL);
  if (!ValidName(name) || !Validate() || !Inspect(name, result)) return false;
  return Ordinary(*result) && Validate();
}

std::optional<ManagedDirectory> ManagedDirectory::OpenChild(
    const std::string& name, DirectoryPolicy policy, bool create, mode_t created_mode) const {
  if (!ValidName(name) || !Validate()) return std::nullopt;
  if (created_mode == kPolicyCreatedMode) created_mode = policy == DirectoryPolicy::kOpenTree ? 0777 : 0700;
  if ((created_mode & ~01777) != 0 ||
      ((created_mode & 01000) && policy != DirectoryPolicy::kApprovedTree) || (policy == DirectoryPolicy::kPrivate && created_mode != 0700) ||
      (policy == DirectoryPolicy::kPrivateTraversable && created_mode != 0700 && created_mode != 0711) ||
      (policy_ == DirectoryPolicy::kPrivate && policy != DirectoryPolicy::kPrivate) ||
      (policy_ == DirectoryPolicy::kPrivateTraversable && !PrivateFamily(policy))) {
    errno = EINVAL;
    return std::nullopt;
  }
  struct stat before{}, after{};
  if (!Inspect(name, &before)) {
    if (!create || errno != ENOENT) return std::nullopt;
    int made;
#if !PLAYDEN_MANAGED_NATIVE
    if (backend_ == DirectoryBackend::kDescriptorRelative)
      made = ::mkdirat(fd_, name.c_str(), 0700);
    else
#endif
      made = ::mkdir((path_ / name).c_str(), 0700);
    // No taking over an unexpected path that appeared during creation.
    if (made != 0 || !Validate() || !Inspect(name, &before)) return std::nullopt;
    // Before the identity is recorded, so the new mode is the observed one.
    if (S_ISDIR(before.st_mode) && before.st_uid == uid_ && (before.st_mode & 07777) != created_mode) {
      // By descriptor, and only the folder just made: a swapped-in link is never followed.
      int made_fd;
#if !PLAYDEN_MANAGED_NATIVE
      if (backend_ == DirectoryBackend::kDescriptorRelative)
        made_fd = ::openat(fd_, name.c_str(), kDirectoryFlags);
      else
#endif
        made_fd = ::open((path_ / name).c_str(), kDirectoryFlags);
      if (made_fd < 0) return std::nullopt;
      struct stat opened{};
      bool changed = ::fstat(made_fd, &opened) == 0;
      if (changed && (!S_ISDIR(opened.st_mode) || opened.st_dev != before.st_dev ||
                      opened.st_ino != before.st_ino || opened.st_uid != uid_))
        changed = Refuse(ESTALE);
      if (changed) changed = ::fchmod(made_fd, created_mode) == 0;
      ClosePreservingError(made_fd);
      if (!changed || !Validate() || !Inspect(name, &before)) return std::nullopt;
    }
  }
  if (!S_ISDIR(before.st_mode)) {
    errno = S_ISLNK(before.st_mode) ? ELOOP : ENOTDIR;
    return std::nullopt;
  }
  if (!Admitted(before, uid_, policy)) {
    errno = EACCES;
    return std::nullopt;
  }
  ManagedDirectory child;
  child.fd_ = OpenFile(name, kDirectoryFlags, 0);
  if (child.fd_ < 0 || ::fstat(child.fd_, &after) != 0) return std::nullopt;
  if (!SameIdentity(before, after)) { errno = ESTALE; return std::nullopt; }
  child.path_ = path_ / name;
  child.root_ = (PrivateFamily(policy) && !PrivateFamily(policy_)) ||
                (policy == DirectoryPolicy::kOpenTree && policy_ != DirectoryPolicy::kOpenTree) ? child.path_ : root_;
  child.policy_ = policy;
  child.backend_ = backend_;
  child.uid_ = uid_;
  child.observations_ = observations_;
  child.observations_.push_back({child.path_, after, Checked(policy) ? policy : DirectoryPolicy::kApprovedTree});
  if (!child.Validate()) return std::nullopt;
  return child;
}

int ManagedDirectory::OpenRegularFile(const std::string& name, bool require_owner,
                                     std::optional<mode_t> mode) const {
  struct stat before{}, after{}, named{};
  if (!InspectChild(name, &before)) return -1;
  if (!S_ISREG(before.st_mode)) { errno = EINVAL; return -1; }
  const int file = OpenFile(name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
  if (file < 0) return -1;
  bool valid = ::fstat(file, &after) == 0;
  if (valid && !SameIdentity(before, after)) valid = Refuse(ESTALE);
  if (valid && ((require_owner && after.st_uid != uid_) ||
                (mode && (after.st_mode & 07777) != *mode))) valid = Refuse(EACCES);
  if (valid) valid = Validate() && Inspect(name, &named);
  if (valid && !SameIdentity(after, named)) valid = Refuse(ESTALE);
  if (!valid) { ClosePreservingError(file); return -1; }
  return file;
}

int ManagedDirectory::CreateExclusiveFile(const std::string& name, mode_t mode) const {
  if ((mode & ~0777) != 0) { errno = EINVAL; return -1; }
  if (!ValidName(name) || !Validate()) return -1;
  const int file = OpenFile(name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
  if (file < 0) return -1;
  struct stat info{}, named{};
  const bool unowned = policy_ == DirectoryPolicy::kUnownedTree;
  bool valid = (::fchmod(file, mode) == 0 || unowned) && ::fstat(file, &info) == 0;
  if (valid && (!S_ISREG(info.st_mode) ||
                (!unowned && (info.st_uid != uid_ || (info.st_mode & 07777) != mode))))
    valid = Refuse(EACCES);
  if (valid) valid = Validate() && Inspect(name, &named);
  if (valid && !SameIdentity(info, named)) valid = Refuse(ESTALE);
  if (!valid) { ClosePreservingError(file); return -1; }
  return file;
}

bool ManagedDirectory::RenameChild(const std::string& from,
    const ManagedDirectory& destination, const std::string& to, bool replace_existing) const {
  if (backend_ != destination.backend_ || uid_ != destination.uid_) return Refuse(EINVAL);
  if (!ValidName(from) || !destination.ValidName(to) || !Validate() || !destination.Validate()) return false;
  if (observations_.back().identity.st_dev != destination.observations_.back().identity.st_dev)
    return Refuse(EXDEV);
  struct stat source{}, target{};
  const bool unowned = policy_ == DirectoryPolicy::kUnownedTree &&
                       destination.policy_ == DirectoryPolicy::kUnownedTree;
  if (!InspectChild(from, &source)) return false;
  if (!unowned && source.st_uid != uid_) return Refuse(EACCES);
  if (S_ISDIR(source.st_mode) && PrivateFamily(destination.policy_) && !Private(source, uid_, destination.policy_))
    return Refuse(EACCES);
  if (destination.InspectChild(to, &target)) {
    if (!replace_existing || S_ISDIR(target.st_mode)) return Refuse(EEXIST);
    if ((!unowned && target.st_uid != uid_) || !S_ISREG(source.st_mode)) return Refuse(EACCES);
  } else if (errno != ENOENT) return false;
  // A missing child does not establish that the destination context survived.
  if (!Validate() || !destination.Validate()) return false;
  int renamed;
#if !PLAYDEN_MANAGED_NATIVE
  if (backend_ == DirectoryBackend::kDescriptorRelative)
    renamed = ::renameat(fd_, from.c_str(), destination.fd_, to.c_str());
  else
#endif
    renamed = ::rename((path_ / from).c_str(), (destination.path_ / to).c_str());
  if (renamed != 0 || !Validate() || !destination.Validate() ||
      !destination.InspectChild(to, &target)) return false;
  if (unowned)
    return (source.st_dev == target.st_dev && (source.st_mode & S_IFMT) == (target.st_mode & S_IFMT) &&
            source.st_size == target.st_size) || Refuse(ESTALE);
  return SameIdentity(source, target) || Refuse(ESTALE);
}

bool ManagedDirectory::Remove(const std::string& name, bool directory) const {
  struct stat info{};
  if (!InspectChild(name, &info)) return false;
  if (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) return Refuse(EINVAL);
  if (policy_ != DirectoryPolicy::kUnownedTree && info.st_uid != uid_) return Refuse(EACCES);
  int removed;
#if !PLAYDEN_MANAGED_NATIVE
  if (backend_ == DirectoryBackend::kDescriptorRelative)
    removed = ::unlinkat(fd_, name.c_str(), directory ? AT_REMOVEDIR : 0);
  else
#endif
    removed = directory ? ::rmdir((path_ / name).c_str()) : ::unlink((path_ / name).c_str());
  return removed == 0 && Validate();
}

namespace {
std::optional<bool> (*g_ownership_probe)(const fs::path&) = nullptr;

std::optional<bool> ProbeOwnership(const fs::path& directory) {
  const auto probe = directory / (".playden-ownership-probe-" + std::to_string(::getpid()));
  ::unlink(probe.c_str());
  const int file = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (file < 0) return std::nullopt;
  struct stat info{};
  const bool chmodded = ::fchmod(file, 0600) == 0;
  const bool inspected = ::fstat(file, &info) == 0;
  ::close(file);
  ::unlink(probe.c_str());
  if (!inspected) return std::nullopt;
  return chmodded && info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0600;
}
}  // namespace

std::optional<bool> RecordsOwnership(const fs::path& directory) {
  return g_ownership_probe ? g_ownership_probe(directory) : ProbeOwnership(directory);
}

void SetOwnershipProbeForTesting(std::optional<bool> (*probe)(const fs::path&)) { g_ownership_probe = probe; }

bool ManagedDirectory::RemoveRegularFile(const std::string& name) const { return Remove(name, false); }
bool ManagedDirectory::RemoveEmptyDirectory(const std::string& name) const { return Remove(name, true); }
bool ManagedDirectory::Sync() const { return Validate() && ::fsync(fd_) == 0 && Validate(); }

}  // namespace playden::platform
