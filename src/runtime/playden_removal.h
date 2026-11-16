// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Uninstall Playden (#868): removes every file Playden made on the console, after its PS5 titles are
// gone (RuntimeHost::RemovePlayden does those first). Every step is idempotent, and a journal
// (/data/playden/uninstall.json) says where a removal stopped, so the next start finishes it instead
// of making Playden's folders again.
//
// Nothing outside the named roots is touched: entries are looked at with lstat, a link is deleted as
// a link and never followed, and the player's own files (a game folder waiting to be imported) stay.
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace playden::runtime {

struct PlaydenRemovalPaths {
  std::filesystem::path data_root;          // /data/playden
  std::filesystem::path games_files_root;   // /data/playden-games
  std::filesystem::path import_root;        // /data/playden-import (the player's: removed only when empty)
  std::filesystem::path app_root;           // /data/homebrew/FAKE77031 (stays: the player deletes the tile)
  std::vector<std::filesystem::path> drive_mounts;  // /mnt/ext0 ... /mnt/usb7: <mount>/playden on each
};

// The console's paths (platform/data_roots.h), under `prefix` for tests.
PlaydenRemovalPaths ConsoleRemovalPaths(const std::filesystem::path& prefix = {});

enum class RemovalStep { kTitles, kFiles };

struct RemovalJournal {
  RemovalStep step = RemovalStep::kTitles;
  bool keep_saves = true;
};

// The journal at data_root/uninstall.json; nothing when there is none (or it is unreadable: a damaged
// one is treated as a removal that reached its files, so nothing is made again).
std::optional<RemovalJournal> LoadRemovalJournal(const std::filesystem::path& data_root);
bool SaveRemovalJournal(const std::filesystem::path& data_root, const RemovalJournal& journal);
// Whether a removal was started (the file is there, readable or not).
bool RemovalStarted(const std::filesystem::path& data_root);

struct PlaydenRemovalResult {
  bool complete = false;                   // everything Playden's is gone (what was kept aside)
  std::vector<std::string> kept;           // kept on purpose: kept saves, a non-empty import folder
  std::vector<std::string> failed;         // could not be removed (path: reason)
};

// Root work (the kit, the title rule and updates belong to root). Runs `work` with root's effective
// UID; false when root can't be raised.
using RemovalAsRoot = std::function<bool(const std::function<bool()>&)>;

// Removes Playden's files: game files under games_files_root and <mount>/playden on each drive
// (an import folder there stays when it holds anything), import_root when empty, the root-owned
// folders under data_root (as root), the rest of data_root (with keep_saves, games/*.kept stays), and
// the app folder's leftovers from before 0.6 (playden-data, logs). The journal goes last, then
// data_root itself when nothing was kept in it. Safe to run again after any stop.
PlaydenRemovalResult RemovePlaydenFiles(const PlaydenRemovalPaths& paths, bool keep_saves, const RemovalAsRoot& as_root,
                                        const std::function<void(const std::string&)>& log);

}  // namespace playden::runtime
