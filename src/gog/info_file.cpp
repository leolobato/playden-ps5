// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/info_file.h"

#include "gog/path_rules.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::gog {
namespace {

using Value = steam::json::Value;

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::size_t kMaxTasks = 256;

std::string Text(const Value& object, std::string_view key) {
  const auto* value = object.Find(key);
  if (!value) return {};
  if (value->is_string()) return value->text();
  // IDs are strings in every file seen; a number is read as one too.
  if (value->is_number())
    if (const auto number = value->AsUint64()) return std::to_string(*number);
  return {};
}

bool Flag(const Value& object, std::string_view key) {
  const auto* value = object.Find(key);
  return value && value->is_bool() && value->boolean();
}

bool HasControl(std::string_view text) {
  for (const char c : text)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return true;
  return false;
}

// At most kMaxLaunchLabelBytes, never splitting a UTF-8 character (the record's JSON must stay valid UTF-8); control
// characters become spaces.
std::string Label(std::string_view name) {
  std::size_t end = std::min(name.size(), kMaxLaunchLabelBytes);
  // A cut before a continuation byte would split a character: move back to where it starts.
  while (end > 0 && end < name.size() && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80) --end;
  std::string label(name.substr(0, end));
  for (char& c : label)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
  return label;
}

bool IsFileTask(const PlayTask& task) { return task.type == "FileTask"; }

}  // namespace

std::optional<Info> ParseInfo(std::string_view text) {
  if (text.substr(0, kBom.size()) == kBom) text.remove_prefix(kBom.size());
  if (text.size() > kMaxInfoBytes) return std::nullopt;
  steam::json::Limits limits;
  limits.max_bytes = kMaxInfoBytes;
  limits.max_depth = 16;
  limits.max_values = 64 * 1024;
  limits.max_string_bytes = 64 * 1024;
  const auto root = steam::json::Parse(text, limits);
  if (!root || !root->is_object()) return std::nullopt;
  Info info;
  info.game_id = Text(*root, "gameId");
  info.root_game_id = Text(*root, "rootGameId");
  info.build_id = Text(*root, "buildId");
  info.name = Text(*root, "name");
  if (const auto* tasks = root->Find("playTasks"); tasks && tasks->is_array()) {
    for (const auto& entry : tasks->array()) {
      if (info.tasks.size() >= kMaxTasks) break;
      if (!entry.is_object()) continue;
      PlayTask task;
      task.type = Text(entry, "type");
      task.category = Text(entry, "category");
      task.path = Text(entry, "path");
      task.arguments = Text(entry, "arguments");
      if (const auto* working = entry.Find("workingDir"); working && working->is_string())
        task.working_dir = working->text();
      task.name = Text(entry, "name");
      task.primary = Flag(entry, "isPrimary");
      task.hidden = Flag(entry, "isHidden");
      info.tasks.push_back(std::move(task));
    }
  }
  return info;
}

std::vector<stores::LaunchOption> LaunchesFromInfo(const Info& info) {
  // Every FileTask that can start, in file order, as the launch option it becomes.
  struct Usable {
    const PlayTask* task;
    stores::LaunchOption option;
  };
  std::vector<Usable> usable;
  for (const auto& task : info.tasks) {
    if (!IsFileTask(task)) continue;
    const auto path = NormalizePath(task.path);
    if (!path) continue;
    if (HasControl(task.arguments) || task.arguments.size() > kMaxLaunchArgumentBytes) continue;
    std::string working;
    if (task.working_dir && !task.working_dir->empty()) {
      const auto directory = NormalizePath(*task.working_dir);
      if (!directory) continue;
      working = *directory;
    }
    usable.push_back({&task, {*path, task.arguments, working, Label(task.name)}});
  }
  std::vector<stores::LaunchOption> launches;
  if (usable.empty()) return launches;
  std::size_t primary = 0;
  for (std::size_t index = 0; index < usable.size(); ++index)
    if (usable[index].task->primary) {
      primary = index;
      break;
    }
  std::vector<bool> taken(usable.size(), false);
  const auto take = [&](std::size_t index) {
    if (taken[index] || launches.size() >= kMaxLaunchOptions) return;
    taken[index] = true;
    launches.push_back(usable[index].option);
  };
  take(primary);
  if (usable[primary].task->category == "launcher")
    for (std::size_t index = 0; index < usable.size(); ++index)
      if (usable[index].task->hidden && usable[index].task->category == "game") take(index);
  for (std::size_t index = 0; index < usable.size(); ++index)
    if (!usable[index].task->hidden && usable[index].task->category != "document") take(index);
  return launches;
}

std::optional<std::string> FindInfoFile(const std::string& root, std::uint64_t product) {
  if (root.empty() || root.front() != '/') return std::nullopt;
  const std::string name = "goggame-" + std::to_string(product) + ".info";
  const auto regular = [](const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
  };
  // Always by the listing, so the result is the name as the folder spells it (the host's volume may ignore case).
  DIR* listing = ::opendir(root.c_str());
  if (!listing) return std::nullopt;
  const auto folded = FoldCase(name);
  std::optional<std::string> found;
  while (const auto* entry = ::readdir(listing))
    if (FoldCase(entry->d_name) == folded && regular(root + "/" + entry->d_name)) {
      found = root + "/" + entry->d_name;
      break;
    }
  ::closedir(listing);
  return found;
}

std::optional<Info> ReadInfo(const std::string& path) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return std::nullopt;
  struct stat info {};
  bool ok = ::fstat(file, &info) == 0 && S_ISREG(info.st_mode) &&
            static_cast<std::uint64_t>(info.st_size) <= kMaxInfoBytes + kBom.size();
  std::string text;
  char buffer[4096];
  while (ok) {
    const auto count = ::read(file, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || text.size() + static_cast<std::size_t>(count) > kMaxInfoBytes + kBom.size()) ok = false;
    else if (count == 0) break;
    else text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(file);
  if (!ok) return std::nullopt;
  return ParseInfo(text);
}

}  // namespace playden::gog
