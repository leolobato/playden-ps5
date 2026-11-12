// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace playden::runtime {

// A folder of a game's Steam Cloud saves inside the bottle's Windows user folder (drive_c/users/prospero), as the
// download's facts name it and the bottle gets it (SteamworksIdentity::save_folders, InstallService::AddProfileFolders):
// a path relative to that folder with '/' separators, at most kMaxProfileSaveFolderDepth plain names (no ".", "..",
// empty part, drive, wildcard, placeholder or control character) and kMaxProfileSaveFolderBytes in all.
inline constexpr std::size_t kMaxProfileSaveFolders = 32;
inline constexpr std::size_t kMaxProfileSaveFolderDepth = 16;
inline constexpr std::size_t kMaxProfileSaveFolderBytes = 512;

inline bool IsProfileSaveFolder(std::string_view folder) {
  if (folder.empty() || folder.size() > kMaxProfileSaveFolderBytes || folder.front() == '/') return false;
  for (const char value : folder) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte < 0x20 || byte == 0x7f || value == '\\' || value == ':' || value == '*' || value == '?' || value == '"' ||
        value == '<' || value == '>' || value == '|' || value == '%' || value == '{' || value == '}')
      return false;
  }
  std::size_t parts = 0, start = 0;
  while (start <= folder.size()) {
    const auto end = std::min(folder.find('/', start), folder.size());
    const auto part = folder.substr(start, end - start);
    if (part.empty() || part == "." || part == ".." || ++parts > kMaxProfileSaveFolderDepth) return false;
    start = end + 1;
  }
  return true;
}

}  // namespace playden::runtime
