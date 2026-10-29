// content_language.h - The languages Steam ships game content in.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playden::steam {

// A language Steam tags depots with, and the name a player reads.
struct ContentLanguage {
    std::string_view code;  // as app info writes it: "german", "koreana", "schinese"
    std::string_view name;  // "German", "Korean", "Simplified Chinese"
};

// What Playden has always installed, and what a game that carries no depot in the chosen
// language falls back to.
inline constexpr std::string_view kDefaultContentLanguage = "english";

// Every language Steam uses, in the order the picker shows them: the default first, then the
// rest by name. The same set the Steamworks emulator is told about.
const std::vector<ContentLanguage>& ContentLanguages();

// True when `code` is one Steam uses. Comparison ignores case.
bool IsContentLanguage(std::string_view code);

// The player-facing name of `code` ("german" -> "German"), or `code` itself when Steam has no
// such language, so a depot tagged with something unexpected still reads sensibly.
std::string LanguageName(std::string_view code);

// The language an install uses from the ones a game carries (lower case, as Steam names them):
// the requested one when carried, else English, else the first one. With none carried, the
// requested one.
std::string ResolveContentLanguage(const std::vector<std::string>& carried, std::string_view requested);

} // namespace playden::steam
