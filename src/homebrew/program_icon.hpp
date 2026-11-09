#pragma once
// program_icon.hpp - A Windows program's own icon, read from its PE resources.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/artwork.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace playden::homebrew {

// Bounds for a program's icon: one image's bytes and its side in pixels.
inline constexpr std::size_t kMaxProgramIconBytes = 1024 * 1024;
inline constexpr std::uint32_t kMaxProgramIconSide = 512;

// The largest image of the program's first icon group (RT_GROUP_ICON), as Explorer shows it, decoded to
// RGBA: PNG or a BMP (1, 4, 8, 24 or 32 bits, with its transparency mask). Nothing for a file that isn't
// a Windows program, has no icon, or is malformed; reads are bounded and never follow a symbolic link.
std::optional<ArtworkImage> ReadProgramIcon(const std::string& path);

// The largest image of an .ico file (a game's own icon file, homebrew/gog_icon.hpp), decoded as
// ReadProgramIcon decodes a program's, with the same bounds.
std::optional<ArtworkImage> ReadIconFile(const std::string& path);

// The program-icon source of the UI's artwork (homebrew/artwork.hpp): the program's console path (or the
// game's .ico file's) behind a prefix no store URL has, and for a .ico the program to fall back to after a line
// break (which no path here holds). ProgramIconPath gives the first path back, ProgramIconFallbackPath the
// program, or nothing for any other source.
inline constexpr const char* kProgramIconScheme = "program-icon:";
inline std::string ProgramIconSource(const std::string& console_path, const std::string& fallback_program = "") {
  if (console_path.empty()) return {};
  return kProgramIconScheme + console_path + (fallback_program.empty() ? std::string() : "\n" + fallback_program);
}
std::optional<std::string> ProgramIconPath(const std::string& source);
std::optional<std::string> ProgramIconFallbackPath(const std::string& source);

// Loads program-icon sources from the game's files (a console path under `host_prefix` on the desktop
// build; a .ico path with ReadIconFile, else ReadProgramIcon), as a 2:3 poster made from the icon
// (homebrew/title_icon.hpp ProgramIconArt), and passes every other source to `next`. Both outlive it.
class ProgramIconArtworkBackend final : public ArtworkBackend {
 public:
  ProgramIconArtworkBackend(ArtworkBackend& next, std::string host_prefix = "");
  std::optional<ArtworkImage> Load(const ArtworkKey&, const std::atomic<bool>& cancel) override;

 private:
  ArtworkBackend& next_;
  std::string host_prefix_;
};

}  // namespace playden::homebrew
