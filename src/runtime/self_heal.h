// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Startup repairs of modes on what Playden owns. Playden's own data is open (#868): folders 0777 and
// files 0666, so the console owner can delete it over FTP (root has no permission bypass on /data).
// OpenPlaydenModes only adds bits. The bottles of live games and the runtime kit still need exact
// modes (their title shell and kit admission check them), and are given those back. Entries of other
// owners and files with other links are left alone, links are never followed, and every change goes
// through a descriptor opened without following a link.
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

struct OpenModesResult {
  std::size_t walked = 0;   // roots walked (each one not yet open, or all of them on the first start)
  std::size_t opened = 0;   // folders and files given more bits
  std::size_t skipped = 0;  // entries that could not be opened or changed
  bool truncated = false;   // a walk stopped at its bound; the next start goes on
};

// Opens what Playden alone checks, under data_root (/data/playden) and homebrew_root (/data/homebrew):
// runtime-state, user/, art/, title-remover/, staging/title-*, the bottles no title uses
// (games/<id>.kept, .saves, .set-aside-*), and Playden's PS5 titles (FAKE78xxx holding Playden's owner
// marker). Folders become 0777 and files 0666 (execute bits kept); nothing loses a permission bit.
// Secrets (platform/data_roots.h) and a title's game-launch.json are left as they are. A root is
// walked when its own folder is not 0777, and every root once (runtime-state/open-modes.v1 records
// it), so a start after an older Playden tightened something opens it again without walking
// everything on each start. Logs one summary line.
OpenModesResult OpenPlaydenModes(const std::filesystem::path& data_root, const std::filesystem::path& homebrew_root,
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
