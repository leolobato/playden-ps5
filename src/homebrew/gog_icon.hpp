#pragma once
// gog_icon.hpp - The icon a GOG copy of a game ships in its folder (goggame-<id>.info and .ico).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

namespace playden::homebrew {

// The file name, in the game's folder, of the .ico GOG names for the game: from its goggame-*.info, the play
// task whose path is `program` (relative to the folder, '\' or '/', in any case) when it names an icon, else
// the first "game" task that does (the base game's .info first); without one, the folder's only
// goggame-*.ico. The name is a plain file in `folder` (an absolute path) ending in .ico; empty when there is
// none. Reads are bounded (a few small .info files) and never follow a symbolic link.
std::string GogIconFile(const std::string& folder, const std::string& program);

}  // namespace playden::homebrew
