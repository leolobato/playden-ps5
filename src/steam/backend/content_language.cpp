// content_language.cpp - The languages Steam ships game content in.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "steam/backend/content_language.h"

#include <algorithm>

namespace playden::steam {
namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string Lowered(std::string_view value) {
    std::string result(value);
    for (char& c : result) c = Lower(c);
    return result;
}

} // namespace

const std::vector<ContentLanguage>& ContentLanguages() {
    // Steam's own names, which are what a depot's `language` holds. A few do not read like the
    // language they stand for: "koreana" is Korean, "brazilian" is Brazilian Portuguese, and
    // "latam" is Latin American Spanish.
    static const std::vector<ContentLanguage> languages = {
        {"english", "English"},
        {"arabic", "Arabic"},
        {"bulgarian", "Bulgarian"},
        {"schinese", "Simplified Chinese"},
        {"tchinese", "Traditional Chinese"},
        {"czech", "Czech"},
        {"danish", "Danish"},
        {"dutch", "Dutch"},
        {"finnish", "Finnish"},
        {"french", "French"},
        {"german", "German"},
        {"greek", "Greek"},
        {"hungarian", "Hungarian"},
        {"italian", "Italian"},
        {"japanese", "Japanese"},
        {"koreana", "Korean"},
        {"norwegian", "Norwegian"},
        {"polish", "Polish"},
        {"portuguese", "Portuguese"},
        {"brazilian", "Portuguese (Brazil)"},
        {"romanian", "Romanian"},
        {"russian", "Russian"},
        {"spanish", "Spanish (Spain)"},
        {"latam", "Spanish (Latin America)"},
        {"swedish", "Swedish"},
        {"thai", "Thai"},
        {"turkish", "Turkish"},
        {"ukrainian", "Ukrainian"},
        {"vietnamese", "Vietnamese"},
    };
    return languages;
}

bool IsContentLanguage(std::string_view code) {
    const auto wanted = Lowered(code);
    const auto& languages = ContentLanguages();
    return std::any_of(languages.begin(), languages.end(),
                       [&](const ContentLanguage& language) { return language.code == wanted; });
}

std::string LanguageName(std::string_view code) {
    const auto wanted = Lowered(code);
    for (const auto& language : ContentLanguages())
        if (language.code == wanted) return std::string(language.name);
    return std::string(code);
}

std::string ResolveContentLanguage(const std::vector<std::string>& carried, std::string_view requested) {
    const auto wanted = Lowered(requested.empty() ? kDefaultContentLanguage : requested);
    if (carried.empty() || std::find(carried.begin(), carried.end(), wanted) != carried.end()) return wanted;
    const std::string english(kDefaultContentLanguage);
    if (std::find(carried.begin(), carried.end(), english) != carried.end()) return english;
    return carried.front();
}

} // namespace playden::steam
