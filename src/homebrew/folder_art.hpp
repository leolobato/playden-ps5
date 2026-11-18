#pragma once
// folder_art.hpp - PS5 title art a player puts in an imported game's folder (<game>/playden/, #444).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/artwork.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace playden::homebrew {

// The folder at the root of a game's folder that holds what a player gives Playden for that game.
inline constexpr const char* kFolderArtDirectory = "playden";
// PNG pictures in it: the title's icon (any size, its centred square is used) and its Home background
// (any size, scaled and cropped to fill 16:9).
inline constexpr const char* kFolderIconName = "icon.png";
inline constexpr const char* kFolderBackgroundName = "background.png";
// The PS5 title background: pic0.dds and pic1.dds, the size and format of Playden's own (DXT1).
inline constexpr std::uint32_t kTitleBackgroundWidth = 3840;
inline constexpr std::uint32_t kTitleBackgroundHeight = 2160;

// The art made from a game folder's pictures. A picture that is missing gives nothing; one that can't be used
// gives nothing and a sentence in `problems`.
struct FolderArt {
  std::optional<std::string> icon_png;        // icon0.png, 512x512
  std::optional<std::string> background_dds;  // pic0.dds and pic1.dds
  std::vector<std::string> problems;
};

// Reads and converts `<game_folder>/playden/icon.png` and `background.png` (host paths). Links are never followed.
FolderArt MakeFolderArt(const std::filesystem::path& game_folder);

// The same files checked when a game is added (each decoded, nothing converted): one sentence for the player per
// picture that can't be used (empty when every picture there can be).
std::vector<std::string> CheckFolderArt(const std::filesystem::path& game_folder);

// The picture scaled to cover 3840x2160 (its centre kept), see-through parts black, as a DXT1 DDS file.
// Nothing for an empty or malformed image.
std::optional<std::string> MakeTitleBackground(const ArtworkImage& picture);

}  // namespace playden::homebrew
