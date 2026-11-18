// SPDX-License-Identifier: GPL-3.0-or-later
// Title art from an imported game's playden folder (#444): the icon and the DXT1 background, and the pictures that
// can't be used.
#include "homebrew/folder_art.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "miniz.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
using playden::homebrew::ArtworkImage;

namespace {

ArtworkImage Solid(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                   std::uint8_t a = 255) {
  ArtworkImage image{width, height, {}};
  image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
    image.rgba[i] = r, image.rgba[i + 1] = g, image.rgba[i + 2] = b, image.rgba[i + 3] = a;
  }
  return image;
}

std::string Png(const ArtworkImage& image) {
  std::string png;
  stbi_write_png_to_func([](void* context, void* data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
  }, &png, static_cast<int>(image.width), static_cast<int>(image.height), 4, image.rgba.data(),
                         static_cast<int>(image.width * 4));
  return png;
}

void Write(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t U32(const std::string& bytes, std::size_t at) {
  std::uint32_t value = 0;
  for (int i = 3; i >= 0; --i) value = value << 8 | static_cast<std::uint8_t>(bytes[at + i]);
  return value;
}

// The colour of pixel (x, y) in a 3840x2160 DXT1 DDS file.
void DxtPixel(const std::string& dds, std::uint32_t x, std::uint32_t y, int rgb[3]) {
  const std::size_t block = 128 + (static_cast<std::size_t>(y / 4) * (3840 / 4) + x / 4) * 8;
  const auto colour = [&](std::size_t at, int out[3]) {
    const unsigned value = static_cast<std::uint8_t>(dds[at]) | static_cast<std::uint8_t>(dds[at + 1]) << 8;
    const int r = value >> 11 & 31, g = value >> 5 & 63, b = value & 31;
    out[0] = r << 3 | r >> 2, out[1] = g << 2 | g >> 4, out[2] = b << 3 | b >> 2;
  };
  int palette[4][3];
  colour(block, palette[0]);
  colour(block + 2, palette[1]);
  for (int c = 0; c < 3; ++c) {
    palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
    palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
  }
  const unsigned indices = U32(dds, block + 4);
  const int index = indices >> (2 * ((y % 4) * 4 + x % 4)) & 3;
  for (int c = 0; c < 3; ++c) rgb[c] = palette[index][c];
}

bool Near(const int rgb[3], int r, int g, int b) {
  return std::abs(rgb[0] - r) <= 8 && std::abs(rgb[1] - g) <= 8 && std::abs(rgb[2] - b) <= 8;
}

// A 3840x2160 RGBA PNG with 16 bits a channel, all one colour: 66 MB of rows, a few hundred KB compressed.
std::string Png16Bit4K(std::uint16_t r, std::uint16_t g, std::uint16_t b) {
  constexpr std::uint32_t kWidth = 3840, kHeight = 2160;
  std::vector<unsigned char> rows(static_cast<std::size_t>(kHeight) * (1 + kWidth * 8));
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    unsigned char* row = &rows[static_cast<std::size_t>(y) * (1 + kWidth * 8)];
    row[0] = 0;  // no filter
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const std::uint16_t channels[4] = {r, g, b, 0xffff};
      for (int c = 0; c < 4; ++c) row[1 + x * 8 + c * 2] = channels[c] >> 8, row[2 + x * 8 + c * 2] = channels[c] & 0xff;
    }
  }
  mz_ulong compressed_size = mz_compressBound(static_cast<mz_ulong>(rows.size()));
  std::vector<unsigned char> compressed(compressed_size);
  CHECK(mz_compress2(compressed.data(), &compressed_size, rows.data(), static_cast<mz_ulong>(rows.size()), 1) == MZ_OK);
  std::string png("\x89PNG\r\n\x1a\n", 8);
  const auto chunk = [&png](const char* type, const unsigned char* data, std::size_t size) {
    std::string body(type, 4);
    body.append(reinterpret_cast<const char*>(data), size);
    for (int i = 3; i >= 0; --i) png.push_back(static_cast<char>(size >> (8 * i)));
    png += body;
    const auto crc = mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(body.data()), body.size());
    for (int i = 3; i >= 0; --i) png.push_back(static_cast<char>(crc >> (8 * i)));
  };
  const unsigned char header[13] = {0, 0, 0x0f, 0x00, 0, 0, 0x08, 0x70, 16, 6, 0, 0, 0};  // 3840x2160, 16-bit RGBA
  chunk("IHDR", header, sizeof(header));
  chunk("IDAT", compressed.data(), compressed_size);
  chunk("IEND", nullptr, 0);
  return png;
}

bool Mentions(const std::vector<std::string>& problems, const std::string& words) {
  for (const auto& problem : problems)
    if (problem.find(words) != std::string::npos) return true;
  return false;
}

}  // namespace

int main() {
  using namespace playden::homebrew;

  // The background: Playden's own pic0.dds layout, 3840x2160 DXT1 in one level.
  {
    const auto dds = MakeTitleBackground(Solid(1920, 1080, 200, 40, 90));
    CHECK(dds && dds->size() == 128 + 3840 / 4 * (2160 / 4) * 8);
    CHECK(dds->compare(0, 4, "DDS ") == 0 && U32(*dds, 4) == 124);
    CHECK(U32(*dds, 12) == 2160 && U32(*dds, 16) == 3840 && U32(*dds, 28) == 1);
    CHECK(U32(*dds, 80) == 0x4 && dds->compare(84, 4, "DXT1") == 0);
    int rgb[3];
    DxtPixel(*dds, 0, 0, rgb);
    CHECK(Near(rgb, 200, 40, 90));
    DxtPixel(*dds, 3839, 2159, rgb);
    CHECK(Near(rgb, 200, 40, 90));
  }
  // A wider picture keeps its centre: red, green and blue thirds of a 3:1 picture leave only green.
  {
    ArtworkImage wide = Solid(900, 300, 0, 0, 0);
    for (std::uint32_t y = 0; y < 300; ++y)
      for (std::uint32_t x = 0; x < 900; ++x) wide.rgba[(y * 900 + x) * 4 + (x < 300 ? 0 : x < 600 ? 1 : 2)] = 255;
    const auto dds = MakeTitleBackground(wide);
    CHECK(dds);
    int rgb[3];
    DxtPixel(*dds, 1920, 1080, rgb);
    CHECK(Near(rgb, 0, 255, 0));
    DxtPixel(*dds, 0, 1080, rgb);  // the 16:9 window spans x 183..717: its left edge is red, not blue
    CHECK(Near(rgb, 255, 0, 0));
    DxtPixel(*dds, 3839, 1080, rgb);
    CHECK(Near(rgb, 0, 0, 255));
  }
  // A taller one too, and a picture larger than the background is averaged down; see-through parts are black.
  {
    ArtworkImage tall = Solid(400, 1600, 0, 0, 0);
    for (std::uint32_t y = 0; y < 1600; ++y)
      for (std::uint32_t x = 0; x < 400; ++x) tall.rgba[(y * 400 + x) * 4 + (y < 600 ? 0 : y < 1000 ? 1 : 2)] = 255;
    const auto dds = MakeTitleBackground(tall);
    int rgb[3];
    DxtPixel(*dds, 1920, 0, rgb);  // the window spans y 687..912: green only
    CHECK(Near(rgb, 0, 255, 0));
    DxtPixel(*dds, 1920, 2159, rgb);
    CHECK(Near(rgb, 0, 255, 0));
    const auto large = MakeTitleBackground(Solid(3840, 2160, 10, 20, 250));
    DxtPixel(*large, 100, 100, rgb);
    CHECK(Near(rgb, 10, 20, 250));
    const auto clear = MakeTitleBackground(Solid(16, 9, 255, 255, 255, 0));
    DxtPixel(*clear, 7, 7, rgb);
    CHECK(Near(rgb, 0, 0, 0));
  }
  // Malformed pictures make no background.
  CHECK(!MakeTitleBackground(ArtworkImage{}));
  CHECK(!MakeTitleBackground(ArtworkImage{10, 10, std::vector<std::uint8_t>(3)}));

  // The game folder: nothing there is nothing used and nothing said.
  const fs::path root = fs::current_path() / "homebrew_folder_art_test.files";
  fs::remove_all(root);
  const auto game = root / "game";
  fs::create_directories(game);
  {
    const auto art = MakeFolderArt(game);
    CHECK(!art.icon_png && !art.background_dds && art.problems.empty());
    CHECK(CheckFolderArt(game).empty());
  }
  // Both pictures, of any size: a 512x512 icon of the centred square and a 3840x2160 background.
  fs::create_directories(game / "playden");
  ArtworkImage icon = Solid(300, 200, 0, 0, 255);
  for (std::uint32_t y = 0; y < 200; ++y)
    for (std::uint32_t x = 50; x < 250; ++x) icon.rgba[(y * 300 + x) * 4 + 0] = 255, icon.rgba[(y * 300 + x) * 4 + 2] = 0;
  Write(game / "playden" / "icon.png", Png(icon));
  Write(game / "playden" / "background.png", Png(Solid(640, 360, 30, 160, 60)));
  {
    CHECK(CheckFolderArt(game).empty());
    const auto art = MakeFolderArt(game);
    CHECK(art.problems.empty() && art.icon_png && art.background_dds);
    int width = 0, height = 0, components = 0;
    auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(art.icon_png->data()),
                                         static_cast<int>(art.icon_png->size()), &width, &height, &components, 3);
    CHECK(pixels && width == 512 && height == 512);
    CHECK(pixels[0] == 255 && pixels[2] == 0);  // the red centre square, no blue edge
    stbi_image_free(pixels);
    int rgb[3];
    DxtPixel(*art.background_dds, 1000, 1000, rgb);
    CHECK(Near(rgb, 30, 160, 60));
  }
  // Only a background: the icon stays Playden's, nothing is said.
  fs::remove(game / "playden" / "icon.png");
  {
    const auto art = MakeFolderArt(game);
    CHECK(!art.icon_png && art.background_dds && art.problems.empty());
  }
  // Pictures that can't be used: not a PNG, damaged, larger than 4K, a link, a folder. Each says so; a good one
  // beside them is still used.
  Write(game / "playden" / "icon.png", std::string("\xff\xd8\xff\xe0 a jpeg", 12));
  {
    const auto art = MakeFolderArt(game);
    CHECK(!art.icon_png && art.background_dds && Mentions(art.problems, "playden/icon.png is not a PNG picture."));
    CHECK(Mentions(CheckFolderArt(game), "playden/icon.png is not a PNG picture."));
  }
  auto damaged = Png(Solid(64, 64, 1, 2, 3));
  damaged.resize(40);
  Write(game / "playden" / "icon.png", damaged);
  CHECK(Mentions(MakeFolderArt(game).problems, "playden/icon.png is a damaged PNG picture."));
  CHECK(Mentions(CheckFolderArt(game), "playden/icon.png is a damaged PNG picture."));
  // Damaged past its header (the compressed rows): the check when the game is added decodes it, and says so then.
  auto corrupt = Png(Solid(64, 64, 1, 2, 3));
  for (std::size_t at = 45; at < 60; ++at) corrupt[at] = static_cast<char>(0xff);
  Write(game / "playden" / "icon.png", corrupt);
  CHECK(Mentions(CheckFolderArt(game), "playden/icon.png is a damaged PNG picture."));
  CHECK(Mentions(MakeFolderArt(game).problems, "playden/icon.png is a damaged PNG picture."));
  // A 4K picture with 16 bits a channel decodes within its budget, both when checked and when used.
  fs::remove(game / "playden" / "icon.png");
  Write(game / "playden" / "background.png", Png16Bit4K(0x2000, 0xa000, 0x4000));
  CHECK(CheckFolderArt(game).empty());
  {
    const auto art = MakeFolderArt(game);
    CHECK(art.problems.empty() && art.background_dds);
    int rgb[3];
    DxtPixel(*art.background_dds, 2000, 1000, rgb);
    CHECK(Near(rgb, 0x20, 0xa0, 0x40));
  }
  // A 4000x2200 header: larger than the background, refused before it is decoded.
  auto huge = Png(Solid(4, 4, 1, 2, 3));
  const unsigned char size[8] = {0, 0, 0x0f, 0xa0, 0, 0, 0x08, 0x98};
  std::memcpy(huge.data() + 16, size, sizeof(size));
  Write(game / "playden" / "icon.png", huge);
  CHECK(Mentions(MakeFolderArt(game).problems, "playden/icon.png is larger than 3840x2160 pixels."));
  CHECK(Mentions(CheckFolderArt(game), "playden/icon.png is larger than 3840x2160 pixels."));
  fs::remove(game / "playden" / "icon.png");
  Write(root / "outside.png", Png(icon));
  fs::create_symlink(root / "outside.png", game / "playden" / "icon.png");
  {
    const auto art = MakeFolderArt(game);
    CHECK(!art.icon_png && Mentions(art.problems, "playden/icon.png is not a file."));
  }
  fs::remove(game / "playden" / "icon.png");
  fs::create_directory(game / "playden" / "icon.png");
  CHECK(Mentions(MakeFolderArt(game).problems, "playden/icon.png is not a file."));
  // A playden folder that is a link is never followed out of the game's folder.
  fs::remove_all(game / "playden");
  fs::create_directories(root / "elsewhere");
  Write(root / "elsewhere" / "icon.png", Png(icon));
  fs::create_directory_symlink(root / "elsewhere", game / "playden");
  {
    const auto art = MakeFolderArt(game);
    CHECK(!art.icon_png && !art.background_dds && art.problems.empty());
  }
  fs::remove_all(root);
  std::puts("folder art tests passed");
}
