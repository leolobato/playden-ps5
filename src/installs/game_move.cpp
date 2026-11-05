// SPDX-License-Identifier: GPL-3.0-or-later
#include "installs/game_move.h"

#include "installs/install_record.h"
#include "steam/backend/depot_installer.h"
#include "platform/usb_mount.h"
#include "stores/local/copy_plan.h"
#include "stores/local/source_walk.h"

#include <mbedtls/sha1.h>

#include <algorithm>
#include <array>
#include <tuple>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace playden::installs {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;
using stores::local::CopyFile;
using stores::local::CopyPlan;
using stores::local::kCopyJournalDir;
using stores::local::SourceWalk;

constexpr const char* kFileList = steam::DepotInstaller::kFileList;

// GameMoveOptions::hold_effective_uid, taken around each ManagedDirectory call.
using UidHold = std::function<std::shared_ptr<void>()>;
std::shared_ptr<void> Held(const UidHold& hold) { return hold ? hold() : nullptr; }
constexpr const char* kSwitchFile = "switch";  // in the journal: kMoveSwitchName

bool Exists(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0;
}

bool Missing(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) != 0 && errno == ENOENT;
}

std::optional<std::uint64_t> FileSize(const std::string& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return std::nullopt;
  return info.st_size > 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
}

std::string Parent(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string Basename(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::pair<std::int64_t, std::int64_t> ModifiedTime(const struct stat& info) {
#if defined(__APPLE__)
  return {std::max<std::int64_t>(0, info.st_mtimespec.tv_sec), static_cast<std::int64_t>(info.st_mtimespec.tv_nsec)};
#else
  return {std::max<std::int64_t>(0, info.st_mtim.tv_sec), static_cast<std::int64_t>(info.st_mtim.tv_nsec)};
#endif
}

bool AsPlanned(const struct stat& info, const CopyFile& file) {
  const auto [seconds, nsec] = ModifiedTime(info);
  return S_ISREG(info.st_mode) && info.st_size >= 0 && static_cast<std::uint64_t>(info.st_size) == file.bytes &&
         seconds == file.mtime && nsec == file.mtime_nsec;
}

bool SyncFolder(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

// A folder's mode in the copy: the source folder's where the destination keeps modes (never without
// the owner's rights), else 0777 as on an exFAT drive.
mode_t FolderMode(const std::string& source, bool keep_modes) {
  struct stat info {};
  if (!keep_modes || ::lstat(source.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return 0777;
  return (info.st_mode & 0777) | 0700;
}

// The copy's folders through ManagedDirectory: the game folder, its journal, and the chain of the
// folder the current file goes in (created as needed), so files in one folder open it once.
class Destination {
 public:
  Destination(std::string from, std::string to, UidHold hold)
      : from_(std::move(from)), to_(std::move(to)), hold_(std::move(hold)) {}

  bool Open(DirectoryPolicy policy) {
    const auto held = Held(hold_);
    policy_ = policy;
    keep_modes_ = policy == DirectoryPolicy::kApprovedTree;
    auto games = ManagedDirectory::Open(Parent(to_), policy);
    if (!games) return false;
    root_ = games->OpenChild(Basename(to_), policy, true, FolderMode(from_, keep_modes_));
    if (!root_) return false;
    journal_ = root_->OpenChild(kCopyJournalDir, policy, true, 0777);
    return journal_.has_value();
  }
  const ManagedDirectory& root() const { return *root_; }
  const ManagedDirectory& journal() const { return *journal_; }
  bool keep_modes() const { return keep_modes_; }
  const UidHold& hold() const { return hold_; }

  // The folder `relative` ('/' separators; empty: the game folder), made where missing.
  const ManagedDirectory* Folder(const std::string& relative) {
    const auto held = Held(hold_);
    std::vector<std::string> parts;
    for (std::size_t start = 0; !relative.empty();) {
      const auto slash = relative.find('/', start);
      parts.push_back(relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
      if (slash == std::string::npos) break;
      start = slash + 1;
    }
    std::size_t common = 0;
    while (common < chain_.size() && common < parts.size() && chain_[common].first == parts[common]) ++common;
    chain_.erase(chain_.begin() + static_cast<std::ptrdiff_t>(common), chain_.end());
    std::string path;
    for (std::size_t index = 0; index < parts.size(); ++index) {
      path += (index ? "/" : "") + parts[index];
      if (index < common) continue;
      const auto& parent = chain_.empty() ? *root_ : chain_.back().second;
      auto child = parent.OpenChild(parts[index], policy_, true, FolderMode(from_ + "/" + path, keep_modes_));
      if (!child) return nullptr;
      chain_.emplace_back(parts[index], std::move(*child));
    }
    return chain_.empty() ? &*root_ : &chain_.back().second;
  }

 private:
  std::string from_, to_;
  UidHold hold_;
  DirectoryPolicy policy_ = DirectoryPolicy::kApprovedTree;
  bool keep_modes_ = true;
  std::optional<ManagedDirectory> root_, journal_;
  std::vector<std::pair<std::string, ManagedDirectory>> chain_;
};

enum class CopyStatus { kOk, kChanged, kCanceled, kReadFailed, kWriteFailed, kNoSpace, kMismatch };

bool WriteAll(int fd, const char* data, std::size_t size, int* error) {
  while (size > 0) {
    const auto written = ::write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR) continue;
      *error = errno;
      return false;
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

using Digest = std::array<unsigned char, 20>;

// Reads the rest of `fd` into the digest; the bytes read, or nothing on a read error.
std::optional<std::uint64_t> HashRest(int fd, mbedtls_sha1_context& context, std::vector<char>& buffer) {
  std::uint64_t total = 0;
  for (;;) {
    const auto count = ::read(fd, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (count == 0) return total;
    mbedtls_sha1_update(&context, reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<std::size_t>(count));
    total += static_cast<std::uint64_t>(count);
  }
}

// Copies the plan's file `file` through the journal's `<index>.part`: written, synced, read back and
// compared with what was read (size and SHA-1), then renamed to `name` in `folder`. The source must
// still be as planned (size and time) before and after.
CopyStatus CopyOne(SourceWalk& walk, const CopyFile& file, const std::string& part, const ManagedDirectory& journal,
                   const ManagedDirectory& folder, const std::string& name, bool keep_modes, std::vector<char>& buffer,
                   const std::atomic<bool>& cancel, const std::function<void(std::uint64_t)>& copied_bytes, const UidHold& hold,
                   int* error) {
  const int in = walk.OpenFile(file.path);
  if (in < 0) {
    *error = errno;
    if (*error == ENOENT || *error == ENOTDIR || *error == ELOOP || *error == EMLINK || *error == EISDIR)
      return CopyStatus::kChanged;
    return CopyStatus::kReadFailed;
  }
  struct stat before {};
  if (::fstat(in, &before) != 0) { *error = errno; ::close(in); return CopyStatus::kReadFailed; }
  if (!AsPlanned(before, file)) { ::close(in); return CopyStatus::kChanged; }
  // A part left by a stop before it was renamed goes first: parts are created exclusively.
  auto held = Held(hold);
  if (!journal.RemoveRegularFile(part) && errno != ENOENT) { *error = errno; ::close(in); return CopyStatus::kWriteFailed; }
  const int out = journal.CreateExclusiveFile(part, keep_modes ? (before.st_mode & 0777) | 0600 : 0666);
  held.reset();
  if (out < 0) {
    *error = errno;
    ::close(in);
    return *error == ENOSPC ? CopyStatus::kNoSpace : CopyStatus::kWriteFailed;
  }
  mbedtls_sha1_context read_hash;
  mbedtls_sha1_init(&read_hash);
  mbedtls_sha1_starts(&read_hash);
  const auto stop = [&](CopyStatus status, int fd) {
    mbedtls_sha1_free(&read_hash);
    ::close(in);
    if (fd >= 0) ::close(fd);
    const auto removing = Held(hold);
    journal.RemoveRegularFile(part);
    return status;
  };
  std::uint64_t copied = 0;
  for (;;) {
    if (cancel.load()) return stop(CopyStatus::kCanceled, out);
    const auto count = ::read(in, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) continue;
      *error = errno;
      return stop(CopyStatus::kReadFailed, out);
    }
    if (count == 0) break;
    if (copied + static_cast<std::uint64_t>(count) > file.bytes) return stop(CopyStatus::kChanged, out);  // it grew
    mbedtls_sha1_update(&read_hash, reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<std::size_t>(count));
    if (!WriteAll(out, buffer.data(), static_cast<std::size_t>(count), error))
      return stop(*error == ENOSPC ? CopyStatus::kNoSpace : CopyStatus::kWriteFailed, out);
    copied += static_cast<std::uint64_t>(count);
    copied_bytes(static_cast<std::uint64_t>(count));
  }
  struct stat after {};
  if (copied != file.bytes || ::fstat(in, &after) != 0 || !AsPlanned(after, file)) return stop(CopyStatus::kChanged, out);
  if (::fsync(out) != 0) {
    *error = errno;
    return stop(*error == ENOSPC ? CopyStatus::kNoSpace : CopyStatus::kWriteFailed, out);
  }
  if (::close(out) != 0) {
    *error = errno;
    return stop(CopyStatus::kWriteFailed, -1);
  }
  Digest expected{};
  mbedtls_sha1_finish(&read_hash, expected.data());
  // Read back from the drive and compared before it takes the file's name.
  held = Held(hold);
  const int check = journal.OpenRegularFile(part);
  held.reset();
  if (check < 0) { *error = errno; return stop(CopyStatus::kWriteFailed, -1); }
  mbedtls_sha1_context written_hash;
  mbedtls_sha1_init(&written_hash);
  mbedtls_sha1_starts(&written_hash);
  const auto read_back = HashRest(check, written_hash, buffer);
  Digest actual{};
  mbedtls_sha1_finish(&written_hash, actual.data());
  mbedtls_sha1_free(&written_hash);
  ::close(check);
  if (!read_back || *read_back != file.bytes || actual != expected) return stop(CopyStatus::kMismatch, -1);
  held = Held(hold);
  if (!journal.RenameChild(part, folder, name, true)) { held.reset(); *error = errno; return stop(CopyStatus::kWriteFailed, -1); }
  held.reset();
  mbedtls_sha1_free(&read_hash);
  ::close(in);
  return CopyStatus::kOk;
}

// The source as it is now (`fresh`) against the plan the copy followed (`old`): a file counts as copied
// when the source still has it as planned (size and time) and the copy has it at that size.
struct Comparison {
  std::vector<char> done;              // per file of `fresh`
  std::uint64_t done_bytes = 0;
  std::vector<std::string> gone;       // planned files the source no longer has
  bool changed = false;                // `fresh` is not the plan saved
};
// `checked` hears each file of `fresh` compared (how many, of how many).
Comparison Compare(const CopyPlan& old, const CopyPlan& fresh, const std::string& to,
                   const std::function<void(std::uint64_t, std::uint64_t)>& checked = {}) {
  Comparison result;
  std::unordered_map<std::string, const CopyFile*> planned;
  for (const auto& file : old.files) planned.emplace(file.path, &file);
  result.done.assign(fresh.files.size(), 0);
  for (std::size_t index = 0; index < fresh.files.size(); ++index) {
    const auto& file = fresh.files[index];
    const auto found = planned.find(file.path);
    const bool same = found != planned.end() && found->second->bytes == file.bytes && found->second->mtime == file.mtime &&
                      found->second->mtime_nsec == file.mtime_nsec;
    result.changed = result.changed || !same;
    const auto size = FileSize(to + "/" + file.path);
    if (same && size && *size == file.bytes) {
      result.done[index] = 1;
      result.done_bytes += file.bytes;
    }
    if (checked) checked(index + 1, fresh.files.size());
  }
  std::unordered_set<std::string> present;
  for (const auto& file : fresh.files) present.insert(file.path);
  for (const auto& file : old.files)
    if (!present.count(file.path)) result.gone.push_back(file.path);
  // Walks list folders in no set order.
  auto old_dirs = old.dirs, fresh_dirs = fresh.dirs;
  std::sort(old_dirs.begin(), old_dirs.end());
  std::sort(fresh_dirs.begin(), fresh_dirs.end());
  result.changed = result.changed || !result.gone.empty() || old_dirs != fresh_dirs;
  return result;
}

std::string FolderOf(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Before a new plan is saved, the copy's files it doesn't count as done go: a copy of an older version at
// the planned size would otherwise pass as done on the next resume. So do the copies of files the game
// deleted: only the old plan names them. Their folders are synced, so no stop brings them back under the
// new plan. False when one could not be removed.
bool RemoveStale(Destination& destination, const CopyPlan& fresh, const Comparison& compared, const std::string& to) {
  std::vector<std::string> paths = compared.gone;
  for (std::size_t index = 0; index < fresh.files.size(); ++index)
    if (!compared.done[index]) paths.push_back(fresh.files[index].path);
  std::unordered_set<std::string> folders;
  for (const auto& path : paths) {
    if (Missing(to + "/" + path)) continue;
    const auto held = Held(destination.hold());
    const auto* folder = destination.Folder(FolderOf(path));
    if (!folder || (!folder->RemoveRegularFile(Basename(path)) && errno != ENOENT)) return false;
    folders.insert(FolderOf(path));
  }
  for (const auto& folder : folders)
    if (!SyncFolder(folder.empty() ? to : to + "/" + folder)) return false;
  return true;
}

std::string PartName(std::size_t index) { return std::to_string(index) + ".part"; }
std::string PartName(const char* name) { return std::string(name) + ".part"; }

}  // namespace

DirectoryPolicy GamesFolderPolicy(const std::string& games_root) {
  const auto records = platform::RecordsOwnership(games_root);
  return records && !*records ? DirectoryPolicy::kUnownedTree : DirectoryPolicy::kApprovedTree;
}

GameMover::GameMover(GameMoveOptions options) : options_(std::move(options)) {
  if (options_.buffer_bytes == 0) options_.buffer_bytes = 1024 * 1024;
}

void GameMover::Log(const std::string& line) const {
  if (options_.log) options_.log(line);
}

bool GameMover::Reached(MoveCheckpoint checkpoint) const {
  return !options_.checkpoint || options_.checkpoint(checkpoint);
}

InstallRunner::Result GameMover::Copy(const GameId& id, const std::string& from, const std::string& to,
                                      const std::string& volume_id, const std::atomic<bool>& cancel,
                                      const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                                      const std::function<void(InstallStep)>& step,
                                      const std::function<void(std::uint64_t, std::uint64_t)>& checked) {
  InstallRunner::Result result;
  const auto key = ToKey(id);
  const auto fail = [&](const std::string& failure, const std::string& why) {
    Log("move failed game=" + key + " to=" + to + ": " + why);
    result.failure = failure;
    return result;
  };
  const auto space_of = [this](const std::string& path) {
    return options_.space ? options_.space(path) : steam::QueryVolumeSpace(path);
  };
  const auto room_for = [&](std::uint64_t bytes, const std::string& path) -> std::string {
    const auto space = space_of(path);
    if (!space) return options_.space || steam::VolumeSpaceSupported() ? "Free space is unavailable. Check the storage and try again." : "";
    if (space->free_bytes < steam::kInstallSpaceMargin || bytes > space->free_bytes - steam::kInstallSpaceMargin)
      return "Not enough free space for the game. Make room, then Resume.";
    return {};
  };
  if (from == to) return fail("The game is already on this drive.", "the copy is the game's folder");
  if (ReadUpdateState(from)) return fail("Finish or cancel the game's update first.", "an update is in progress");
  if (Missing(from + "/" + kInstallRecordName)) return fail("The game's folder is missing.", from + " has no install record");

  // Probed once: it writes a file in the games folder, so root must not rise meanwhile.
  const auto policy = [&] {
    const auto held = Held(options_.hold_effective_uid);
    return GamesFolderPolicy(Parent(to));
  }();
  // The plan and file list are written with plain file calls: held too, so they are never root's.
  const auto save_plan = [&](const CopyPlan& saving) {
    const auto held = Held(options_.hold_effective_uid);
    return stores::local::SaveCopyPlan(saving, to);
  };
  const bool switching = Exists(to + "/" + kMoveSwitchName);
  const auto saved = stores::local::LoadCopyPlan(to);
  if (!saved) {
    if (switching) return fail("The move's copy is damaged. Check the storage, then Resume.", "switch marker without a plan");
    if (!Missing(to)) {
      // A copy of ours stopped before its plan was saved goes; anything else stays and refuses.
      std::string why;
      if (!Exists(to + "/" + kCopyJournalDir) || Exists(to + "/" + kInstallRecordName) ||
          !steam::RemoveManagedInstall(to, &why) || !Missing(to))
        return fail("A folder on the destination is in the way of the move.", to + " is in the way " + why);
    }
  } else if (platform::WithoutUsbMountNumber(saved->source) != platform::WithoutUsbMountNumber(from)) {
    // (Drives may have changed places since the plan was saved: that's still a copy of this folder.)
    // A partial copy of another folder goes with Cancel; one with an install record is an install.
    if (Exists(to + "/" + kInstallRecordName))
      return fail("A folder on the destination is in the way of the move.", to + " has a record, plan source " + saved->source);
    return fail("The move's copy is of another folder. Cancel it, then move the game again.", "plan source " + saved->source);
  }
  // Stopped at the switch (the copy was whole and checked when its marker was written): the title may already
  // play from the copy, and if it doesn't, the switch checks the copy against the game again with the title
  // stopped. Walking the game and comparing it first would only do that twice. The copy is still checked
  // (every planned file at its size): one that lost files since goes the whole way again.
  bool at_switch = switching && saved &&
                   platform::WithoutUsbMountNumber(saved->source) == platform::WithoutUsbMountNumber(from);
  if (at_switch) {
    if (step) step(InstallStep::kVerifying);
    for (std::size_t index = 0; index < saved->files.size(); ++index) {
      const auto& file = saved->files[index];
      const auto size = FileSize(to + "/" + file.path);
      if (!size || *size != file.bytes) {
        Log("move copy changed at the switch game=" + key + " path=" + file.path);
        at_switch = false;
        break;
      }
      if (checked) checked(index + 1, saved->files.size());
    }
  }
  CopyPlan plan;
  Comparison state;
  if (at_switch) {
    plan = *saved;
    state.done.assign(plan.files.size(), 1);
    state.done_bytes = plan.total;
    Log("move resumes at the switch game=" + key + " to=" + to);
  } else {
    // The source as it is now: a resume keeps what is copied of files the game hasn't changed since.
    auto build = stores::local::BuildCopyPlan(from);
    if (!build.ok) return fail(build.failure, "plan errno=" + std::to_string(build.error));
    build.plan.source = from;
    plan = std::move(build.plan);
  }
  if (!at_switch && !saved) {
    if (const auto refused = room_for(plan.total, Parent(to)); !refused.empty())
      return fail(refused, "space for " + std::to_string(plan.total) + " bytes");
    Destination made(from, to, options_.hold_effective_uid);
    if (!made.Open(policy))
      return fail("Could not write to the destination.", "open errno=" + std::to_string(errno));
    if (const auto written = save_plan(plan); !written.empty()) {
      steam::RemoveManagedInstall(to);
      return fail(written, "plan not saved");
    }
    state.done.assign(plan.files.size(), 0);
    Log("move planned game=" + key + " from=" + from + " to=" + to + " files=" + std::to_string(plan.files.size()) +
        " bytes=" + std::to_string(plan.total));
  } else if (!at_switch) {
    state = Compare(*saved, plan, to);
    if (state.changed) {
      Destination stale(from, to, options_.hold_effective_uid);
      if (!stale.Open(policy) || !RemoveStale(stale, plan, state, to))
        return fail("Could not write the game files. Check the drive, then Resume.", "stale copies errno=" + std::to_string(errno));
      if (const auto written = save_plan(plan); !written.empty()) return fail(written, "plan not saved");
    }
  }
  if (progress) progress(state.done_bytes, plan.total, state.done_bytes);
  if (const auto refused = at_switch ? std::string() : room_for(plan.total - state.done_bytes, to); !refused.empty())
    return fail(refused, "space for " + std::to_string(plan.total - state.done_bytes) + " bytes");

  Destination destination(from, to, options_.hold_effective_uid);
  if (!destination.Open(policy)) return fail("Could not write to the destination.", "open errno=" + std::to_string(errno));
  Log("move started game=" + key + " to=" + to + " policy=" + (policy == DirectoryPolicy::kUnownedTree ? "unowned" : "owned") +
      " resumed_bytes=" + std::to_string(state.done_bytes));
  // Folders first (the files the game deleted went before the plan was saved).
  const auto prepare = [&] {
    for (const auto& dir : plan.dirs)
      if (!destination.Folder(dir)) return false;
    return true;
  };
  if (!at_switch && !prepare()) return fail("Could not write the game files.", "folders errno=" + std::to_string(errno));

  if (step && !at_switch) step(InstallStep::kCopying);
  std::vector<char> buffer(options_.buffer_bytes);
  SourceWalk walk(from);
  std::uint64_t copied = state.done_bytes;
  const auto on_bytes = [&](std::uint64_t count) {
    copied += count;
    if (progress) progress(copied, plan.total, copied);
  };
  // Copies one planned file into place.
  const auto copy_file = [&](const CopyFile& file, const std::string& part, const std::atomic<bool>& stop,
                             const std::function<void(std::uint64_t)>& counted) {
    const auto* folder = destination.Folder(FolderOf(file.path));
    if (!folder) return CopyStatus::kWriteFailed;  // its folder could not be made
    int error = 0;
    const auto status = CopyOne(walk, file, part, destination.journal(), *folder, Basename(file.path),
                                destination.keep_modes(), buffer, stop, counted, options_.hold_effective_uid, &error);
    if (status != CopyStatus::kOk && status != CopyStatus::kCanceled)
      Log("move copy stopped game=" + key + " path=" + file.path + " status=" + std::to_string(static_cast<int>(status)) +
          " errno=" + std::to_string(error));
    if (status == CopyStatus::kOk && options_.after_file) options_.after_file(file.path);
    return status;
  };
  const auto words = [](CopyStatus status) -> std::string {
    switch (status) {
      case CopyStatus::kChanged: return "The game's files changed while copying. Resume to copy them again.";
      case CopyStatus::kReadFailed: return "Could not read the game's files. Check the drive, then Resume.";
      case CopyStatus::kNoSpace: return "Not enough free space for the game. Make room, then Resume.";
      case CopyStatus::kMismatch: return "A copied file didn't match the game's. Check the drive, then Resume.";
      default: return "Could not write the game files. Check the drive, then Resume.";
    }
  };
  for (std::size_t index = 0; index < plan.files.size(); ++index) {
    if (state.done[index]) continue;
    if (cancel.load()) {
      result.canceled = true;
      return result;
    }
    const auto& file = plan.files[index];
    const auto before = copied;
    const auto status = copy_file(file, PartName(index), cancel, on_bytes);
    if (status != CopyStatus::kOk && copied != before) {
      copied = before;  // the file's bytes went with its part
      if (progress) progress(copied, plan.total, copied);
    }
    if (status == CopyStatus::kCanceled) {
      result.canceled = true;
      return result;
    }
    if (status != CopyStatus::kOk) {
      result.failure = words(status);
      return result;
    }
  }

  if (!at_switch) {
    if (step) step(InstallStep::kVerifying);
    for (std::size_t index = 0; index < plan.files.size(); ++index) {
      const auto& file = plan.files[index];
      const auto size = FileSize(to + "/" + file.path);
      if (!size || *size != file.bytes) return fail("Some copied files are missing. Resume to copy them again.", "verify " + file.path);
      if (checked) checked(index + 1, plan.files.size());
    }
  }
  if (!Reached(MoveCheckpoint::kCopied)) {
    result.canceled = true;
    return result;
  }

  // From here the title may follow the copy: the marker keeps it from being deleted as a partial copy.
  if (!switching) {
    const auto held = Held(options_.hold_effective_uid);
    const int marker = destination.journal().CreateExclusiveFile(kSwitchFile, 0666);
    const bool marked = marker >= 0 && ::fsync(marker) == 0;
    if (marker >= 0) ::close(marker);
    if (!marked || !destination.journal().Sync()) return fail("Could not write the game files.", "switch marker errno=" + std::to_string(errno));
  }
  if (!Reached(MoveCheckpoint::kSwitchMarked)) {
    result.canceled = true;
    return result;
  }
  if (step) step(InstallStep::kSwitching);
  const auto unmark = [&] {
    const auto held = Held(options_.hold_effective_uid);
    destination.journal().RemoveRegularFile(kSwitchFile);
    destination.journal().Sync();
  };

  GameRelocation relocation;
  relocation.game = id;
  relocation.to = to;
  relocation.volume_id = volume_id;
  // With the title stopped: files the game changed or added since they were copied are copied again,
  // files it deleted go from the copy, then the file list and the install record (last) follow.
  relocation.move_files = [&]() -> std::string {
    const auto started = std::chrono::steady_clock::now();
    const auto ms = [&] {
      return std::to_string(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    };
    auto rebuilt = stores::local::BuildCopyPlan(from);
    if (!rebuilt.ok) {
      Log("move recheck failed game=" + key + " errno=" + std::to_string(rebuilt.error) + ": " + rebuilt.failure);
      return rebuilt.failure;
    }
    rebuilt.plan.source = from;
    Log("move rechecks game=" + key + " files=" + std::to_string(rebuilt.plan.files.size()) + " walk_ms=" + ms());
    const auto now = Compare(plan, rebuilt.plan, to, checked);
    if (now.changed) {
      if (!RemoveStale(destination, rebuilt.plan, now, to)) return "Could not write the game files. Check the drive, then Resume.";
      if (const auto written = save_plan(rebuilt.plan); !written.empty()) return written;
      plan = std::move(rebuilt.plan);
    }
    if (!prepare()) return "Could not write the game files. Check the drive, then Resume.";
    const std::atomic<bool> never{false};
    for (std::size_t index = 0; index < plan.files.size(); ++index) {
      if (now.done[index]) continue;
      const auto& file = plan.files[index];
      Log("move copies again game=" + key + " path=" + file.path + " bytes=" + std::to_string(file.bytes));
      const auto status = copy_file(file, PartName(index), never, [](std::uint64_t) {});
      if (status != CopyStatus::kOk) return words(status);
    }
    // Playden's own files, which the plan leaves out: the file list, then the record.
    for (const char* name : {kFileList, kInstallRecordName}) {
      struct stat info {};
      if (::lstat((from + "/" + name).c_str(), &info) != 0) {
        if (errno == ENOENT && std::string(name) == kFileList) continue;
        Log("move marker missing game=" + key + " name=" + name + " errno=" + std::to_string(errno));
        return "The game's install record is missing. Check the drive, then Resume.";
      }
      CopyFile own;
      own.path = name;
      own.bytes = info.st_size > 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
      std::tie(own.mtime, own.mtime_nsec) = ModifiedTime(info);
      const auto status = copy_file(own, PartName(name), never, [](std::uint64_t) {});
      if (status != CopyStatus::kOk) return words(status);
    }
    // The copy's folders on the storage before the title's record names it.
    bool synced = SyncFolder(to);
    for (const auto& dir : plan.dirs) synced = SyncFolder(to + "/" + dir) && synced;
    if (!synced) return "The move could not be saved on the storage. Resume to try again.";
    Log("move rechecked game=" + key + " ms=" + ms());
    return {};
  };
  GameRelocationResult relocated;
  if (options_.relocate) {
    relocated = options_.relocate(relocation);
  } else {
    relocated.message = relocation.move_files();
    if (!relocated.message.empty()) relocated.outcome = GameRelocationResult::Outcome::kFailed;
  }
  if (relocated.outcome == GameRelocationResult::Outcome::kBusy) {
    // Nothing changed: the game still plays from its old folder.
    unmark();
    Log("move waits game=" + key + ": " + relocated.message);
    result.retry_later = true;
    result.waiting = relocated.message.empty() ? std::string(InstallRunner::kWaitingForGame) : relocated.message;
    return result;
  }
  if (relocated.outcome == GameRelocationResult::Outcome::kFailed) {
    unmark();
    return fail(relocated.message.empty() ? "The game's PS5 title could not follow the move." : relocated.message,
                "relocation " + relocated.message);
  }
  // The copy's record goes last: without it the copy isn't whole, and the old folder must stay.
  if (Missing(to + "/" + kInstallRecordName))
    return fail("The game's install record is missing from the copy. Check the drive, then Resume.", "no record in the copy");
  Log("move switched game=" + key + " to=" + to + (relocated.message.empty() ? "" : " note=" + relocated.message));
  if (!Reached(MoveCheckpoint::kSwitched)) {
    result.canceled = true;
    return result;
  }
  const auto record = ReadInstallRecord(to + "/" + kInstallRecordName);
  result.ok = true;
  result.install_bytes = record && record->install_bytes > 0 ? record->install_bytes : plan.total;
  return result;
}

std::string GameMover::Finish(const std::string& from, const std::string& to) {
  if (!Missing(from)) {
    std::string why;
    if (!steam::RemoveManagedInstall(from, &why)) {
      Log("move left the old folder from=" + from + ": " + why);
      return "Playden couldn't delete the game's old folder. Resume to try again.";
    }
    Log("move deleted the old folder from=" + from);
  }
  if (!Reached(MoveCheckpoint::kOldDeleted)) return "The move stopped. Resume to finish it.";
  // The journal: parts left by a stop, the plan and the switch marker.
  const auto journal_path = to + "/" + kCopyJournalDir;
  if (Missing(journal_path)) return {};
  const auto held = Held(options_.hold_effective_uid);
  const auto policy = GamesFolderPolicy(Parent(to));
  auto root = ManagedDirectory::Open(to, policy);
  auto journal = root ? root->OpenChild(kCopyJournalDir, policy) : std::nullopt;
  std::vector<std::string> names;
  if (DIR* folder = journal ? ::opendir(journal_path.c_str()) : nullptr) {
    while (const auto* entry = ::readdir(folder)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(folder);
  }
  bool removed = journal.has_value();
  for (const auto& name : names) removed = journal->RemoveRegularFile(name) && removed;
  removed = removed && root->RemoveEmptyDirectory(kCopyJournalDir);
  if (!removed) Log("move left its journal at=" + journal_path + " errno=" + std::to_string(errno));
  return {};
}

std::string GameMover::Discard(const std::string& from, const std::string& to) {
  if (Exists(to + "/" + kMoveSwitchName)) return "The game's PS5 title may already use the copy.";
  if (Missing(to)) return {};
  // Only a partial copy goes: a journal and no install record (copied last, after the switch marker), or
  // a record this move copied before its title refused (the plan is of `from`). A folder that was in the
  // way, an install of its own included, stays.
  const auto plan = Exists(to + "/" + kCopyJournalDir) ? stores::local::LoadCopyPlan(to) : std::nullopt;
  if (!Exists(to + "/" + kCopyJournalDir) ||
      (Exists(to + "/" + kInstallRecordName) && !(plan && plan->source == from))) {
    Log("move kept a folder it didn't make at=" + to);
    return "The folder on the destination isn't the move's copy.";
  }
  std::string failure;
  const bool removed = steam::RemoveManagedInstall(to, &failure);
  Log("move discarded the copy at=" + to + (removed ? "" : ": " + failure));
  return failure;
}

}  // namespace playden::installs
