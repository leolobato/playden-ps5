// folder_art.cpp - PS5 title art a player puts in an imported game's folder (<game>/playden/, #444).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/folder_art.hpp"

#include "homebrew/artwork_backend.hpp"
#include "homebrew/title_icon.hpp"
#include "platform/directory_walk.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::homebrew {
namespace {

enum class FileRead { kOk, kMissing, kNotFile, kTooLarge, kUnreadable };

// The file `name` in the game's playden folder, never through a link: each folder on the way and the file are opened
// without following links and checked against the folder that holds them (platform/directory_walk.h).
FileRead ReadArtFile(const std::filesystem::path& game_folder, const char* name, std::vector<std::uint8_t>* bytes) {
  const int folder = platform::OpenDirectoryWalk(game_folder / kFolderArtDirectory);
  if (folder < 0) {
    // No playden folder, or a file or link by that name: nothing to use.
    if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP || errno == EMLINK) return FileRead::kMissing;
    return FileRead::kUnreadable;
  }
  const int fd = platform::OpenWalkChildUnprivileged(folder, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  const int error = errno;
  ::close(folder);
  if (fd < 0) {
    if (error == ENOENT) return FileRead::kMissing;
    return error == ELOOP || error == EMLINK ? FileRead::kNotFile : FileRead::kUnreadable;
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    return FileRead::kNotFile;
  }
  if (static_cast<std::uint64_t>(info.st_size) > kMaxPlayerArtBytes) {
    ::close(fd);
    return FileRead::kTooLarge;
  }
  bytes->assign(static_cast<std::size_t>(info.st_size), 0);
  std::size_t offset = 0;
  bool ok = true;
  while (offset < bytes->size()) {
    const auto count = ::read(fd, bytes->data() + offset, bytes->size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ok = false; break; }
    offset += static_cast<std::size_t>(count);
  }
  ::close(fd);
  return ok ? FileRead::kOk : FileRead::kUnreadable;
}

// One sentence for a picture that can't be used, or empty.
std::string Problem(const char* name, FileRead read, PlayerArtCheck check) {
  const std::string file = std::string(kFolderArtDirectory) + "/" + name;
  switch (read) {
    case FileRead::kMissing: return {};
    case FileRead::kNotFile: return file + " is not a file.";
    case FileRead::kTooLarge: return file + " is larger than 32 MB.";
    case FileRead::kUnreadable: return "Playden couldn't read " + file + ".";
    case FileRead::kOk: break;
  }
  switch (check) {
    case PlayerArtCheck::kOk: return {};
    case PlayerArtCheck::kNotPng: return file + " is not a PNG picture.";
    case PlayerArtCheck::kTooLarge: return file + " is larger than 3840x2160 pixels.";
    case PlayerArtCheck::kUnreadable: return file + " is a damaged PNG picture.";
  }
  return {};
}

bool Valid(const ArtworkImage& image) {
  return image.width != 0 && image.height != 0 &&
         image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4;
}

// One output pixel's colour from the picture's window [x0, x1) x [y0, y1), in picture pixels: averaged when it
// spans more than a pixel, bilinear when the picture is scaled up. See-through parts are black.
void Sample(const ArtworkImage& picture, double x0, double x1, double y0, double y1, std::uint8_t out[3]) {
  const auto alpha_weighted = [&](std::uint32_t x, std::uint32_t y, double weight, double sum[3]) {
    const auto* p = &picture.rgba[(static_cast<std::size_t>(y) * picture.width + x) * 4];
    for (int c = 0; c < 3; ++c) sum[c] += weight * p[c] * p[3] / 255.0;
  };
  double sum[3] = {0, 0, 0};
  if (x1 - x0 <= 1.0 && y1 - y0 <= 1.0) {
    const double cx = std::clamp((x0 + x1) / 2 - 0.5, 0.0, picture.width - 1.0);
    const double cy = std::clamp((y0 + y1) / 2 - 0.5, 0.0, picture.height - 1.0);
    const auto ix = static_cast<std::uint32_t>(cx), iy = static_cast<std::uint32_t>(cy);
    const std::uint32_t jx = std::min(ix + 1, picture.width - 1), jy = std::min(iy + 1, picture.height - 1);
    const double fx = cx - ix, fy = cy - iy;
    alpha_weighted(ix, iy, (1 - fx) * (1 - fy), sum);
    alpha_weighted(jx, iy, fx * (1 - fy), sum);
    alpha_weighted(ix, jy, (1 - fx) * fy, sum);
    alpha_weighted(jx, jy, fx * fy, sum);
  } else {
    const auto sx0 = static_cast<std::uint32_t>(std::floor(x0));
    const auto sy0 = static_cast<std::uint32_t>(std::floor(y0));
    const auto sx1 = std::clamp(static_cast<std::uint32_t>(std::ceil(x1)), sx0 + 1, picture.width);
    const auto sy1 = std::clamp(static_cast<std::uint32_t>(std::ceil(y1)), sy0 + 1, picture.height);
    for (std::uint32_t y = sy0; y < sy1; ++y)
      for (std::uint32_t x = sx0; x < sx1; ++x) alpha_weighted(x, y, 1.0, sum);
    const double count = static_cast<double>(sx1 - sx0) * (sy1 - sy0);
    for (double& value : sum) value /= count;
  }
  for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(std::clamp(std::lround(sum[c]), 0L, 255L));
}

std::uint16_t Rgb565(const int rgb[3]) {
  const auto r = static_cast<std::uint16_t>((rgb[0] * 31 + 127) / 255);
  const auto g = static_cast<std::uint16_t>((rgb[1] * 63 + 127) / 255);
  const auto b = static_cast<std::uint16_t>((rgb[2] * 31 + 127) / 255);
  return static_cast<std::uint16_t>(r << 11 | g << 5 | b);
}

void Expand565(std::uint16_t colour, int rgb[3]) {
  const int r = colour >> 11 & 31, g = colour >> 5 & 63, b = colour & 31;
  rgb[0] = r << 3 | r >> 2, rgb[1] = g << 2 | g >> 4, rgb[2] = b << 3 | b >> 2;
}

// One 4x4 block (16 RGB pixels) as DXT1 in its four-colour mode: endpoints on one of the four diagonals of the
// block's colour box (the one that fits best), each pixel the nearest of the four colours.
void EncodeBlock(const std::uint8_t pixels[16][3], std::uint8_t out[8]) {
  int low[3] = {255, 255, 255}, high[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i)
    for (int c = 0; c < 3; ++c) low[c] = std::min<int>(low[c], pixels[i][c]), high[c] = std::max<int>(high[c], pixels[i][c]);
  std::uint32_t best_error = UINT32_MAX;
  std::uint16_t best_colours[2] = {0, 0};
  std::uint32_t best_indices = 0;
  for (int diagonal = 0; diagonal < 4; ++diagonal) {
    int a[3] = {high[0], high[1], high[2]}, b[3] = {low[0], low[1], low[2]};
    if (diagonal & 1) std::swap(a[1], b[1]);
    if (diagonal & 2) std::swap(a[2], b[2]);
    std::uint16_t c0 = Rgb565(a), c1 = Rgb565(b);
    if (c0 < c1) std::swap(c0, c1);
    int palette[4][3];
    Expand565(c0, palette[0]);
    Expand565(c1, palette[1]);
    for (int c = 0; c < 3; ++c) {
      palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
      palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
    }
    // Equal endpoints: DXT1 reads the block in its three-colour mode, where index 3 is transparent black.
    const int choices = c0 == c1 ? 1 : 4;
    std::uint32_t error = 0, indices = 0;
    for (int i = 0; i < 16; ++i) {
      std::uint32_t nearest = UINT32_MAX;
      int index = 0;
      for (int k = 0; k < choices; ++k) {
        std::uint32_t distance = 0;
        for (int c = 0; c < 3; ++c) {
          const int d = pixels[i][c] - palette[k][c];
          distance += static_cast<std::uint32_t>(d * d);
        }
        if (distance < nearest) nearest = distance, index = k;
      }
      error += nearest;
      indices |= static_cast<std::uint32_t>(index) << (2 * i);
    }
    if (error < best_error) {
      best_error = error, best_indices = indices;
      best_colours[0] = c0, best_colours[1] = c1;
    }
  }
  out[0] = static_cast<std::uint8_t>(best_colours[0]), out[1] = static_cast<std::uint8_t>(best_colours[0] >> 8);
  out[2] = static_cast<std::uint8_t>(best_colours[1]), out[3] = static_cast<std::uint8_t>(best_colours[1] >> 8);
  for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<std::uint8_t>(best_indices >> (8 * i));
}

void PutU32(std::string* out, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) (*out)[at + i] = static_cast<char>(value >> (8 * i));
}

// The 128-byte header of a single-level DXT1 DDS file: the layout of Playden's own pic0.dds.
std::string DdsHeader(std::uint32_t width, std::uint32_t height) {
  std::string header(128, '\0');
  std::memcpy(header.data(), "DDS ", 4);
  PutU32(&header, 4, 124);                                  // header size
  PutU32(&header, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);   // caps, height, width, pixel format, linear size
  PutU32(&header, 12, height);
  PutU32(&header, 16, width);
  PutU32(&header, 20, (width / 4) * (height / 4) * 8);      // linear size
  PutU32(&header, 28, 1);                                   // mipmaps
  PutU32(&header, 76, 32);                                  // pixel format size
  PutU32(&header, 80, 0x4);                                 // four-character code
  std::memcpy(header.data() + 84, "DXT1", 4);
  PutU32(&header, 108, 0x1000);                             // texture
  return header;
}

}  // namespace

std::optional<std::string> MakeTitleBackground(const ArtworkImage& picture) {
  if (!Valid(picture)) return std::nullopt;
  constexpr std::uint32_t kWidth = kTitleBackgroundWidth, kHeight = kTitleBackgroundHeight;
  // The centred window of the picture with the background's shape.
  double window_w = picture.width, window_h = picture.height;
  if (static_cast<std::uint64_t>(picture.width) * kHeight > static_cast<std::uint64_t>(picture.height) * kWidth)
    window_w = static_cast<double>(picture.height) * kWidth / kHeight;
  else
    window_h = static_cast<double>(picture.width) * kHeight / kWidth;
  const double left = (picture.width - window_w) / 2, top = (picture.height - window_h) / 2;
  const double step_x = window_w / kWidth, step_y = window_h / kHeight;
  std::string dds = DdsHeader(kWidth, kHeight);
  dds.reserve(dds.size() + static_cast<std::size_t>(kWidth / 4) * (kHeight / 4) * 8);
  // Four rows at a time, on the heap: the whole 3840x2160 picture is never held a second time.
  std::vector<std::uint8_t> rows(static_cast<std::size_t>(kWidth) * 4 * 3);
  for (std::uint32_t band = 0; band < kHeight; band += 4) {
    for (std::uint32_t row = 0; row < 4; ++row) {
      const double y0 = top + (band + row) * step_y, y1 = y0 + step_y;
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        const double x0 = left + x * step_x;
        Sample(picture, x0, x0 + step_x, y0, y1, &rows[(static_cast<std::size_t>(row) * kWidth + x) * 3]);
      }
    }
    for (std::uint32_t x = 0; x < kWidth; x += 4) {
      std::uint8_t block[16][3];
      for (int i = 0; i < 16; ++i)
        std::memcpy(block[i], &rows[(static_cast<std::size_t>(i / 4) * kWidth + x + i % 4) * 3], 3);
      char encoded[8];
      EncodeBlock(block, reinterpret_cast<std::uint8_t*>(encoded));
      dds.append(encoded, sizeof(encoded));
    }
  }
  return dds;
}

std::vector<std::string> CheckFolderArt(const std::filesystem::path& game_folder) {
  std::vector<std::string> problems;
  for (const char* name : {kFolderIconName, kFolderBackgroundName}) {
    std::vector<std::uint8_t> bytes;
    const auto read = ReadArtFile(game_folder, name, &bytes);
    auto check = read == FileRead::kOk ? CheckPlayerArt(bytes) : PlayerArtCheck::kOk;
    // Decoded in full, one at a time: a picture damaged past its header is told now, not when the title is made.
    if (read == FileRead::kOk && check == PlayerArtCheck::kOk && !DecodePlayerArt(bytes)) check = PlayerArtCheck::kUnreadable;
    auto problem = Problem(name, read, check);
    if (!problem.empty()) problems.push_back(std::move(problem));
  }
  return problems;
}

FolderArt MakeFolderArt(const std::filesystem::path& game_folder) {
  FolderArt art;
  // One picture decoded at a time.
  const auto picture = [&](const char* name) -> std::optional<ArtworkImage> {
    std::vector<std::uint8_t> bytes;
    const auto read = ReadArtFile(game_folder, name, &bytes);
    const auto check = read == FileRead::kOk ? CheckPlayerArt(bytes) : PlayerArtCheck::kOk;
    if (auto problem = Problem(name, read, check); !problem.empty()) {
      art.problems.push_back(std::move(problem));
      return std::nullopt;
    }
    if (read != FileRead::kOk) return std::nullopt;
    auto image = DecodePlayerArt(bytes);
    if (!image) art.problems.push_back(Problem(name, FileRead::kOk, PlayerArtCheck::kUnreadable));
    return image;
  };
  if (const auto icon = picture(kFolderIconName)) {
    art.icon_png = MakeTitleIcon(*icon);
    if (!art.icon_png) art.problems.push_back(Problem(kFolderIconName, FileRead::kOk, PlayerArtCheck::kUnreadable));
  }
  if (const auto background = picture(kFolderBackgroundName)) {
    art.background_dds = MakeTitleBackground(*background);
    if (!art.background_dds)
      art.problems.push_back(Problem(kFolderBackgroundName, FileRead::kOk, PlayerArtCheck::kUnreadable));
  }
  return art;
}

}  // namespace playden::homebrew
