// i18n.hpp - Translation catalogs for the user-facing strings of the homebrew UI.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playden::homebrew::i18n {

// A language the catalog directory offers. `code` is the file stem ("es"), `name` the
// endonym shown in Settings ("Español").
struct Language {
    std::string code;
    std::string name;
};

inline constexpr std::string_view kSourceLanguage = "en";

// Reads every <code>.json in `directory`. Safe to call again; catalogs already read are kept,
// so references handed out by translate() stay valid for the life of the process. Returns the
// number of catalogs available afterwards.
std::size_t load(const std::string& directory);

// Languages available in load() order, source language first. Always holds at least English.
std::vector<Language> available();

// Makes `code` the language translate() answers with. Unknown codes leave the selection
// untouched and return false.
bool select(std::string_view code);

// The selected language code.
std::string selected();

// `source` translated into the selected language, or `source` itself when the catalog has no
// entry for it. The reference stays valid for the life of the process.
const std::string& translate(std::string_view source);

} // namespace playden::homebrew::i18n

namespace playden::homebrew {

// Marks a user-facing literal and resolves it against the selected catalog.
inline const std::string& tr(std::string_view source) { return i18n::translate(source); }

} // namespace playden::homebrew
