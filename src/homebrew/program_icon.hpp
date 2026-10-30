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

// The program-icon source of the UI's artwork (homebrew/artwork.hpp): the program's console path behind
// a prefix no store URL has. ProgramIconPath gives the path back, or nothing for any other source.
inline constexpr const char* kProgramIconScheme = "program-icon:";
inline std::string ProgramIconSource(const std::string& console_path) {
  return console_path.empty() ? std::string() : kProgramIconScheme + console_path;
}
std::optional<std::string> ProgramIconPath(const std::string& source);

// Loads program-icon sources from the game's files (a console path under `host_prefix` on the desktop
// build) and passes every other source to `next`. Both outlive it.
class ProgramIconArtworkBackend final : public ArtworkBackend {
 public:
  ProgramIconArtworkBackend(ArtworkBackend& next, std::string host_prefix = "");
  std::optional<ArtworkImage> Load(const ArtworkKey&, const std::atomic<bool>& cancel) override;

 private:
  ArtworkBackend& next_;
  std::string host_prefix_;
};

}  // namespace playden::homebrew
