#include "platform/storage.h"
#include "platform/ps5/title_allow_list.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden {
namespace {

std::string ErrnoMessage(const std::string& action,
                         const std::string& relative_path,
                         int error_number) {
  return action + " '" + relative_path + "': " + std::strerror(error_number);
}

std::string WriteErrorMessage(const std::string& action,
                              const std::string& relative_path,
                              int error_number,
                              bool numeric_only) {
  if (!numeric_only) return ErrnoMessage(action, relative_path, error_number);
  return action + " '" + relative_path + "' (errno " + std::to_string(error_number) + ")";
}

bool IsSafeRelativePath(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
      path.find('\0') != std::string::npos ||
      (path.size() >= 2 && path[1] == ':')) {
    return false;
  }

  std::size_t segment_start = 0;
  while (segment_start <= path.size()) {
    const std::size_t separator = path.find('/', segment_start);
    const std::size_t segment_end =
        separator == std::string::npos ? path.size() : separator;
    const std::string segment =
        path.substr(segment_start, segment_end - segment_start);
    if (segment.empty() || segment == "." || segment == "..") {
      return false;
    }
    if (separator == std::string::npos) {
      break;
    }
    segment_start = separator + 1;
  }
  return true;
}

}  // namespace

Storage::Storage(std::string root) : root_(std::move(root)) {
  while (root_.size() > 1 && root_.back() == '/') {
    root_.pop_back();
  }
}

Result<void> Storage::EnsureRootDirectory() const {
  if(root_.empty() || root_.find('\0')!=std::string::npos)
    return Result<void>::Failure(StorageError::kInvalidPath,"Storage root is invalid.");
  // Open, so the console owner can delete it over FTP (#868); an existing root keeps its mode.
  if(mkdir(root_.c_str(),0777)==0) (void)chmod(root_.c_str(),0777);
  else if(errno!=EEXIST)
    return Result<void>::Failure(StorageError::kOpenFailed,"Storage root could not be created.");
  struct stat info{};
  if(stat(root_.c_str(),&info)!=0 || !S_ISDIR(info.st_mode))
    return Result<void>::Failure(StorageError::kOpenFailed,"Storage root is unavailable.");
  return Result<void>::Success();
}

Result<std::string> Storage::Resolve(const std::string& relative_path) const {
  if (root_.empty()) {
    return Result<std::string>::Failure(StorageError::kInvalidPath,
                                        "storage root is empty");
  }
  if (!IsSafeRelativePath(relative_path)) {
    return Result<std::string>::Failure(
        StorageError::kInvalidPath,
        "storage path must be a normalized relative path: '" + relative_path + "'");
  }
  return Result<std::string>::Success(root_ + "/" + relative_path);
}

Result<Bytes> Storage::Read(const std::string& relative_path, std::size_t max_bytes) const {
  if (max_bytes == 0 || max_bytes > 32 * 1024 * 1024)
    return Result<Bytes>::Failure(StorageError::kReadFailed, "invalid bounded read size");
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) {
    return Result<Bytes>::Failure(resolved.error(), resolved.message());
  }

  errno = 0;
  std::FILE* file = std::fopen(resolved.value().c_str(), "rb");
  if (file == nullptr) {
    const int open_error = errno;
    const StorageError error =
        open_error == ENOENT ? StorageError::kNotFound : StorageError::kOpenFailed;
    return Result<Bytes>::Failure(
        error, ErrnoMessage("could not open", relative_path, open_error));
  }

  Bytes bytes;
  std::uint8_t buffer[4096];
  while (true) {
    const std::size_t count = std::fread(buffer, 1, sizeof(buffer), file);
    if (count > max_bytes - bytes.size()) {
      std::fclose(file);
      return Result<Bytes>::Failure(
          StorageError::kReadFailed,
          "could not read '" + relative_path +
              "': file exceeds the " + std::to_string(max_bytes) + "-byte limit");
    }
    bytes.insert(bytes.end(), buffer, buffer + count);
    if (count < sizeof(buffer)) {
      if (std::ferror(file) != 0) {
        const int read_error = errno;
        std::fclose(file);
        return Result<Bytes>::Failure(
            StorageError::kReadFailed,
            ErrnoMessage("could not read", relative_path, read_error));
      }
      break;
    }
  }
  std::fclose(file);
  return Result<Bytes>::Success(std::move(bytes));
}

Result<void> Storage::WriteAtomic(const std::string& relative_path,
                                  const Bytes& bytes) const {
  return WriteAtomicImpl(relative_path, bytes, false);
}

Result<void> Storage::WriteAtomicOpen(const std::string& relative_path, const Bytes& bytes) const {
  // As WriteAtomicPrivate: a root raise on another thread would leave a root-owned file Playden can't replace.
  const auto uid = ps5::HoldEffectiveUid();
  return WriteAtomicImpl(relative_path, bytes, false);
}

Result<void> Storage::WriteAtomicPrivate(const std::string& relative_path,
                                         const Bytes& bytes) const {
  const auto uid = ps5::HoldEffectiveUid();
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    bool retry = false;
    auto written = WriteAtomicImpl(relative_path, bytes, true, &retry);
    if (!retry) return written;
  }
  return Result<void>::Failure(StorageError::kPermissionFailed, "Private storage owner changed repeatedly.");
}

Result<void> Storage::WriteAtomicImpl(const std::string& relative_path,
                                      const Bytes& bytes,
                                      bool private_file, bool* retry) const {
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) {
    return Result<void>::Failure(resolved.error(), resolved.message());
  }

  const std::string temporary_path = resolved.value() + ".tmp";
  errno = 0;
  std::FILE* file = nullptr;
  if (private_file) {
    // Discard a stale temp from an interrupted save, including one created during a root raise.
    if (::unlink(temporary_path.c_str()) != 0 && errno != ENOENT)
      return Result<void>::Failure(StorageError::kOpenFailed, "Private storage temporary file could not be removed.");
    const int descriptor = open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) {
      return Result<void>::Failure(StorageError::kOpenFailed,
                                   "private storage open failed for '" + relative_path +
                                       "' (errno " + std::to_string(errno) + ")");
    }
    if (fchmod(descriptor, 0600) != 0) {
      const int permission_error = errno;
      close(descriptor);
      std::remove(temporary_path.c_str());
      return Result<void>::Failure(StorageError::kPermissionFailed,
                                   "private storage permissions failed for '" + relative_path +
                                       "' (errno " + std::to_string(permission_error) + ")");
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0) {
      const int permission_error = errno;
      close(descriptor);
      std::remove(temporary_path.c_str());
      return Result<void>::Failure(
          StorageError::kPermissionFailed,
          "private storage permission verification failed for '" + relative_path +
              "' (errno " + std::to_string(permission_error) + ")");
    }
    if ((status.st_mode & 0777) != 0600) {
      const unsigned observed_mode = static_cast<unsigned>(status.st_mode & 0777);
      close(descriptor);
      std::remove(temporary_path.c_str());
      return Result<void>::Failure(
          StorageError::kPermissionFailed,
          "private storage mode verification failed for '" + relative_path +
              "' (mode " + std::to_string(observed_mode) + ")");
    }
    file = fdopen(descriptor, "wb");
    if (!file) {
      const int stream_error = errno;
      close(descriptor);
      std::remove(temporary_path.c_str());
      return Result<void>::Failure(StorageError::kOpenFailed,
                                   "private storage stream failed for '" + relative_path +
                                       "' (errno " + std::to_string(stream_error) + ")");
    }
  } else {
    // 0666 so FTP (root, no permission bypass on /data) can read and replace it (#868). A filesystem
    // that keeps no modes (exFAT) refuses the chmod; the file is fine as it is there. A stale temporary file is
    // removed and a new one made exclusively: a name linked to another file is never written through.
    if (::unlink(temporary_path.c_str()) != 0 && errno != ENOENT)
      return Result<void>::Failure(StorageError::kOpenFailed, "Storage temporary file could not be removed.");
    const int descriptor = open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (descriptor >= 0) {
      (void)fchmod(descriptor, 0666);
      file = fdopen(descriptor, "wb");
      if (!file) close(descriptor);
    }
  }
  if (file == nullptr) {
    return Result<void>::Failure(
        StorageError::kOpenFailed,
        WriteErrorMessage("could not open temporary file for", relative_path, errno, private_file));
  }

  if (!bytes.empty() &&
      std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
    const int write_error = errno;
    std::fclose(file);
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kWriteFailed,
        WriteErrorMessage("could not write", relative_path, write_error, private_file));
  }
  if (std::fflush(file) != 0) {
    const int flush_error = errno;
    std::fclose(file);
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kWriteFailed,
        WriteErrorMessage("could not flush", relative_path, flush_error, private_file));
  }
  // On disk before the rename: a crash or power loss after it must find the old file or the new one,
  // never an empty or zero-filled one under the real name.
  if (::fsync(fileno(file)) != 0) {
    const int sync_error = errno;
    std::fclose(file);
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kSyncFailed,
        WriteErrorMessage("could not sync", relative_path, sync_error, private_file));
  }
  if (private_file) {
    struct stat status{};
    const bool inspected = ::fstat(fileno(file), &status) == 0;
    if (!inspected || status.st_uid != ::geteuid()) {
      std::fclose(file);
      const bool removed = ::unlink(temporary_path.c_str()) == 0;
      if (retry) *retry = inspected && removed;
      return Result<void>::Failure(StorageError::kPermissionFailed, "Private storage temporary owner is not the app user.");
    }
  }
  if (std::fclose(file) != 0) {
    const int close_error = errno;
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kWriteFailed,
        WriteErrorMessage("could not close", relative_path, close_error, private_file));
  }

  errno = 0;
  if (std::rename(temporary_path.c_str(), resolved.value().c_str()) != 0) {
    const int rename_error = errno;
    std::remove(temporary_path.c_str());
    return Result<void>::Failure(
        StorageError::kRenameFailed,
        WriteErrorMessage("could not replace", relative_path, rename_error, private_file));
  }
  // The rename itself reaches the disk with the folder. The new contents are already in place, so a failed
  // folder sync doesn't fail the save. Opened by path: a title's openat takes no real folder descriptor.
  const auto slash = resolved.value().rfind('/');
  const std::string folder = slash == std::string::npos ? std::string(".") : resolved.value().substr(0, slash);
  if (const int directory = ::open(folder.empty() ? "/" : folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      directory >= 0) {
    ::fsync(directory);
    ::close(directory);
  }
  return Result<void>::Success();
}

Result<void> Storage::Remove(const std::string& relative_path) const {
  const auto resolved = Resolve(relative_path);
  if (!resolved.ok()) return Result<void>::Failure(resolved.error(), resolved.message());
  errno = 0;
  if (unlink(resolved.value().c_str()) == 0 || errno == ENOENT) return Result<void>::Success();
  const int remove_error = errno;
  return Result<void>::Failure(StorageError::kRemoveFailed,
                               "storage remove failed for '" + relative_path + "' (errno " +
                                   std::to_string(remove_error) + ")");
}

Result<std::vector<StoredFile>> Storage::ListFiles() const {
  using Listed = Result<std::vector<StoredFile>>;
  if (root_.empty()) return Listed::Failure(StorageError::kInvalidPath, "storage root is empty");
  DIR* directory = opendir(root_.c_str());
  if (!directory) {
    if (errno == ENOENT) return Listed::Success({});
    return Listed::Failure(StorageError::kOpenFailed,
                           "storage listing failed (errno " + std::to_string(errno) + ")");
  }
  std::vector<StoredFile> files;
  while (const dirent* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    struct stat info{};
    const std::string path = root_ + "/" + name;
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
#if defined(__APPLE__)
    const auto& modified = info.st_mtimespec;
#else
    const auto& modified = info.st_mtim;
#endif
    files.push_back({name, static_cast<std::int64_t>(modified.tv_sec) * 1000000000 +
                               static_cast<std::int64_t>(modified.tv_nsec)});
  }
  closedir(directory);
  return Listed::Success(std::move(files));
}

}  // namespace playden
