#pragma once
// writable_folders.h - Checks (and fixes) the folders Playden writes into before it writes.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace playden::runtime {

// Runs work with root raised (ps5::WithRoot on the console); false when root could not be raised
// or the work failed. Empty: nothing can be fixed.
using WritableFoldersAsRoot = std::function<bool(const std::function<bool()>& work)>;

// Folders made over FTP belong to root and are often 0755, so the app user can't write into them.
// For each existing folder (missing ones and links are skipped), a probe file is created and
// removed as the app user. Only fixed, approved shared roots may be repaired as root (root owns them, so no
// permission bypass is needed) and probed again. Returns an empty string when every folder can be
// written, or a message naming the first folder that still can't. Each fix and failure is logged.
std::string EnsureWritableFolders(const std::vector<std::filesystem::path>& folders,
                                  const WritableFoldersAsRoot& as_root,
                                  const std::function<void(const std::string&)>& log,
                                  const std::filesystem::path& namespace_root = "/");

// Single-threaded startup, after the grant and before the first runtime write check.
std::string PrepareRuntimeRoot(const std::filesystem::path& root, const WritableFoldersAsRoot& as_root,
                               const std::function<void(const std::string&)>& log,
                               const std::filesystem::path& namespace_root = "/");

// The drop folder, /data/playden-import (0777): FTP copies games into it and Playden plays them in
// place (stores/local/drop_folder_access.h opens each game's files). Made, or opened to 0777, as
// root after the grant; a link or a file at that path is refused. Failing doesn't stop Playden:
// the message is for the log, and games can still come from a drive.
std::string PrepareDropFolder(const std::filesystem::path& folder, const WritableFoldersAsRoot& as_root,
                              const std::function<void(const std::string&)>& log,
                              const std::filesystem::path& namespace_root = "/");

// The app folder's empty playden-data and logs folders (0777): since 0.6.0 they hold only MOVED.txt,
// and the first startup lines go into logs before the grant. A hand copy can leave either out (some
// FTP clients and browser uploads skip empty folders). A missing one is made 0777 as root after the
// grant; an existing one is left as it is, and a link or a file in its place is refused. Only a folder directly in
// /data/homebrew is accepted. Failing doesn't stop Playden: the message is for the log.
std::string PrepareAppFolderStubs(const std::filesystem::path& app_root, const WritableFoldersAsRoot& as_root,
                                  const std::function<void(const std::string&)>& log,
                                  const std::filesystem::path& namespace_root = "/");

// The app's own folders (assets, runtime, sce_module, sce_sys, trust) to 0777: a self-update moves them aside as
// root, and root can't move a folder it doesn't own unless everyone can write to it. A deploy tool or a chmod can
// leave them 0755. Tried as the app user, who owns them after a self-update, then as root, who owns them after an
// FTP copy. Missing folders are skipped, and a link or a file in the place of one is left alone. Only a folder
// directly in /data/homebrew is accepted. Failing doesn't stop Playden: the message is for the log.
std::string OpenAppFolders(const std::filesystem::path& app_root, const WritableFoldersAsRoot& as_root,
                           const std::function<void(const std::string&)>& log,
                           const std::filesystem::path& namespace_root = "/");

}  // namespace playden::runtime
