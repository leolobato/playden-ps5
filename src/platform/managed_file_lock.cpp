#include "platform/managed_file_lock.h"

#include <cerrno>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
extern "C" int playden_ps5_file_lock(int descriptor, int operation);
#endif

namespace playden::platform {
namespace {
bool PrivateLock(const struct stat& value) {
  return S_ISREG(value.st_mode) && value.st_uid == ::geteuid() &&
         (value.st_mode & 07777) == 0600 && value.st_nlink == 1;
}
bool SameLock(const struct stat& first, const struct stat& second) {
  return PrivateLock(first) && PrivateLock(second) &&
         first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}
int AcquireNative(int descriptor, LockMode mode) {
  const int operation = (mode == LockMode::kReader ? LOCK_SH : LOCK_EX) | LOCK_NB;
#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
  return playden_ps5_file_lock(descriptor, operation);
#else
  return ::flock(descriptor, operation);
#endif
}
}

std::optional<ManagedFileLock> ManagedFileLock::Acquire(
    const ManagedDirectory& directory, const std::string& filename, LockMode mode) {
  // The lock file itself is 0600; its folder may be open (a game's bottle, #868).
  if ((directory.policy() != DirectoryPolicy::kPrivate && directory.policy() != DirectoryPolicy::kOpenTree) ||
      (mode != LockMode::kReader && mode != LockMode::kPublisher)) {
    errno = EINVAL;
    return std::nullopt;
  }
  int descriptor = directory.OpenRegularFile(filename, true, 0600);
  if (descriptor < 0) return std::nullopt;
  auto refuse = [descriptor]() -> std::optional<ManagedFileLock> {
    const int failure = errno;
    ::close(descriptor);  // Also releases any acquired lease.
    errno = failure;
    return std::nullopt;
  };
  struct stat before{}, after{}, named{};
  if (::fstat(descriptor, &before) != 0) return refuse();
  if (!PrivateLock(before)) { errno = EPERM; return refuse(); }
  if (AcquireNative(descriptor, mode) != 0) return refuse();
  if (!directory.Validate() || !directory.InspectChild(filename, &named) ||
      ::fstat(descriptor, &after) != 0) return refuse();
  if (!SameLock(before, after) || !SameLock(after, named)) {
    errno = ESTALE;
    return refuse();
  }
  return ManagedFileLock(descriptor);
}

ManagedFileLock::ManagedFileLock(ManagedFileLock&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}
ManagedFileLock& ManagedFileLock::operator=(ManagedFileLock&& other) noexcept {
  if (this != &other) {
    if (descriptor_ >= 0) ::close(descriptor_);
    descriptor_ = std::exchange(other.descriptor_, -1);
  }
  return *this;
}
bool ManagedFileLock::Validate(const ManagedDirectory& directory, const std::string& filename) const {
  return Holds(descriptor_, directory.path() / filename);
}
bool ManagedFileLock::Holds(int descriptor, const std::filesystem::path& lock_path) {
  struct stat opened{}, named{};
  return descriptor >= 0 && ::fstat(descriptor, &opened) == 0 && ::lstat(lock_path.c_str(), &named) == 0 &&
      S_ISREG(opened.st_mode) && S_ISREG(named.st_mode) &&
      opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
}
ManagedFileLock::~ManagedFileLock() {
  if (descriptor_ >= 0) ::close(descriptor_);
}

}  // namespace playden::platform
