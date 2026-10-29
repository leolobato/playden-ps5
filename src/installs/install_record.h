// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::installs {

// One depot of an install, as its record names it.
struct RecordedDepot {
  std::uint32_t id = 0;
  std::uint64_t gid = 0;     // the manifest installed
  std::uint32_t owner = 0;   // whose content it is: the game, or a DLC
  std::uint32_t source = 0;  // the app whose appinfo lists it
  bool operator==(const RecordedDepot& other) const {
    return id == other.id && gid == other.gid && owner == other.owner && source == other.source;
  }
};

// What a finished install's record (.playden-install.json at the game's root) says it holds.
// Version 1 records (and imported games' copies, which list no depots) read as the public branch in
// English, each depot owned by and listed in the game itself.
struct InstalledRecord {
  int version = 1;
  std::uint32_t app_id = 0;
  std::uint64_t install_bytes = 0;
  std::string branch = "public";
  std::string language = "english";
  std::vector<RecordedDepot> depots;
  bool operator==(const InstalledRecord& other) const {
    return version == other.version && app_id == other.app_id && install_bytes == other.install_bytes &&
           branch == other.branch && language == other.language && depots == other.depots;
  }
};

inline constexpr const char* kInstallRecordName = ".playden-install.json";
// Left in the game's root by an update that changed its files (written before the update's record): the
// game's launch, set up from the old files, is made again from the new ones before it starts, and the
// runtime then deletes it. Its content is not read.
inline constexpr const char* kFilesChangedName = ".playden-files-changed";
inline constexpr std::size_t kMaxInstallRecordBytes = 1024 * 1024;

// Reads a record's text: a JSON object; unknown keys are skipped and missing ones keep the defaults.
// Nothing for text that isn't a JSON object or a depot entry without an id.
std::optional<InstalledRecord> ParseInstallRecord(std::string_view text);
// The record at `path`; nothing when it is missing, larger than kMaxInstallRecordBytes or unreadable.
std::optional<InstalledRecord> ReadInstallRecord(const std::string& path);

// A game update in progress keeps its work in <root>/.playden-update: "update.json" (this state), the chunk
// "journal", the changed files under "staging/", and the new tree, file list and record it finishes with.
// Phases: staging (downloading changed files; the game is untouched and playable), then replacing (staged
// files renamed over the game's) and deleting (files the new version dropped). Once replacing starts the
// update must finish before the game runs again; it needs no network for that.
inline constexpr const char* kUpdateDirName = ".playden-update";
inline constexpr const char* kUpdateStateName = "update.json";
enum class UpdatePhase { kStaging, kReplacing, kDeleting };
const char* UpdatePhaseName(UpdatePhase phase);
struct UpdateState {
  int version = 1;
  UpdatePhase phase = UpdatePhase::kStaging;
  std::vector<RecordedDepot> depots;  // the target manifests (id and gid only)
  std::string branch = "public";
  std::string language = "english";
  bool operator==(const UpdateState& other) const {
    return version == other.version && phase == other.phase && depots == other.depots && branch == other.branch &&
           language == other.language;
  }
};
// {"version":1,"phase":"staging|replacing|deleting","depots":[{"id":1,"manifest":"2"}],"branch":"..","language":".."}
std::string FormatUpdateState(const UpdateState& state);
// Nothing for text that isn't such an object or names no known phase.
std::optional<UpdateState> ParseUpdateState(std::string_view text);
// The state in <install_root>/.playden-update/update.json; nothing when there is none or it doesn't read.
std::optional<UpdateState> ReadUpdateState(const std::string& install_root);

}  // namespace playden::installs
