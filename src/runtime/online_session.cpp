#include "runtime/online_session.h"
#include "runtime/steam_host.h"

#include "platform/managed_directory.h"

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <initializer_list>
#include <optional>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;

constexpr const char* kTemporaryName = "online-session.txt.playden-tmp";
constexpr const char* kLaunchTemporaryName = "online-launch.txt.playden-tmp";

bool Clean(const std::string& value) {
  return !value.empty() && std::none_of(value.begin(), value.end(), [](char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
  });
}

// Overwrites and closes: the text held the token.
void Wipe(std::string* text) {
  std::fill(text->begin(), text->end(), '\0');
  text->clear();
}

// The folder, back at 0700 if something opened it up: it holds the token.
std::optional<ManagedDirectory> OpenFolder(const std::filesystem::path& prefix, bool create) {
  const auto drive = ManagedDirectory::Open(prefix / "drive_c", DirectoryPolicy::kApprovedTree);
  if (!drive) return std::nullopt;
  auto folder = drive->OpenChild("playden", DirectoryPolicy::kApprovedTree, create, 0700);
  struct stat status {};
  if (!folder || ::fstat(folder->fd(), &status) != 0) return std::nullopt;
  if ((status.st_mode & 07777) == 0700) return folder;
  // Opened again after the change: the open folder took the old mode as its identity.
  if (::fchmod(folder->fd(), 0700) != 0) return std::nullopt;
  folder.reset();
  return drive->OpenChild("playden", DirectoryPolicy::kApprovedTree);
}

// Writes text as name in the bottle's C:\playden (0600), through a temporary file. Wipes the text.
bool WriteFile(const std::filesystem::path& prefix, const char* name, const char* temporary, std::string* text) {
  const auto folder = OpenFolder(prefix, true);
  bool ok = false;
  if (folder) {
    folder->RemoveRegularFile(temporary);
    const int file = folder->CreateExclusiveFile(temporary, 0600);
    if (file >= 0) {
      std::size_t written = 0;
      while (written < text->size()) {
        const auto count = ::write(file, text->data() + written, text->size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += static_cast<std::size_t>(count);
      }
      ok = written == text->size() && ::fsync(file) == 0;
      ok = ::close(file) == 0 && ok;
      ok = ok && folder->RenameChild(temporary, *folder, name) && folder->Sync();
      if (!ok) folder->RemoveRegularFile(temporary);
    }
  }
  Wipe(text);
  return ok;
}

// Removes the named files from the bottle's C:\playden. True when none is left; removed says whether the first was there.
bool RemoveFiles(const std::filesystem::path& prefix, std::initializer_list<const char*> names, bool* removed) {
  if (removed) *removed = false;
  const auto drive = ManagedDirectory::Open(prefix / "drive_c", DirectoryPolicy::kApprovedTree);
  struct stat folder_status {};
  if (!drive) return errno == ENOENT;
  if (!drive->InspectChild("playden", &folder_status)) return errno == ENOENT;
  const auto folder = drive->OpenChild("playden", DirectoryPolicy::kApprovedTree);
  if (!folder) return false;
  bool ok = true;
  bool first = true;
  for (const char* name : names) {
    struct stat status {};
    if (folder->InspectChild(name, &status)) {
      const bool gone = folder->RemoveRegularFile(name);
      if (gone && removed && first) *removed = true;
      ok = gone && ok;
    } else {
      ok = errno == ENOENT && ok;
    }
    first = false;
  }
  return ok && folder->Sync();
}

}  // namespace

std::uint32_t BridgeLoginId(std::uint32_t playden_login_id) {
  // "ONLI": any fixed change keeps it stable for the device; 0 is never sent.
  const std::uint32_t id = playden_login_id ^ 0x4f4e4c49u;
  return id ? id : 1u;
}

std::string RenderOnlineSession(const OnlineSession& session) {
  if (!session.app_id || !session.steam_id || !session.login_id || !Clean(session.account_name) ||
      !Clean(session.refresh_token) || (!session.machine_name.empty() && !Clean(session.machine_name)) ||
      session.machine_id.size() > 512 ||
      (!session.invites.empty() && session.invites != "ask" && session.invites != "join" && session.invites != "show" &&
       session.invites != "off") ||
      (!session.client_sha256.empty() &&
       (session.client_sha256.size() != 64 ||
        !std::all_of(session.client_sha256.begin(), session.client_sha256.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))))
    return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string machine_id;
  for (const auto byte : session.machine_id) {
    machine_id.push_back(kHex[byte >> 4]);
    machine_id.push_back(kHex[byte & 15]);
  }
  return "app_id=" + std::to_string(session.app_id) + "\nsteam_id=" + std::to_string(session.steam_id) +
         "\naccount_name=" + session.account_name + "\nrefresh_token=" + session.refresh_token +
         "\nmachine_name=" + session.machine_name + "\nmachine_id=" + machine_id +
         "\nlogin_id=" + std::to_string(session.login_id) + "\ncell_id=" + std::to_string(session.cell_id) +
         "\npersona=" + (session.invisible ? "invisible" : "online") + "\n" +
         (session.keep ? "keep=1\n" : "") + (session.invites.empty() ? "" : "invites=" + session.invites + "\n") +
         (session.adapters_off ? "adapters=off\n" : "") +
         (session.notice ? "notice=1\n" : "") +
         (session.client_sha256.empty() ? "" : "client_sha256=" + session.client_sha256 + "\n");
}

bool WriteOnlineSession(const std::filesystem::path& prefix, const OnlineSession& session) {
  std::string text = RenderOnlineSession(session);
  if (text.empty()) return false;
  return WriteFile(prefix, kOnlineSessionFile, kTemporaryName, &text);
}

bool OnlineSessionExists(const std::filesystem::path& prefix) {
  const auto folder = OpenFolder(prefix, false);
  struct stat status {};
  return folder && folder->InspectChild(kOnlineSessionFile, &status) && S_ISREG(status.st_mode);
}

bool TightenOnlineSession(const std::filesystem::path& prefix) {
  const auto folder = OpenFolder(prefix, false);
  if (!folder) return false;
  const int file = folder->OpenRegularFile(kOnlineSessionFile, true);
  if (file < 0) return false;
  struct stat status {};
  const bool ok = ::fstat(file, &status) == 0 && ((status.st_mode & 07777) == 0600 || ::fchmod(file, 0600) == 0);
  ::close(file);
  return ok;
}

bool RemoveOnlineSession(const std::filesystem::path& prefix, bool* removed) {
  return RemoveFiles(prefix, {kOnlineSessionFile, kTemporaryName, kOnlineLaunchFile, kLaunchTemporaryName}, removed);
}

std::string RenderOnlineLaunch(const OnlineLaunch& launch, std::int64_t written_unix_seconds) {
  std::string text;
  if (launch.connect_lobby) text += "connect_lobby=" + std::to_string(launch.connect_lobby) + "\n";
  std::vector<std::uint64_t> invites;
  for (const auto id : launch.invites) {
    if (invites.size() == kOnlineLaunchMaxInvites) break;
    if (id && std::find(invites.begin(), invites.end(), id) == invites.end()) invites.push_back(id);
  }
  for (const auto id : invites) text += "invite=" + std::to_string(id) + "\n";
  if (text.empty()) return text;
  return "written=" + std::to_string(written_unix_seconds) + "\n" + text;
}

std::string RenderOnlineLaunch(const OnlineLaunch& launch) {
  return RenderOnlineLaunch(launch, static_cast<std::int64_t>(std::time(nullptr)));
}

bool WriteOnlineLaunch(const std::filesystem::path& prefix, const OnlineLaunch& launch) {
  std::string text = RenderOnlineLaunch(launch);
  if (text.empty()) return false;
  return WriteFile(prefix, kOnlineLaunchFile, kLaunchTemporaryName, &text);
}

bool SetHostNotify(const std::filesystem::path& prefix, bool on) {
  if (!on) return RemoveFiles(prefix, {kSteamHostNotify, "steam-host.ready.notify.playden-tmp"}, nullptr);
  if (const auto folder = OpenFolder(prefix, false)) {
    struct stat status {};
    if (folder->InspectChild(kSteamHostNotify, &status) && S_ISREG(status.st_mode)) return true;
  }
  std::string empty;
  return WriteFile(prefix, kSteamHostNotify, "steam-host.ready.notify.playden-tmp", &empty);
}

bool RemoveOnlineLaunch(const std::filesystem::path& prefix, bool* removed) {
  return RemoveFiles(prefix, {kOnlineLaunchFile, kLaunchTemporaryName}, removed);
}

bool RemoveSteamClientBottleSignIn(const std::filesystem::path& prefix, bool* removed) {
  if (removed) *removed = false;
  auto folder = ManagedDirectory::Open(prefix / "drive_c", DirectoryPolicy::kApprovedTree);
  if (!folder) return errno == ENOENT;
  // The client made these folders as it pleased: they are taken as they are.
  for (const char* name : {"users", "prospero", "AppData", "Local", "Steam"}) {
    struct stat status {};
    if (!folder->InspectChild(name, &status)) return errno == ENOENT;
    auto child = folder->OpenChild(name, DirectoryPolicy::kApprovedTree);
    if (!child) return false;
    folder = std::move(child);
  }
  struct stat status {};
  if (!folder->InspectChild("local.vdf", &status)) return errno == ENOENT;
  const bool gone = folder->RemoveRegularFile("local.vdf");
  if (removed) *removed = gone;
  return gone && folder->Sync();
}

bool RemoveOnlineSignIn(const std::filesystem::path& prefix, bool* removed) {
  bool handoff = false, token = false;
  const bool session_gone = RemoveOnlineSession(prefix, &handoff);
  const bool token_gone = RemoveSteamClientBottleSignIn(prefix, &token);
  if (removed) *removed = handoff || token;
  return session_gone && token_gone;
}

}  // namespace playden::runtime
