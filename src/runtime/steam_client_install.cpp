// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/steam_client_install.h"

#include "platform/directory_walk.h"
#include "runtime/artifact_hash.h"
#include "runtime/steam_host.h"
#include "update/file_ops.h"
#include "update/zip_extract.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;

constexpr const char* kMarker = ".playden-steam-client";
// Valve's crash reporter: SteamClientLeftOut.
constexpr const char* kLeftOut[] = {"crashhandler.dll", "crashhandler64.dll", "steamerrorreporter.exe", "steamerrorreporter64.exe"};

// std::filesystem::remove_all crashes the console title (results/20261007-game-removal); this walks by path with
// lstat/unlink/rmdir and never follows a link. True when nothing is left.
bool RemoveTree(const fs::path& path) {
  update::RemoveTree(path.string());
  struct stat status {};
  return ::lstat(path.c_str(), &status) != 0 && errno == ENOENT;
}

std::string MarkerText(const std::string& version, const std::string& host_sha256) { return version + " " + host_sha256 + "\n"; }

bool Digits(std::string_view text) {
  return !text.empty() && text.size() <= 20 && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool LowerHex(std::string_view text, std::size_t size) {
  return text.size() == size &&
         std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The next quoted token of Valve's VDF text from at: its text, or nullopt with at at the end. Braces are tokens too.
std::optional<std::string> NextToken(std::string_view text, std::size_t* at) {
  while (*at < text.size() && (text[*at] == ' ' || text[*at] == '\t' || text[*at] == '\r' || text[*at] == '\n')) ++*at;
  if (*at >= text.size()) return std::nullopt;
  if (text[*at] == '{' || text[*at] == '}') return std::string(1, text[(*at)++]);
  if (text[*at] != '"') return std::nullopt;
  const auto end = text.find('"', *at + 1);
  if (end == std::string_view::npos) return std::nullopt;
  std::string token(text.substr(*at + 1, end - *at - 1));
  *at = end + 1;
  return token;
}

bool IsRegular(const fs::path& path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}
bool IsFolder(const fs::path& path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

// Opens the folder without following a link in any component of its path, and gives it the mode. False for a link (or
// a path through one): a chmod by path would change whatever it points at.
bool OpenFolderMode(const fs::path& path, mode_t mode) {
  const int fd = platform::OpenDirectoryWalk(path);
  if (fd < 0) return false;
  const bool ok = ::fchmod(fd, mode) == 0;
  ::close(fd);
  return ok;
}

// The steam.exe of a folder that is not the install it should be goes, and so does a link in the folder's place: the
// folder is open to everyone, and the title starts whatever steam.exe it finds there. True when none is left.
bool RemoveUncheckedHost(const fs::path& root) {
  struct stat status {};
  if (::lstat(root.c_str(), &status) != 0) return errno == ENOENT;
  if (!S_ISDIR(status.st_mode)) return ::unlink(root.c_str()) == 0;
  const int fd = platform::OpenDirectoryWalk(root);
  if (fd < 0) return false;
  ::close(fd);
  return ::unlink((root / "steam.exe").c_str()) == 0 || errno == ENOENT;
}

std::string ReadSmall(const fs::path& path) {
  std::string text;
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return text;
  char buffer[256];
  ssize_t count;
  while (text.size() < 4096 && (count = ::read(fd, buffer, sizeof buffer)) > 0) text.append(buffer, static_cast<std::size_t>(count));
  ::close(fd);
  return text;
}

bool WriteAll(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = ::write(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return true;
}

bool WriteFile(const fs::path& path, std::string_view bytes, mode_t mode) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
  if (fd < 0) return false;
  const bool ok = WriteAll(fd, bytes) && ::fchmod(fd, mode) == 0 && ::fsync(fd) == 0;
  return ::close(fd) == 0 && ok;
}

bool CopyFile(const fs::path& from, const fs::path& to, mode_t mode) {
  const int in = ::open(from.c_str(), O_RDONLY | O_NOFOLLOW);
  if (in < 0) return false;
  const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
  bool ok = out >= 0;
  std::string buffer(1 << 16, '\0');  // on the heap: worker stacks are small on the PS5
  ssize_t count;
  while (ok && (count = ::read(in, buffer.data(), buffer.size())) != 0) {
    if (count < 0 && errno == EINTR) continue;
    ok = count > 0 && WriteAll(out, std::string_view(buffer.data(), static_cast<std::size_t>(count)));
  }
  ::close(in);
  if (out >= 0) ok = ::fchmod(out, mode) == 0 && ::fsync(out) == 0 && ::close(out) == 0 && ok;
  return ok;
}

// The package in downloads/: kept when it is whole and the pinned one, fetched into <file>.part otherwise (resuming
// what an earlier call fetched), then checked and renamed.
bool Download(const SteamClientPackage& package, const fs::path& downloads, const RangeFetch& fetch, fs::path* path,
              std::string* error) {
  *path = downloads / package.file;
  if (IsRegular(*path)) {
    if (HashArtifactFile(*path).value_or("") == package.sha256) return true;
    ::unlink(path->c_str());
  }
  if (!fetch) {
    *error = "not downloaded yet";
    return false;
  }
  const auto part = fs::path(path->string() + ".part");
  struct stat status {};
  std::uint64_t offset = ::lstat(part.c_str(), &status) == 0 && S_ISREG(status.st_mode) ? static_cast<std::uint64_t>(status.st_size) : 0;
  if (offset > package.bytes) {
    ::unlink(part.c_str());
    offset = 0;
  }
  const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
  if (fd < 0) {
    *error = "cannot write " + part.filename().string() + ": " + std::strerror(errno);
    return false;
  }
  bool ok = true;
  while (ok && offset < package.bytes) {
    const auto length = std::min<std::uint64_t>(kRedistChunkBytes, package.bytes - offset);
    std::string reason;
    const auto chunk = fetch(kSteamClientCdn + package.file, offset, length, &reason);
    if (!chunk || chunk->size() != length) {
      *error = "download of " + package.file + " stopped: " + (reason.empty() ? std::string("short answer") : reason);
      ok = false;
    } else if (!WriteAll(fd, *chunk)) {
      *error = std::string("cannot write the download: ") + std::strerror(errno);
      ok = false;
    } else {
      offset += length;
    }
  }
  ok = ::fsync(fd) == 0 && ::close(fd) == 0 && ok;
  if (!ok) return false;
  if (HashArtifactFile(part).value_or("") != package.sha256) {
    ::unlink(part.c_str());
    *error = package.file + " is not the pinned package";
    return false;
  }
  if (::rename(part.c_str(), path->c_str()) != 0) {
    *error = std::string("cannot keep the download: ") + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

std::optional<SteamClientManifest> ParseSteamClientManifest(std::string_view text) {
  // "win64" { "version" "<n>" ... "bins_win64" { "file" "..." "size" "..." "sha2" "..." ... } ... }, where a section may
  // hold sections of its own (steam_win64's "steamrow"). Only the version and the top-level bins_win64 are taken:
  // bins_codecs_win64 (video for streaming) and steam_win64 (Valve's steam.exe, which Playden's replaces) aren't needed.
  std::size_t at = 0;
  if (!NextToken(text, &at) || NextToken(text, &at) != std::optional<std::string>("{")) return std::nullopt;
  SteamClientManifest manifest;
  std::vector<std::string> path;  // the sections the next key is in, below the root
  std::string file, size, sha2;
  bool closed = false;
  while (const auto key = NextToken(text, &at)) {
    if (*key == "}") {
      if (path.empty()) {
        closed = true;
        break;
      }
      path.pop_back();
      continue;
    }
    if (*key == "{") return std::nullopt;
    const auto value = NextToken(text, &at);
    if (!value || *value == "}") return std::nullopt;
    if (*value == "{") {
      if (path.size() >= 8) return std::nullopt;
      path.push_back(*key);
      continue;
    }
    if (path.empty() && *key == "version") manifest.version = *value;
    if (path.size() == 1 && path[0] == "bins_win64") {
      if (*key == "file") file = *value;
      else if (*key == "size") size = *value;
      else if (*key == "sha2") sha2 = *value;
    }
  }
  if (!closed || !Digits(manifest.version) || file.empty() || file.find_first_of("/\\") != std::string::npos ||
      file.find("..") != std::string::npos || !Digits(size) || !LowerHex(sha2, 64))
    return std::nullopt;
  manifest.package = {file, sha2, std::stoull(size)};
  return manifest;
}

bool SteamClientLeftOut(std::string_view relative) {
  for (const char* name : kLeftOut)
    if (relative == name) return true;
  return false;
}

std::optional<std::string> SteamClientInstalled(const std::string& folder, const std::string& host_sha256) {
  const fs::path root(folder);
  // The folder is open to everyone (the client writes its state there): steam.exe is checked each time, since the game's
  // session reaches whatever runs as it. Playden's own record of steamclient64.dll's hash goes to the host with the
  // session, which checks the DLL itself.
  const auto marker = ReadSmall(root / kMarker);
  const auto space = marker.find(' ');
  if (space == std::string::npos || marker != MarkerText(marker.substr(0, space), host_sha256) ||
      !Digits(marker.substr(0, space)) || !IsRegular(root / "steamclient64.dll") || !IsRegular(root / "steam.exe") ||
      HashArtifactFile(root / "steam.exe").value_or("") != host_sha256)
    return std::nullopt;
  return marker.substr(0, space);
}

SteamClientInstallResult InstallSteamClient(const std::string& folder, const std::string& host_exe, const RangeFetch& fetch,
                                            const SteamClientManifest& manifest, bool replace,
                                            const std::string& host_sha256) {
  SteamClientInstallResult result;
  const auto fail = [&](std::string detail) {
    result.result = "failed";
    result.detail = std::move(detail);
    return result;
  };
  if (!replace && SteamClientInstalled(folder, host_sha256) == manifest.version) {
    result.result = "already";
    result.version = manifest.version;
    return result;
  }
  const fs::path root(folder);
  const auto parent = root.parent_path();
  const auto downloads = parent / "downloads";
  for (const auto& made : {parent, downloads}) {
    if (!IsFolder(made) && ::mkdir(made.c_str(), 0777) != 0 && errno != EEXIST)
      return fail("cannot make " + made.string() + ": " + std::strerror(errno));
    if (!OpenFolderMode(made, 0777)) return fail(made.string() + " is not a folder of its own (a link?)");
  }
  // An install that isn't whole or isn't Playden's leaves no steam.exe to run, whatever happens below: without a download
  // the game starts without Steam. A whole earlier version stays until the new one is in place.
  if (!SteamClientInstalled(folder, host_sha256) && !RemoveUncheckedHost(root)) return fail("cannot remove the earlier steam.exe");
  if (HashArtifactFile(host_exe).value_or("") != host_sha256) return fail("Playden's steam.exe is missing or damaged");

  // Unpacked beside the final folder, then renamed: a half-unpacked client is never taken for a whole one.
  const auto staging = parent / (root.filename().string() + ".staging");
  RemoveTree(staging);
  if (::mkdir(staging.c_str(), 0777) != 0) return fail("cannot make the unpack folder: " + std::string(std::strerror(errno)));
  if (!OpenFolderMode(staging, 0777)) return fail("the unpack folder is not a folder of its own");
  std::vector<fs::path> unpacked;
  for (const auto& package : {manifest.package}) {
    fs::path zip;
    std::string error;
    if (!Download(package, downloads, fetch, &zip, &error)) {
      RemoveTree(staging);
      result.download_failed = true;
      return fail(error);
    }
    update::ZipExtractOptions options;
    options.folder_mode = 0777;
    options.windows_separators = true;  // Valve's bins_win64 has a folder "bin\shaders/"
    options.mode_for = [](const std::string& relative, unsigned) -> std::optional<unsigned> {
      const auto dot = relative.rfind('.');
      const auto extension = dot == std::string::npos ? std::string() : relative.substr(dot);
      return extension == ".exe" || extension == ".dll" ? 0755u : 0666u;
    };
    const auto extracted = update::ExtractZip(zip.string(), staging.string(), options);
    if (!extracted.ok) {
      RemoveTree(staging);
      return fail("cannot unpack " + package.file + ": " + extracted.error);
    }
    result.files += extracted.files;
    unpacked.push_back(zip);
  }
  for (const char* name : kLeftOut)
    if (::unlink((staging / name).c_str()) == 0) --result.files;
  result.client_dll_sha256 = HashArtifactFile(staging / "steamclient64.dll").value_or("");
  if (result.client_dll_sha256.empty()) {
    RemoveTree(staging);
    return fail("the package has no steamclient64.dll");
  }
  if (!CopyFile(host_exe, staging / "steam.exe", 0755) ||
      !WriteFile(staging / kMarker, MarkerText(manifest.version, host_sha256), 0644)) {
    RemoveTree(staging);
    return fail("cannot add Playden's steam.exe");
  }
  // An older or broken copy in the way goes; the client's state in it goes with it.
  if (!RemoveTree(root)) {
    RemoveTree(staging);
    return fail("cannot replace the earlier client folder");
  }
  if (::rename(staging.c_str(), root.c_str()) != 0) {
    const auto reason = std::string(std::strerror(errno));
    RemoveTree(staging);
    return fail("cannot put the client in place: " + reason);
  }
  for (const auto& zip : unpacked) ::unlink(zip.c_str());
  // Earlier clients beside it (named by their version) and their unpack folders go.
  if (DIR* listing = ::opendir(parent.c_str())) {
    std::vector<std::string> earlier;
    while (const auto* entry = ::readdir(listing)) {
      const std::string name = entry->d_name;
      if (Digits(name) || (name.size() > 8 && name.compare(name.size() - 8, 8, ".staging") == 0 && name != staging.filename().string()))
        earlier.push_back(name);
    }
    ::closedir(listing);
    for (const auto& name : earlier) RemoveTree(parent / name);
  }
  result.result = "installed";
  result.version = manifest.version;
  return result;
}

bool RemoveSteamClientSignIn(const std::string& folder) {
  bool ok = true;
  for (const char* name : {"config/config.vdf", "config/loginusers.vdf"}) {
    const auto path = fs::path(folder) / name;
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) ok = false;
  }
  return ok;
}

}  // namespace playden::runtime
