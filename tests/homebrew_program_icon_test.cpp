// homebrew_program_icon_test.cpp - A program's icon from its PE resources, and the tile made from it.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/program_icon.hpp"
#include "homebrew/title_icon.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
using namespace playden::homebrew;
using Bytes = std::string;

namespace {

void Put16(Bytes* b, std::size_t at, std::uint32_t v) { (*b)[at] = char(v & 0xff); (*b)[at + 1] = char((v >> 8) & 0xff); }
void Put32(Bytes* b, std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) (*b)[at + i] = char((v >> (8 * i)) & 0xff); }
void Add16(Bytes* b, std::uint32_t v) { b->resize(b->size() + 2); Put16(b, b->size() - 2, v); }
void Add32(Bytes* b, std::uint32_t v) { b->resize(b->size() + 4); Put32(b, b->size() - 4, v); }

constexpr std::uint32_t kRsrcRva = 0x1000;
constexpr std::uint32_t kRsrcFile = 0x200;

struct Icon {
  std::uint8_t width, height;  // as the group lists them (0: 256)
  std::uint16_t bits;
  Bytes data;
};

// A resource section: RT_ICON (3) with one entry per icon (ids 1..n), RT_GROUP_ICON (14) with group 101.
Bytes Resources(const std::vector<Icon>& icons) {
  Bytes group;
  Add16(&group, 0); Add16(&group, 1); Add16(&group, static_cast<std::uint32_t>(icons.size()));
  for (std::size_t i = 0; i < icons.size(); ++i) {
    group += char(icons[i].width); group += char(icons[i].height); group += '\0'; group += '\0';
    Add16(&group, 1); Add16(&group, icons[i].bits); Add32(&group, static_cast<std::uint32_t>(icons[i].data.size()));
    Add16(&group, static_cast<std::uint32_t>(i + 1));
  }
  std::vector<Bytes> blobs;
  for (const auto& icon : icons) blobs.push_back(icon.data);
  blobs.push_back(group);
  const std::size_t n = icons.size();
  // Directories: root (2 entries), icons (n), group (1), then one language directory per blob (1 entry each).
  const auto dir_size = [](std::size_t entries) { return 16 + 8 * entries; };
  const std::uint32_t icons_dir = static_cast<std::uint32_t>(dir_size(2));
  const std::uint32_t group_dir = icons_dir + static_cast<std::uint32_t>(dir_size(n));
  const std::uint32_t languages = group_dir + static_cast<std::uint32_t>(dir_size(1));
  const std::uint32_t leaves = languages + static_cast<std::uint32_t>(dir_size(1) * blobs.size());
  std::uint32_t data = leaves + static_cast<std::uint32_t>(16 * blobs.size());
  Bytes out;
  const auto directory = [&](std::vector<std::pair<std::uint32_t, std::uint32_t>> entries) {
    Add32(&out, 0); Add32(&out, 0); Add32(&out, 0); Add16(&out, 0); Add16(&out, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [id, offset] : entries) { Add32(&out, id); Add32(&out, offset); }
  };
  directory({{3, 0x80000000u | icons_dir}, {14, 0x80000000u | group_dir}});
  std::vector<std::pair<std::uint32_t, std::uint32_t>> icon_entries;
  for (std::size_t i = 0; i < n; ++i)
    icon_entries.push_back({static_cast<std::uint32_t>(i + 1), 0x80000000u | (languages + static_cast<std::uint32_t>(dir_size(1) * i))});
  directory(icon_entries);
  directory({{101, 0x80000000u | (languages + static_cast<std::uint32_t>(dir_size(1) * n))}});
  for (std::size_t i = 0; i < blobs.size(); ++i) directory({{1033, leaves + static_cast<std::uint32_t>(16 * i)}});
  for (const auto& blob : blobs) {
    Add32(&out, kRsrcRva + data); Add32(&out, static_cast<std::uint32_t>(blob.size())); Add32(&out, 0); Add32(&out, 0);
    data += static_cast<std::uint32_t>((blob.size() + 3) / 4 * 4);
  }
  for (const auto& blob : blobs) { out += blob; out.resize((out.size() + 3) / 4 * 4, '\0'); }
  return out;
}

// A 32-bit program whose one section is the resources (or no resource directory at all).
Bytes Program(const Bytes& rsrc, bool with_directory = true) {
  Bytes pe(kRsrcFile, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; Put32(&pe, 0x3c, 0x40);
  pe[0x40] = 'P'; pe[0x41] = 'E';
  Put16(&pe, 0x44, 0x14c); Put16(&pe, 0x46, 1); Put16(&pe, 0x54, 0xe0);
  const std::size_t opt = 0x58;
  Put16(&pe, opt, 0x10b);
  Put32(&pe, opt + 92, 16);
  if (with_directory) { Put32(&pe, opt + 96 + 16, kRsrcRva); Put32(&pe, opt + 96 + 20, static_cast<std::uint32_t>(rsrc.size())); }
  const std::size_t section = opt + 0xe0;
  std::copy_n(".rsrc", 5, pe.begin() + section);
  Put32(&pe, section + 8, static_cast<std::uint32_t>(rsrc.size()));
  Put32(&pe, section + 12, kRsrcRva);
  Put32(&pe, section + 16, static_cast<std::uint32_t>(rsrc.size()));
  Put32(&pe, section + 20, kRsrcFile);
  return pe + rsrc;
}

// A bottom-up BMP icon image (BITMAPINFOHEADER with the doubled height) from top-down RGBA pixels.
Bytes Dib(int w, int h, int bits, const std::vector<std::uint32_t>& rgba, const std::vector<std::uint32_t>& palette = {},
          const std::vector<bool>& transparent = {}) {
  Bytes out;
  Add32(&out, 40); Add32(&out, w); Add32(&out, 2 * h); Add16(&out, 1); Add16(&out, bits);
  Add32(&out, 0); Add32(&out, 0); Add32(&out, 0); Add32(&out, 0);
  Add32(&out, static_cast<std::uint32_t>(palette.size())); Add32(&out, 0);
  for (auto colour : palette) { out += char((colour >> 8) & 0xff); out += char((colour >> 16) & 0xff); out += char(colour >> 24); out += '\0'; }
  const std::size_t stride = (static_cast<std::size_t>(w) * bits + 31) / 32 * 4;
  for (int y = h - 1; y >= 0; --y) {
    Bytes row(stride, '\0');
    for (int x = 0; x < w; ++x) {
      const auto p = rgba[static_cast<std::size_t>(y) * w + x];  // 0xRRGGBBAA, or a palette index
      if (bits == 32) { row[x * 4] = char((p >> 8) & 0xff); row[x * 4 + 1] = char((p >> 16) & 0xff); row[x * 4 + 2] = char(p >> 24); row[x * 4 + 3] = char(p & 0xff); }
      else if (bits == 24) { row[x * 3] = char((p >> 8) & 0xff); row[x * 3 + 1] = char((p >> 16) & 0xff); row[x * 3 + 2] = char(p >> 24); }
      else if (bits == 4) row[x / 2] = char(row[x / 2] | (p << (x % 2 ? 0 : 4)));
    }
    out += row;
  }
  const std::size_t mask_stride = (static_cast<std::size_t>(w) + 31) / 32 * 4;
  for (int y = h - 1; y >= 0; --y) {
    Bytes row(mask_stride, '\0');
    for (int x = 0; x < w; ++x)
      if (!transparent.empty() && transparent[static_cast<std::size_t>(y) * w + x]) row[x / 8] = char(row[x / 8] | (0x80 >> (x % 8)));
    out += row;
  }
  return out;
}

Bytes Png(int w, int h, std::uint32_t rgba) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < pixels.size(); i += 4)
    pixels[i] = rgba >> 24, pixels[i + 1] = (rgba >> 16) & 0xff, pixels[i + 2] = (rgba >> 8) & 0xff, pixels[i + 3] = rgba & 0xff;
  Bytes png;
  stbi_write_png_to_func([](void* context, void* data, int size) { static_cast<Bytes*>(context)->append(static_cast<char*>(data), size); },
                         &png, w, h, 4, pixels.data(), w * 4);
  return png;
}

std::string Write(const fs::path& path, const Bytes& bytes) {
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file);
  std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  return path.string();
}

const std::uint8_t* Pixel(const ArtworkImage& image, std::uint32_t x, std::uint32_t y) {
  return &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
}

struct FakeBackend : ArtworkBackend {
  std::vector<std::string> asked;
  std::optional<ArtworkImage> Load(const ArtworkKey& key, const std::atomic<bool>&) override {
    asked.push_back(key.source);
    return ArtworkImage{1, 1, {1, 2, 3, 255}};
  }
};

}  // namespace

int main() {
  // The real path: the system's temporary folder is behind a link on macOS, and links are refused.
  const fs::path root = fs::canonical(fs::temp_directory_path()) / ("playden-program-icon-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "Game");

  // A PNG and a smaller BMP: the largest, the PNG, is the icon.
  const auto both = Write(root / "Game/Both.exe", Program(Resources({
      {16, 16, 32, Dib(16, 16, 32, std::vector<std::uint32_t>(256, 0x00ff00ffu))},
      {0, 0, 32, Png(256, 256, 0xff0000ffu)}})));
  auto icon = ReadProgramIcon(both);
  CHECK(icon && icon->width == 256 && icon->height == 256 && Pixel(*icon, 10, 10)[0] == 255 && Pixel(*icon, 10, 10)[3] == 255);

  // A 32-bit BMP: rows bottom-up in the file, top-down in the image, with its own alpha.
  std::vector<std::uint32_t> bands(4 * 4);
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) bands[y * 4 + x] = y == 0 ? 0xff000080u : 0x0000ffffu;
  icon = ReadProgramIcon(Write(root / "Game/Bands.exe", Program(Resources({{4, 4, 32, Dib(4, 4, 32, bands)}}))));
  CHECK(icon && icon->width == 4 && icon->height == 4);
  CHECK(Pixel(*icon, 0, 0)[0] == 255 && Pixel(*icon, 0, 0)[3] == 0x80);
  CHECK(Pixel(*icon, 0, 3)[2] == 255 && Pixel(*icon, 0, 3)[0] == 0 && Pixel(*icon, 0, 3)[3] == 255);

  // A 4-bit BMP: palette colours, and the mask's set bits are transparent; 24-bit uses the mask too.
  std::vector<std::uint32_t> indexes(8 * 2, 1);
  indexes[0] = 0;
  std::vector<bool> holes(8 * 2, false);
  holes[7] = true;
  icon = ReadProgramIcon(Write(root / "Game/Old.exe", Program(Resources({
      {8, 2, 4, Dib(8, 2, 4, indexes, {0x000000ffu, 0x00ff00ffu}, holes)}}))));
  CHECK(icon && icon->width == 8 && icon->height == 2);
  CHECK(Pixel(*icon, 0, 0)[1] == 0 && Pixel(*icon, 0, 0)[3] == 255);
  CHECK(Pixel(*icon, 1, 0)[1] == 255 && Pixel(*icon, 7, 0)[3] == 0 && Pixel(*icon, 7, 1)[3] == 255);
  icon = ReadProgramIcon(Write(root / "Game/True.exe", Program(Resources({
      {8, 2, 24, Dib(8, 2, 24, std::vector<std::uint32_t>(16, 0x123456ffu), {}, holes)}}))));
  CHECK(icon && Pixel(*icon, 0, 0)[0] == 0x12 && Pixel(*icon, 0, 0)[2] == 0x56 && Pixel(*icon, 7, 0)[3] == 0);

  // A broken largest image falls back to the next one.
  icon = ReadProgramIcon(Write(root / "Game/Fallback.exe", Program(Resources({
      {0, 0, 32, Bytes("\x89PNG\r\n\x1a\nbroken", 14)},
      {4, 4, 32, Dib(4, 4, 32, bands)}}))));
  CHECK(icon && icon->width == 4);

  // Nothing, and no crash, for what isn't a program with an icon.
  CHECK(!ReadProgramIcon((root / "Game/Missing.exe").string()));
  CHECK(!ReadProgramIcon(Write(root / "Game/Text.exe", "MZ just text")));
  CHECK(!ReadProgramIcon(Write(root / "Game/Bare.exe", Program(Bytes(64, '\0'), false))));
  CHECK(!ReadProgramIcon(Write(root / "Game/Empty.exe", Program(Resources({})))));
  {  // A BMP claiming 4096 pixels a side, and one with more rows than bytes.
    auto big = Dib(4, 4, 32, bands);
    Put32(&big, 4, 4096); Put32(&big, 8, 8192);
    CHECK(!ReadProgramIcon(Write(root / "Game/Big.exe", Program(Resources({{4, 4, 32, big}})))));
    auto short_rows = Dib(4, 4, 32, bands);
    Put32(&short_rows, 8, 2 * 400);
    CHECK(!ReadProgramIcon(Write(root / "Game/Short.exe", Program(Resources({{4, 4, 32, short_rows}})))));
  }
  {  // A directory claiming more entries than fit.
    auto rsrc = Resources({{4, 4, 32, Dib(4, 4, 32, bands)}});
    Put16(&rsrc, 14, 0xffff);
    CHECK(!ReadProgramIcon(Write(root / "Game/Count.exe", Program(rsrc))));
  }
  // Every truncation of a good program reads safely.
  const auto good = Program(Resources({{0, 0, 32, Png(32, 32, 0xff0000ffu)}, {4, 4, 32, Dib(4, 4, 32, bands)}}));
  for (std::size_t size = 0; size < good.size(); size += 3)
    (void)ReadProgramIcon(Write(root / "Game/Cut.exe", good.substr(0, size)));
  // A symbolic link is not followed, as the program or as one of its folders; a FIFO doesn't block.
  fs::create_symlink(root / "Game/Bands.exe", root / "Game/Link.exe");
  CHECK(!ReadProgramIcon((root / "Game/Link.exe").string()));
  fs::create_directory_symlink(root / "Game", root / "Linked");
  CHECK(!ReadProgramIcon((root / "Linked/Bands.exe").string()) && ReadProgramIcon((root / "Game/Bands.exe").string()));
  CHECK(::mkfifo((root / "Game/Pipe.exe").c_str(), 0600) == 0);
  CHECK(!ReadProgramIcon((root / "Game/Pipe.exe").string()));
  CHECK(!ReadProgramIcon("relative/Game.exe"));
  {  // A PNG larger than an icon is skipped before it is decoded; the group's smaller BMP is used.
    auto big = Png(600, 2, 0xff0000ffu);
    CHECK(!ReadProgramIcon(Write(root / "Game/BigPng.exe", Program(Resources({{0, 0, 32, big}})))));
    icon = ReadProgramIcon(Write(root / "Game/BigPngBmp.exe", Program(Resources({{0, 0, 32, big}, {4, 4, 32, Dib(4, 4, 32, bands)}}))));
    CHECK(icon && icon->width == 4);
  }

  // The artwork source carries an absolute .exe path, nothing else.
  CHECK(ProgramIconSource("/data/playden-games/1/Game.exe") == "program-icon:/data/playden-games/1/Game.exe");
  CHECK(ProgramIconSource("").empty());
  CHECK(ProgramIconPath("program-icon:/data/playden-games/1/Game.EXE") == std::string("/data/playden-games/1/Game.EXE"));
  for (const char* bad : {"program-icon:", "program-icon:relative/Game.exe", "program-icon:/a/../b/Game.exe",
                          "program-icon:/a//Game.exe", "program-icon:/a/Game.dll", "https://x/Game.exe", "program-icon:/./Game.exe"})
    CHECK(!ProgramIconPath(bad));

  // The backend reads program icons from the game's files, under the desktop prefix, and passes on the rest.
  FakeBackend next;
  ProgramIconArtworkBackend backend(next, root.string() + "/");
  const std::atomic<bool> cancel{false};
  const playden::GameId game = playden::GameId::Steam(10);
  icon = backend.Load({1, game, ProgramIconSource("/Game/Bands.exe")}, cancel);
  CHECK(icon && icon->width == 4 && next.asked.empty());
  CHECK(!backend.Load({1, game, ProgramIconSource("/Game/Missing.exe")}, cancel) && next.asked.empty());
  icon = backend.Load({1, game, "https://cdn.cloudflare.steamstatic.com/steam/apps/10/library_600x900.jpg"}, cancel);
  CHECK(icon && icon->width == 1 && next.asked.size() == 1);

  // The tile: the icon two thirds of the side, centred on the dark ground; transparency shows the ground.
  ArtworkImage half{2, 2, {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255}};
  auto tile = ProgramIconTile(half, 300);
  CHECK(tile && tile->width == 300 && tile->height == 300);
  CHECK(Pixel(*tile, 0, 0)[0] == 0x24 && Pixel(*tile, 0, 0)[3] == 255);
  CHECK(Pixel(*tile, 150, 150)[0] == 255 && Pixel(*tile, 150, 150)[1] == 0);
  CHECK(Pixel(*tile, 40, 150)[0] == 0x24);  // outside the 200-pixel box
  ArtworkImage clear{1, 1, {255, 255, 255, 0}};
  tile = ProgramIconTile(clear, 30);
  CHECK(tile && Pixel(*tile, 15, 15)[0] == 0x24);
  CHECK(!ProgramIconTile(ArtworkImage{}, 30) && !ProgramIconTile(half, 0));
  const auto png = MakeProgramTitleIcon(half);
  int width = 0, height = 0, components = 0;
  auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png->data()), static_cast<int>(png->size()),
                                       &width, &height, &components, 3);
  CHECK(pixels && width == 512 && height == 512 && pixels[0] == 0x24 && pixels[(256 * 512 + 256) * 3] == 255);
  stbi_image_free(pixels);

  fs::remove_all(root);
  std::puts("program icon tests passed");
}
