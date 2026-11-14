// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A GOG build's goggame-<product>.info: the game's play tasks, read into the launch options a GOG game offers. The
// install runner reads it on its worker at the end of an install and keeps the result in the install record, so Play
// works signed out and offline (the controller's main thread never reads files).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "stores/game_details.h"

namespace playden::gog {

inline constexpr std::size_t kMaxInfoBytes = 1024 * 1024;
inline constexpr std::size_t kMaxLaunchOptions = 32;
inline constexpr std::size_t kMaxLaunchLabelBytes = 120;
inline constexpr std::size_t kMaxLaunchArgumentBytes = 4096;

struct PlayTask {
  std::string type;      // "FileTask" or "URLTask"
  std::string category;  // "game", "launcher", "tool", "document", ...
  std::string path;      // as written: `\` or `/`, any case
  std::string arguments;
  std::optional<std::string> working_dir;
  std::string name;
  bool primary = false;
  bool hidden = false;
};

struct Info {
  std::string game_id;
  std::string root_game_id;
  std::string build_id;
  std::string name;
  std::vector<PlayTask> tasks;
};

// The file's text, after a UTF-8 byte-order mark. Nothing when it is over kMaxInfoBytes or not a JSON object. Fields
// Playden doesn't use (icon, languages, compatibilityFlags, osBitness, ...) are ignored; a play task that isn't an
// object is skipped.
std::optional<Info> ParseInfo(std::string_view text);

// The launch options, the default first:
// - the first FileTask with isPrimary, else the first FileTask (never a URLTask);
// - when that default is a `launcher`, the hidden `game` FileTasks right after it: which one plays better under Wine
//   is not settled, so the game stays one choice away;
// - then every other FileTask that is neither hidden nor a `document`, in file order.
// A task whose path or working folder NormalizePath refuses, or whose arguments hold a control character or are over
// kMaxLaunchArgumentBytes, is left out before the default is chosen. The path keeps its case (the runtime finds the
// file ignoring case). An absent or empty workingDir is "": the install root, as GOG starts it. The label is the task
// name, cut to kMaxLaunchLabelBytes. At most kMaxLaunchOptions.
std::vector<stores::LaunchOption> LaunchesFromInfo(const Info& info);

// The absolute path of goggame-<product>.info at the install root, matched ignoring case in the root's listing and
// spelled as the listing has it; nothing when there is none or it isn't a regular file.
std::optional<std::string> FindInfoFile(const std::string& root, std::uint64_t product);

// The file at `path`, read and parsed; nothing when it is missing, a link, too large or doesn't parse.
std::optional<Info> ReadInfo(const std::string& path);

}  // namespace playden::gog
