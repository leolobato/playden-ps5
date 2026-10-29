// writable_folders.cpp - Checks (and fixes) the folders Playden writes into before it writes.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "runtime/writable_folders.h"
#include "platform/directory_walk.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace playden::runtime {
namespace {
// access(2) is refused in the PS5 sandbox: create and remove a file instead. 0 or the errno.
int Probe(const std::filesystem::path& folder) {
  static std::atomic<unsigned long long> sequence{0};
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    const auto probe = folder / (".playden-writable-" + std::to_string(getpid()) + "-" +
                                 std::to_string(sequence.fetch_add(1)));
    const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return errno;
    }
    const bool closed = close(fd) == 0;
    if (unlink(probe.c_str()) != 0) return errno;
    return closed ? 0 : EIO;
  }
  return EEXIST;
}

mode_t RepairMode(const std::filesystem::path& folder, const std::filesystem::path& namespace_root) {
  const auto relative = folder.lexically_relative(namespace_root).generic_string();
  if (relative == "data/playden" || relative == "data/homebrew") return 01777;
  if (relative == "data/playden-games" || relative == "data/playden-import") return 0777;
  for (const auto* volume : {"ext0", "ext1", "usb0", "usb1"})
    for (const auto* folder : {"/playden/games", "/playden/import"})
      if (relative == std::string("mnt/") + volume + folder) return 0777;
  return 0;
}

std::string Error(int error) { return std::to_string(error) + " (" + std::strerror(error) + ")"; }
}  // namespace

std::string EnsureWritableFolders(const std::vector<std::filesystem::path>& folders,
                                  const WritableFoldersAsRoot& as_root,
                                  const std::function<void(const std::string&)>& log,
                                  const std::filesystem::path& namespace_root) {
  const auto note = [&log](const std::string& line) { if (log) log(line); };
  for (const auto& folder : folders) {
    if (folder.empty()) continue;
    struct stat info{};
    // Missing folders are made by their owners; links are never followed.
    if (lstat(folder.c_str(), &info) != 0) {
      if (errno != ENOENT) note("writable folder skipped path=" + folder.string() + " lstat=" + Error(errno));
      continue;
    }
    if (!S_ISDIR(info.st_mode)) continue;
    const int first = Probe(folder);
    const mode_t mode = RepairMode(folder, namespace_root);
    // The sticky bit is not compared: the PS5's /data drops it.
    if (first == 0 && (!mode || (info.st_mode & 0777) == (mode & 0777))) continue;
    int fix_error = 0;
    bool ran = false;
    const auto repair = [&] {
      ran = true;
      // By descriptor: a link swapped in after the lstat is refused, never followed as root.
      const int fd = platform::OpenDirectoryWalk(folder);
      if (fd < 0) { fix_error = errno; return false; }
      struct stat current{};
      bool ok = fstat(fd, &current) == 0 || (fix_error = errno, false);
      if (ok && !S_ISDIR(current.st_mode)) ok = (fix_error = ENOTDIR, false);
      if (ok && !mode && current.st_uid != geteuid()) ok = (fix_error = EPERM, false);
      if (ok && fchmod(fd, mode ? mode : 0777) != 0) ok = (fix_error = errno, false);
      close(fd);
      return ok;
    };
    // Arbitrary game/working folders may only be repaired by their own app user.
    const bool fixed = mode ? as_root && as_root(repair) : repair();
    const int second = fixed ? Probe(folder) : first;
    if (fixed && second == 0) {
      char before[8], after[8];
      std::snprintf(before, sizeof(before), "%04o", unsigned(info.st_mode & 07777));
      std::snprintf(after, sizeof(after), "%04o", unsigned(mode ? mode : 0777));
      note("writable folder fixed path=" + folder.string() + " mode=" + before + " -> " + after + " uid=" +
           std::to_string(info.st_uid) + " probe=" + Error(first));
      continue;
    }
    note("writable folder failed path=" + folder.string() + " uid=" + std::to_string(info.st_uid) +
         " probe=" + Error(first) +
         (!mode ? " fix=outside-approved-roots" : !as_root ? " fix=unavailable"
                   : !ran ? " fix=root-refused"
                          : fixed ? " after-fix=" + Error(second) : " chmod=" + Error(fix_error)));
    return "Playden can't write to " + folder.string() +
           ". Make that folder writable over FTP (777 on the folder only, not its contents), then try again.";
  }
  return {};
}

std::string PrepareRuntimeRoot(const std::filesystem::path& root, const WritableFoldersAsRoot& as_root,
                               const std::function<void(const std::string&)>& log,
                               const std::filesystem::path& namespace_root) {
  if (root != namespace_root / "data/playden") return "Playden can't fix this runtime folder.";
  // The step that failed and its errno, for the startup screen: the log may not be writable yet.
  std::string failed = "root access";
  int failed_error = 0;
  std::vector<std::string> notes;
  const bool prepared = as_root && as_root([&] {
    const auto fail = [&](const char* step) { failed = step; failed_error = errno; return false; };
    const int parent = platform::OpenDirectoryWalk(root.parent_path());
    if (parent < 0) return fail("open /data");
    int fd = platform::OpenWalkChild(parent, "playden", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT &&
        (platform::MakeWalkChild(parent, "playden", 01777) == 0 || errno == EEXIST))
      fd = platform::OpenWalkChild(parent, "playden", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    bool ok = fd >= 0 || fail("open the folder");
    // Sticky when the filesystem keeps it; the PS5's /data reports 0777 for 1777, which still works.
    if (ok && ::fchmod(fd, 01777) != 0) {
      notes.push_back("sticky chmod=" + Error(errno));
      if (::fchmod(fd, 0777) != 0) ok = fail("chmod");
    }
    // A mode change that isn't on disk yet is redone at the next start: not worth refusing to start.
    if (ok && ::fsync(fd) != 0) notes.push_back("fsync=" + Error(errno));
    if (ok && ::fsync(parent) != 0) notes.push_back("parent fsync=" + Error(errno));
    if (fd >= 0) ::close(fd);
    ::close(parent);
    return ok;
  });
  if (!prepared && failed == "root access") failed_error = as_root ? errno : ENOSYS;
  if (log) for (const auto& line : notes) log("runtime root repair path=" + root.string() + " " + line);
  if (prepared) return {};
  const std::string detail = failed + " failed" + (failed_error ? " error=" + Error(failed_error) : "");
  if (log) log("runtime root repair failed path=" + root.string() + " " + detail);
  return "Playden can't write to " + root.string() + " (" + detail +
         "). Make that folder writable over FTP (777 on the folder only, not its contents), then try again.";
}

std::string PrepareDropFolder(const std::filesystem::path& folder, const WritableFoldersAsRoot& as_root,
                              const std::function<void(const std::string&)>& log,
                              const std::filesystem::path& namespace_root) {
  if (folder != namespace_root / "data/playden-import") {
    if (log) log("drop folder refused path=" + folder.string());
    return "Playden can't make this import folder.";
  }
  std::string failed = "root access";
  int failed_error = 0;
  bool made = false;
  const bool prepared = as_root && as_root([&] {
    const auto fail = [&](const char* step) { failed = step; failed_error = errno; return false; };
    const int parent = platform::OpenDirectoryWalk(folder.parent_path());
    if (parent < 0) return fail("open /data");
    constexpr int kFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int fd = platform::OpenWalkChild(parent, "playden-import", kFlags);
    if (fd < 0 && errno == ENOENT) {
      made = platform::MakeWalkChild(parent, "playden-import", 0777) == 0;
      if (made || errno == EEXIST) fd = platform::OpenWalkChild(parent, "playden-import", kFlags);
    }
    // A link or a file in its place is left alone (O_NOFOLLOW, O_DIRECTORY).
    bool ok = fd >= 0 || fail("open the folder");
    // 0777 without the sticky bit, like /data/playden-games (whose /data drops it anyway): FTP
    // writes games here as root, Playden opens them in place, and "Move into Playden" renames
    // folders out of here whoever made them. mkdir is umasked, and an FTP-made folder is 0755.
    struct stat info{};
    if (ok && ::fstat(fd, &info) != 0) ok = fail("stat");
    if (ok && (info.st_mode & 07777) != 0777 && ::fchmod(fd, 0777) != 0) ok = fail("chmod");
    if (ok && made && ::fsync(parent) != 0 && log) log("drop folder path=" + folder.string() + " parent fsync=" + Error(errno));
    if (fd >= 0) ::close(fd);
    ::close(parent);
    return ok;
  });
  if (!prepared && failed == "root access") failed_error = as_root ? errno : ENOSYS;
  if (prepared) {
    if (made && log) log("drop folder made path=" + folder.string());
    return {};
  }
  const std::string detail = failed + " failed" + (failed_error ? " error=" + Error(failed_error) : "");
  if (log) log("drop folder failed path=" + folder.string() + " " + detail);
  return "Playden can't make " + folder.string() + " (" + detail + "). Games can still be imported from a drive.";
}

std::string PrepareAppFolderStubs(const std::filesystem::path& app_root, const WritableFoldersAsRoot& as_root,
                                  const std::function<void(const std::string&)>& log,
                                  const std::filesystem::path& namespace_root) {
  const auto name = app_root.filename().string();
  if (app_root.parent_path() != namespace_root / "data/homebrew" || name.empty() || name == "." || name == "..") {
    if (log) log("app folder stubs refused path=" + app_root.string());
    return "Playden can't repair this app folder.";
  }
  std::string failed = "root access", stub;
  int failed_error = 0;
  std::vector<std::string> made;
  const bool prepared = as_root && as_root([&] {
    const auto fail = [&](const char* step) { failed = step; failed_error = errno; return false; };
    constexpr int kFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    bool ok = true;
    for (const char* child : {"playden-data", "logs"}) {
      // The app folder is opened again for each stub: the by-path walk reads the parent's listing,
      // whose offset a second lookup through the same descriptor would continue from.
      const int parent = platform::OpenDirectoryWalk(app_root);
      if (parent < 0) return fail("open the app folder");
      stub = child;
      int fd = platform::OpenWalkChild(parent, child, kFlags);
      bool created = false;
      if (fd < 0 && errno == ENOENT) {
        created = platform::MakeWalkChild(parent, child, 0777) == 0;
        if (created || errno == EEXIST) fd = platform::OpenWalkChild(parent, child, kFlags);
      }
      // A link or a file in its place is left alone (O_NOFOLLOW, O_DIRECTORY).
      ok = fd >= 0 || fail("open the folder");
      // mkdir is umasked: the app user writes the first log lines. An existing folder keeps its
      // mode, since before the layout move it can still hold the user's data.
      if (ok && created && ::fchmod(fd, 0777) != 0) ok = fail("chmod");
      if (fd >= 0) ::close(fd);
      if (ok && created && ::fsync(parent) != 0 && log)
        log("app folder stubs path=" + app_root.string() + " parent fsync=" + Error(errno));
      ::close(parent);
      if (!ok) break;
      if (created) made.push_back(child);
    }
    return ok;
  });
  if (!prepared && failed == "root access") failed_error = as_root ? errno : ENOSYS;
  if (log) for (const auto& child : made) log("app folder stub made path=" + (app_root / child).string());
  if (prepared) return {};
  const std::string detail = failed + " failed" + (failed_error ? " error=" + Error(failed_error) : "");
  const auto where = stub.empty() ? app_root : app_root / stub;
  if (log) log("app folder stub failed path=" + where.string() + " " + detail);
  return "Playden can't make " + where.string() + " (" + detail + ").";
}

}  // namespace playden::runtime
