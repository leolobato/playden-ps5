// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/file_store.h"

#include "platform/directory_walk.h"
#include "platform/random_id.h"
#include "runtime/artifact_hash.h"
#include "runtime/redist_names.h"
#include "web/zip.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace playden::web {
namespace {

namespace fs = std::filesystem;
using platform::DirectoryPolicy;
using platform::ManagedDirectory;

// Console storage takes large writes far better than small ones: 127 KiB at a time gave 34 MB/s on internal storage
// and 1 MiB at a time gave 89 MB/s, with the same disk and the same network (2026-11-04, 1 GiB, wired).
constexpr std::size_t kTransferBytes = 1024 * 1024;
constexpr std::uint64_t kSpaceMargin = 64ull * 1024 * 1024;
constexpr std::size_t kMaxZipEntries = 65535;
constexpr std::uint64_t kMaxZipBytes = 0x7fffffffu;
constexpr std::size_t kMaxZipDepth = 32;
constexpr std::size_t kMaxTreeFolderEntries = 65536;  // more stays in the folder until the next delete
constexpr std::size_t kTreeBatch = 64;                // files removed under one hold of the locks

const FileError kOk{};
FileError Busy() { return {503, "Playden is busy. Try again in a moment."}; }
FileError NotFound() { return {404, "That file or folder isn't there."}; }
FileError BadPath() { return {400, "That name can't be used."}; }

FileError FromErrno(int error, const std::string& what) {
  switch (error) {
    case ENOENT: return NotFound();
    case EACCES: case EPERM: return {403, "This file belongs to another user, so Playden can't change it."};
    case EEXIST: return {409, "A file or folder with that name is already there."};
    case ENOTEMPTY: return {409, "This folder isn't empty."};
    case ENOSPC: return {507, "There is no room left on this drive."};
    case ENAMETOOLONG: return {400, "That name is too long."};
    default: return {500, what + ": " + std::strerror(error)};
  }
}

struct RawEntry {
  std::string name;
  struct stat info;
};

// Every regular file and folder a folder lists, links left out. Path operations: hold the lock.
bool ReadFolder(const std::string& path, std::vector<RawEntry>* out, std::size_t limit, bool* truncated, std::uint64_t* others = nullptr) {
  DIR* directory = ::opendir(path.c_str());
  if (!directory) return false;
  while (true) {
    if (out->size() >= limit) {
      if (truncated) *truncated = true;
      break;
    }
    const auto* entry = ::readdir(directory);
    if (!entry) break;
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    struct stat info{};
    if (::lstat((path + "/" + name).c_str(), &info) != 0) continue;
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
      if (others) ++*others;
      continue;
    }
    out->push_back({name, info});
  }
  ::closedir(directory);
  return true;
}

bool HardLinked(const struct stat& info) { return S_ISREG(info.st_mode) && info.st_nlink != 1; }

std::string Join(const std::vector<std::string>& parts, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) out += (i ? "/" : "") + parts[i];
  return out;
}

struct FdCloser {
  int fd = -1;
  ~FdCloser() { if (fd >= 0) ::close(fd); }
};

}  // namespace

FileStore::FileStore(Options options) : options_(std::move(options)) {
  if (!options_.random_id) options_.random_id = [] { return playden::RandomUuid(); };
}

std::shared_ptr<void> FileStore::Hold() {
  if (!options_.try_hold) return std::make_shared<int>(0);
  for (int attempt = 0; attempt < std::max(options_.hold_tries, 1); ++attempt) {
    if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(options_.hold_retry_ms));
    if (auto held = options_.try_hold()) return held;
  }
  return nullptr;
}

FileError FileStore::WithHold(const std::function<FileError()>& work) {
  const auto held = Hold();
  if (!held) return Busy();
  return work();
}

bool FileStore::Step(const std::string& step) {
  if (!options_.fault || options_.fault(step)) return true;
  errno = EIO;
  return false;
}

std::int64_t FileStore::Now() const {
  if (options_.now) return options_.now();
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::optional<std::string> FileStore::RandomText() { return options_.random_id(); }

std::optional<DirectoryPolicy> FileStore::WritePolicy(const FileRoot& root) {
  if (!root.game_folder) return DirectoryPolicy::kApprovedTree;
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto found = records_ownership_.find(root.path);
  if (found == records_ownership_.end()) {
    // Writes a probe file, so it waits for the first write. The answer can't change for a folder this session.
    const auto records = platform::RecordsOwnership(root.path);
    if (!records) return std::nullopt;
    found = records_ownership_.emplace(root.path, *records).first;
  }
  return found->second ? DirectoryPolicy::kApprovedTree : DirectoryPolicy::kUnownedTree;
}

// Opens the root and steps down, one part at a time: every policy refuses a link in any component. Reads of a game
// folder use the no-owners policy, so they never need the write probe. Hold the lock.
FileError FileStore::Locate(const FileRoot& root, const std::vector<std::string>& parts, bool writing, bool create_root,
                            std::optional<ManagedDirectory>* out) {
  DirectoryPolicy policy = root.game_folder ? DirectoryPolicy::kUnownedTree : DirectoryPolicy::kApprovedTree;
  if (writing) {
    const auto chosen = WritePolicy(root);
    if (!chosen) return {500, "Playden couldn't write a test file in this folder."};
    policy = *chosen;
  }
  auto current = ManagedDirectory::Open(root.path, policy);
  if (!current && errno == ENOENT && create_root && !root.create_from.empty()) {
    auto base = ManagedDirectory::Open(root.create_from, policy);
    if (!base) return FromErrno(errno, "The game's folder can't be opened");
    const auto relative = fs::path(root.path).lexically_relative(root.create_from);
    for (const auto& part : relative) {
      auto next = base->OpenChild(part.string(), policy, true, root.folder_mode);
      if (!next || !next->Sync() || !base->Sync()) return FromErrno(errno, "The folder can't be made");
      base = std::move(next);
    }
    current = std::move(base);
  }
  if (!current) return FromErrno(errno, "This folder can't be opened");
  for (const auto& part : parts) {
    auto next = current->OpenChild(part, policy);
    if (!next) return FromErrno(errno, "This folder can't be opened");
    current = std::move(next);
  }
  *out = std::move(current);
  return kOk;
}

FileError FileStore::List(const FileRoot& root, const std::string& path, FileListing* out) {
  const auto parts = SplitRelativePath(path);
  if (!parts || PrivatePath(*parts, root.private_paths)) return NotFound();
  const auto held = Hold();
  if (!held) return Busy();
  CleanUpDeferred();
  *out = {};
  std::optional<ManagedDirectory> folder;
  if (auto error = Locate(root, *parts, false, false, &folder); !error.ok()) {
    // A root that isn't made yet is an empty folder.
    if (error.status == 404 && parts->empty()) return kOk;
    return error;
  }
  out->exists = true;
  out->read_only = ReadOnlyPath(*parts);
  std::vector<RawEntry> raw;
  if (!ReadFolder(folder->path().string(), &raw, kMaxListedEntries * 2, &out->truncated))
    return FromErrno(errno, "This folder can't be listed");
  // An upload cut off by a game's start leaves its partial file, which nothing else may ever come back for.
  if (std::any_of(raw.begin(), raw.end(), [](const RawEntry& entry) { return entry.name.rfind(kUploadPrefix, 0) == 0 || entry.name.rfind(kRestorePrefix, 0) == 0; }))
    Sweep(*folder);
  std::map<std::string, PreviousVersion> previous;
  const std::string prefix = kPreviousPrefix;
  for (const auto& entry : raw) {
    if (entry.name.rfind(prefix, 0) != 0 || entry.name.size() <= prefix.size()) continue;
    if (!S_ISREG(entry.info.st_mode) || HardLinked(entry.info)) continue;
    const auto name = entry.name.substr(prefix.size());
    if (!UsableName(name) || HiddenName(name)) continue;
    previous[LowerAscii(name)] = {static_cast<std::uint64_t>(entry.info.st_size), static_cast<std::int64_t>(entry.info.st_mtime)};
  }
  auto with_path = *parts;
  with_path.emplace_back();
  for (const auto& entry : raw) {
    if (HiddenName(entry.name)) continue;
    if (!UsableName(entry.name) || HardLinked(entry.info)) { ++out->unshown; continue; }
    with_path.back() = entry.name;
    if (PrivatePath(with_path, root.private_paths)) continue;
    if (out->entries.size() >= kMaxListedEntries) { out->truncated = true; break; }
    FileEntry listed;
    listed.name = entry.name;
    listed.directory = S_ISDIR(entry.info.st_mode);
    listed.size = listed.directory ? 0 : static_cast<std::uint64_t>(entry.info.st_size);
    listed.modified = entry.info.st_mtime;
    listed.read_only = out->read_only || (listed.directory && ReadOnlyFolderName(entry.name));
    if (!listed.directory) {
      const auto found = previous.find(LowerAscii(entry.name));
      if (found != previous.end()) {
        listed.previous = found->second;
        previous.erase(found);
      }
    }
    out->entries.push_back(std::move(listed));
  }
  // A previous version with nothing beside it: all that's left of a replace a crash stopped.
  for (const auto& entry : raw) {
    if (entry.name.rfind(prefix, 0) != 0 || !S_ISREG(entry.info.st_mode) || HardLinked(entry.info)) continue;
    const auto name = entry.name.substr(prefix.size());
    const auto found = previous.find(LowerAscii(name));
    if (found == previous.end()) continue;
    FileEntry listed;
    listed.name = name;
    listed.size = found->second.size;
    listed.modified = found->second.modified;
    listed.read_only = out->read_only;
    listed.previous = found->second;
    listed.recoverable = true;
    out->entries.push_back(std::move(listed));
    previous.erase(found);
  }
  std::sort(out->entries.begin(), out->entries.end(), [](const FileEntry& left, const FileEntry& right) {
    if (left.directory != right.directory) return left.directory;
    return LowerAscii(left.name) < LowerAscii(right.name);
  });
  return kOk;
}

FileError FileStore::OpenForRead(const FileRoot& root, const std::string& path, bool previous, OpenedFile* out) {
  auto parts = SplitRelativePath(path);
  if (!parts || parts->empty() || PrivatePath(*parts, root.private_paths)) return NotFound();
  const auto name = parts->back();
  parts->pop_back();
  const auto held = Hold();
  if (!held) return Busy();
  std::optional<ManagedDirectory> folder;
  if (auto error = Locate(root, *parts, false, false, &folder); !error.ok()) return error;
  // The walk for reads, not OpenRegularFile: exFAT numbers an empty file differently from one look to the next. It
  // still refuses a link, and takes a regular file only from the folder's device.
  const int walk = platform::OpenDirectoryWalk(folder->path());
  if (walk < 0) return FromErrno(errno, "This folder can't be opened");
  const std::string opened_name = previous ? kPreviousPrefix + name : name;
  const int fd = platform::OpenWalkChildUnprivileged(walk, opened_name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  const int open_error = errno;
  ::close(walk);
  if (fd < 0) return open_error == ELOOP || open_error == EMLINK ? NotFound() : FromErrno(open_error, "This file can't be opened");
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || HardLinked(info)) {
    ::close(fd);
    return NotFound();
  }
  // Reads stay blocking from here: a regular file's descriptor only needed the flag for the open.
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  out->fd = fd;
  out->size = static_cast<std::uint64_t>(info.st_size);
  out->name = name;
  return kOk;
}

FileError FileStore::ReadText(const FileRoot& root, const std::string& path, FileText* out) {
  OpenedFile file;
  if (auto error = OpenForRead(root, path, false, &file); !error.ok()) return error;
  FdCloser closer{file.fd};
  if (file.size > kMaxTextBytes) return {413, "This file is too large to edit here. Download it instead."};
  std::string text(static_cast<std::size_t>(file.size), '\0');
  std::size_t done = 0;
  while (done < text.size()) {
    const auto count = ::read(file.fd, text.data() + done, text.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return FromErrno(errno, "This file can't be read");
    done += static_cast<std::size_t>(count);
  }
  if (!DecodeText(text, &out->text, &out->encoding, &out->bom)) return {415, "This file uses an encoding the editor can't show. Download it instead."};
  // Of the bytes on disk, not the decoded text: the save compares it with the file as it is then.
  const auto hash = runtime::HashArtifactBytes(text);
  if (!hash) return {500, "This file can't be read."};
  out->sha256 = *hash;
  return kOk;
}

bool FileStore::RemoveTemporary(const std::string& folder, const std::string& name, DirectoryPolicy policy) {
  auto opened = ManagedDirectory::Open(folder, policy);
  if (!opened) return errno == ENOENT;
  if (!opened->RemoveRegularFile(name)) return errno == ENOENT;
  opened->Sync();
  return true;
}

void FileStore::DeferCleanup(const FileRoot&, std::string folder, std::string name, DirectoryPolicy policy) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  deferred_.push_back({std::move(folder), std::move(name), policy});
}

void FileStore::CleanUpDeferred() {
  std::vector<Deferred> work;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    work.swap(deferred_);
  }
  if (work.empty()) return;
  // One try: the caller's work goes on whether or not this can run now.
  std::shared_ptr<void> held = options_.try_hold ? options_.try_hold() : std::make_shared<int>(0);
  std::vector<Deferred> left;
  for (auto& item : work)
    if (!held || !RemoveTemporary(item.folder, item.name, item.policy)) left.push_back(std::move(item));
  if (left.empty()) return;
  std::lock_guard<std::mutex> lock(state_mutex_);
  for (auto& item : left) deferred_.push_back(std::move(item));
}

std::size_t FileStore::DeferredCleanups() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return deferred_.size();
}

// Temporary files a crash or quit left in this folder, once they are old. An upload's is only part of a file. A
// restore's is the player's file, so it finishes the swap, or undoes it if the restored file never arrived. It goes
// only if the file and its previous version both came back since: that version was replaced as usual. Hold the lock.
void FileStore::Sweep(const ManagedDirectory& folder) {
  std::vector<RawEntry> raw;
  if (!ReadFolder(folder.path().string(), &raw, kMaxListedEntries * 2, nullptr)) return;
  const auto now = Now();
  for (const auto& entry : raw) {
    if (entry.name.rfind(kUploadPrefix, 0) != 0 && entry.name.rfind(kRestorePrefix, 0) != 0) continue;
    const bool restore = entry.name.rfind(kRestorePrefix, 0) == 0;
    // A rename's temporary can be a folder (a new case for its name): put back, never removed.
    if (!S_ISREG(entry.info.st_mode) && !(restore && S_ISDIR(entry.info.st_mode))) continue;
    // A restore's temporary is the player's file, with its old time: its age is in its name.
    std::int64_t made = entry.info.st_mtime;
    if (entry.name.rfind(kRestorePrefix, 0) == 0) {
      const auto digits = entry.name.substr(std::strlen(kRestorePrefix), entry.name.find('-', std::strlen(kRestorePrefix)) - std::strlen(kRestorePrefix));
      if (!digits.empty() && digits.size() <= 18 && digits.find_first_not_of("0123456789") == std::string::npos) made = std::atoll(digits.c_str());
    }
    if (now - made < options_.stale_seconds) continue;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (active_uploads_.count((folder.path() / entry.name).string())) continue;
    }
    if (entry.name.rfind(kRestorePrefix, 0) == 0) {
      const auto dot = entry.name.find('.', std::strlen(kRestorePrefix));
      if (dot != std::string::npos && dot + 1 < entry.name.size()) {
        const auto name = entry.name.substr(dot + 1);
        const auto free = [&](const std::string& child) {
          struct stat there{};
          return !folder.InspectChild(child, &there) && errno == ENOENT;
        };
        const bool directory = S_ISDIR(entry.info.st_mode);
        const auto target = free(name) ? name : !directory && free(kPreviousPrefix + name) ? kPreviousPrefix + name : std::string();
        if (!target.empty()) {
          if (folder.RenameChild(entry.name, folder, target, false)) folder.Sync();
          continue;
        }
      }
    }
    if (S_ISREG(entry.info.st_mode)) folder.RemoveRegularFile(entry.name);
  }
}

FileError FileStore::Upload(const FileRoot& root, const std::string& path, const UploadOptions& upload,
                            const UploadReader& read, bool* replaced) {
  if (replaced) *replaced = false;
  auto parts = SplitRelativePath(path);
  if (!parts || parts->empty() || PrivatePath(*parts, root.private_paths)) return BadPath();
  if (ReadOnlyPath(*parts)) return {403, "Playden writes this folder itself, so it can't be changed here."};
  const auto name = parts->back();
  if (!NewName(name)) return BadPath();
  parts->pop_back();
  std::string temporary, folder_path;
  DirectoryPolicy policy;
  int fd = -1;
  {
    const auto held = Hold();
    if (!held) return Busy();
    CleanUpDeferred();
    std::optional<ManagedDirectory> folder;
    if (auto error = Locate(root, *parts, true, parts->empty(), &folder); !error.ok()) return error;
    policy = folder->policy();
    if (options_.free_space) {
      const auto free = options_.free_space(folder->path().string());
      if (free && upload.length + kSpaceMargin > *free) return {507, "There is no room for this file on this drive."};
    }
    struct stat existing{};
    if (folder->InspectChild(name, &existing)) {
      if (S_ISDIR(existing.st_mode)) return {409, "A folder with that name is already there."};
      if (!upload.overwrite) return {409, "A file with that name is already there."};
      if (policy != DirectoryPolicy::kUnownedTree && existing.st_uid != ::geteuid())
        return FromErrno(EACCES, "");
    } else if (errno != ENOENT) {
      return FromErrno(errno, "This folder can't be read");
    }
    Sweep(*folder);
    const auto id = RandomText();
    if (!id) return {500, "Playden couldn't make a temporary name."};
    temporary = kUploadPrefix + *id;
    fd = folder->CreateExclusiveFile(temporary, root.file_mode);
    if (fd < 0) return FromErrno(errno, "The file can't be made");
    folder_path = folder->path().string();
    std::lock_guard<std::mutex> lock(state_mutex_);
    active_uploads_.insert(folder_path + "/" + temporary);
  }
  const auto finish = [&](FileError error) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      active_uploads_.erase(folder_path + "/" + temporary);
    }
    if (!error.ok()) {
      // Cleaned up now if the lock is free. A setup holds it for minutes: then at a later file request.
      const auto held = options_.try_hold ? options_.try_hold() : std::make_shared<int>(0);
      if (!held || !RemoveTemporary(folder_path, temporary, policy)) DeferCleanup(root, folder_path, temporary, policy);
    }
    return error;
  };
  // Bytes move with no lock held.
  {
    FdCloser closer{fd};
    std::vector<char> buffer(kTransferBytes);
    std::uint64_t left = upload.length;
    while (left > 0) {
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
      // The whole buffer before each write: a read gives only what has arrived.
      std::size_t have = 0;
      while (have < want) {
        const auto count = read(buffer.data() + have, want - have);
        if (count <= 0) return finish({408, "The upload stopped before the end."});
        have += static_cast<std::size_t>(count);
      }
      std::size_t written = 0;
      while (written < have) {
        const auto wrote = ::write(fd, buffer.data() + written, have - written);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return finish(FromErrno(errno, "The file can't be written"));
        written += static_cast<std::size_t>(wrote);
      }
      left -= static_cast<std::uint64_t>(have);
    }
    if (!Step("sync-temporary") || ::fsync(fd) != 0) return finish(FromErrno(errno ? errno : EIO, "The file can't be saved"));
  }
  FileError error;
  bool changed_disk = false;
  {
    std::lock_guard<std::mutex> commit(commit_mutex_);
    parts->push_back(name);
    // The editor's check: read and hashed with no path lock, as any file's bytes. Nothing of Playden's can change the
    // file meanwhile (the commit lock), and the commit checks that its size and time are still those read.
    struct stat checked{};
    if (upload.if_sha256) {
      FileText current;
      if (const auto read_error = ReadText(root, path, &current); !read_error.ok())
        return finish(read_error.status == 503 ? read_error : FileError{409, "This file changed since you opened it."});
      if (current.sha256 != *upload.if_sha256) return finish({409, "This file changed since you opened it."});
      const auto held = Hold();
      if (!held) return finish(Busy());
      std::optional<ManagedDirectory> folder;
      const std::vector<std::string> above(parts->begin(), parts->end() - 1);
      if (auto located = Locate(root, above, true, false, &folder); !located.ok()) return finish(located);
      if (!folder->InspectChild(name, &checked)) return finish({409, "This file changed since you opened it."});
    }
    const auto held = Hold();
    if (!held) return finish(Busy());
    changed_disk = CommitUpload(root, *parts, temporary, upload, upload.if_sha256 ? &checked : nullptr, replaced, &error);
  }
  if (changed_disk) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    active_uploads_.erase(folder_path + "/" + temporary);
    return error;
  }
  return finish(error);
}

// The Replace sequence, each step followed by a sync of the folder. A crash leaves one of: the old file with the
// temporary; the file with the old previous version gone; only the previous version (listed as Recoverable).
// True when the temporary is gone or must stay as it is. Holds the commit lock and the path lock.
bool FileStore::CommitUpload(const FileRoot& root, const std::vector<std::string>& parts, const std::string& temporary,
                             const UploadOptions& upload, const struct stat* expected, bool* replaced, FileError* error) {
  const auto name = parts.back();
  const std::vector<std::string> above(parts.begin(), parts.end() - 1);
  std::optional<ManagedDirectory> folder;
  if (auto located = Locate(root, above, true, false, &folder); !located.ok()) {
    *error = located;
    return false;
  }
  const auto policy = folder->policy();
  struct stat existing{};
  const bool exists = folder->InspectChild(name, &existing);
  if (!exists && errno != ENOENT) {
    *error = FromErrno(errno, "This folder can't be read");
    return false;
  }
  if (exists && S_ISDIR(existing.st_mode)) { *error = {409, "A folder with that name is already there."}; return false; }
  if (exists && !upload.overwrite) { *error = {409, "A file with that name is already there."}; return false; }
  if (exists && policy != DirectoryPolicy::kUnownedTree && existing.st_uid != ::geteuid()) {
    *error = FromErrno(EACCES, "");
    return false;
  }
  if (expected && (!exists || existing.st_size != expected->st_size || existing.st_mtime != expected->st_mtime)) {
    *error = {409, "This file changed since you opened it."};
    return false;
  }
  const auto failed = [&](const char* step) {
    // Something on disk already moved: the page lists the folder again, and shows what is there.
    *error = {500, std::string("The save stopped at \"") + step + "\". Check this folder."};
    return true;
  };
  if (exists) {
    const auto backup = kPreviousPrefix + name;
    struct stat old{};
    if (folder->InspectChild(backup, &old)) {
      if (!Step("remove-backup") || !folder->RemoveRegularFile(backup)) {
        *error = FromErrno(errno ? errno : EIO, "The previous version can't be replaced");
        return false;
      }
    }
    if (!Step("move-current") || !folder->RenameChild(name, *folder, backup, false)) {
      *error = FromErrno(errno ? errno : EIO, "The file can't be replaced");
      return false;
    }
    if (!Step("sync-backup") || !folder->Sync()) return failed("sync");
  }
  if (!Step("publish") || !folder->RenameChild(temporary, *folder, name, false)) {
    if (exists) return failed("publish");
    *error = FromErrno(errno ? errno : EIO, "The file can't be saved");
    return false;
  }
  if (!Step("sync-publish") || !folder->Sync()) return failed("sync");
  if (replaced) *replaced = exists;
  *error = {};
  return true;
}

FileError FileStore::MakeFolder(const FileRoot& root, const std::string& path, bool parents) {
  auto parts = SplitRelativePath(path);
  if (!parts || parts->empty() || PrivatePath(*parts, root.private_paths)) return BadPath();
  if (ReadOnlyPath(*parts)) return {403, "Playden writes this folder itself, so it can't be changed here."};
  if (parents) {
    std::lock_guard<std::mutex> commit(commit_mutex_);
    const auto held = Hold();
    if (!held) return Busy();
    std::optional<ManagedDirectory> folder;
    if (auto error = Locate(root, {}, true, true, &folder); !error.ok()) return error;
    for (const auto& part : *parts) {
      struct stat existing{};
      const bool exists = folder->InspectChild(part, &existing);
      if (!exists && errno != ENOENT) return FromErrno(errno, "This folder can't be read");
      if (exists && !S_ISDIR(existing.st_mode)) return {409, "A file named " + part + " is in the way."};
      if (!exists && !NewName(part)) return BadPath();
      auto next = folder->OpenChild(part, folder->policy(), !exists, root.folder_mode);
      if (!next) return FromErrno(errno, "The folder can't be made");
      if (!exists && (!next->Sync() || !folder->Sync())) return FromErrno(errno, "The folder can't be saved");
      folder = std::move(next);
    }
    return kOk;
  }
  const auto name = parts->back();
  if (!NewName(name)) return BadPath();
  parts->pop_back();
  std::lock_guard<std::mutex> commit(commit_mutex_);
  const auto held = Hold();
  if (!held) return Busy();
  std::optional<ManagedDirectory> folder;
  if (auto error = Locate(root, *parts, true, parts->empty(), &folder); !error.ok()) return error;
  struct stat existing{};
  if (folder->InspectChild(name, &existing)) return FromErrno(EEXIST, "");
  auto made = folder->OpenChild(name, folder->policy(), true, root.folder_mode);
  if (!made) return FromErrno(errno, "The folder can't be made");
  if (!made->Sync() || !folder->Sync()) return FromErrno(errno, "The folder can't be saved");
  return kOk;
}

FileError FileStore::Delete(const FileRoot& root, const std::string& path) {
  auto parts = SplitRelativePath(path);
  if (!parts || parts->empty() || PrivatePath(*parts, root.private_paths)) return NotFound();
  if (ReadOnlyPath(*parts)) return {403, "Playden writes this folder itself, so it can't be changed here."};
  const auto name = parts->back();
  parts->pop_back();
  std::lock_guard<std::mutex> commit(commit_mutex_);
  const auto held = Hold();
  if (!held) return Busy();
  std::optional<ManagedDirectory> folder;
  if (auto error = Locate(root, *parts, true, false, &folder); !error.ok()) return error;
  const auto backup = kPreviousPrefix + name;
  struct stat target{}, old{};
  const bool exists = folder->InspectChild(name, &target);
  if (!exists && errno != ENOENT) return FromErrno(errno, "This folder can't be read");
  const bool has_backup = folder->InspectChild(backup, &old);
  if (!exists && !has_backup) return NotFound();
  if (exists && S_ISDIR(target.st_mode)) {
    // What a cut-off upload left in it mustn't keep a folder that looks empty.
    if (auto inside = folder->OpenChild(name, folder->policy())) Sweep(*inside);
    if (!folder->RemoveEmptyDirectory(name)) return FromErrno(errno, "The folder can't be deleted");
    if (!folder->Sync()) return FromErrno(errno, "The change can't be saved");
    return kOk;
  }
  if (exists && HardLinked(target)) return NotFound();
  // The previous version first: a crash leaves the file, not a file the player thought was gone.
  if (exists && folder->policy() != DirectoryPolicy::kUnownedTree && target.st_uid != ::geteuid()) return FromErrno(EACCES, "");
  if (has_backup) {
    if (!Step("delete-backup") || !folder->RemoveRegularFile(backup)) return FromErrno(errno ? errno : EIO, "The file can't be deleted");
    if (!folder->Sync()) return FromErrno(errno, "The change can't be saved");
  }
  if (exists) {
    if (!Step("delete-current") || !folder->RemoveRegularFile(name)) return FromErrno(errno ? errno : EIO, "The file can't be deleted");
    if (!folder->Sync()) return FromErrno(errno, "The change can't be saved");
  }
  return kOk;
}

FileError FileStore::DeleteTree(const FileRoot& root, const std::string& path, TreeSummary* out) { return WalkTree(root, path, true, out); }

FileError FileStore::SummarizeTree(const FileRoot& root, const std::string& path, TreeSummary* out) { return WalkTree(root, path, false, out); }

// Iterative, because worker stacks are small and a folder's depth is the player's: a folder is read and its files go,
// then its folders are walked, then it goes itself if it is empty by then.
FileError FileStore::WalkTree(const FileRoot& root, const std::string& path, bool remove, TreeSummary* out) {
  *out = {};
  const auto top = SplitRelativePath(path);
  if (!top || top->empty() || PrivatePath(*top, root.private_paths)) return NotFound();
  if (ReadOnlyPath(*top)) return {403, "Playden writes this folder itself, so it can't be changed here."};
  const auto locks = [&](std::unique_lock<std::mutex>* commit) {
    if (remove) *commit = std::unique_lock<std::mutex>(commit_mutex_);
    return Hold();
  };
  struct Frame {
    std::vector<std::string> parts;
    bool read = false;
  };
  std::vector<Frame> stack;
  stack.push_back({*top});
  while (!stack.empty()) {
    const auto current = stack.back().parts;
    if (stack.back().read) {
      stack.pop_back();
      if (!remove) {
        ++out->folders;
        continue;
      }
      std::unique_lock<std::mutex> commit;
      const auto held = locks(&commit);
      if (!held) return Busy();
      const std::vector<std::string> above(current.begin(), current.end() - 1);
      std::optional<ManagedDirectory> parent;
      if (auto error = Locate(root, above, true, false, &parent); !error.ok()) {
        if (error.status == 404) continue;
        return error;
      }
      if (!Step("tree-folder") || !parent->RemoveEmptyDirectory(current.back())) {
        const int error = errno;
        if (error == ENOENT || error == ENOTEMPTY || error == EEXIST) continue;
        if (error == EACCES || error == EPERM) { ++out->left; continue; }
        return FromErrno(error, "The folder can't be deleted");
      }
      if (!parent->Sync()) return FromErrno(errno, "The change can't be saved");
      ++out->folders;
      continue;
    }
    stack.back().read = true;
    std::vector<RawEntry> raw;
    bool truncated = false;
    {
      std::unique_lock<std::mutex> commit;
      const auto held = locks(&commit);
      if (!held) return Busy();
      std::optional<ManagedDirectory> folder;
      if (auto error = Locate(root, current, remove, false, &folder); !error.ok()) {
        if (current.size() == top->size()) return error;
        // A folder below that can't be opened, or a path too long to reach, stays with what's in it. The rest goes on.
        if (error.status != 404) ++out->left;
        stack.pop_back();
        continue;
      }
      if (remove) Sweep(*folder);
      if (!ReadFolder(folder->path().string(), &raw, kMaxTreeFolderEntries, &truncated, &out->left)) {
        if (current.size() == top->size()) return FromErrno(errno, "This folder can't be listed");
        ++out->left;
        stack.pop_back();
        continue;
      }
    }
    if (truncated) ++out->left;
    const std::string prefix = kPreviousPrefix;
    std::set<std::string> beside;
    for (const auto& entry : raw)
      if (S_ISREG(entry.info.st_mode)) beside.insert(LowerAscii(entry.name));
    std::vector<std::string> doomed;
    auto child = current;
    child.emplace_back();
    for (const auto& entry : raw) {
      const bool previous = S_ISREG(entry.info.st_mode) && entry.name.rfind(prefix, 0) == 0 && entry.name.size() > prefix.size();
      const auto shown = previous ? entry.name.substr(prefix.size()) : entry.name;
      child.back() = shown;
      if (!UsableName(shown) || HiddenName(shown) || PrivatePath(child, root.private_paths) || HardLinked(entry.info)) {
        ++out->left;
        continue;
      }
      if (S_ISDIR(entry.info.st_mode)) {
        if (ReadOnlyFolderName(shown)) ++out->left;
        else stack.push_back({child});
        continue;
      }
      out->bytes += static_cast<std::uint64_t>(entry.info.st_size);
      // A previous version counts once, with its file; alone, the page lists it as a file.
      if (!previous || !beside.count(LowerAscii(shown))) ++out->files;
      doomed.push_back(entry.name);
    }
    if (!remove) continue;
    for (std::size_t done = 0; done < doomed.size();) {
      std::unique_lock<std::mutex> commit;
      const auto held = locks(&commit);
      if (!held) return Busy();
      std::optional<ManagedDirectory> folder;
      if (auto error = Locate(root, current, true, false, &folder); !error.ok()) {
        if (error.status == 404) break;
        return error;
      }
      for (const auto end = std::min(doomed.size(), done + kTreeBatch); done < end; ++done) {
        if (Step("tree-file") && folder->RemoveRegularFile(doomed[done])) continue;
        const int error = errno;
        if (error == ENOENT) continue;
        if (error == EACCES || error == EPERM) { ++out->left; continue; }
        return FromErrno(error, "The file can't be deleted");
      }
      if (!folder->Sync()) return FromErrno(errno, "The change can't be saved");
    }
  }
  return kOk;
}

// Moves and renames. In one root a rename does it; across roots each file is copied whole, then published, then its
// source goes. Path operations hold the commit lock and the path lock a step at a time; bytes move with neither.
namespace {

FileError ReadOnly() { return {403, "Playden writes this folder itself, so it can't be changed here."}; }
FileError KeepsSignIn() { return {403, "Playden keeps a sign-in in this folder, so it can't be moved or renamed here."}; }

struct Present {
  std::string name;
  bool directory = false;
};

// What a folder has, by name without regard to case, as the page and Wine see it. A previous version alone is its
// file (listed as Recoverable).
std::map<std::string, Present> PresentNames(const std::vector<RawEntry>& raw) {
  std::map<std::string, Present> out;
  const std::string prefix = kPreviousPrefix;
  const auto previous = [&](const RawEntry& entry) { return entry.name.rfind(prefix, 0) == 0 && entry.name.size() > prefix.size(); };
  for (const auto& entry : raw)
    if (!previous(entry)) out[LowerAscii(entry.name)] = {entry.name, S_ISDIR(entry.info.st_mode)};
  for (const auto& entry : raw)
    if (previous(entry)) out.emplace(LowerAscii(entry.name.substr(prefix.size())), Present{entry.name.substr(prefix.size()), false});
  return out;
}

std::string Lowered(const FileRoot& root, const std::vector<std::string>& parts) {
  return LowerAscii(parts.empty() ? root.path : root.path + "/" + Join(parts, parts.size()));
}

bool Below(const std::string& path, const std::string& folder) {
  return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/';
}

// What a step of a folder's move does with an error: the item stays (the console won't let this user change it, or
// it is gone already), or the move stops.
enum class Outcome { kDone, kLeft, kGone, kStop };
Outcome Classify(const FileError& error) {
  if (error.ok()) return Outcome::kDone;
  if (error.status == 403) return Outcome::kLeft;
  if (error.status == 404) return Outcome::kGone;
  return Outcome::kStop;
}

}  // namespace

struct FileStore::MoveFrame {
  std::vector<std::string> source, target;
  bool read = false;
};

// A move across roots: what it has done so far, for whoever watches it.
struct FileStore::MoveTally {
  const MoveObserver* observe = nullptr;
  MoveProgress progress;
  // False: the watcher wants it to stop.
  bool Report() { return !observe || !*observe || (*observe)(progress); }
};

namespace {
FileError Stopped() { return {500, "The move stopped before it finished. Move it again to finish it."}; }
}  // namespace

FileError FileStore::Move(const FileRoot& from, const std::string& path, const FileRoot& to, const std::string& folder,
                          const std::string& name, MoveClash clash, MoveSummary* out, const MoveObserver& observe) {
  *out = {};
  MoveTally tally;
  tally.observe = &observe;
  bool counted = false;  // a file: its size is the total
  const auto source = SplitRelativePath(path);
  if (!source || source->empty() || PrivatePath(*source, from.private_paths)) return NotFound();
  if (ReadOnlyPath(*source)) return ReadOnly();
  if (HoldsPrivatePath(*source, from.private_paths)) return KeepsSignIn();
  auto target = SplitRelativePath(folder);
  if (!target || PrivatePath(*target, to.private_paths)) return NotFound();
  if (!NewName(name)) return BadPath();
  target->push_back(name);
  if (ReadOnlyPath(*target)) return ReadOnly();
  if (PrivatePath(*target, to.private_paths) || HoldsPrivatePath(*target, to.private_paths)) return KeepsSignIn();
  const bool same_root = from.path == to.path;
  const auto source_text = Lowered(from, *source);
  if (Below(Lowered(to, *target), source_text)) return {400, "A folder can't go inside itself."};
  const std::vector<std::string> source_above(source->begin(), source->end() - 1);
  const std::vector<std::string> target_above(target->begin(), target->end() - 1);
  const auto& source_name = source->back();
  out->name = name;
  bool directory = false, replace = false;
  std::vector<MoveFrame> stack;
  {
    std::lock_guard<std::mutex> commit(commit_mutex_);
    const auto held = Hold();
    if (!held) return Busy();
    CleanUpDeferred();
    std::optional<ManagedDirectory> from_folder, to_folder;
    if (auto error = Locate(from, source_above, true, false, &from_folder); !error.ok()) return error;
    struct stat info{};
    if (!from_folder->InspectChild(source_name, &info)) return errno == ENOENT ? NotFound() : FromErrno(errno, "This folder can't be read");
    if (HardLinked(info)) return NotFound();
    directory = S_ISDIR(info.st_mode);
    if (same_root && *source == *target) return kOk;
    if (!directory) {
      struct stat old{};
      tally.progress.total_files = 1;
      tally.progress.total_bytes = static_cast<std::uint64_t>(info.st_size);
      // Replace keeps the file there as the previous version: this one's own isn't copied then.
      if (clash != MoveClash::kReplace && from_folder->InspectChild(kPreviousPrefix + source_name, &old) && S_ISREG(old.st_mode))
        tally.progress.total_bytes += static_cast<std::uint64_t>(old.st_size);
      counted = true;
    }
    // Only in the same folder: on a case-sensitive drive "A" and "a" can be two folders.
    if (same_root && source_above == target_above && LowerAscii(source_name) == LowerAscii(name)) {
      // A new case for the same name. A case-sensitive drive can have both names as two files: that's a clash.
      struct stat other{};
      if (from_folder->InspectChild(name, &other) && (other.st_ino != info.st_ino || other.st_dev != info.st_dev))
        return FromErrno(EEXIST, "");
      if (auto error = CaseRename(*from_folder, source_name, name); !error.ok()) return error;
      ++(directory ? out->folders : out->files);
      return kOk;
    }
    if (auto error = Locate(to, target_above, true, target_above.empty(), &to_folder); !error.ok()) return error;
    std::vector<RawEntry> raw;
    if (!ReadFolder(to_folder->path().string(), &raw, kMaxTreeFolderEntries, nullptr)) return FromErrno(errno, "This folder can't be read");
    const auto present = PresentNames(raw);
    const auto found = present.find(LowerAscii(name));
    if (found != present.end()) {
      if (clash == MoveClash::kAsk) {
        out->clash = true;
        out->clash_folder = found->second.directory;
        out->name = found->second.name;
        return {409, "\"" + found->second.name + "\" is already there."};
      }
      if (clash == MoveClash::kKeepBoth) {
        std::optional<std::string> free;
        for (int number = 2; number < 1000 && !free; ++number) {
          auto candidate = NumberedName(name, directory, number);
          if (!candidate) return {400, "That name is too long for a second copy."};
          if (!present.count(LowerAscii(*candidate))) free = std::move(candidate);
        }
        if (!free) return FromErrno(EEXIST, "");
        target->back() = *free;
      } else {
        if (found->second.directory != directory)
          return {409, std::string(found->second.directory ? "A folder" : "A file") + " named \"" + found->second.name + "\" is in the way."};
        target->back() = found->second.name;
        // A merge into the folder that holds this one would take it into itself.
        if (directory && Below(source_text, Lowered(to, *target))) return {409, "A folder can't be merged into a folder that holds it."};
        replace = true;
      }
      out->name = target->back();
    }
    if (!directory && same_root) {
      auto error = RenameFile(*from_folder, source_name, *to_folder, target->back(), replace);
      if (error.ok()) ++out->files;
      return error;
    }
    if (directory && same_root && !replace) {
      if (!Step("move-rename") || !from_folder->RenameChild(source_name, *to_folder, target->back(), false))
        return FromErrno(errno ? errno : EIO, "The folder can't be moved");
      if (!from_folder->Sync() || !to_folder->Sync()) return FromErrno(errno, "The change can't be saved");
      ++out->folders;
      return kOk;
    }
    if (directory) {
      if (!replace) {
        auto made = to_folder->OpenChild(target->back(), to_folder->policy(), true, to.folder_mode);
        if (!made || !made->Sync() || !to_folder->Sync()) return FromErrno(errno, "The folder can't be made");
      }
      stack.push_back({*source, *target});
    }
  }
  if (!counted && !same_root) {
    // What a delete of the source would take: the same files a move copies. Without it the page shows no fraction.
    TreeSummary tree;
    if (WalkTree(from, path, false, &tree).ok()) {
      tally.progress.total_files = tree.files;
      tally.progress.total_bytes = tree.bytes;
    }
  }
  if (!tally.Report()) return Stopped();
  if (!directory) {
    auto error = CopyFile(from, *source, to, *target, replace, &tally);
    if (error.ok()) {
      ++out->files;
      ++tally.progress.files;
      tally.Report();
    }
    return error;
  }
  return MoveEntries(from, to, &stack, out, &tally);
}

// A folder's contents, iteratively (worker stacks are small): each folder's files move, then its folders, then it goes
// itself if it is empty by then. In one root a folder that isn't in the way goes whole.
FileError FileStore::MoveEntries(const FileRoot& from, const FileRoot& to, std::vector<MoveFrame>* stack, MoveSummary* out, MoveTally* tally) {
  const bool same_root = from.path == to.path;
  const std::string prefix = kPreviousPrefix;
  while (!stack->empty()) {
    // Folders too: a stop with only folders left must not finish the move.
    if (tally && !tally->Report()) return Stopped();
    if (stack->back().read) {
      const auto source = stack->back().source;
      stack->pop_back();
      ++out->folders;
      std::lock_guard<std::mutex> commit(commit_mutex_);
      const auto held = Hold();
      if (!held) return Busy();
      std::optional<ManagedDirectory> parent;
      if (auto error = Locate(from, {source.begin(), source.end() - 1}, true, false, &parent); !error.ok()) {
        if (error.status == 404) continue;
        return error;
      }
      if (!Step("move-folder") || !parent->RemoveEmptyDirectory(source.back())) {
        const int error = errno;
        if (error == ENOENT || error == ENOTEMPTY || error == EEXIST) continue;
        if (error == EACCES || error == EPERM) { ++out->left; continue; }
        return FromErrno(error, "The folder can't be removed");
      }
      if (!parent->Sync()) return FromErrno(errno, "The change can't be saved");
      continue;
    }
    stack->back().read = true;
    const auto frame = stack->back();
    struct Pending {
      std::string name, target;
      bool replace = false;
    };
    std::vector<Pending> files;
    {
      std::lock_guard<std::mutex> commit(commit_mutex_);
      const auto held = Hold();
      if (!held) return Busy();
      std::optional<ManagedDirectory> source_folder, target_folder;
      if (auto error = Locate(from, frame.source, true, false, &source_folder); !error.ok()) {
        // Gone, or can't be opened: what's in it stays. The rest goes on.
        if (error.status != 404) ++out->left;
        stack->pop_back();
        continue;
      }
      if (auto error = Locate(to, frame.target, true, false, &target_folder); !error.ok()) return error;
      Sweep(*source_folder);
      std::vector<RawEntry> raw, there;
      bool truncated = false;
      if (!ReadFolder(source_folder->path().string(), &raw, kMaxTreeFolderEntries, &truncated, &out->left)) {
        ++out->left;
        stack->pop_back();
        continue;
      }
      if (truncated) ++out->left;
      if (!ReadFolder(target_folder->path().string(), &there, kMaxTreeFolderEntries, nullptr)) return FromErrno(errno, "This folder can't be read");
      const auto present = PresentNames(there);
      std::set<std::string> beside;
      for (const auto& entry : raw)
        if (S_ISREG(entry.info.st_mode) && entry.name.rfind(prefix, 0) != 0) beside.insert(LowerAscii(entry.name));
      bool changed = false;
      auto child = frame.source;
      child.emplace_back();
      for (const auto& entry : raw) {
        if (S_ISREG(entry.info.st_mode) && entry.name.rfind(prefix, 0) == 0 && entry.name.size() > prefix.size()) {
          // Goes with its file. Alone, it is what a stopped replace left: it stays here to be restored.
          if (!beside.count(LowerAscii(entry.name.substr(prefix.size())))) ++out->left;
          continue;
        }
        child.back() = entry.name;
        const bool folder = S_ISDIR(entry.info.st_mode);
        auto target = frame.target;
        target.push_back(entry.name);
        if (!UsableName(entry.name) || HiddenName(entry.name) || PrivatePath(child, from.private_paths) || HardLinked(entry.info) ||
            (folder && ReadOnlyFolderName(entry.name)) || PrivatePath(target, to.private_paths) || HoldsPrivatePath(target, to.private_paths)) {
          ++out->left;
          continue;
        }
        const auto found = present.find(LowerAscii(entry.name));
        if (found != present.end()) {
          // A file and a folder of the same name: neither replaces the other.
          if (found->second.directory != folder) { ++out->left; continue; }
          target.back() = found->second.name;
          if (folder) stack->push_back({child, target});
          else files.push_back({entry.name, target.back(), true});
          continue;
        }
        if (!folder) {
          files.push_back({entry.name, entry.name, false});
          continue;
        }
        if (same_root) {
          if (!Step("move-rename") || !source_folder->RenameChild(entry.name, *target_folder, entry.name, false)) {
            if (errno == EACCES || errno == EPERM) { ++out->left; continue; }
            if (errno == ENOENT) continue;
            return FromErrno(errno ? errno : EIO, "The folder can't be moved");
          }
          ++out->folders;
          changed = true;
          continue;
        }
        auto made = target_folder->OpenChild(entry.name, target_folder->policy(), true, to.folder_mode);
        if (!made || !made->Sync()) return FromErrno(errno, "The folder can't be made");
        changed = true;
        stack->push_back({child, target});
      }
      if (changed && (!source_folder->Sync() || !target_folder->Sync())) return FromErrno(errno, "The change can't be saved");
    }
    if (same_root) {
      // Renames, a batch under one hold of the locks.
      for (std::size_t done = 0; done < files.size();) {
        std::lock_guard<std::mutex> commit(commit_mutex_);
        const auto held = Hold();
        if (!held) return Busy();
        std::optional<ManagedDirectory> source_folder, target_folder;
        if (auto error = Locate(from, frame.source, true, false, &source_folder); !error.ok()) return error;
        if (auto error = Locate(to, frame.target, true, false, &target_folder); !error.ok()) return error;
        for (const auto end = std::min(files.size(), done + kTreeBatch); done < end; ++done) {
          const auto error = RenameFile(*source_folder, files[done].name, *target_folder, files[done].target, files[done].replace);
          switch (Classify(error)) {
            case Outcome::kDone: ++out->files; break;
            case Outcome::kLeft: ++out->left; break;
            case Outcome::kGone: break;
            case Outcome::kStop: return error;
          }
        }
      }
      continue;
    }
    for (const auto& file : files) {
      auto source = frame.source, target = frame.target;
      source.push_back(file.name);
      target.push_back(file.target);
      tally->progress.name = file.name;
      if (!tally->Report()) return Stopped();
      const auto error = CopyFile(from, source, to, target, file.replace, tally);
      switch (Classify(error)) {
        case Outcome::kDone:
          ++out->files;
          ++tally->progress.files;
          if (!tally->Report()) return Stopped();
          break;
        case Outcome::kLeft: ++out->left; break;
        case Outcome::kGone: break;
        case Outcome::kStop: return error;
      }
    }
  }
  return kOk;
}

// In one root: a file and its previous version. With replace, the file there becomes the previous version, as an
// upload's Replace keeps it, and the moved file's own previous version goes. Hold both locks.
FileError FileStore::RenameFile(const ManagedDirectory& from, const std::string& name, const ManagedDirectory& to,
                                const std::string& target, bool replace) {
  const auto backup = kPreviousPrefix + name, target_backup = kPreviousPrefix + target;
  struct stat info{}, old{}, existing{}, there{};
  if (!from.InspectChild(name, &info)) return errno == ENOENT ? NotFound() : FromErrno(errno, "This folder can't be read");
  if (!S_ISREG(info.st_mode) || HardLinked(info)) return NotFound();
  if (from.policy() != DirectoryPolicy::kUnownedTree && info.st_uid != ::geteuid()) return FromErrno(EACCES, "");
  const bool has_backup = from.InspectChild(backup, &old) && S_ISREG(old.st_mode) && !HardLinked(old);
  const bool exists = to.InspectChild(target, &existing);
  if (!exists && errno != ENOENT) return FromErrno(errno, "This folder can't be read");
  if (exists && !replace) return FromErrno(EEXIST, "");
  if (exists && !S_ISREG(existing.st_mode)) return {409, "A folder with that name is already there."};
  if (exists && to.policy() != DirectoryPolicy::kUnownedTree && existing.st_uid != ::geteuid()) return FromErrno(EACCES, "");
  const bool target_has_backup = to.InspectChild(target_backup, &there);
  if (exists) {
    if (target_has_backup && (!Step("remove-backup") || !to.RemoveRegularFile(target_backup)))
      return FromErrno(errno ? errno : EIO, "The previous version can't be replaced");
    if (!Step("move-current") || !to.RenameChild(target, to, target_backup, false)) return FromErrno(errno ? errno : EIO, "The file can't be replaced");
    if (!to.Sync()) return {500, "The move stopped at \"sync\". Check this folder."};
  }
  if (!Step("move-rename") || !from.RenameChild(name, to, target, false)) {
    // The file there is its previous version now: the page lists it, and Restore brings it back.
    if (exists) return {500, "The move stopped at \"move\". Check this folder."};
    return FromErrno(errno ? errno : EIO, "The file can't be moved");
  }
  if (!from.Sync() || !to.Sync()) return {500, "The move stopped at \"sync\". Check this folder."};
  if (has_backup) {
    const bool carried = !exists && !target_has_backup;
    if (!Step("move-previous") || (carried ? !from.RenameChild(backup, to, target_backup, false) : !from.RemoveRegularFile(backup)))
      return {500, "The move stopped at \"previous version\". Check this folder."};
    if (!from.Sync() || !to.Sync()) return {500, "The move stopped at \"sync\". Check this folder."};
  }
  return kOk;
}

// A new case for a name, through a temporary name: a drive that ignores case takes the new name for the old one. The
// temporary carries the name, so the sweep finishes the rename if it stops between the two steps. Hold both locks.
FileError FileStore::CaseRename(const ManagedDirectory& folder, const std::string& name, const std::string& target) {
  const auto rename = [&](const std::string& from, const std::string& to) -> FileError {
    const auto id = RandomText();
    if (!id) return {500, "Playden couldn't make a temporary name."};
    const auto aside = kRestorePrefix + std::to_string(Now()) + "-" + *id + "." + to;
    if (!Step("rename-aside") || !folder.RenameChild(from, folder, aside, false)) return FromErrno(errno ? errno : EIO, "This can't be renamed");
    if (!folder.Sync()) return FromErrno(errno, "The change can't be saved");
    if (!Step("rename-back") || !folder.RenameChild(aside, folder, to, false) || !folder.Sync())
      return {500, "The rename stopped. Check this folder in a while."};
    return kOk;
  };
  if (auto error = rename(name, target); !error.ok()) return error;
  struct stat old{};
  if (folder.InspectChild(kPreviousPrefix + name, &old) && S_ISREG(old.st_mode)) return rename(kPreviousPrefix + name, kPreviousPrefix + target);
  return kOk;
}

// A temporary file in the folder with the bytes of fd, synced. Its name is in active_uploads_ until DropTemporary or
// the caller takes it out. Holds no lock while bytes move.
FileError FileStore::StageCopy(const FileRoot& to, const std::vector<std::string>& folder, int fd, std::uint64_t size,
                               std::string* temporary, std::string* folder_path, DirectoryPolicy* policy, MoveTally* tally) {
  int out_fd = -1;
  {
    const auto held = Hold();
    if (!held) return Busy();
    std::optional<ManagedDirectory> opened;
    if (auto error = Locate(to, folder, true, false, &opened); !error.ok()) return error;
    if (options_.free_space) {
      const auto free = options_.free_space(opened->path().string());
      if (free && size + kSpaceMargin > *free) return {507, "There is no room for this on that drive."};
    }
    const auto id = RandomText();
    if (!id) return {500, "Playden couldn't make a temporary name."};
    *temporary = kUploadPrefix + *id;
    out_fd = opened->CreateExclusiveFile(*temporary, to.file_mode);
    if (out_fd < 0) return FromErrno(errno, "The file can't be made");
    *folder_path = opened->path().string();
    *policy = opened->policy();
    std::lock_guard<std::mutex> lock(state_mutex_);
    active_uploads_.insert(*folder_path + "/" + *temporary);
  }
  FdCloser closer{out_fd};
  std::vector<char> buffer(kTransferBytes);
  std::uint64_t total = 0;
  while (true) {
    const auto count = ::read(fd, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      const int error = errno;
      DropTemporary(to, *folder_path, *temporary, *policy);
      return FromErrno(error, "The file can't be read");
    }
    if (count == 0) break;
    std::size_t written = 0;
    while (written < static_cast<std::size_t>(count)) {
      const auto wrote = ::write(out_fd, buffer.data() + written, static_cast<std::size_t>(count) - written);
      if (wrote < 0 && errno == EINTR) continue;
      if (wrote <= 0) {
        const int error = wrote < 0 ? errno : EIO;
        DropTemporary(to, *folder_path, *temporary, *policy);
        return FromErrno(error, "The file can't be written");
      }
      written += static_cast<std::size_t>(wrote);
    }
    total += static_cast<std::uint64_t>(count);
    tally->progress.bytes += static_cast<std::uint64_t>(count);
    if (!tally->Report()) {
      DropTemporary(to, *folder_path, *temporary, *policy);
      return Stopped();
    }
  }
  if (total != size) {
    DropTemporary(to, *folder_path, *temporary, *policy);
    return {409, "A file changed while it was moved. Try again."};
  }
  if (!Step("copy-sync") || ::fsync(out_fd) != 0) {
    const int error = errno ? errno : EIO;
    DropTemporary(to, *folder_path, *temporary, *policy);
    return FromErrno(error, "The file can't be saved");
  }
  return kOk;
}

void FileStore::DropTemporary(const FileRoot& root, const std::string& folder_path, const std::string& temporary, DirectoryPolicy policy) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    active_uploads_.erase(folder_path + "/" + temporary);
  }
  // Cleaned up now if the lock is free. A setup holds it for minutes: then at a later file request.
  const auto held = options_.try_hold ? options_.try_hold() : std::make_shared<int>(0);
  if (!held || !RemoveTemporary(folder_path, temporary, policy)) DeferCleanup(root, folder_path, temporary, policy);
}

// Across roots: the file (and its previous version) copied whole to the target, published as an upload is, and only
// then the source deleted, if it is still what was copied. A stop leaves the file whole on one side or both.
FileError FileStore::CopyFile(const FileRoot& from, const std::vector<std::string>& source, const FileRoot& to,
                              const std::vector<std::string>& target, bool replace, MoveTally* tally) {
  tally->progress.name = source.back();
  const std::vector<std::string> source_above(source.begin(), source.end() - 1), target_above(target.begin(), target.end() - 1);
  const auto source_path = Join(source, source.size());
  // One the console won't let this user delete would end up in both places.
  bool has_backup = false;
  {
    const auto held = Hold();
    if (!held) return Busy();
    std::optional<ManagedDirectory> folder;
    if (auto error = Locate(from, source_above, true, false, &folder); !error.ok()) return error;
    struct stat info{}, old{};
    if (!folder->InspectChild(source.back(), &info)) return errno == ENOENT ? NotFound() : FromErrno(errno, "This folder can't be read");
    if (!S_ISREG(info.st_mode) || HardLinked(info)) return NotFound();
    if (folder->policy() != DirectoryPolicy::kUnownedTree && info.st_uid != ::geteuid()) return FromErrno(EACCES, "");
    has_backup = folder->InspectChild(kPreviousPrefix + source.back(), &old) && S_ISREG(old.st_mode) && !HardLinked(old);
  }
  std::string temporary, folder_path;
  DirectoryPolicy policy;
  // Its previous version goes first, unless it replaces a file (that one becomes the previous version). A stop between
  // the two leaves it listed as Recoverable beside the source, and a second run with Replace puts the file beside it.
  if (has_backup && !replace) {
    OpenedFile previous;
    const auto opened = OpenForRead(from, source_path, true, &previous);
    if (!opened.ok() && opened.status != 404) return opened;
    if (opened.ok()) {
      FdCloser previous_closer{previous.fd};
      if (auto staged = StageCopy(to, target_above, previous.fd, previous.size, &temporary, &folder_path, &policy, tally); !staged.ok()) return staged;
      bool published = false;
      {
        std::lock_guard<std::mutex> commit(commit_mutex_);
        const auto held = Hold();
        std::optional<ManagedDirectory> folder;
        if (held && Locate(to, target_above, true, false, &folder).ok() && Step("copy-previous") &&
            folder->RenameChild(temporary, *folder, kPreviousPrefix + target.back(), false))
          published = folder->Sync();
      }
      if (!published) {
        DropTemporary(to, folder_path, temporary, policy);
        return {500, "The move stopped at \"previous version\". Nothing moved."};
      }
      std::lock_guard<std::mutex> lock(state_mutex_);
      active_uploads_.erase(folder_path + "/" + temporary);
    }
  }
  OpenedFile file;
  if (auto error = OpenForRead(from, source_path, false, &file); !error.ok()) return error;
  FdCloser closer{file.fd};
  struct stat copied{};
  if (::fstat(file.fd, &copied) != 0) return FromErrno(errno, "This file can't be read");
  if (auto error = StageCopy(to, target_above, file.fd, file.size, &temporary, &folder_path, &policy, tally); !error.ok()) return error;
  FileError error;
  bool changed_disk = false;
  {
    std::lock_guard<std::mutex> commit(commit_mutex_);
    const auto held = Hold();
    if (held) {
      UploadOptions options;
      options.length = file.size;
      options.overwrite = replace;
      changed_disk = CommitUpload(to, target, temporary, options, nullptr, nullptr, &error);
    } else {
      error = Busy();
    }
  }
  if (!changed_disk) {
    DropTemporary(to, folder_path, temporary, policy);
    return error;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    active_uploads_.erase(folder_path + "/" + temporary);
  }
  if (!error.ok()) return error;
  std::lock_guard<std::mutex> commit(commit_mutex_);
  const auto held = Hold();
  if (!held) return {503, "Playden is busy, so the file is in both places. Move it again in a moment."};
  std::optional<ManagedDirectory> folder;
  if (auto located = Locate(from, source_above, true, false, &folder); !located.ok()) return located.status == 404 ? kOk : located;
  struct stat now{}, old{};
  if (!folder->InspectChild(source.back(), &now)) return errno == ENOENT ? kOk : FromErrno(errno, "This folder can't be read");
  // To the nanosecond the drive keeps: a same-size save written in the same second still counts as changed.
  if (now.st_size != copied.st_size || now.st_mtimespec.tv_sec != copied.st_mtimespec.tv_sec ||
      now.st_mtimespec.tv_nsec != copied.st_mtimespec.tv_nsec)
    return {409, "\"" + source.back() + "\" changed while it was moved, so it is in both places."};
  // The previous version first: a stop leaves the file, not a file the player thought was gone.
  if (folder->InspectChild(kPreviousPrefix + source.back(), &old) && S_ISREG(old.st_mode) &&
      (!Step("move-delete-previous") || !folder->RemoveRegularFile(kPreviousPrefix + source.back())))
    return FromErrno(errno ? errno : EIO, "The moved file's previous version can't be deleted");
  if (!Step("move-delete-source") || !folder->RemoveRegularFile(source.back()))
    return {500, "The file is in both places: the old one can't be deleted (" + std::string(std::strerror(errno ? errno : EIO)) + ")."};
  if (!folder->Sync()) return FromErrno(errno, "The change can't be saved");
  return kOk;
}

// Steps: file -> temporary; previous -> file; sync; temporary -> previous; sync. A crash leaves the previous version
// listed as Recoverable, or the swap done with a temporary the sweep removes when it is old.
FileError FileStore::Restore(const FileRoot& root, const std::string& path) {
  auto parts = SplitRelativePath(path);
  if (!parts || parts->empty() || PrivatePath(*parts, root.private_paths)) return NotFound();
  if (ReadOnlyPath(*parts)) return {403, "Playden writes this folder itself, so it can't be changed here."};
  const auto name = parts->back();
  parts->pop_back();
  std::lock_guard<std::mutex> commit(commit_mutex_);
  const auto held = Hold();
  if (!held) return Busy();
  std::optional<ManagedDirectory> folder;
  if (auto error = Locate(root, *parts, true, false, &folder); !error.ok()) return error;
  const auto backup = kPreviousPrefix + name;
  struct stat old{}, current{};
  if (!folder->InspectChild(backup, &old) || !S_ISREG(old.st_mode) || HardLinked(old)) return {404, "There is no previous version of this file."};
  const bool exists = folder->InspectChild(name, &current);
  if (exists && !S_ISREG(current.st_mode)) return {409, "A folder with that name is already there."};
  if (exists && folder->policy() != DirectoryPolicy::kUnownedTree && current.st_uid != ::geteuid()) return FromErrno(EACCES, "");
  std::string swapped;
  if (exists) {
    const auto id = RandomText();
    if (!id) return {500, "Playden couldn't make a temporary name."};
    // The name is in here so the sweep can put the file back if the swap never finishes.
    swapped = kRestorePrefix + std::to_string(Now()) + "-" + *id + "." + name;
    if (!Step("restore-aside") || !folder->RenameChild(name, *folder, swapped, false))
      return FromErrno(errno ? errno : EIO, "The file can't be restored");
  }
  if (!Step("restore-back") || !folder->RenameChild(backup, *folder, name, false))
    return {500, "The restore stopped at \"back\". Check this folder."};
  if (!Step("restore-sync") || !folder->Sync()) return {500, "The restore stopped at \"sync\". Check this folder."};
  if (exists) {
    if (!Step("restore-keep") || !folder->RenameChild(swapped, *folder, backup, false))
      return {500, "The restore stopped at \"keep\". Check this folder."};
    if (!folder->Sync()) return {500, "The restore stopped at \"sync\". Check this folder."};
  }
  return kOk;
}

FileError FileStore::RedistFolders(const FileRoot& root, std::vector<RedistFolderInfo>* out) {
  out->clear();
  fs::path top_path;
  {
    const auto held = Hold();
    if (!held) return Busy();
    std::optional<ManagedDirectory> top;
    if (auto error = Locate(root, {}, false, false, &top); !error.ok()) return error;
    top_path = top->path();
  }
  // A game folder can be large, so this looks with no path lock: it only reads a folder open to all, and a look that
  // fails while root is raised elsewhere shows less until the page asks again.
  std::vector<std::string> packages;
  runtime::FindPhysxPackages(top_path, std::string(), 0, false, &packages);
  const auto add = [&](const std::string& relative, const std::string& absolute) {
    RedistFolderInfo info;
    info.path = relative;
    std::vector<RawEntry> raw;
    if (ReadFolder(absolute, &raw, kMaxListedEntries, nullptr))
      for (const auto& entry : raw) {
        if (HiddenName(entry.name) || !UsableName(entry.name)) continue;
        ++info.entries;
        if (S_ISREG(entry.info.st_mode)) info.bytes += static_cast<std::uint64_t>(entry.info.st_size);
        if (info.sample.size() < 3) info.sample.push_back(entry.name);
      }
    for (const auto& package : packages)
      if (package.rfind(relative + "/", 0) == 0) info.physx_installer = package;
    out->push_back(std::move(info));
  };
  std::vector<RawEntry> raw;
  if (!ReadFolder(top_path.string(), &raw, kMaxListedEntries, nullptr)) return FromErrno(errno, "This folder can't be listed");
  for (const auto& entry : raw) {
    if (!S_ISDIR(entry.info.st_mode) || HiddenName(entry.name) || !UsableName(entry.name)) continue;
    const auto absolute = (top_path / entry.name).string();
    if (runtime::RedistFolder(entry.name)) {
      add(entry.name, absolute);
      continue;
    }
    std::vector<RawEntry> below;
    if (!ReadFolder(absolute, &below, kMaxListedEntries, nullptr)) continue;
    for (const auto& child : below)
      if (S_ISDIR(child.info.st_mode) && !HiddenName(child.name) && UsableName(child.name) && runtime::RedistFolder(child.name))
        add(entry.name + "/" + child.name, absolute + "/" + child.name);
  }
  std::sort(out->begin(), out->end(), [](const RedistFolderInfo& left, const RedistFolderInfo& right) {
    return LowerAscii(left.path) < LowerAscii(right.path);
  });
  return kOk;
}

FileError FileStore::WriteZip(const FileRoot& root, const std::string& path, std::int64_t modified, const ByteWriter& write,
                              const std::function<bool()>& begin, bool* started) {
  *started = false;
  const auto parts = SplitRelativePath(path);
  if (!parts || PrivatePath(*parts, root.private_paths)) return NotFound();
  struct Item {
    std::vector<std::string> parts;  // relative to the root; the last is the file
    std::string zip_name;
  };
  std::vector<Item> items;
  std::uint64_t budget = 22;
  const std::string top = parts->empty() ? fs::path(root.path).filename().string() : parts->back();
  // Iterative, because worker stacks are small and a folder's depth is the player's. Each folder is opened under the
  // lock on its own, so a long walk doesn't keep root from being raised.
  std::vector<std::vector<std::string>> folders{*parts};
  while (!folders.empty()) {
    auto current = std::move(folders.back());
    folders.pop_back();
    std::vector<RawEntry> raw;
    {
      const auto held = Hold();
      if (!held) return Busy();
      std::optional<ManagedDirectory> folder;
      if (auto error = Locate(root, current, false, false, &folder); !error.ok()) {
        if (error.status == 404 && !current.empty() && current != *parts) continue;
        return error;
      }
      if (!ReadFolder(folder->path().string(), &raw, kMaxZipEntries + 1, nullptr)) return FromErrno(errno, "This folder can't be listed");
    }
    for (const auto& entry : raw) {
      if (HiddenName(entry.name) || !UsableName(entry.name) || HardLinked(entry.info)) continue;
      auto child = current;
      child.push_back(entry.name);
      if (PrivatePath(child, root.private_paths)) continue;
      if (S_ISDIR(entry.info.st_mode)) {
        if (child.size() >= kMaxZipDepth + parts->size()) return {413, "This folder goes too deep for a zip. Download smaller folders."};
        folders.push_back(std::move(child));
        continue;
      }
      std::string zip_name = top;
      for (std::size_t i = parts->size(); i < child.size(); ++i) zip_name += "/" + child[i];
      budget += StoredZip::EntryOverhead(zip_name.size()) + static_cast<std::uint64_t>(entry.info.st_size);
      if (items.size() >= kMaxZipEntries || budget > kMaxZipBytes)
        return {413, "Too large for a zip. Download smaller folders."};
      items.push_back({std::move(child), std::move(zip_name)});
    }
  }
  std::sort(items.begin(), items.end(), [](const Item& left, const Item& right) { return left.zip_name < right.zip_name; });
  if (!begin()) return kOk;
  *started = true;
  StoredZip zip(write, modified);
  std::vector<char> buffer(kTransferBytes);
  for (const auto& item : items) {
    OpenedFile file;
    if (const auto error = OpenForRead(root, Join(item.parts, item.parts.size()), false, &file); !error.ok()) {
      if (error.status == 404) continue;  // gone since the walk
      return error;
    }
    FdCloser closer{file.fd};
    if (!zip.BeginEntry(item.zip_name)) return {500, "The zip stopped."};
    while (true) {
      const auto count = ::read(file.fd, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) return {500, "The zip stopped."};
      if (count == 0) break;
      // Refused past the archive limit (a file grew after the walk): the stream ends with no final chunk.
      if (!zip.EntryData(buffer.data(), static_cast<std::size_t>(count))) return {500, "The zip stopped."};
    }
    if (!zip.EndEntry()) return {500, "The zip stopped."};
  }
  if (!zip.Finish()) return {500, "The zip stopped."};
  return kOk;
}

}  // namespace playden::web
