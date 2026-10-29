// homebrew_translations_test.cpp - The catalogs Playden ships: shape, button prefixes and glyphs.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "homebrew/i18n.hpp"
#include "homebrew/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::string read_file(const std::string& path) {
    std::FILE* input = std::fopen(path.c_str(), "rb");
    if (!input) return std::string();
    std::string result;
    char buffer[4096];
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), input)) result.append(buffer, count);
    std::fclose(input);
    return result;
}

// The glyphs the font baker puts in the atlas for the "european" set: printable ASCII, the UI
// symbols, Latin-1 Supplement and Latin Extended-A, Greek, Cyrillic and a few marks. A catalog
// that reaches outside this set would draw blanks on the console.
bool baked_glyph(unsigned codepoint) {
    if (codepoint >= 32 && codepoint < 127) return true;
    if (codepoint >= 0x00A1 && codepoint <= 0x017F) return true;
    if (codepoint >= 0x0386 && codepoint <= 0x03CE) return true;
    if (codepoint >= 0x0410 && codepoint <= 0x044F) return true;
    static const std::set<unsigned> marks = {
        0x00B7, 0x00D7, 0x00A9, 0x00B0, 0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192,
        0x2193, 0x2713, 0x2588, 0x25CF, 0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x0401, 0x0451, 0x2018,
        0x2019, 0x201C, 0x201D, 0x201E, 0x20AC, 0x2122};
    return marks.count(codepoint) != 0;
}

// Decodes UTF-8 and reports the first codepoint the atlas has no glyph for.
bool drawable(std::string_view text, unsigned* offender) {
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        unsigned codepoint = 0;
        std::size_t length = 0;
        if (lead < 0x80) { codepoint = lead; length = 1; }
        else if ((lead & 0xE0) == 0xC0) { codepoint = lead & 0x1FU; length = 2; }
        else if ((lead & 0xF0) == 0xE0) { codepoint = lead & 0x0FU; length = 3; }
        else if ((lead & 0xF8) == 0xF0) { codepoint = lead & 0x07U; length = 4; }
        else { *offender = lead; return false; }
        if (index + length > text.size()) { *offender = lead; return false; }
        for (std::size_t step = 1; step < length; ++step) {
            const auto next = static_cast<unsigned char>(text[index + step]);
            if ((next & 0xC0) != 0x80) { *offender = lead; return false; }
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if (!baked_glyph(codepoint)) { *offender = codepoint; return false; }
        index += length;
    }
    return true;
}

// A legend names the button that runs it. The prefix picks the glyph and is dropped before the
// words are drawn, so a translation has to keep it.
const char* legend_prefix(std::string_view text) {
    static const char* prefixes[] = {"Cross ", "Circle ", "Square ", "Triangle ",
                                     "Options ", "Touchpad ", "L1/R1 "};
    for (const char* prefix : prefixes)
        if (text.rfind(prefix, 0) == 0) return prefix;
    return nullptr;
}

void catalogs_are_listed_with_the_source_language_first(const std::string& directory) {
    const auto manifest = playden::homebrew::json::parse(read_file(directory + "/index.json"));
    check(manifest.has_value(), "index.json parses");
    if (!manifest) return;
    const auto* languages = manifest->get("languages");
    const auto* listed = languages ? languages->array() : nullptr;
    check(listed != nullptr && !listed->empty(), "index.json lists languages");
    if (!listed || listed->empty()) return;
    const auto* first = listed->front().get("code");
    const auto* code = first ? first->string() : nullptr;
    check(code != nullptr && *code == "en", "the source language leads the list");
    for (const auto& entry : *listed) {
        const auto* name = entry.get("name");
        const auto* name_text = name ? name->string() : nullptr;
        check(name_text != nullptr && !name_text->empty(), "every language is named for the Settings row");
    }
}

void every_listed_catalog_loads(const std::string& directory) {
    const auto manifest = playden::homebrew::json::parse(read_file(directory + "/index.json"));
    if (!manifest) return;
    const auto* languages = manifest->get("languages");
    const auto* listed = languages ? languages->array() : nullptr;
    if (!listed) return;
    const auto count = playden::homebrew::i18n::load(directory);
    check(count == listed->size(), "every language in index.json has a catalog that parses");
    for (const auto& entry : *listed) {
        const auto* code = entry.get("code");
        const auto* text = code ? code->string() : nullptr;
        if (!text) continue;
        check(playden::homebrew::i18n::select(*text), "the catalog for " + *text + " can be selected");
    }
    playden::homebrew::i18n::select(playden::homebrew::i18n::kSourceLanguage);
}

void a_catalog_is_sound(const std::string& directory, const std::string& code) {
    const auto parsed = playden::homebrew::json::parse(read_file(directory + "/" + code + ".json"));
    check(parsed.has_value(), code + ".json parses");
    if (!parsed) return;
    const auto* strings = parsed->get("strings");
    const auto* entries = strings ? strings->object() : nullptr;
    check(entries != nullptr, code + ".json has a strings object");
    if (!entries) return;
    check(!entries->empty(), code + ".json is not empty");

    std::set<std::string> values;
    for (const auto& [key, value] : *entries) {
        const auto* text = value.string();
        check(text != nullptr, "every entry of " + code + " is a string: " + key);
        if (!text) continue;
        check(!key.empty(), code + " has no empty key");
        // An empty value deliberately leaves a string in English; it still has to be a real key.
        if (text->empty()) continue;

        unsigned offender = 0;
        if (!drawable(*text, &offender)) {
            char note[64];
            std::snprintf(note, sizeof(note), " uses U+%04X, which the font atlas lacks: ", offender);
            check(false, code + note + key);
        }
        if (const char* prefix = legend_prefix(key))
            check(text->rfind(prefix, 0) == 0,
                  code + " keeps the button prefix of the legend: " + key);
        check(legend_prefix(*text) == nullptr || legend_prefix(key) != nullptr,
              code + " does not invent a button prefix: " + key);
        values.insert(*text);
    }

    // A translation that reads like a source string would be looked up again. The catalog is
    // built so that cannot happen, and i18n guards it too; this keeps the data honest.
    for (const auto& value : values)
        check(entries->find(value) == entries->end(),
              code + " has a translation that is also one of its own source strings: " + value);
}

// The template translators start from has to keep up with the catalogs, or a new language would
// be written against strings Playden no longer shows. It lives outside the packaged asset tree:
// only the files index.json lists are shipped.
void the_template_covers_every_catalog(const std::string& directory, const std::string& template_path) {
    const auto parsed = playden::homebrew::json::parse(read_file(template_path));
    check(parsed.has_value(), "the translation template parses");
    if (!parsed) return;
    const auto* strings = parsed->get("strings");
    const auto* entries = strings ? strings->object() : nullptr;
    check(entries != nullptr && !entries->empty(), "the template offers the strings to translate");
    if (!entries) return;
    const auto* code = parsed->get("code");
    const auto* code_text = code ? code->string() : nullptr;
    check(code_text != nullptr && code_text->empty(), "the template claims no language of its own");
    for (const auto& [key, value] : *entries) {
        const auto* text = value.string();
        check(text != nullptr && text->empty(), "the template leaves every string to translate: " + key);
    }

    const auto spanish = playden::homebrew::json::parse(read_file(directory + "/es.json"));
    const auto* translated = spanish ? spanish->get("strings") : nullptr;
    const auto* catalog = translated ? translated->object() : nullptr;
    if (!catalog) return;
    for (const auto& [key, value] : *catalog)
        check(entries->find(key) != entries->end(), "the template still lists the string: " + key);
}

} // namespace

int main(int argc, char** argv) {
    const std::string directory = argc > 1 ? argv[1] : "assets/translations";
    const std::string template_path = argc > 2 ? argv[2] : "docs/translation-template.json";
    catalogs_are_listed_with_the_source_language_first(directory);
    every_listed_catalog_loads(directory);
    a_catalog_is_sound(directory, "es");
    a_catalog_is_sound(directory, "tr");
    the_template_covers_every_catalog(directory, template_path);

    if (failures != 0) {
        std::fprintf(stderr, "%d catalog assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("homebrew_translations_test: all assertions passed");
    return EXIT_SUCCESS;
}
