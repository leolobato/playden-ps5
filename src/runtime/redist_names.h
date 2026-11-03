// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Where a game keeps its redistributables and what PhysX's installers are called. Play's search for a package
// (runtime/redist_install.cpp) and the web page's list of folders use these same rules.
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace playden::runtime {

inline constexpr int kRedistMaxDepth = 4;
inline constexpr std::size_t kMaxPhysxPackages = 8;

inline std::string RedistLower(std::string text) {
  for (auto& c : text)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return text;
}

// Folders where Steam games keep their redistributables, looked into below the game's top level.
inline bool RedistFolder(const std::string& name) {
  static const std::set<std::string> kNames = {
      "_commonredist", "commonredist", "redist", "redists", "_redist", "redistributables", "redistributable",
      "support", "__installer", "_installer", "installer", "installers", "prerequisites", "prereqs", "physx",
      "directx", "vcredist", "dotnet"};
  return kNames.count(RedistLower(name)) != 0;
}

inline bool PhysxPackageName(const std::string& name) {
  const auto lower = RedistLower(name);
  if (lower.rfind("physx", 0) != 0 || lower.size() <= 4) return false;
  const auto extension = lower.substr(lower.size() - 4);
  return extension == ".msi" || extension == ".exe";
}

// PhysX's installers in the game's redistributable folders, top-level ones or ones a level down (a game whose
// files sit in one folder of what was imported), by name.
inline void FindPhysxPackages(const std::filesystem::path& folder, const std::string& relative, int depth, bool in_redist,
                              std::vector<std::string>* packages) {
  std::error_code error;
  std::vector<std::pair<std::string, bool>> children;
  for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
    std::error_code type_error;
    if (it->is_symlink(type_error)) continue;
    children.emplace_back(it->path().filename().string(), it->is_directory(type_error));
  }
  std::sort(children.begin(), children.end());
  for (const auto& [name, directory] : children) {
    if (packages->size() >= kMaxPhysxPackages) return;
    const auto path = relative.empty() ? name : relative + "/" + name;
    if (!directory && in_redist && PhysxPackageName(name)) packages->push_back(path);
    else if (directory && depth < kRedistMaxDepth && (in_redist || depth == 0 || RedistFolder(name)))
      FindPhysxPackages(folder / name, path, depth + 1, in_redist || RedistFolder(name), packages);
  }
}

}  // namespace playden::runtime
