// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Walks a fake data tree and lists what FTP (root, with no permission bypass on /data) could not
// delete or read: a folder of the app user's that is not 0777, and a file of the app user's that
// others can't read, outside the named exceptions. Links are not followed.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace playden::test {

struct OpenModesRules {
  // Relative to the walked root: a folder skipped with everything under it (a live bottle until the
  // runtime kit opens them).
  std::function<bool(const std::filesystem::path&)> skip;
  // Relative to the walked root: a file allowed to stay owner-only (a secret, game-launch.json).
  std::function<bool(const std::filesystem::path&)> private_file;
};

inline std::string OctalMode(unsigned mode) {
  char text[8];
  std::snprintf(text, sizeof(text), "%04o", mode & 07777);
  return text;
}

inline void WalkOpenModes(const std::filesystem::path& root, const std::filesystem::path& relative,
                          const OpenModesRules& rules, std::vector<std::string>* problems) {
  const auto path = relative.empty() ? root : root / relative;
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0 || S_ISLNK(info.st_mode)) return;
  const bool mine = info.st_uid == ::geteuid();
  const auto mode = info.st_mode & 07777;
  if (S_ISREG(info.st_mode)) {
    if (mine && (mode & 0004) == 0 && !(rules.private_file && rules.private_file(relative)))
      problems->push_back("file " + relative.string() + " mode " + OctalMode(mode));
    return;
  }
  if (!S_ISDIR(info.st_mode)) return;
  if (!relative.empty() && rules.skip && rules.skip(relative)) return;
  if (mine && (mode & 0777) != 0777) problems->push_back("folder " + relative.string() + " mode " + OctalMode(mode));
  DIR* listing = ::opendir(path.c_str());
  if (!listing) return;
  std::vector<std::string> names;
  while (const auto* entry = ::readdir(listing)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") names.push_back(name);
  }
  ::closedir(listing);
  for (const auto& name : names) WalkOpenModes(root, relative / name, rules, problems);
}

inline std::vector<std::string> OpenModesProblems(const std::filesystem::path& root, const OpenModesRules& rules = {}) {
  std::vector<std::string> problems;
  WalkOpenModes(root, {}, rules, &problems);
  return problems;
}

}  // namespace playden::test
