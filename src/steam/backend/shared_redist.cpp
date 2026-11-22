// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/shared_redist.h"

#include "installs/install_record.h"
#include "steam/backend/app_info.h"

#include <mbedtls/sha1.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <utility>

namespace playden::steam {

const char* SharedDepotStatusName(SharedDepotResult::Status status) {
  switch (status) {
    case SharedDepotResult::Status::kReady: return "ready";
    case SharedDepotResult::Status::kNotCached: return "not-cached";
    case SharedDepotResult::Status::kRefused: return "refused";
    case SharedDepotResult::Status::kUnavailable: return "unavailable";
    case SharedDepotResult::Status::kFailed: return "failed";
    case SharedDepotResult::Status::kCanceled: return "canceled";
  }
  return "failed";
}

namespace {

std::string DepotFolder(const std::string& cache_root, std::uint32_t depot) {
  return cache_root + "/" + std::to_string(depot);
}

// Makes each missing folder of path in turn (the cache root may not be there yet). Links are not followed into: a
// component that is one fails.
bool MakeFolders(const std::string& path) {
  if (path.empty() || path[0] != '/') return false;
  for (std::size_t at = 1; at <= path.size(); ++at) {
    if (at != path.size() && path[at] != '/') continue;
    const auto part = path.substr(0, at);
    struct stat info {};
    if (::lstat(part.c_str(), &info) == 0) {
      if (!S_ISDIR(info.st_mode)) return false;
      continue;
    }
    if (::mkdir(part.c_str(), 0777) != 0 && errno != EEXIST) return false;
    (void)::chmod(part.c_str(), 0777);  // open to FTP whatever the umask, like the rest of Playden's data (#868)
  }
  return true;
}

}  // namespace

std::optional<std::string> CachedSharedDepot(const std::string& cache_root, std::uint32_t depot) {
  const auto folder = DepotFolder(cache_root, depot);
  const auto record = installs::ReadInstallRecord(folder + "/" + DepotInstaller::kInstallRecord);
  if (!record || record->app_id != kCommonRedistApp || record->depots.size() != 1 || record->depots[0].id != depot)
    return std::nullopt;
  return folder;
}

namespace {

std::string LowerText(std::string text) {
  for (auto& value : text)
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + 32);
  return text;
}

// The SHA-1 of a file as lower-case hex, read in pieces off the stack (small worker stacks).
std::optional<std::string> FileSha1(const std::string& path, std::uint64_t* size) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return std::nullopt;
  mbedtls_sha1_context context;
  mbedtls_sha1_init(&context);
  mbedtls_sha1_starts(&context);
  const auto buffer = std::make_unique<unsigned char[]>(64 * 1024);
  bool ok = true;
  *size = 0;
  for (;;) {
    const auto count = ::read(file, buffer.get(), 64 * 1024);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) { ok = false; break; }
    if (count == 0) break;
    mbedtls_sha1_update(&context, buffer.get(), static_cast<std::size_t>(count));
    *size += static_cast<std::uint64_t>(count);
  }
  ::close(file);
  unsigned char digest[20] = {};
  mbedtls_sha1_finish(&context, digest);
  mbedtls_sha1_free(&context);
  if (!ok) return std::nullopt;
  std::string hex;
  for (const auto byte : digest) { hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
  return hex;
}

}  // namespace

std::optional<std::string> VerifiedSharedDepotFile(const std::string& folder, const std::string& name, std::string* why) {
  std::FILE* tree = std::fopen((folder + "/" + DepotInstaller::kFileTree).c_str(), "r");
  if (!tree) {
    *why = "no file list";
    return std::nullopt;
  }
  const auto wanted = LowerText(name);
  std::string sha, path;
  std::uint64_t size = 0;
  char line[1024];
  bool first = true, found = false;
  while (std::fgets(line, sizeof line, tree)) {
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (std::exchange(first, false)) {
      if (text != "v1") break;
      continue;
    }
    const auto tab = text.find('\t'), second = tab == std::string::npos ? tab : text.find('\t', tab + 1);
    if (second == std::string::npos) continue;
    const auto entry = text.substr(second + 1);
    const auto slash = entry.rfind('/');
    if (LowerText(slash == std::string::npos ? entry : entry.substr(slash + 1)) != wanted) continue;
    // A name listed twice is not one Playden can pick between.
    if (found) {
      std::fclose(tree);
      *why = name + " is listed twice";
      return std::nullopt;
    }
    found = true;
    sha = text.substr(0, tab);
    size = std::strtoull(text.substr(tab + 1, second - tab - 1).c_str(), nullptr, 10);
    path = entry;
  }
  std::fclose(tree);
  if (!found || path.empty() || path[0] == '/' || path.find("..") != std::string::npos) {
    *why = "the depot doesn't list " + name;
    return std::nullopt;
  }
  const auto full = folder + "/" + path;
  std::uint64_t actual = 0;
  const auto hash = FileSha1(full, &actual);
  if (!hash || actual != size || *hash != sha) {
    *why = hash ? name + " is not the file the depot lists" : name + " could not be read";
    return std::nullopt;
  }
  return full;
}

SharedRedistCache::SharedRedistCache(AppInfoFetch app_info, DepotInstaller& installer)
    : app_info_(std::move(app_info)), installer_(installer) {
  // A manifest bigger than appinfo said is not downloaded either.
  installer_.SetManifestCheck([](const DepotManifest& manifest) {
    std::uint64_t bytes = 0;
    for (const auto& file : manifest.files) bytes += file.size;
    return bytes > kMaxSharedDepotBytes ? InstallError::kCorrupt : InstallError::kNone;
  });
}

SharedDepotResult SharedRedistCache::Get(const StoredSession* session, std::uint32_t depot, const std::string& cache_root,
                                         bool download, const std::atomic<bool>& cancel, const std::string& file_name) {
  using Status = SharedDepotResult::Status;
  SharedDepotResult out;
  const auto finish = [&](Status status, std::string detail) {
    out.status = status;
    out.detail = std::move(detail);
    return out;
  };
  if (cache_root.empty() || cache_root[0] != '/' || cache_root.find("/..") != std::string::npos || depot == 0)
    return finish(Status::kFailed, "no cache folder");
  std::string damaged;
  if (const auto kept = CachedSharedDepot(cache_root, depot)) {
    if (file_name.empty()) {
      out.folder = *kept;
      return finish(Status::kReady, "");
    }
    if (const auto file = VerifiedSharedDepotFile(*kept, file_name, &damaged)) {
      out.folder = *kept;
      out.file = *file;
      return finish(Status::kReady, "");
    }
    damaged = "the kept copy: " + damaged;
    if (!download) return finish(Status::kNotCached, damaged);
    // Downloaded again from scratch.
    std::string failure;
    if (!RemoveManagedInstall(*kept, &failure)) return finish(Status::kFailed, damaged + "; it could not be removed: " + failure);
  }
  if (!download) return finish(Status::kNotCached, "not downloaded yet");
  if (!session || !app_info_) return finish(Status::kUnavailable, "not signed in to Steam");
  const auto info = app_info_(*session, {kCommonRedistApp}, cancel);
  if (cancel) return finish(Status::kCanceled, "canceled");
  if (!info.ok()) {
    const bool later = info.error == CmError::kNetwork || info.error == CmError::kTimeout;
    return finish(later ? Status::kUnavailable : Status::kFailed, std::string("appinfo ") + CmErrorName(info.error));
  }
  const auto buffer = info.buffers.find(kCommonRedistApp);
  const auto app = buffer == info.buffers.end() ? std::nullopt : ParseAppInfo(kCommonRedistApp, buffer->second);
  if (!app) return finish(Status::kFailed, "no appinfo");
  const auto listed = std::find_if(app->depots.begin(), app->depots.end(), [&](const DepotInfo& d) { return d.id == depot; });
  if (listed == app->depots.end()) return finish(Status::kFailed, "depot not listed");
  if (!listed->manifest_gid || *listed->manifest_gid == 0) return finish(Status::kFailed, "no public manifest");
  if (listed->size > kMaxSharedDepotBytes) return finish(Status::kFailed, "depot too large (" + std::to_string(listed->size) + " bytes)");
  const auto folder = DepotFolder(cache_root, depot);
  if (!MakeFolders(folder)) return finish(Status::kFailed, "could not create the cache folder");
  InstallTarget target;
  target.app_id = kCommonRedistApp;
  target.install_root = folder;
  DepotInfo wanted = *listed;
  wanted.source_app = kCommonRedistApp;
  wanted.owner_app = kCommonRedistApp;
  wanted.shared_install = false;
  wanted.depot_from_app.reset();
  target.depots = {wanted};
  const auto outcome = installer_.Run(*session, target, cancel);
  switch (outcome.error) {
    case InstallError::kNone: {
      if (!CachedSharedDepot(cache_root, depot)) return finish(Status::kFailed, "downloaded, but its record is unreadable");
      out.folder = folder;
      out.downloaded = true;
      if (file_name.empty()) return finish(Status::kReady, "");
      std::string why;
      const auto file = VerifiedSharedDepotFile(folder, file_name, &why);
      if (!file) return finish(Status::kFailed, "downloaded, but " + why);
      out.file = *file;
      return finish(Status::kReady, "");
    }
    case InstallError::kCanceled: return finish(Status::kCanceled, "canceled");
    case InstallError::kRejected: return finish(Status::kRefused, outcome.detail);
    case InstallError::kNetwork:
    case InstallError::kNoSession: return finish(Status::kUnavailable, std::string(InstallErrorName(outcome.error)) + " " + outcome.detail);
    default: return finish(Status::kFailed, std::string(InstallErrorName(outcome.error)) + " " + outcome.detail);
  }
}

}  // namespace playden::steam
