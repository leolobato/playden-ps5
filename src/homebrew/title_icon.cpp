// title_icon.cpp - A game's PS5 title icon from its store cover art, or its program's icon.
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

namespace {

// The UI's placeholder tile colour: what shows through a see-through icon.
constexpr std::uint8_t kGround[3] = {0x24, 0x20, 0x1d};
// The backdrop: the icon averaged down to this many cells on its longer side, at this brightness.
constexpr std::uint32_t kBackdropCells = 16;
constexpr double kBackdropBrightness = 0.65;
// A poster darkens from the lower part of its square down to this at the bottom, so its title reads.
constexpr double kTitleAreaBrightness = 0.5;
constexpr double kTitleAreaStart = 0.8;
// A centred icon's share of the square, and the rounding of an opaque one's corners.
constexpr double kCentredIconShare = 0.62;
constexpr double kCentredIconRounding = 0.08;
// A small icon on a PS5 title icon: a whole number of pixels per icon pixel, up to this many across
// (9/16 of the tile: 32 pixels scale by 9 and 48 by 6), over a backdrop at this brightness.
constexpr std::uint32_t kSmallTitleIconLimit = kTitleIconSize * 9 / 16;
constexpr double kSmallTitleIconBrightness = 0.45;

bool Valid(const ArtworkImage& image) {
  return image.width != 0 && image.height != 0 &&
         image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4;
}

ArtworkImage Blank(std::uint32_t width, std::uint32_t height) {
  ArtworkImage image;
  image.width = width, image.height = height;
  image.rgba.assign(static_cast<std::size_t>(width) * height * 4, 255);
  return image;
}

std::uint8_t* Pixel(ArtworkImage& image, std::uint32_t x, std::uint32_t y) {
  return &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
}
const std::uint8_t* Pixel(const ArtworkImage& image, std::uint32_t x, std::uint32_t y) {
  return &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
}

// An icon scaled to fit a side x side box, keeping its shape, as premultiplied RGBA (0..1) at its place.
struct Layer {
  long left = 0, top = 0;
  std::uint32_t width = 0, height = 0;
  std::vector<double> rgba;
};

// Bilinear on alpha-weighted colours, or nearest-neighbour when `pixelated`; `radius` rounds the corners
// with anti-aliased edges.
Layer Scale(const ArtworkImage& icon, double centre_x, double centre_y, double side, double radius, bool pixelated = false) {
  const double scale = side / std::max(icon.width, icon.height);
  Layer layer;
  layer.width = static_cast<std::uint32_t>(std::max(1.0, std::round(icon.width * scale)));
  layer.height = static_cast<std::uint32_t>(std::max(1.0, std::round(icon.height * scale)));
  layer.left = std::lround(centre_x - layer.width / 2.0);
  layer.top = std::lround(centre_y - layer.height / 2.0);
  layer.rgba.resize(static_cast<std::size_t>(layer.width) * layer.height * 4);
  // A thin icon's corners can round no further than half its shorter side.
  radius = std::min(radius, std::min(layer.width, layer.height) / 2.0);
  for (std::uint32_t y = 0; y < layer.height; ++y)
    for (std::uint32_t x = 0; x < layer.width; ++x) {
      const double offset = pixelated ? 0.0 : 0.5;
      const double fit_x = (x + 0.5) / scale - offset, fit_y = (y + 0.5) / scale - offset;
      const double sx = std::clamp(pixelated ? std::floor(fit_x) : fit_x, 0.0, icon.width - 1.0);
      const double sy = std::clamp(pixelated ? std::floor(fit_y) : fit_y, 0.0, icon.height - 1.0);
      const auto x0 = static_cast<std::uint32_t>(sx), y0 = static_cast<std::uint32_t>(sy);
      const std::uint32_t x1 = std::min(x0 + 1, icon.width - 1), y1 = std::min(y0 + 1, icon.height - 1);
      const double fx = sx - x0, fy = sy - y0;
      const std::uint8_t* corners[4] = {Pixel(icon, x0, y0), Pixel(icon, x1, y0), Pixel(icon, x0, y1), Pixel(icon, x1, y1)};
      const double weights[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
      double colour[3] = {0, 0, 0}, alpha = 0;
      for (int c = 0; c < 4; ++c) {
        const double a = weights[c] * corners[c][3] / 255.0;
        alpha += a;
        for (int k = 0; k < 3; ++k) colour[k] += a * corners[c][k] / 255.0;
      }
      double coverage = 1.0;
      if (radius > 0) {
        const double px = x + 0.5, py = y + 0.5;
        const double cx = std::clamp(px, radius, layer.width - radius), cy = std::clamp(py, radius, layer.height - radius);
        coverage = std::clamp(radius - std::hypot(px - cx, py - cy) + 0.5, 0.0, 1.0);
      }
      double* out = &layer.rgba[(static_cast<std::size_t>(y) * layer.width + x) * 4];
      for (int k = 0; k < 3; ++k) out[k] = colour[k] * coverage;
      out[3] = alpha * coverage;
    }
  return layer;
}

void Composite(ArtworkImage& target, const Layer& layer) {
  for (std::uint32_t y = 0; y < layer.height; ++y)
    for (std::uint32_t x = 0; x < layer.width; ++x) {
      const long tx = layer.left + static_cast<long>(x), ty = layer.top + static_cast<long>(y);
      if (tx < 0 || ty < 0 || tx >= static_cast<long>(target.width) || ty >= static_cast<long>(target.height)) continue;
      const double* in = &layer.rgba[(static_cast<std::size_t>(y) * layer.width + x) * 4];
      std::uint8_t* out = Pixel(target, static_cast<std::uint32_t>(tx), static_cast<std::uint32_t>(ty));
      for (int k = 0; k < 3; ++k)
        out[k] = static_cast<std::uint8_t>(std::clamp(std::lround(in[k] * 255.0 + (1 - in[3]) * out[k]), 0L, 255L));
    }
}

// A soft shadow of the layer's shape, `blur` pixels wide and `drop` pixels lower, darkening the target.
void Shadow(ArtworkImage& target, const Layer& layer, std::uint32_t blur, long drop, double strength) {
  const std::uint32_t width = layer.width + 2 * blur, height = layer.height + 2 * blur;
  std::vector<double> mask(static_cast<std::size_t>(width) * height, 0.0), pass(mask.size());
  for (std::uint32_t y = 0; y < layer.height; ++y)
    for (std::uint32_t x = 0; x < layer.width; ++x)
      mask[static_cast<std::size_t>(y + blur) * width + x + blur] = layer.rgba[(static_cast<std::size_t>(y) * layer.width + x) * 4 + 3];
  // Three box blurs come close to a Gaussian.
  const long reach = std::max<long>(1, blur / 3);
  for (int round = 0; round < 3; ++round)
    for (int vertical = 0; vertical < 2; ++vertical) {
      const std::uint32_t lines = vertical ? width : height, length = vertical ? height : width;
      for (std::uint32_t line = 0; line < lines; ++line) {
        const auto index = [&](long i) {
          return vertical ? static_cast<std::size_t>(i) * width + line : static_cast<std::size_t>(line) * width + i;
        };
        double sum = 0;
        for (long i = -reach; i <= reach; ++i) sum += i >= 0 && i < static_cast<long>(length) ? mask[index(i)] : 0.0;
        for (long i = 0; i < static_cast<long>(length); ++i) {
          pass[index(i)] = sum / (2 * reach + 1);
          if (i - reach >= 0) sum -= mask[index(i - reach)];
          if (i + reach + 1 < static_cast<long>(length)) sum += mask[index(i + reach + 1)];
        }
      }
      mask.swap(pass);
    }
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      const long tx = layer.left - static_cast<long>(blur) + static_cast<long>(x);
      const long ty = layer.top - static_cast<long>(blur) + drop + static_cast<long>(y);
      if (tx < 0 || ty < 0 || tx >= static_cast<long>(target.width) || ty >= static_cast<long>(target.height)) continue;
      const double keep = 1.0 - strength * mask[static_cast<std::size_t>(y) * width + x];
      std::uint8_t* out = Pixel(target, static_cast<std::uint32_t>(tx), static_cast<std::uint32_t>(ty));
      for (int k = 0; k < 3; ++k) out[k] = static_cast<std::uint8_t>(std::lround(out[k] * keep));
    }
}

// The icon over the dark ground, covering width x height, averaged into a few cells, blurred among them
// and smoothed back up: a heavy blur for little work. Darkened, and more so towards a tall image's bottom.
ArtworkImage Backdrop(const ArtworkImage& icon, std::uint32_t width, std::uint32_t height, double base_brightness) {
  const double scale = std::max(static_cast<double>(width) / icon.width, static_cast<double>(height) / icon.height);
  const double window_w = width / scale, window_h = height / scale;
  const double origin_x = (icon.width - window_w) / 2, origin_y = (icon.height - window_h) / 2;
  const std::uint32_t cells_x = std::max<std::uint32_t>(1, width >= height ? kBackdropCells : kBackdropCells * width / height);
  const std::uint32_t cells_y = std::max<std::uint32_t>(1, height >= width ? kBackdropCells : kBackdropCells * height / width);
  std::vector<double> cells(static_cast<std::size_t>(cells_x) * cells_y * 3);
  for (std::uint32_t cy = 0; cy < cells_y; ++cy)
    for (std::uint32_t cx = 0; cx < cells_x; ++cx) {
      const auto x0 = static_cast<std::uint32_t>(origin_x + window_w * cx / cells_x);
      const auto y0 = static_cast<std::uint32_t>(origin_y + window_h * cy / cells_y);
      const auto x1 = std::clamp(static_cast<std::uint32_t>(origin_x + window_w * (cx + 1) / cells_x), x0 + 1, icon.width);
      const auto y1 = std::clamp(static_cast<std::uint32_t>(origin_y + window_h * (cy + 1) / cells_y), y0 + 1, icon.height);
      double sum[3] = {0, 0, 0};
      for (std::uint32_t y = y0; y < y1; ++y)
        for (std::uint32_t x = x0; x < x1; ++x) {
          const std::uint8_t* p = Pixel(icon, x, y);
          for (int k = 0; k < 3; ++k) sum[k] += (p[k] * p[3] + kGround[k] * (255.0 - p[3])) / 255.0;
        }
      const double count = static_cast<double>(x1 - x0) * (y1 - y0);
      for (int k = 0; k < 3; ++k) cells[(static_cast<std::size_t>(cy) * cells_x + cx) * 3 + k] = sum[k] / count;
    }
  // Two 3x3 box blurs over the cells, so no cell's edge shows once scaled up.
  for (int round = 0; round < 2; ++round) {
    std::vector<double> blurred(cells.size());
    for (std::uint32_t cy = 0; cy < cells_y; ++cy)
      for (std::uint32_t cx = 0; cx < cells_x; ++cx)
        for (int k = 0; k < 3; ++k) {
          double sum = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const auto x = static_cast<std::uint32_t>(std::clamp<long>(static_cast<long>(cx) + dx, 0, cells_x - 1));
              const auto y = static_cast<std::uint32_t>(std::clamp<long>(static_cast<long>(cy) + dy, 0, cells_y - 1));
              sum += cells[(static_cast<std::size_t>(y) * cells_x + x) * 3 + k];
            }
          blurred[(static_cast<std::size_t>(cy) * cells_x + cx) * 3 + k] = sum / 9;
        }
    cells.swap(blurred);
  }
  ArtworkImage image = Blank(width, height);
  const std::uint32_t square = std::min(width, height);
  for (std::uint32_t y = 0; y < height; ++y) {
    const double gy = std::clamp((y + 0.5) * cells_y / height - 0.5, 0.0, cells_y - 1.0);
    const auto y0 = static_cast<std::uint32_t>(gy);
    const std::uint32_t y1 = std::min(y0 + 1, cells_y - 1);
    const double ty = gy - y0, fy = ty * ty * (3 - 2 * ty);
    double brightness = base_brightness;
    const double fade_top = square * kTitleAreaStart;
    if (height > square && y >= fade_top)
      brightness *= 1.0 - (1.0 - kTitleAreaBrightness) * (y + 1.0 - fade_top) / (height - fade_top);
    for (std::uint32_t x = 0; x < width; ++x) {
      const double gx = std::clamp((x + 0.5) * cells_x / width - 0.5, 0.0, cells_x - 1.0);
      const auto x0 = static_cast<std::uint32_t>(gx);
      const std::uint32_t x1 = std::min(x0 + 1, cells_x - 1);
      const double tx = gx - x0, fx = tx * tx * (3 - 2 * tx);
      const auto cell = [&](std::uint32_t cx, std::uint32_t cy, int k) {
        return cells[(static_cast<std::size_t>(cy) * cells_x + cx) * 3 + k];
      };
      std::uint8_t* out = Pixel(image, x, y);
      for (int k = 0; k < 3; ++k) {
        const double value = (1 - fx) * (1 - fy) * cell(x0, y0, k) + fx * (1 - fy) * cell(x1, y0, k) +
                             (1 - fx) * fy * cell(x0, y1, k) + fx * fy * cell(x1, y1, k);
        out[k] = static_cast<std::uint8_t>(std::lround(value * brightness));
      }
    }
  }
  return image;
}

bool Small(const ArtworkImage& icon) { return std::max(icon.width, icon.height) < kSmallProgramIconSide; }

// A centred icon on its backdrop, with a shadow.
void PlaceCentred(ArtworkImage& art, const Layer& layer, std::uint32_t square) {
  Shadow(art, layer, static_cast<std::uint32_t>(std::lround(square * 0.04)), std::lround(square * 0.02), 0.6);
  Composite(art, layer);
}

}  // namespace

bool ProgramIconIsOpaque(const ArtworkImage& icon) {
  if (!Valid(icon)) return false;
  std::size_t solid = 0;
  for (std::size_t i = 3; i < icon.rgba.size(); i += 4) solid += icon.rgba[i] >= 224;
  return solid * 100 >= static_cast<std::size_t>(icon.width) * icon.height * 99;
}

std::optional<ArtworkImage> ProgramIconArt(const ArtworkImage& icon, std::uint32_t width, std::uint32_t height) {
  if (!Valid(icon) || width == 0 || height == 0 || width > kMaxArtworkDimension || height > kMaxArtworkDimension)
    return std::nullopt;
  ArtworkImage art = Backdrop(icon, width, height, kBackdropBrightness);
  const std::uint32_t square = std::min(width, height);
  const double centre_x = width / 2.0, centre_y = square / 2.0;
  const bool opaque = ProgramIconIsOpaque(icon);
  if (opaque && !Small(icon)) {
    Composite(art, Scale(icon, centre_x, centre_y, square, 0.0));
    return art;
  }
  const double side = square * kCentredIconShare;
  PlaceCentred(art, Scale(icon, centre_x, centre_y, side, opaque ? side * kCentredIconRounding : 0.0), square);
  return art;
}

std::optional<std::string> MakeProgramTitleIcon(const ArtworkImage& icon) {
  if (!Valid(icon)) return std::nullopt;
  if (Small(icon)) {
    ArtworkImage art = Backdrop(icon, kTitleIconSize, kTitleIconSize, kSmallTitleIconBrightness);
    const std::uint32_t longer = std::max(icon.width, icon.height);
    const double side = kSmallTitleIconLimit / longer * longer;
    const double centre = kTitleIconSize / 2.0;
    const double radius = ProgramIconIsOpaque(icon) ? side * kCentredIconRounding : 0.0;
    PlaceCentred(art, Scale(icon, centre, centre, side, radius, true), kTitleIconSize);
    return MakeTitleIcon(art);
  }
  const auto art = ProgramIconArt(icon, kTitleIconSize, kTitleIconSize);
  return art ? MakeTitleIcon(*art) : std::nullopt;
}

}  // namespace playden::homebrew
