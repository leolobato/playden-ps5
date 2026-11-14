// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Playden's content languages (Steam's names, steam/backend/content_language.h) as GOG writes them in build metas: a
// GOG code ("de-DE"), deprecated codes some games still use ("de"; Jazz Jackrabbit 2's depots say "en") and the English
// name (gen 1 repositories say "English").

#include <string>
#include <string_view>
#include <vector>

namespace playden::gog {

// The GOG codes of a Steam language, the current code first, then the deprecated ones and the English name. Empty for
// a language GOG has no code for here. Case is ignored.
const std::vector<std::string_view>& LanguageCodes(std::string_view steam_code);

// True when a depot's languages carry `steam_code`: "*" (every language) matches, otherwise any entry equal to one of
// its codes, ignoring case.
bool LanguageMatches(const std::vector<std::string>& depot_languages, std::string_view steam_code);

// True when one of the depot's languages equals one of `codes`, ignoring case. "*" is not special here.
bool CarriesAny(const std::vector<std::string>& depot_languages, const std::vector<std::string_view>& codes);

// The current GOG code ("german" -> "de-DE"), for the install script and the record. Empty when unknown.
std::string GogCode(std::string_view steam_code);

// The Steam language whose codes include `gog_code` ("en" -> "english"), or empty.
std::string SteamCodeFor(std::string_view gog_code);

}  // namespace playden::gog
