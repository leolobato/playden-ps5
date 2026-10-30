#pragma once
// title_icon.hpp - A game's PS5 title icon from its store cover art, or its program's icon.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/artwork.hpp"

#include <optional>
#include <string>

namespace playden::homebrew {

inline constexpr std::uint32_t kTitleIconSize = 512;

// The centred square of the cover, scaled to 512x512 by area averaging and
// encoded as an opaque RGB PNG. Nothing for an empty or malformed image.
std::optional<std::string> MakeTitleIcon(const ArtworkImage& cover);

// A game without cover art: its program's own icon (homebrew/program_icon.hpp), scaled to fill
// two thirds of a dark 512x512 tile and centred, encoded as MakeTitleIcon does.
std::optional<std::string> MakeProgramTitleIcon(const ArtworkImage& icon);
// The same tile as an image, for the UI's artwork.
std::optional<ArtworkImage> ProgramIconTile(const ArtworkImage& icon, std::uint32_t side);

}  // namespace playden::homebrew
