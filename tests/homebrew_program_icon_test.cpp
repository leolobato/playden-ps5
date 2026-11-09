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

// An .ico file: ICONDIR, one 16-byte entry per image, then the images in order.
Bytes IcoFile(const std::vector<Icon>& icons) {
  Bytes out;
  Add16(&out, 0); Add16(&out, 1); Add16(&out, static_cast<std::uint32_t>(icons.size()));
  std::uint32_t offset = static_cast<std::uint32_t>(6 + 16 * icons.size());
  for (const auto& icon : icons) {
    out += char(icon.width); out += char(icon.height); out += '\0'; out += '\0';
    Add16(&out, 1); Add16(&out, icon.bits); Add32(&out, static_cast<std::uint32_t>(icon.data.size())); Add32(&out, offset);
    offset += static_cast<std::uint32_t>(icon.data.size());
  }
  for (const auto& icon : icons) out += icon.data;
  return out;
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

  // An .ico file (a GOG copy's goggame-<id>.ico): its largest image, PNG or BMP, as a program's icon reads.
  {
    icon = ReadIconFile(Write(root / "Game/Both.ico", IcoFile({
        {16, 16, 32, Dib(16, 16, 32, std::vector<std::uint32_t>(256, 0x00ff00ffu))},
        {0, 0, 32, Png(256, 256, 0xff000080u)},
        {48, 48, 32, Dib(48, 48, 32, std::vector<std::uint32_t>(48 * 48, 0x0000ffffu))}})));
    CHECK(icon && icon->width == 256 && icon->height == 256 && Pixel(*icon, 10, 10)[0] == 255 && Pixel(*icon, 10, 10)[3] == 0x80);
    icon = ReadIconFile(Write(root / "Game/Bands.ico", IcoFile({{4, 4, 32, Dib(4, 4, 32, bands)}, {2, 2, 32, Dib(2, 2, 32, {0, 0, 0, 0})}})));
    CHECK(icon && icon->width == 4 && Pixel(*icon, 0, 0)[0] == 255 && Pixel(*icon, 0, 0)[3] == 0x80);
    icon = ReadIconFile(Write(root / "Game/Old.ico", IcoFile({{8, 2, 4, Dib(8, 2, 4, indexes, {0x000000ffu, 0x00ff00ffu}, holes)}})));
    CHECK(icon && icon->width == 8 && Pixel(*icon, 1, 0)[1] == 255 && Pixel(*icon, 7, 0)[3] == 0);
    // A broken or oversized largest image falls back to the next one; with none left, nothing.
    icon = ReadIconFile(Write(root / "Game/Fallback.ico", IcoFile({
        {0, 0, 32, Bytes("\x89PNG\r\n\x1a\nbroken", 14)}, {0, 0, 32, Png(600, 2, 0xff0000ffu)}, {4, 4, 32, Dib(4, 4, 32, bands)}})));
    CHECK(icon && icon->width == 4);
    CHECK(!ReadIconFile(Write(root / "Game/BigPng.ico", IcoFile({{0, 0, 32, Png(600, 2, 0xff0000ffu)}}))));
    auto big = Dib(4, 4, 32, bands);
    Put32(&big, 4, 4096); Put32(&big, 8, 8192);
    CHECK(!ReadIconFile(Write(root / "Game/BigBmp.ico", IcoFile({{4, 4, 32, big}}))));
    {  // An image claiming more bytes than an icon may have is not read.
      auto huge = IcoFile({{4, 4, 32, Dib(4, 4, 32, bands)}});
      Put32(&huge, 6 + 8, static_cast<std::uint32_t>(kMaxProgramIconBytes + 1));
      CHECK(!ReadIconFile(Write(root / "Game/Huge.ico", huge)));
    }
    // Malformed: no images, the wrong type, too many entries, an image past the end; every truncation reads safely.
    CHECK(!ReadIconFile(Write(root / "Game/Empty.ico", IcoFile({}))));
    auto cursor = IcoFile({{4, 4, 32, Dib(4, 4, 32, bands)}});
    Put16(&cursor, 2, 2);
    CHECK(!ReadIconFile(Write(root / "Game/Cursor.ico", cursor)));
    auto many = IcoFile({{4, 4, 32, Dib(4, 4, 32, bands)}});
    Put16(&many, 4, 0xffff);
    CHECK(!ReadIconFile(Write(root / "Game/Many.ico", many)));
    auto past = IcoFile({{4, 4, 32, Dib(4, 4, 32, bands)}});
    Put32(&past, 6 + 12, 0x7fffffffu);
    CHECK(!ReadIconFile(Write(root / "Game/Past.ico", past)));
    CHECK(!ReadIconFile(Write(root / "Game/Text.ico", "not an icon")));
    CHECK(!ReadIconFile((root / "Game/Missing.ico").string()));
    const auto good_ico = IcoFile({{0, 0, 32, Png(32, 32, 0xff0000ffu)}, {4, 4, 32, Dib(4, 4, 32, bands)}});
    for (std::size_t size = 0; size < good_ico.size(); size += 3)
      (void)ReadIconFile(Write(root / "Game/Cut.ico", good_ico.substr(0, size)));
    // A symbolic link is not followed, as the file or as one of its folders; a FIFO doesn't block.
    fs::create_symlink(root / "Game/Bands.ico", root / "Game/Link.ico");
    CHECK(!ReadIconFile((root / "Game/Link.ico").string()));
    CHECK(!ReadIconFile((root / "Linked/Bands.ico").string()) && ReadIconFile((root / "Game/Bands.ico").string()));
    CHECK(::mkfifo((root / "Game/Pipe.ico").c_str(), 0600) == 0);
    CHECK(!ReadIconFile((root / "Game/Pipe.ico").string()));
    CHECK(!ReadIconFile("relative/Game.ico"));
  }

  // The artwork source carries an absolute .exe path, nothing else.
  CHECK(ProgramIconSource("/data/playden-games/1/Game.exe") == "program-icon:/data/playden-games/1/Game.exe");
  CHECK(ProgramIconSource("").empty());
  CHECK(ProgramIconPath("program-icon:/data/playden-games/1/Game.EXE") == std::string("/data/playden-games/1/Game.EXE"));
  // Or the game's own .ico file.
  CHECK(ProgramIconPath("program-icon:/data/playden-games/1/goggame-1.ICO") == std::string("/data/playden-games/1/goggame-1.ICO"));
  for (const char* bad : {"program-icon:", "program-icon:relative/Game.exe", "program-icon:/a/../b/Game.exe",
                          "program-icon:/a//Game.exe", "program-icon:/a/Game.dll", "https://x/Game.exe", "program-icon:/./Game.exe",
                          "program-icon:/a/Game.exe\n/a/Other.exe"})
    CHECK(!ProgramIconPath(bad));
  // A .ico names the program to fall back to.
  CHECK(ProgramIconSource("/a/goggame-1.ico", "/a/Game.exe") == "program-icon:/a/goggame-1.ico\n/a/Game.exe");
  CHECK(ProgramIconPath("program-icon:/a/goggame-1.ico\n/a/Game.exe") == std::string("/a/goggame-1.ico"));
  CHECK(ProgramIconFallbackPath("program-icon:/a/goggame-1.ico\n/a/Game.exe") == std::string("/a/Game.exe"));
  CHECK(!ProgramIconFallbackPath("program-icon:/a/goggame-1.ico"));
  CHECK(!ProgramIconFallbackPath("program-icon:/a/goggame-1.ico\n/a/../Game.exe"));
  CHECK(!ProgramIconFallbackPath("program-icon:/a/goggame-1.ico\n/a/other.ico"));

  // The backend reads program icons from the game's files, under the desktop prefix, and passes on the rest.
  FakeBackend next;
  ProgramIconArtworkBackend backend(next, root.string() + "/");
  const std::atomic<bool> cancel{false};
  const playden::GameId game = playden::GameId::Steam(10);
  icon = backend.Load({1, game, ProgramIconSource("/Game/Bands.exe")}, cancel);
  CHECK(icon && icon->width == kProgramIconPosterWidth && icon->height == kProgramIconPosterHeight && next.asked.empty());
  CHECK(!backend.Load({1, game, ProgramIconSource("/Game/Missing.exe")}, cancel) && next.asked.empty());
  // An .ico source is read as an icon file, not as a program.
  icon = backend.Load({1, game, ProgramIconSource("/Game/Both.ico")}, cancel);
  CHECK(icon && icon->width == kProgramIconPosterWidth && icon->height == kProgramIconPosterHeight && next.asked.empty());
  CHECK(!backend.Load({1, game, ProgramIconSource("/Game/Text.ico")}, cancel) && next.asked.empty());
  // One that can't be read falls back to its program's icon, as the title's icon does.
  icon = backend.Load({1, game, ProgramIconSource("/Game/Text.ico", "/Game/Bands.exe")}, cancel);
  CHECK(icon && icon->width == kProgramIconPosterWidth && next.asked.empty());
  icon = backend.Load({1, game, "https://cdn.cloudflare.steamstatic.com/steam/apps/10/library_600x900.jpg"}, cancel);
  CHECK(icon && icon->width == 1 && next.asked.size() == 1);

  // Program icon art: a large opaque icon fills the top square; the part below it is darker.
  const auto solid = [](std::uint32_t side, std::uint8_t alpha) {
    ArtworkImage image{side, side, {}};
    for (std::uint32_t i = 0; i < side * side; ++i) image.rgba.insert(image.rgba.end(), {255, 0, 0, alpha});
    return image;
  };
  const ArtworkImage large = solid(kSmallProgramIconSide, 255);
  CHECK(ProgramIconIsOpaque(large) && !ProgramIconIsOpaque(solid(8, 200)));
  auto art = ProgramIconArt(large, 320, 480);
  CHECK(art && art->width == 320 && art->height == 480 && art->rgba.size() == 320u * 480 * 4);
  CHECK(Pixel(*art, 2, 2)[0] == 255 && Pixel(*art, 160, 160)[0] == 255 && Pixel(*art, 317, 317)[0] == 255);
  CHECK(Pixel(*art, 160, 330)[0] < 170 && Pixel(*art, 160, 478)[0] < Pixel(*art, 160, 330)[0]);
  CHECK(Pixel(*art, 160, 400)[1] == 0 && Pixel(*art, 160, 400)[3] == 255);
  // A small one, or a see-through one, sits in the middle of the square over the darker backdrop.
  for (const auto& centred : {solid(kSmallProgramIconSide - 1, 255), solid(kSmallProgramIconSide, 100)}) {
    art = ProgramIconArt(centred, 300, 300);
    CHECK(art && art->height == 300);
    CHECK(Pixel(*art, 4, 4)[0] < 200 && Pixel(*art, 150, 150)[0] > Pixel(*art, 4, 4)[0]);
  }
  CHECK(Pixel(*ProgramIconArt(solid(kSmallProgramIconSide - 1, 255), 300, 300), 150, 150)[0] == 255);
  // Opaque means at least 99% of the pixels at alpha 224 or more.
  CHECK(ProgramIconIsOpaque(solid(10, 224)) && !ProgramIconIsOpaque(solid(10, 223)));
  ArtworkImage gaps = solid(10, 255);
  gaps.rgba[3] = 0;  // 1 in 100
  CHECK(ProgramIconIsOpaque(gaps));
  gaps.rgba[7] = 0;  // 2 in 100
  CHECK(!ProgramIconIsOpaque(gaps));
  // 128 pixels fill the square; 127 do not. A wide icon fits its width; a wide image keeps the square centred.
  CHECK(Pixel(*ProgramIconArt(solid(128, 255), 300, 300), 2, 2)[0] == 255);
  CHECK(Pixel(*ProgramIconArt(solid(127, 255), 300, 300), 2, 2)[0] < 255);
  ArtworkImage wide{256, 128, {}};
  for (std::uint32_t i = 0; i < 256u * 128; ++i) wide.rgba.insert(wide.rgba.end(), {255, 0, 0, 255});
  art = ProgramIconArt(wide, 320, 480);
  CHECK(art && Pixel(*art, 2, 160)[0] == 255 && Pixel(*art, 160, 10)[0] < 255);
  art = ProgramIconArt(solid(128, 255), 480, 320);
  CHECK(art && Pixel(*art, 240, 160)[0] == 255 && Pixel(*art, 4, 160)[0] < 255);
  for (const auto& [w, h] : {std::pair<std::uint32_t, std::uint32_t>{1, 1}, {1, 480}, {480, 1}})
    CHECK(ProgramIconArt(solid(128, 255), w, h) && ProgramIconArt(solid(16, 100), w, h));
  // A thin opaque icon still gets rounded corners within its own size.
  for (const auto& [w, h] : {std::pair<std::uint32_t, std::uint32_t>{1, 127}, {127, 1}}) {
    ArtworkImage thin{w, h, {}};
    for (std::uint32_t i = 0; i < w * h; ++i) thin.rgba.insert(thin.rgba.end(), {255, 0, 0, 255});
    CHECK(ProgramIconArt(thin, 320, 480) && ProgramIconArt(thin, 480, 320) && MakeProgramTitleIcon(thin));
  }
  ArtworkImage clear{1, 1, {255, 255, 255, 0}};
  art = ProgramIconArt(clear, 30, 30);  // nothing but the dark ground
  CHECK(art && Pixel(*art, 15, 15)[0] < 0x24);
  CHECK(!ProgramIconArt(ArtworkImage{}, 30, 30) && !ProgramIconArt(large, 0, 30) && !ProgramIconArt(large, 30, kMaxArtworkDimension + 1));

  // The PS5 title icon: the art at 512, or a small icon scaled by whole pixels over a darker backdrop.
  const auto decode = [](const std::optional<std::string>& png, int x, int y) {
    std::vector<std::uint8_t> rgb;
    int width = 0, height = 0, components = 0;
    if (!png) return rgb;
    auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png->data()), static_cast<int>(png->size()),
                                         &width, &height, &components, 3);
    if (pixels && width == 512 && height == 512) rgb.assign(pixels + (y * 512 + x) * 3, pixels + (y * 512 + x) * 3 + 3);
    stbi_image_free(pixels);
    return rgb;
  };
  auto at = decode(MakeProgramTitleIcon(large), 4, 4);
  CHECK(at.size() == 3 && at[0] == 255 && at[1] == 0);
  // A larger see-through icon is the same art as on the library's square, unchanged.
  const ArtworkImage see_through = solid(kSmallProgramIconSide, 100);
  CHECK(MakeProgramTitleIcon(see_through) == MakeTitleIcon(*ProgramIconArt(see_through, kTitleIconSize, kTitleIconSize)));
  CHECK(MakeProgramTitleIcon(large) == MakeTitleIcon(*ProgramIconArt(large, kTitleIconSize, kTitleIconSize)));
  // The backdrop of a small icon is at 45%, darker than the library's.
  at = decode(MakeProgramTitleIcon(solid(16, 255)), 4, 4);
  CHECK(at.size() == 3 && at[0] == 115 && at[1] == 0);
  // The largest whole-number scale within 288 pixels, centred: the row through the middle crosses its edges.
  const auto spans = [&](std::uint32_t side, int first, int last) {
    const auto icon = MakeProgramTitleIcon(solid(side, 255));
    const auto red = [&](int x) { const auto rgb = decode(icon, x, 256); return rgb.size() == 3 && rgb[0] == 255; };
    return red(first) && red(last) && !red(first - 1) && !red(last + 1);
  };
  CHECK(spans(16, 112, 399));   // 18x
  CHECK(spans(32, 112, 399));   // 9x
  CHECK(spans(48, 112, 399));   // 6x
  CHECK(spans(64, 128, 383));   // 4x
  CHECK(spans(97, 159, 352));   // 2x
  CHECK(spans(127, 129, 382));  // 2x
  // A wide icon scales by its longer side, keeping its shape.
  ArtworkImage banner{32, 16, {}};
  for (std::uint32_t i = 0; i < 32u * 16; ++i) banner.rgba.insert(banner.rgba.end(), {255, 0, 0, 255});
  at = decode(MakeProgramTitleIcon(banner), 256, 256 - 72);
  CHECK(at.size() == 3 && at[0] == 255);
  at = decode(MakeProgramTitleIcon(banner), 256, 256 - 73);
  CHECK(at.size() == 3 && at[0] < 255);
  // Each icon pixel stays sharp: a hard edge between two colours, with no blend across it.
  ArtworkImage halves{32, 32, {}};
  for (std::uint32_t y = 0; y < 32; ++y)
    for (std::uint32_t x = 0; x < 32; ++x)
      halves.rgba.insert(halves.rgba.end(), {static_cast<std::uint8_t>(x < 16 ? 255 : 0), 0, static_cast<std::uint8_t>(x < 16 ? 0 : 255), 255});
  const auto sharp = MakeProgramTitleIcon(halves);
  at = decode(sharp, 255, 256);
  CHECK(at.size() == 3 && at[0] == 255 && at[2] == 0);
  at = decode(sharp, 256, 256);
  CHECK(at.size() == 3 && at[0] == 0 && at[2] == 255);
  // An opaque small icon has rounded corners.
  at = decode(MakeProgramTitleIcon(solid(32, 255)), 112, 112);
  CHECK(at.size() == 3 && at[0] < 255);
  at = decode(MakeProgramTitleIcon(solid(32, 255)), 136, 136);
  CHECK(at.size() == 3 && at[0] == 255);
  // A see-through one keeps its own shape: its right half is solid up to the corner, its left half clear.
  ArtworkImage half_clear{32, 32, {}};
  for (std::uint32_t y = 0; y < 32; ++y)
    for (std::uint32_t x = 0; x < 32; ++x)
      half_clear.rgba.insert(half_clear.rgba.end(), {255, 0, 0, static_cast<std::uint8_t>(x < 16 ? 0 : 255)});
  const auto cutout = MakeProgramTitleIcon(half_clear);
  at = decode(cutout, 399, 112);
  CHECK(at.size() == 3 && at[0] == 255);
  at = decode(cutout, 256, 256);
  CHECK(at.size() == 3 && at[0] == 255);
  at = decode(cutout, 255, 256);
  CHECK(at.size() == 3 && at[0] < 255);
  // Thin and single-pixel icons scale too, centred.
  CHECK(spans(1, 112, 399));  // 288x
  const ArtworkImage tall{1, 127, std::vector<std::uint8_t>(solid(1, 255).rgba.size() * 127, 255)};
  const ArtworkImage flat{127, 1, tall.rgba};
  const auto red_at = [&](const ArtworkImage& icon, int x, int y) {
    const auto rgb = decode(MakeProgramTitleIcon(icon), x, y);
    return rgb.size() == 3 && rgb[0] == 255;
  };
  CHECK(red_at(tall, 255, 256) && red_at(tall, 256, 256) && !red_at(tall, 254, 256) && !red_at(tall, 257, 256));
  CHECK(red_at(flat, 256, 255) && red_at(flat, 256, 256) && !red_at(flat, 256, 254) && !red_at(flat, 256, 257));
  // A clear single pixel leaves only the backdrop of the dark ground.
  at = decode(MakeProgramTitleIcon(ArtworkImage{1, 1, {255, 255, 255, 0}}), 256, 256);
  CHECK(at.size() == 3 && at[0] < 0x24);
  CHECK(!MakeProgramTitleIcon(ArtworkImage{}));

  fs::remove_all(root);
  std::puts("program icon tests passed");
}
