// gog_icon.cpp - The icon a GOG copy of a game ships in its folder (goggame-<id>.info and .ico).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/gog_icon.hpp"

#include "steam/backend/json.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::homebrew {
namespace {

namespace json = playden::steam::json;

constexpr std::size_t kMaxListedEntries = 4096;  // in the game's folder
constexpr std::size_t kMaxInfoFiles = 4;
constexpr std::size_t kMaxInfoBytes = 256 * 1024;
constexpr std::size_t kMaxNameBytes = 255;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return out;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}
bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// An absolute path none of whose components is a symbolic link.
bool NoLinks(const std::string& path) {
  if (path.empty() || path.front() != '/') return false;
  for (std::size_t end = path.find('/', 1);; end = path.find('/', end + 1)) {
    struct stat info {};
    if (::lstat(path.substr(0, end).c_str(), &info) != 0 || S_ISLNK(info.st_mode)) return false;
    if (end == std::string::npos) return true;
  }
}

bool IsRegularFile(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// A small regular file's bytes; opened without following a link or blocking on a FIFO.
std::optional<std::string> ReadSmall(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  struct stat info {};
  std::optional<std::string> bytes;
  if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && static_cast<std::uint64_t>(info.st_size) <= kMaxInfoBytes) {
    std::string read(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < read.size()) {
      const auto got = ::read(fd, read.data() + done, read.size() - done);
      if (got <= 0) break;
      done += static_cast<std::size_t>(got);
    }
    if (done == read.size()) bytes = std::move(read);
  }
  ::close(fd);
  return bytes;
}

// A path as GOG and Windows write it, to compare: '/' between parts, no leading "./", lower case.
std::string Normal(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  while (StartsWith(path, "./")) path.erase(0, 2);
  while (StartsWith(path, "/")) path.erase(0, 1);
  return Lower(path);
}

// A plain file name in the game's folder ending in .ico.
bool PlainIconName(const std::string& name) {
  if (name.empty() || name.size() > kMaxNameBytes || !EndsWith(Lower(name), ".ico")) return false;
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
      name.find("..") != std::string::npos)
    return false;
  return std::none_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

struct Task {
  std::string category, path, icon;
};

// The play tasks of one .info file; nothing when it isn't the JSON GOG writes. `base`: it is the base game's.
std::optional<std::vector<Task>> Tasks(const std::string& text, bool* base) {
  const auto root = json::Parse(text, {kMaxInfoBytes, 8, 16 * 1024, 4096});
  if (!root || !root->is_object()) return std::nullopt;
  const auto* game = root->Find("gameId");
  const auto* root_game = root->Find("rootGameId");
  *base = game && root_game && game->is_string() && root_game->is_string() && game->text() == root_game->text();
  const auto* tasks = root->Find("playTasks");
  if (!tasks || !tasks->is_array()) return std::nullopt;
  std::vector<Task> out;
  for (const auto& task : tasks->array()) {
    if (!task.is_object()) continue;
    const auto text_of = [&task](std::string_view key) {
      const auto* value = task.Find(key);
      return value && value->is_string() ? value->text() : std::string();
    };
    const auto type = text_of("type");
    if (!type.empty() && type != "FileTask") continue;
    out.push_back({text_of("category"), text_of("path"), text_of("icon")});
  }
  return out;
}

}  // namespace

std::string GogIconFile(const std::string& folder, const std::string& program) {
  if (!NoLinks(folder)) return {};
  std::vector<std::string> infos, icons;
  if (DIR* directory = ::opendir(folder.c_str())) {
    std::size_t listed = 0;
    while (const dirent* entry = ::readdir(directory)) {
      if (++listed > kMaxListedEntries) break;
      const std::string name = entry->d_name;
      const auto lower = Lower(name);
      if (!StartsWith(lower, "goggame-") || name.size() > kMaxNameBytes) continue;
      if (EndsWith(lower, ".info")) infos.push_back(name);
      else if (EndsWith(lower, ".ico")) icons.push_back(name);
    }
    ::closedir(directory);
  }
  std::sort(infos.begin(), infos.end());
  const auto usable = [&folder](const std::string& name) {
    return PlainIconName(name) && IsRegularFile(folder + "/" + name);
  };
  // The base game's play tasks first (a DLC has its own .info beside it).
  std::vector<std::vector<Task>> files;
  std::size_t bases = 0;
  for (std::size_t i = 0; i < infos.size() && i < kMaxInfoFiles; ++i) {
    const auto text = ReadSmall(folder + "/" + infos[i]);
    bool base = false;
    auto tasks = text ? Tasks(*text, &base) : std::nullopt;
    if (!tasks) continue;
    files.insert(files.begin() + static_cast<std::ptrdiff_t>(base ? bases++ : files.size()), std::move(*tasks));
  }
  const auto wanted = Normal(program);
  if (!wanted.empty())
    for (const auto& tasks : files)
      for (const auto& task : tasks)
        if (!task.icon.empty() && Normal(task.path) == wanted && usable(task.icon)) return task.icon;
  for (const auto& tasks : files)
    for (const auto& task : tasks)
      if (task.category == "game" && !task.icon.empty() && usable(task.icon)) return task.icon;
  if (icons.size() == 1 && usable(icons.front())) return icons.front();
  return {};
}

}  // namespace playden::homebrew
