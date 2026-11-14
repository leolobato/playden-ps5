// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A GOG update put in place: the files gog::GogInstaller::Stage downloaded beside the installed game go over the
// game's, the files the new build dropped are deleted, and the new build's plan, file list and record replace the old
// ones. It writes the update.json phases InstallService, UpdateLocked, game_move and the scan read, as the Steam
// installer's update does, but it is not DepotInstaller::FinishUpdate: that one skips every `.playden*` path, so an
// updated support or redist file would never be applied or deleted.
//
// The work folder, <root>/.playden-update/ (installs::kUpdateDirName), as Stage and the runner leave it:
// - update.json: {phase staging} while the files download (the runner writes it before Stage), then replacing, then
//   deleting. Once it says replacing the update must finish before the game runs again;
// - staged (one path per line: the files to put in place), old (the live .playden-files when staging began), files
//   (the new build's file list), paths (both versions' paths, for an uninstall in between);
// - staging/<path>: each staged file, at the path the new plan places it; gog-journal: Stage's journal;
// - gog/: the new build's plan (files), metas (meta/<hash>), depot manifests (manifests/<name>) and redist.list;
// - record: the new .playden-install.json, written by the runner before the phase leaves staging;
// - dlc: the DLC the staging was planned without (the runner's sidecar, kGogUpdateDlcName): the excluded GOG IDs, one
//   decimal per line, sorted; empty for none. Written by the runner right after update.json {staging}, before Stage.
//
// Paths it acts on: a game path (relative, no empty, "." or ".." component, no control character) whose first
// component doesn't start with ".playden" in any case, or a path under .playden-gog/support/ or a placed redist path
// (gog::IsRedistPlacement). Any other line in the lists is skipped and counted. Paths are matched by gog::FoldCase
// (exFAT ignores case).
//
// ApplyGogUpdate, phase by phase; every step can run again after an interruption at any point, and none needs the
// network:
// - replacing: each staged path still present in staging/ is renamed over <root>/<path>. Its folders are made from the
//   top; something that isn't a folder in the way goes only when it is a file the old list has and the new list
//   doesn't, otherwise kStorage and the phase stays replacing. A folder where a file now goes is cleared when it holds
//   only the old version's files the new list lacks and folders (its files, then its folders, then itself go);
//   anything else in it (a file the game wrote, a link) leaves it as it is: kStorage. Then the folders the new list
//   names are made and the phase becomes deleting;
// - deleting: the old list's files whose key the new list lacks are deleted (also one the new list names in another
//   spelling, unless it is the same file on disk), never through a link (every parent lstat'ed a real folder); the
//   folders that leaves empty go, deepest first. .playden-files-changed (installs::kFilesChangedName) is written when
//   a file was staged or the old list has a file the new one lacks, then removing `old` marks the deletion done;
// - finishing: gog/files, the metas and manifests (by name: an existing hash name kept, a gen 1 "v1-" manifest
//   replaced), gog/redist.list and files are
//   renamed into .playden-gog/ and .playden-files, each only when still there; a leftover .playden-gog/measure/ goes;
//   `after_replace` runs; record is renamed to .playden-install.json and the folder synced; then the work folder goes:
//   the staged files by the staged list (the console's readdir is unreliable), the journal, gog/, the lists and dlc,
//   update.json last, then whatever is left of the folder.
// Cancel stops between files: the phase stays and the next call carries on.
//
// Plain absolute-path POSIX and stdio calls only (no descriptor-relative calls, no streams), so the console runs the
// same code. Nothing is logged: the runner logs the counts.

#include "gog/installer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace playden::stores {

// The work folder's record, renamed over .playden-install.json when the update finishes.
inline constexpr char kGogUpdateRecordName[] = "record";
// The work folder's DLC sidecar: the excluded GOG IDs the staging was planned with (see the top of this file).
inline constexpr char kGogUpdateDlcName[] = "dlc";

struct GogUpdateHooks {
  // After every file is in place and the new plan and lists are, before the record: the runner's install scripts.
  // Runs again when a finish is carried on. May be empty.
  std::function<void(const std::string& root)> after_replace;
  // Tests: after each staged file is renamed into place, with its path. May be empty.
  std::function<void(const std::string& path)> replaced;
};

struct GogApplyResult {
  // kNone; kCanceled (the phase stays); kStorage (a file or folder could not be written, or something the game wrote is
  // in the way); kProtocol (no update past staging at `root`).
  gog::GogInstaller::Outcome::Error error = gog::GogInstaller::Outcome::Error::kProtocol;
  std::uint64_t install_bytes = 0;  // the new record's
  std::uint64_t replaced = 0;       // staged files this call renamed into place
  std::uint64_t deleted = 0;        // old files this call deleted
  std::uint64_t skipped = 0;        // list lines outside the paths an update may touch
};

// Whether an update may touch `path` (see the top of this file).
bool GogUpdatePath(std::string_view path);

// Puts the update at `root` in place (its phase replacing or deleting), as the top of this file says.
GogApplyResult ApplyGogUpdate(const std::string& root, const std::atomic<bool>& cancel, const GogUpdateHooks& hooks = {});

// Whether the update at `root` is fully staged, so putting it in place needs no session and no network: update.json
// says staging, and the work folder holds the staged, old and new lists, a saved plan that reads, and the record of
// that plan's build. The runner deletes the record before every Stage and writes it only after one that finished.
bool GogUpdateStaged(const std::string& root);

// Whether every folder the update at `root` needs could be made, and every folder where a staged file now goes could
// be cleared, without changing anything: false when something the old version didn't have (a file the game wrote, a
// link) is in the way. The runner asks before the phase leaves staging, so such an update stops while the game is
// still playable.
bool GogUpdateFoldersClear(const std::string& root);

// Deletes a measure's folder, <root>/.playden-gog/measure/ (gog::MeasureLayout), never through a link: a link at
// measure/ is removed, not followed, and nothing is deleted when the game folder or .playden-gog is not a real folder.
// True when it is gone.
bool RemoveGogMeasure(const std::string& root);

// Deletes a canceled update's work folder (steam::DiscardManagedUpdate), leaving the installed game as it was: the
// staged .playden-gog/ files go with the folder. Refuses once the phase is past staging. `failure`, when given, says
// why it failed.
bool DiscardGogUpdate(const std::string& root, std::string* failure = nullptr);

}  // namespace playden::stores
