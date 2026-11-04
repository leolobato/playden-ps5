#include "steam/install_volumes.h"

#include <sys/stat.h>
#if PLAYDEN_PLATFORM_PS5
#include <sys/mount.h>
extern "C" int sceKernelDlsym(int, const char*, void**);
#else
#include <sys/statvfs.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

namespace playden::steam {
namespace {

std::string ExternalRoot(const std::string& path) {
  for (const auto* root : {"/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",
                           "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"}) {
    const std::size_t length = std::strlen(root);
    if (path.compare(0, length, root) == 0 &&
        (path.size() == length || path[length] == '/')) return root;
  }
  return {};
}

// Collapse redundant separators/dots, but reject parent traversal so an external path
// cannot escape its required root (including through a symlink before the '..').
std::string QueryPath(const std::string& path) {
  if (path.empty() || path.find('\0') != std::string::npos) return {};
  std::string result = path[0] == '/' ? "/" : "";
  for (std::size_t start = 0; start < path.size();) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string::npos ? slash : slash - start);
    if (part == "..") return {};
    if (!part.empty() && part != ".") {
      if (!result.empty() && result.back() != '/') result += '/';
      result += part;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return result.empty() ? "." : result;
}

bool IsDirectory(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// A drive's mount point sits on a different device than the folder holding it.
bool IsMountPoint(const std::string& path) {
  if (path == "/") return true;
  struct stat mount {}, parent {};
  const auto slash = path.find_last_of('/');
  const auto parent_path = slash == 0 ? std::string("/") : path.substr(0, slash);
  if (slash == std::string::npos || lstat(path.c_str(), &mount) != 0 || !S_ISDIR(mount.st_mode) ||
      stat(parent_path.c_str(), &parent) != 0)
    return false;
  return mount.st_dev != parent.st_dev;
}

// mkdir -p for an absolute path; existing components are fine.
bool MakeDirectories(const std::string& path) {
  if (path.empty()) return false;
  for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    const auto prefix = path.substr(0, slash);
    if (!IsDirectory(prefix) && mkdir(prefix.c_str(), 0777) != 0 && !IsDirectory(prefix)) return false;
  }
  return IsDirectory(path) || mkdir(path.c_str(), 0777) == 0 || IsDirectory(path);
}

// Like MakeDirectories, but only below `mount`, and each folder it creates is made 0777 whatever
// the umask, so the app user can write there after root made it.
bool MakeSharedDirectories(const std::string& mount, const std::string& path) {
  if (path.compare(0, mount.size() + 1, mount + "/") != 0) return false;
  for (std::size_t slash = path.find('/', mount.size() + 1);; slash = path.find('/', slash + 1)) {
    const auto prefix = slash == std::string::npos ? path : path.substr(0, slash);
    if (!IsDirectory(prefix)) {
      if (mkdir(prefix.c_str(), 0777) != 0 && !IsDirectory(prefix)) return false;
      if (chmod(prefix.c_str(), 0777) != 0) return false;
    }
    if (slash == std::string::npos) return true;
  }
}

bool CanWrite(const std::string& directory) {
  const auto probe = directory + "/.playden-probe";
  FILE* file = std::fopen(probe.c_str(), "w");
  if (!file) return false;
  const bool written = std::fputs("ok\n", file) >= 0;
  const bool closed = std::fclose(file) == 0;
  std::remove(probe.c_str());
  return written && closed;
}

#if PLAYDEN_PLATFORM_PS5
using Fstatfs = int (*)(int, struct statfs*);
// The ordinary statfs/fstatfs imports are absent in an app process (they live in libkernel_sys).
// Only call the underscore export after resolving it in the app's libkernel.
Fstatfs ResolvedFstatfs() {
  static const Fstatfs query = [] {
    void* symbol = nullptr;
    if (sceKernelDlsym(0x2001, "_fstatfs", &symbol) != 0) return Fstatfs{};
    return reinterpret_cast<Fstatfs>(symbol);
  }();
  return query;
}
#endif

}  // namespace

bool VolumeSpaceSupported() {
#if PLAYDEN_PLATFORM_PS5
  return ResolvedFstatfs() != nullptr;
#else
  return true;
#endif
}

std::optional<VolumeSpace> detail::VolumeSpaceFromBlocks(std::int64_t block_size, std::uint64_t blocks,
                                                       std::int64_t available_blocks) {
  if (block_size <= 0 || blocks == 0) return std::nullopt;
  const auto unit = static_cast<std::uint64_t>(block_size);
  const auto available = available_blocks < 0 ? 0 : static_cast<std::uint64_t>(available_blocks);
  if (available > blocks || blocks > std::numeric_limits<std::uint64_t>::max() / unit)
    return std::nullopt;
  return VolumeSpace{blocks * unit, available * unit};
}

VolumeSpace detail::WithoutConsoleReserve(VolumeSpace space, std::uint64_t reserve) {
  space.free_bytes -= std::min(space.free_bytes, reserve);
  return space;
}

bool detail::VolumeMountMatches(const std::string& path, const std::string& mount) {
  const auto root = ExternalRoot(path);
  return root.empty() || mount == root;
}

std::vector<InstallVolume> DiscoverInstallVolumes(const std::vector<VolumeCandidate>& candidates, const AsRoot& as_root) {
  std::vector<InstallVolume> volumes;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto& candidate = candidates[index];
    if (index == 0) {
      volumes.push_back({candidate.id, candidate.label, candidate.games_root, true});
      continue;
    }
    if (!IsDirectory(candidate.mount)) continue;
    if (candidate.require_mount_point && !IsMountPoint(candidate.mount)) continue;
    bool available = MakeDirectories(candidate.games_root) && CanWrite(candidate.games_root);
    if (!available && as_root &&
        as_root([&] { return MakeSharedDirectories(candidate.mount, candidate.games_root); }))
      available = CanWrite(candidate.games_root);
    volumes.push_back({candidate.id, candidate.label, candidate.games_root, available});
  }
  return volumes;
}

std::vector<VolumeCandidate> ConsoleVolumeCandidates(const std::string& title_root, const std::string& games_root) {
  const auto primary = games_root.empty() ? title_root + "/games" : games_root;
  const auto slash = primary.find_last_of('/');
  const auto mount = games_root.empty() ? title_root : slash == 0 ? std::string("/") : primary.substr(0, slash);
  return {
      {InstallService::kPrimaryVolumeId, "Console storage", mount, primary},
      {"m2", "M.2 SSD", "/mnt/ext1", "/mnt/ext1/playden/games", true},
      {"extended", "Extended storage", "/mnt/ext0", "/mnt/ext0/playden/games", true},
      {"usb0", "USB drive", "/mnt/usb0", "/mnt/usb0/playden/games", true},
      {"usb1", "USB drive 2", "/mnt/usb1", "/mnt/usb1/playden/games", true},
      {"usb2", "USB drive 3", "/mnt/usb2", "/mnt/usb2/playden/games", true},
      {"usb3", "USB drive 4", "/mnt/usb3", "/mnt/usb3/playden/games", true},
      {"usb4", "USB drive 5", "/mnt/usb4", "/mnt/usb4/playden/games", true},
      {"usb5", "USB drive 6", "/mnt/usb5", "/mnt/usb5/playden/games", true},
      {"usb6", "USB drive 7", "/mnt/usb6", "/mnt/usb6/playden/games", true},
      {"usb7", "USB drive 8", "/mnt/usb7", "/mnt/usb7/playden/games", true},
  };
}

std::optional<VolumeSpace> QueryVolumeSpace(const std::string& path, std::uint64_t console_reserve) {
  std::string directory = QueryPath(path);
  if (directory.empty()) return std::nullopt;
  const auto external_root = ExternalRoot(directory);
#if PLAYDEN_PLATFORM_PS5
  const auto query = ResolvedFstatfs();
  if (!query) return std::nullopt;
#else
  if (!external_root.empty() && !IsMountPoint(external_root)) return std::nullopt;
#endif
  int fd = -1;
  while ((fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY)) < 0) {
    // Only nonexistent destinations permit fallback, never permission/IO errors
    // or a file in the path. An absent external root must not fall back to /mnt.
    if (errno != ENOENT || directory == external_root) return std::nullopt;
    const auto slash = directory.rfind('/');
    if (slash == std::string::npos || slash == 0) return std::nullopt;
    directory.resize(slash);
  }
#if PLAYDEN_PLATFORM_PS5
  struct statfs info {};
  const bool ok = query(fd, &info) == 0;
  close(fd);
  if (!ok) return std::nullopt;
  const auto length = strnlen(info.f_mntonname, sizeof(info.f_mntonname));
  if (length == sizeof(info.f_mntonname) ||
      !detail::VolumeMountMatches(QueryPath(path), std::string(info.f_mntonname, length)))
    return std::nullopt;
  if (info.f_bsize > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return std::nullopt;
  const auto space = detail::VolumeSpaceFromBlocks(static_cast<std::int64_t>(info.f_bsize), info.f_blocks, info.f_bavail);
  // Console storage (no external drive root) keeps space for the system.
  if (space && external_root.empty()) return detail::WithoutConsoleReserve(*space, console_reserve);
  return space;
#else
  struct statvfs info {};
  const bool ok = fstatvfs(fd, &info) == 0;
  close(fd);
  const auto unit = info.f_frsize ? info.f_frsize : info.f_bsize;
  // Host statvfs fields are unsigned, while the console reports a signed bavail.
  if (!ok || unit > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      static_cast<std::uint64_t>(info.f_bavail) > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return std::nullopt;
  return detail::VolumeSpaceFromBlocks(static_cast<std::int64_t>(unit), info.f_blocks,
                                      static_cast<std::int64_t>(info.f_bavail));
#endif
}

}  // namespace playden::steam
