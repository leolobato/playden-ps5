// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/publisher_pack_install.h"

#include "platform/managed_directory.h"
#include "runtime/artifact_hash.h"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;

constexpr mode_t kFolderMode = 0755;
constexpr mode_t kFileMode = 0644;
constexpr std::size_t kCopyBytes = 256 << 10;

bool Fail(std::string* reason, std::string text) {
  if (reason) *reason = std::move(text);
  return false;
}

bool Lstat(const std::string& path, struct stat* info) { return ::lstat(path.c_str(), info) == 0; }

bool IsFolder(const std::string& path) {
  struct stat info {};
  return Lstat(path, &info) && S_ISDIR(info.st_mode);
}

bool IsRegular(const std::string& path) {
  struct stat info {};
  return Lstat(path, &info) && S_ISREG(info.st_mode);
}

std::vector<std::string> List(const std::string& path) {
  std::vector<std::string> names;
  if (DIR* dir = ::opendir(path.c_str())) {
    while (const dirent* entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(dir);
  }
  return names;
}

// lstat/unlink/rmdir by absolute path, never following a link: std::filesystem::remove_all crashes the console
// title (results/20261007-game-removal), and the title can't walk by descriptor. True when nothing is left.
bool RemoveTree(const std::string& path) {
  struct stat info {};
  if (!Lstat(path, &info)) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return ::unlink(path.c_str()) == 0;
  bool ok = true;
  for (const auto& name : List(path)) ok = RemoveTree(path + "/" + name) && ok;
  return ::rmdir(path.c_str()) == 0 && ok;
}

bool WriteAll(int fd, const char* data, std::size_t size) {
  while (size > 0) {
    const auto count = ::write(fd, data, size);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

// A new file in folder holding source's bytes, synced. The buffer is on the heap: worker stacks are small.
bool CopyInto(const ManagedDirectory& folder, const std::string& name, const std::string& source) {
  const int in = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (in < 0) return false;
  const int out = folder.CreateExclusiveFile(name, kFileMode);
  if (out < 0) {
    ::close(in);
    return false;
  }
  const auto buffer = std::make_unique<char[]>(kCopyBytes);
  bool ok = true;
  for (;;) {
    const auto count = ::read(in, buffer.get(), kCopyBytes);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) ok = false;
    if (count <= 0) break;
    if (!WriteAll(out, buffer.get(), static_cast<std::size_t>(count))) {
      ok = false;
      break;
    }
  }
  ok = ::fsync(out) == 0 && ok;
  ok = ::close(out) == 0 && ok;
  ::close(in);
  return ok;
}

bool WriteNew(const ManagedDirectory& folder, const std::string& name, const std::string& text) {
  const int out = folder.CreateExclusiveFile(name, kFileMode);
  if (out < 0) return false;
  bool ok = WriteAll(out, text.data(), text.size());
  ok = ::fsync(out) == 0 && ok;
  return ::close(out) == 0 && ok;
}

std::string Stem(const std::string& archive) {
  constexpr std::string_view suffix = ".tzst";
  return archive.size() > suffix.size() && archive.compare(archive.size() - suffix.size(), suffix.size(), suffix) == 0
             ? archive.substr(0, archive.size() - suffix.size())
             : archive;
}

// The steps that touch the pack folder (perhaps root's).
bool PlaceArchive(const PublisherPackInstall& install, const std::string& archive, std::string* reason) {
  const auto log = [&](const std::string& line) {
    if (install.log) install.log(line);
  };
  const std::string root = install.pack_root;
  const auto slash = root.rfind('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 == root.size())
    return Fail(reason, "the pack folder is not an absolute path: " + root);
  auto parent = ManagedDirectory::Open(root.substr(0, slash), DirectoryPolicy::kApprovedTree);
  if (!parent) return Fail(reason, "cannot open " + root.substr(0, slash));
  auto pack = parent->OpenChild(root.substr(slash + 1), DirectoryPolicy::kApprovedTree, true, kFolderMode);
  if (!pack) return Fail(reason, "cannot open " + root);

  // A staging name of Playden's own: the upload tool and the builds page stage in ".<version>.upload" and may be
  // writing there right now. What a previous download left behind goes first.
  const std::string staging_name = "." + install.version + ".download";
  const std::string staging_path = root + "/" + staging_name;
  if (!RemoveTree(staging_path)) return Fail(reason, "cannot remove " + staging_path);
  auto staging = pack->OpenChild(staging_name, DirectoryPolicy::kApprovedTree, true, kFolderMode);
  if (!staging) return Fail(reason, "cannot create " + staging_path);
  const auto abandon = [&](std::string text) {
    RemoveTree(staging_path);
    return Fail(reason, std::move(text));
  };
  if (!CopyInto(*staging, install.release.archive, archive)) return abandon("cannot write " + install.release.archive);
  // Read back: the copy must still be the pinned archive.
  const auto copied = HashArtifactFile(staging_path + "/" + install.release.archive);
  if (!copied || *copied != install.release.sha256) return abandon(install.release.archive + " did not read back");
  if (!WriteNew(*staging, "release.json", PublisherReleaseJson(install.version, {install.release})))
    return abandon("cannot write release.json");
  if (!staging->Sync()) return abandon("cannot sync " + staging_path);
  staging.reset();

  // This version's folder (damaged, incomplete, or holding archives this build doesn't pin) is set aside, and put
  // back if the new one can't take its place. A folder set aside by an install that stopped before its rename is
  // put back first, so the only copy of a version is never removed.
  const std::string final_path = root + "/" + install.version;
  const std::string aside_name = "." + install.version + ".replaced";
  const std::string aside_path = root + "/" + aside_name;
  struct stat info {};
  if (!Lstat(final_path, &info) && Lstat(aside_path, &info) &&
      !pack->RenameChild(aside_name, *pack, install.version, false))
    return abandon("cannot put " + aside_path + " back");
  if (!RemoveTree(aside_path)) return abandon("cannot remove " + aside_path);
  const bool had_version = Lstat(final_path, &info);
  if (had_version && !pack->RenameChild(install.version, *pack, aside_name, false)) {
    // Not ours to rename (another owner): a damaged copy is only removed, which needs write access to the folder.
    log("publisher pack: cannot move " + final_path + " aside, removing it");
    if (!RemoveTree(final_path)) return abandon("cannot replace " + final_path);
  }
  const bool aside = had_version && Lstat(aside_path, &info);
  if (!pack->RenameChild(staging_name, *pack, install.version, false)) {
    if (aside && !pack->RenameChild(aside_name, *pack, install.version, false))
      log("publisher pack: could not put " + aside_path + " back; the next install does");
    return abandon("cannot rename " + staging_path);
  }
  // The old copy goes only once the new one is durably in place.
  if (!pack->Sync() || !parent->Sync()) return Fail(reason, "cannot sync " + root);
  if (aside && !RemoveTree(aside_path)) log("publisher pack: could not remove " + aside_path);

  // Other versions: this build can't use them (the same rule CheckPublisherPack uses to find one).
  for (const auto& name : List(root)) {
    if (name.empty() || name[0] == '.' || name == "cache" || name == install.version) continue;
    const std::string path = root + "/" + name;
    if (!IsFolder(path) || !IsRegular(path + "/release.json")) continue;
    log(RemoveTree(path) ? "publisher pack: removed version " + name : "publisher pack: could not remove " + path);
  }
  return true;
}

}  // namespace

std::string PublisherArchiveUrl(const std::string& version, const PublisherRelease& release) {
  return std::string(kPublisherPackBaseUrl) + version + "/" + release.archive;
}

std::string PublisherReleaseJson(const std::string& version, const std::vector<PublisherRelease>& releases) {
  std::string artifacts, trees;
  for (const auto& release : releases) {
    artifacts += std::string(artifacts.empty() ? "" : ",\n") + "    \"" + release.archive + "\": \"" + release.sha256 + "\"";
    trees += std::string(trees.empty() ? "" : ",\n") + "    \"" + release.archive + "\": \"" + release.tree_sha256 + "\"";
  }
  return "{\n  \"version\": \"" + version + "\",\n  \"artifacts\": {\n" + artifacts + "\n  },\n  \"tree_sha256\": {\n" +
         trees + "\n  }\n}\n";
}

bool InstallPublisherPack(const PublisherPackInstall& install, const std::string& archive, std::string* reason) {
  const auto digest = HashArtifactFile(archive);
  if (!digest || *digest != install.release.sha256)
    return Fail(reason, "the downloaded " + install.release.archive + " does not match its pin");
  std::string why;
  bool placed = false;
  // as_root fails when root can't be raised or the app's user can't be put back: nothing more is done then.
  const bool privileges_ok = install.as_root ? install.as_root([&] { return placed = PlaceArchive(install, archive, &why); })
                                             : (placed = PlaceArchive(install, archive, &why));
  if (!placed) return Fail(reason, why.empty() ? "cannot install the pack" : why);
  if (!privileges_ok) return Fail(reason, "the pack is in place, but root could not be given up");

  // Unpacked copies of other archives: Playden's own files, removed without root. The cache folder is checked
  // first (ManagedDirectory refuses a link anywhere in its path), so nothing is removed through a link.
  if (!install.cache_root.empty() && IsFolder(install.cache_root)) {
    const auto cache = ManagedDirectory::Open(install.cache_root, DirectoryPolicy::kApprovedTree);
    if (!cache) {
      if (install.log) install.log("publisher cache: " + install.cache_root + " refused, not cleaned");
      return true;
    }
    const auto keep = Stem(install.release.archive);
    for (const auto& name : List(install.cache_root)) {
      if (name.empty() || name[0] == '.' || name == keep || name.rfind("playden-", 0) != 0) continue;
      struct stat info {};
      if (!cache->InspectChild(name, &info) || !S_ISDIR(info.st_mode)) continue;
      const bool removed = RemoveTree(install.cache_root + "/" + name);
      if (install.log)
        install.log(removed ? "publisher cache: removed " + name : "publisher cache: could not remove " + name);
    }
  }
  return true;
}

}  // namespace playden::runtime
