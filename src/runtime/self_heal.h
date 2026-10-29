// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Undoes a recursive chmod over FTP (often 777 on all of /data/playden) on what Playden owns.
// Private folders and files must be owner-only, and kit files must have their manifest modes,
// or startup and the runtime refuse them. Only tightens: no bit is ever added, entries of other
// owners and files with other links are left alone, links are never followed, and every change
// goes through a descriptor opened without following a link.
#include "runtime/writable_folders.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace playden::runtime {

struct SelfHealResult {
  std::size_t tightened = 0;  // folders and files given back their mode
  std::size_t foreign = 0;    // open or wrong entries of another owner: left, and logged
  std::size_t failed = 0;     // entries that could not be opened or changed
};

// Under data_root (/data/playden): runtime-state, its requests/ and trash/ folders,
// user/game-settings-private and art/ back to 0700; the files directly in runtime-state,
// requests/, user/ and game-settings-private back to 0600. user/ itself stays shared. Bottles in
// games/ are not touched: a suspended game can hold them open.
SelfHealResult TightenPrivateModes(const std::filesystem::path& data_root,
                                   const std::function<void(const std::string&)>& log);

// Whether a bottle (/data/playden/games/<install id>) looks opened up: its root, library or
// control folder is neither 0700 nor 0711. One lstat each; a missing bottle is not.
bool BottleTooOpen(const std::filesystem::path& data_root);

// The bottle at data_root: the folders the title's start check wants owner-only back to 0700 (0711
// kept), and the files directly in the game prefix, profiles, input presets, control folder, logs
// and title metadata back to 0600. drive_c is not entered. Only while the game's title is stopped.
SelfHealResult TightenBottleModes(const std::filesystem::path& data_root,
                                  const std::function<void(const std::string&)>& log);

// Files of the kit at kit_root whose mode has more bits than its manifest.json gives them get
// the manifest's mode, as root (the kit is often root's). The manifest's hash is checked when the
// kit is admitted; here it only says which bits to take away.
SelfHealResult TightenKitModes(const std::filesystem::path& kit_root, const WritableFoldersAsRoot& as_root,
                               const std::function<void(const std::string&)>& log);

}  // namespace playden::runtime
