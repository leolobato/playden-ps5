// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/managed_directory.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Steam Workshop items (#856) handed to gbe_fork in Offline emulation (Steam features = emulate or steamclient).
// gbe_fork (dll/settings_parser.cpp, parse_mods_folder) reads steam_settings/mods.json; without one (or with one it
// can't parse) it lists every steam_settings/mods/<number>/ folder itself. An entry without "path" means
// steam_settings/mods/<key>. Its GetItemInstallInfo answers with "path" and "primary_filesize" (an int32).
//
// Playden writes mods.json only while it has items to hand over, and lists beside them each mods/<number>/ folder
// placed by hand, which its file would otherwise hide. steam_settings/.playden-mods holds the SHA-256 of the mods.json
// Playden wrote (one per line; two only while it replaces it). A mods.json that matches none was made by hand: it is
// never written over or removed.
inline constexpr const char* kWorkshopModsFile = "mods.json";
inline constexpr const char* kWorkshopModsMarker = ".playden-mods";
// Where the items are inside the bottle, as Windows sees them: <W>\content\<appid>\<id> (steam/backend/workshop_items.h).
inline constexpr const char* kWorkshopContentWindowsFolder = "C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content";
// A mods.json larger than this is not read, so never taken for Playden's.
inline constexpr std::size_t kWorkshopModsMaxBytes = 4u << 20;

// One item the player turned on, from the bottle's Workshop state.
struct WorkshopMod {
  std::uint64_t id = 0;
  std::string title;
  std::uint32_t time_updated = 0;
  std::uint64_t bytes = 0;            // total_files_sizes; primary_filesize when there is no primary file
  std::string primary_filename;       // empty: none
  std::uint64_t primary_bytes = 0;    // the primary file's size, when primary_filename is set
  // A picture beside it in the item's folder, named to gbe_fork by its full path (not steam_settings/mod_images).
  std::string preview_filename;       // empty: none
  std::uint64_t preview_bytes = 0;
  // The author's SteamID; 0: none named, and gbe_fork shows the player as the author.
  std::uint64_t owner = 0;
};

// text as a JSON string (quotes included): quote, backslash and control characters escaped, every byte sequence that
// is not valid UTF-8 (overlong, surrogate, past U+10FFFF, cut short) replaced by U+FFFD. gbe_fork's JSON parser
// refuses the whole file for one bad byte.
std::string WorkshopJsonString(std::string_view text);

// mods.json for items (their folders checked already) and the hand-placed folder names (all digits) in
// steam_settings/mods/. Ordered by ID; a hand-placed folder wins over an item with the same ID (the player put it
// there), and a second hand-placed folder for an ID already listed is left out.
std::string BuildWorkshopModsJson(std::uint32_t app_id, const std::vector<WorkshopMod>& items,
                                  const std::vector<std::string>& hand_placed);

// The items whose folder <workshop_folder>/content/<app_id>/<id> is a real folder (not a link). A primary_filename or
// preview_filename that is not a plain name of a regular file in that folder is dropped; one that is gets its size.
std::vector<WorkshopMod> PresentWorkshopMods(const std::string& workshop_folder, std::uint32_t app_id,
                                             std::vector<WorkshopMod> items);

// The all-digit folder names in <settings>/mods/ (real folders, no links), sorted by number.
std::vector<std::string> HandPlacedMods(const platform::ManagedDirectory& settings);

enum class WorkshopModsOutcome {
  kNone,      // nothing to hand over and no file of Playden's
  kWritten,   // Playden's mods.json written
  kKept,      // Playden's mods.json already says this
  kRemoved,   // Playden's mods.json (and its marker) removed
  kHandMade,  // a mods.json Playden didn't write: left alone
  kFailed,    // the folder, or one on the way to it, is not a real one; or a write failed
};
const char* WorkshopModsOutcomeName(WorkshopModsOutcome outcome);

struct WorkshopModsResult {
  WorkshopModsOutcome outcome = WorkshopModsOutcome::kNone;
  std::size_t hand_placed = 0;  // hand-placed folders listed in the file written
};

// Puts items' mods.json in the steam_settings/ folder at <root>/<relative>, which must exist, or, with no items,
// removes Playden's. The folder is opened from `root`, the folder the caller trusts (the game's install root in
// Offline emulation, the bottle's drive_c for the client in it), through ManagedDirectory with `policy`: every
// folder on the way must be a real one, never a link, and each read, write and removal happens in the folder so
// opened. Writes are atomic; a link at mods.json or the marker is never followed or replaced.
WorkshopModsResult ApplyWorkshopMods(const std::string& root, platform::DirectoryPolicy policy, const std::string& relative,
                                     std::uint32_t app_id, const std::vector<WorkshopMod>& items);
// ApplyWorkshopMods with no items; a folder missing on the way is kNone.
WorkshopModsResult RemoveWorkshopMods(const std::string& root, platform::DirectoryPolicy policy, const std::string& relative);

}  // namespace playden::runtime
