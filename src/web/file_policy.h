// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::web {

// What the page may see and change inside a game's folders. Pure: no file system access. Names compare without
// regard to case, as exFAT and Wine do.

// A name new files may have: the previous-version prefix must still fit in the file system's 255 bytes.
inline constexpr std::size_t kMaxNewNameBytes = 230;
inline constexpr std::size_t kMaxNameBytes = 255;

// Playden's files beside the player's. Cloud sync skips names starting ".playden" (steam/backend/cloud_sync.cpp).
inline constexpr const char* kPreviousPrefix = ".playden-bak.";
inline constexpr const char* kUploadPrefix = ".playden-upload-";
inline constexpr const char* kRestorePrefix = ".playden-restore-";

std::string LowerAscii(std::string_view text);
bool ValidUtf8(std::string_view text);
// A text file's bytes as UTF-8 for the editor. UTF-8, or UTF-16 with a byte order mark; encoding is "utf-8", "utf-16le"
// or "utf-16be", and bom says whether the file starts with one. False for anything else, or for text holding a NUL.
bool DecodeText(std::string_view bytes, std::string* text, std::string* encoding, bool* bom);
// Hidden and refused: Playden's own files and secrets kept in a game's folder.
bool HiddenName(std::string_view name);
// Readable, never changed: Playden writes these folders each time it prepares the game.
bool ReadOnlyFolderName(std::string_view name);
// An existing entry the page may name: not ".", "..", no separators or control characters, valid UTF-8.
bool UsableName(std::string_view name);
// A name a new file or folder may take.
bool NewName(std::string_view name);

// A request's path, relative to a root: its parts, or nothing when any part isn't usable or is hidden. Empty text
// is the root itself. No path that leaves a root, hidden or not, gets through.
std::optional<std::vector<std::string>> SplitRelativePath(std::string_view path);
// Whether a path passes through a read-only folder.
bool ReadOnlyPath(const std::vector<std::string>& parts);
// Whether parts is at or below one of the private paths (each '/'-separated, relative to the same root).
bool PrivatePath(const std::vector<std::string>& parts, const std::vector<std::string>& private_paths);
// Whether parts holds one of the private paths below it: moving or renaming it would take that path along.
bool HoldsPrivatePath(const std::vector<std::string>& parts, const std::vector<std::string>& private_paths);
// The name a second copy takes when the player keeps both: "save (2).dat", "Saves (2)". Nothing when it can't be a new
// name (too long).
std::optional<std::string> NumberedName(std::string_view name, bool folder, int number);

}  // namespace playden::web
