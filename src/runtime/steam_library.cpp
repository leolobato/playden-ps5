// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/steam_library.h"

#include "platform/managed_directory.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <optional>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;

// A VDF string: quoted, with \ and " escaped as the client writes them; control characters dropped.
std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '\\' || c == '"') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out + "\"";
}

std::string Line(int depth, std::string_view key, std::string_view value) {
  return std::string(static_cast<std::size_t>(depth), '\t') + Quoted(key) + "\t\t" + Quoted(value) + "\n";
}

std::string Open(int depth, std::string_view key) {
  const std::string tabs(static_cast<std::size_t>(depth), '\t');
  return tabs + Quoted(key) + "\n" + tabs + "{\n";
}

std::string Close(int depth) { return std::string(static_cast<std::size_t>(depth), '\t') + "}\n"; }

std::optional<std::string> ReadSmall(const std::filesystem::path& path) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return std::nullopt;
  std::string text;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(file, buffer, sizeof buffer);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0 || text.size() > 256 * 1024) break;
    text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(file);
  return text;
}

// Whether every component of an absolute path is a real directory, none a link: the client's folder is open to
// everyone, so a link put in its place must not send a write elsewhere.
bool NoLinkOnTheWay(const std::filesystem::path& folder) {
  if (!folder.is_absolute()) return false;
  std::filesystem::path walked;
  for (const auto& part : folder) {
    walked /= part;
    struct stat info {};
    if (::lstat(walked.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
  }
  return true;
}

// The client's folder is open to everyone (runtime/steam_client_install.h): a plain write through a temporary file,
// in a folder with no link on the way.
bool WriteOpen(const std::filesystem::path& folder, const std::string& name, const std::string& text) {
  if (!NoLinkOnTheWay(folder.parent_path())) return false;
  if (::mkdir(folder.c_str(), 0777) != 0 && errno != EEXIST) return false;
  if (!NoLinkOnTheWay(folder)) return false;
  const auto path = folder / name;
  const auto temporary = folder / (name + ".playden-tmp");
  ::unlink(temporary.c_str());
  const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
  if (file < 0) return false;
  ::fchmod(file, 0666);
  std::size_t written = 0;
  while (written < text.size()) {
    const auto count = ::write(file, text.data() + written, text.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    written += static_cast<std::size_t>(count);
  }
  const bool closed = ::close(file) == 0;
  const bool ok = written == text.size() && closed;
  if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  return true;
}

// Whether a libraryfolders.vdf has a library whose "path" is the bottle's: a "path" key followed, on its line, by that
// value. Not a full VDF parse; the client writes one key and value per line.
bool ListsBottleLibrary(const std::string& text) {
  const std::string key = "\"path\"", value = Quoted(kSteamLibraryWindowsPath);
  for (std::size_t at = 0; at < text.size();) {
    const auto end = std::min(text.find('\n', at), text.size());
    std::string_view line(text.data() + at, end - at);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
    if (line.size() > key.size() && line.compare(0, key.size(), key) == 0) {
      auto rest = line.substr(key.size());
      while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
      if (rest == value) return true;
    }
    at = end + 1;
  }
  return false;
}

}  // namespace

std::string SteamLibraryFoldersText(const std::string& client_windows_path) {
  std::string text = Open(0, "libraryfolders");
  const auto library = [&](const char* index, const std::string& path) {
    text += Open(1, index);
    text += Line(2, "path", path);
    text += Line(2, "label", "");
    text += Line(2, "contentid", "0");
    text += Line(2, "totalsize", "0");
    text += Open(2, "apps");
    text += Close(2);
    text += Close(1);
  };
  library("0", client_windows_path);
  library("1", kSteamLibraryWindowsPath);
  return text + Close(0);
}

std::string SteamAppManifestName(std::uint32_t app_id) { return "appmanifest_" + std::to_string(app_id) + ".acf"; }

std::string SteamAppManifestText(const SteamAppManifest& manifest) {
  const auto& record = manifest.record;
  std::string text = Open(0, "AppState");
  text += Line(1, "appid", std::to_string(manifest.app_id));
  text += Line(1, "Universe", "1");
  text += Line(1, "name", manifest.name.empty() ? std::to_string(manifest.app_id) : manifest.name);
  text += Line(1, "StateFlags", "4");
  text += Line(1, "installdir", manifest.install_dir);
  text += Line(1, "LastUpdated", std::to_string(manifest.last_updated));
  text += Line(1, "SizeOnDisk", std::to_string(record.install_bytes));
  text += Line(1, "buildid", "0");
  text += Line(1, "AutoUpdateBehavior", "0");
  text += Line(1, "AllowOtherDownloadsWhileRunning", "0");
  text += Line(1, "ScheduledAutoUpdate", "0");
  text += Open(1, "InstalledDepots");
  for (const auto& depot : record.depots) {
    if (!depot.id || !depot.gid) continue;
    text += Open(2, std::to_string(depot.id));
    text += Line(3, "manifest", std::to_string(depot.gid));
    text += Line(3, "size", "0");
    if (depot.owner && depot.owner != manifest.app_id) text += Line(3, "dlcappid", std::to_string(depot.owner));
    text += Close(2);
  }
  text += Close(1);
  text += Open(1, "UserConfig");
  text += Line(2, "language", record.language.empty() ? "english" : record.language);
  if (!record.branch.empty() && record.branch != "public") text += Line(2, "betakey", record.branch);
  text += Close(1);
  text += Open(1, "MountedConfig");
  text += Line(2, "language", record.language.empty() ? "english" : record.language);
  if (!record.branch.empty() && record.branch != "public") text += Line(2, "betakey", record.branch);
  text += Close(1);
  return text + Close(0);
}

bool WriteSteamLibraryFolders(const std::filesystem::path& client_folder, const std::string& client_windows_path) {
  const auto text = SteamLibraryFoldersText(client_windows_path);
  bool ok = true;
  for (const char* sub : {"steamapps", "config"}) {
    const auto folder = client_folder / sub;
    const auto existing = NoLinkOnTheWay(folder) ? ReadSmall(folder / "libraryfolders.vdf") : std::nullopt;
    if (existing && ListsBottleLibrary(*existing)) continue;
    ok = WriteOpen(folder, "libraryfolders.vdf", text) && ok;
  }
  return ok;
}

bool WriteSteamAppManifest(const std::filesystem::path& prefix, std::uint32_t app_id, const std::string& text) {
  auto directory = ManagedDirectory::Open(prefix, DirectoryPolicy::kApprovedTree);
  for (const char* name : {"drive_c", "Program Files (x86)", "Steam", "steamapps"}) {
    if (!directory) return false;
    directory = directory->OpenChild(name, DirectoryPolicy::kApprovedTree, true);
  }
  if (!directory) return false;
  const auto name = SteamAppManifestName(app_id);
  if (const auto existing = ReadSmall(directory->path() / name); existing && *existing == text) return true;
  const auto temporary = name + ".playden-tmp";
  directory->RemoveRegularFile(temporary);
  const int file = directory->CreateExclusiveFile(temporary, 0644);
  if (file < 0) return false;
  std::size_t written = 0;
  while (written < text.size()) {
    const auto count = ::write(file, text.data() + written, text.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    written += static_cast<std::size_t>(count);
  }
  const bool closed = ::close(file) == 0;
  if (written != text.size() || !closed || !directory->RenameChild(temporary, *directory, name, true)) {
    directory->RemoveRegularFile(temporary);
    return false;
  }
  return true;
}

}  // namespace playden::runtime
