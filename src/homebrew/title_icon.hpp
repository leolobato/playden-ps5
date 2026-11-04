#pragma once
// title_icon.hpp - A game's PS5 title icon from its store cover art, or its program's icon.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/artwork.hpp"

#include <optional>
#include <string>

namespace playden::homebrew {

inline constexpr std::uint32_t kTitleIconSize = 512;
// A program icon smaller than this (on its longer side) is too blurry to fill a tile: an old game's 32 or
// 48 pixel icon. It is centred instead, and on the console's Home scaled by whole pixels to stay sharp.
inline constexpr std::uint32_t kSmallProgramIconSide = 128;
// The size of a library poster made from a program icon: the 2:3 shape of the store's covers.
inline constexpr std::uint32_t kProgramIconPosterWidth = 320;
inline constexpr std::uint32_t kProgramIconPosterHeight = 480;

// The centred square of the cover, scaled to 512x512 by area averaging and
// encoded as an opaque RGB PNG. Nothing for an empty or malformed image.
std::optional<std::string> MakeTitleIcon(const ArtworkImage& cover);

// Whether an icon is a full picture with no see-through background (99% of its pixels nearly opaque).
bool ProgramIconIsOpaque(const ArtworkImage& icon);

// Art for a game without a cover, from its program's own icon (homebrew/program_icon.hpp), as an opaque
// width x height image. A blurred, darkened copy of the icon covers it all. The icon goes in the top square
// (all of a square image): a large opaque icon fills that square; a small or see-through one sits in its
// middle, with a shadow. Below the square, a poster gets darker so the game's title reads over it.
std::optional<ArtworkImage> ProgramIconArt(const ArtworkImage& icon, std::uint32_t width, std::uint32_t height);

// The PS5 title icon of a game without a cover, encoded as MakeTitleIcon does: ProgramIconArt, or, for a
// small icon, the same layout with sharper, smaller art on a darker backdrop: the icon scaled by the largest
// whole number that fits 9/16 of the tile, without smoothing, and corners rounded if it is opaque. Each
// Home tile is small, and a smooth upscale of an old game's icon looks blurry there.
std::optional<std::string> MakeProgramTitleIcon(const ArtworkImage& icon);

}  // namespace playden::homebrew
