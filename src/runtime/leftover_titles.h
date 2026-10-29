// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime/install_service.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_set>
#include <vector>

namespace playden::runtime {

struct LeftoverTitle {
  std::string title_id;
  std::filesystem::path title_path;
  std::string name;
  std::uint64_t bytes = 0;
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
};

// Observations from lstat and a read-only walk. Paths are constructed by the
// detector from the root and validated title ID, never supplied by the lister.
struct LeftoverTitleCandidate {
  std::string title_id;
  bool is_directory = false;
  bool is_symlink = false;
  uid_t owner = 0;
  bool readable = false;
  std::string name;
  std::uint64_t bytes = 0;
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
};

using LeftoverTitleLister = std::function<std::optional<std::vector<LeftoverTitleCandidate>>(
    const std::filesystem::path&, uid_t, const std::unordered_set<std::string>& excluded_title_ids)>;
using LeftoverTitleActivity = std::function<NativeActivity(const std::string&)>;

std::optional<std::vector<LeftoverTitleCandidate>> ListLeftoverTitleCandidates(
    const std::filesystem::path& homebrew_root, uid_t effective_uid,
    const std::unordered_set<std::string>& excluded_title_ids);

// An unread repository is never interpreted as an empty one. Any record,
// including an interrupted removal, reserves its title regardless of state.
std::vector<LeftoverTitle> FindLeftoverTitles(
    const std::filesystem::path& homebrew_root, bool repository_loaded,
    const std::vector<RuntimeInstall>& records, const LeftoverTitleActivity& activity,
    uid_t effective_uid, const LeftoverTitleLister& lister = ListLeftoverTitleCandidates);

}  // namespace playden::runtime
