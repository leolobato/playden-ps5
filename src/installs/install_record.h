// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "library/store.h"

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

// What a GOG install holds: the build, its generation, the product IDs installed (the base game first, then each DLC),
// the GOG language code ("en-US"), the build meta's hash (32 hex digits) and the owned DLC the player turned off
// (sorted, unique, never the base game): an update leaves them out too.
struct GogRecord {
  std::string build_id;
  int generation = 2;
  std::vector<std::uint64_t> products;
  std::string language;
  std::string meta_id;
  std::vector<std::uint64_t> excluded;
  bool operator==(const GogRecord& other) const {
    return build_id == other.build_id && generation == other.generation && products == other.products &&
           language == other.language && meta_id == other.meta_id && excluded == other.excluded;
  }
};

// At most this many products and excluded DLC in a GOG record; a longer list makes the "gog" object unreadable.
inline constexpr std::size_t kMaxGogRecordProducts = 1024;

// A program the install's own files name (GOG's play tasks), kept so Play works signed out and offline. The paths
// are relative to the game folder with `/`; an empty working directory is the game folder itself.
struct RecordedLaunch {
  std::string executable;
  std::string arguments;
  std::string working_directory;
  std::string label;
  bool operator==(const RecordedLaunch& other) const {
    return executable == other.executable && arguments == other.arguments &&
           working_directory == other.working_directory && label == other.label;
  }
};

inline constexpr std::size_t kMaxRecordedLaunches = 32;
inline constexpr std::size_t kMaxRecordedLaunchBytes = 4096;  // each string of a launch

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
  // DLC the player turned off when installing: its updates leave them out too, and they don't count as new DLC.
  std::vector<std::uint32_t> excluded_dlc;
  // A GOG install's build and products; nothing for every other store.
  std::optional<GogRecord> gog;
  // The programs the install's files name (GOG); empty for Steam, whose launch options come from appinfo.
  std::vector<RecordedLaunch> launches;
  bool operator==(const InstalledRecord& other) const {
    return version == other.version && app_id == other.app_id && install_bytes == other.install_bytes &&
           branch == other.branch && language == other.language && depots == other.depots &&
           excluded_dlc == other.excluded_dlc && gog == other.gog && launches == other.launches;
  }
};

// Whether a depot of the install is a DLC's. Steam may then offer launch options that it didn't before the install.
inline bool HoldsDlc(const InstalledRecord& record) {
  for (const auto& depot : record.depots)
    if (depot.owner != 0 && depot.owner != record.app_id) return true;
  return false;
}

inline constexpr const char* kInstallRecordName = ".playden-install.json";
// Left in the game's root by an update that changed its files (written before the update's record): the
// game's launch, set up from the old files, is made again from the new ones before it starts, and the
// runtime then deletes it. Its content is not read.
inline constexpr const char* kFilesChangedName = ".playden-files-changed";
inline constexpr std::size_t kMaxInstallRecordBytes = 1024 * 1024;

// Reads a record's text: a JSON object; unknown keys are skipped and missing ones keep the defaults.
// Nothing for text that isn't a JSON object or a depot entry without an id. A malformed "gog" object or "launches"
// entry is left out, never the whole record: Steam records go through the same parser. At most
// kMaxRecordedLaunches launches are read, each string at most kMaxRecordedLaunchBytes.
std::optional<InstalledRecord> ParseInstallRecord(std::string_view text);

// A GOG install's record:
// {"version":2,"app_id":0,"store":"gog","game":"<id>","install_bytes":N,"files":N,"branch":"public",
//  "language":"<steam code>","depots":[],"gog":{"build_id":"..","generation":2,"products":["<id>"],
//  "language":"en-US","meta":"<hash>","excluded":["<id>"]},"launches":[{"executable":"..","arguments":"..",
//  "working_directory":"..","label":".."}]}
// "install_bytes" comes early and without a space, as InstallService reads it from the first 4096 bytes. No depots:
// the Steam DLC fields stay Steam's. "excluded" (the DLC turned off, as canonical decimal strings) is written only when
// it lists one, so a record without it is as earlier builds wrote it; at most kMaxGogRecordProducts are written,
// sorted, without repeats or the base game. Earlier builds skip the key: an update made by one of them brings the DLC
// back. Launches past kMaxRecordedLaunches, or with an empty program or a string over
// kMaxRecordedLaunchBytes, are left out, as ParseInstallRecord would leave them.
std::string FormatGogInstallRecord(const GameId& game, std::uint64_t install_bytes, std::uint64_t files,
                                   const std::string& steam_language, const GogRecord& gog,
                                   const std::vector<RecordedLaunch>& launches);
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
