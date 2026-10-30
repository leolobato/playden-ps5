// title_icon.cpp - A game's PS5 title icon from its store cover art.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/title_icon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// A private copy of the PNG writer: the app's renderer has its own.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace playden::homebrew {

std::optional<std::string> MakeTitleIcon(const ArtworkImage& cover) {
  if (cover.width == 0 || cover.height == 0 ||
      cover.rgba.size() != static_cast<std::size_t>(cover.width) * cover.height * 4)
    return std::nullopt;
  const std::uint32_t side = std::min(cover.width, cover.height);
  const std::uint32_t left = (cover.width - side) / 2, top = (cover.height - side) / 2;
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kTitleIconSize) * kTitleIconSize * 3);
  for (std::uint32_t y = 0; y < kTitleIconSize; ++y) {
    const std::uint32_t y0 = top + y * side / kTitleIconSize;
    const std::uint32_t y1 = std::max(y0 + 1, top + (y + 1) * side / kTitleIconSize);
    for (std::uint32_t x = 0; x < kTitleIconSize; ++x) {
      const std::uint32_t x0 = left + x * side / kTitleIconSize;
      const std::uint32_t x1 = std::max(x0 + 1, left + (x + 1) * side / kTitleIconSize);
      std::uint64_t sum[3] = {0, 0, 0};
      for (std::uint32_t sy = y0; sy < y1; ++sy)
        for (std::uint32_t sx = x0; sx < x1; ++sx) {
          const auto* p = &cover.rgba[(static_cast<std::size_t>(sy) * cover.width + sx) * 4];
          // Transparent areas become black, as the console shows icons on dark tiles.
          for (int c = 0; c < 3; ++c) sum[c] += static_cast<std::uint64_t>(p[c]) * p[3] / 255;
        }
      const std::uint64_t count = static_cast<std::uint64_t>(y1 - y0) * (x1 - x0);
      auto* out = &pixels[(static_cast<std::size_t>(y) * kTitleIconSize + x) * 3];
      for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
    }
  }
  std::string png;
  const auto append = [](void* context, void* data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
  };
  if (!stbi_write_png_to_func(append, &png, kTitleIconSize, kTitleIconSize, 3, pixels.data(),
                              static_cast<int>(kTitleIconSize * 3)) || png.empty())
    return std::nullopt;
  return png;
}

std::optional<ArtworkImage> ProgramIconTile(const ArtworkImage& icon, std::uint32_t side) {
  if (icon.width == 0 || icon.height == 0 || side == 0 || side > kMaxArtworkDimension ||
      icon.rgba.size() != static_cast<std::size_t>(icon.width) * icon.height * 4)
    return std::nullopt;
  // The UI's placeholder tile colour, so the icon sits on the same dark ground.
  constexpr std::uint8_t kGround[3] = {0x24, 0x20, 0x1d};
  ArtworkImage tile;
  tile.width = tile.height = side;
  tile.rgba.resize(static_cast<std::size_t>(side) * side * 4);
  for (std::size_t i = 0; i < tile.rgba.size(); i += 4)
    tile.rgba[i] = kGround[0], tile.rgba[i + 1] = kGround[1], tile.rgba[i + 2] = kGround[2], tile.rgba[i + 3] = 255;
  // Two thirds of the tile, keeping the icon's shape; bilinear, on alpha-weighted colours.
  const double scale = side * 2.0 / 3.0 / std::max(icon.width, icon.height);
  const auto box_w = static_cast<std::uint32_t>(std::max(1.0, icon.width * scale));
  const auto box_h = static_cast<std::uint32_t>(std::max(1.0, icon.height * scale));
  const std::uint32_t left = (side - box_w) / 2, top = (side - box_h) / 2;
  const auto at = [&](std::uint32_t x, std::uint32_t y) { return &icon.rgba[(static_cast<std::size_t>(y) * icon.width + x) * 4]; };
  for (std::uint32_t y = 0; y < box_h; ++y)
    for (std::uint32_t x = 0; x < box_w; ++x) {
      const double sx = std::clamp((x + 0.5) / scale - 0.5, 0.0, icon.width - 1.0);
      const double sy = std::clamp((y + 0.5) / scale - 0.5, 0.0, icon.height - 1.0);
      const auto x0 = static_cast<std::uint32_t>(sx), y0 = static_cast<std::uint32_t>(sy);
      const std::uint32_t x1 = std::min(x0 + 1, icon.width - 1), y1 = std::min(y0 + 1, icon.height - 1);
      const double fx = sx - x0, fy = sy - y0;
      const std::uint8_t* corners[4] = {at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)};
      const double weights[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
      double colour[3] = {0, 0, 0}, alpha = 0;
      for (int c = 0; c < 4; ++c) {
        const double a = weights[c] * corners[c][3] / 255.0;
        alpha += a;
        for (int k = 0; k < 3; ++k) colour[k] += a * corners[c][k];
      }
      auto* out = &tile.rgba[(static_cast<std::size_t>(top + y) * side + left + x) * 4];
      for (int k = 0; k < 3; ++k) out[k] = static_cast<std::uint8_t>(std::lround(colour[k] + (1 - alpha) * out[k]));
    }
  return tile;
}

std::optional<std::string> MakeProgramTitleIcon(const ArtworkImage& icon) {
  const auto tile = ProgramIconTile(icon, kTitleIconSize);
  return tile ? MakeTitleIcon(*tile) : std::nullopt;
}

}  // namespace playden::homebrew
