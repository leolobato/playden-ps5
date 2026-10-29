// homebrew_i18n_test.cpp - Translation catalogs: loading, fallback and language changes.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "homebrew/i18n.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

std::string directory;

void write_file(const std::string& name, const std::string& body) {
    const auto path = directory + "/" + name;
    std::FILE* file = std::fopen(path.c_str(), "wb");
    check(file != nullptr, "the fixture file opens");
    if (!file) return;
    std::fwrite(body.data(), 1, body.size(), file);
    std::fclose(file);
}

void write_fixtures() {
    char pattern[] = "/tmp/playden-i18n-XXXXXX";
    const char* created = mkdtemp(pattern);
    check(created != nullptr, "creates a temporary catalog directory");
    directory = created ? created : "";
    write_file("index.json",
               R"({"languages":[{"code":"en","name":"English"},{"code":"es","name":"Espa\u00f1ol"},)"
               R"({"code":"zz","name":"Missing"}]})");
    write_file("es.json",
               R"({"code":"es","name":"Espa\u00f1ol","strings":{)"
               R"("Settings":"Ajustes",)"
               R"("Sign out":"Cerrar sesi\u00f3n",)"
               R"("Cross Select":"Cross Seleccionar",)"
               R"("Reduced motion":""}})");
}

void remove_fixtures() {
    if (directory.empty()) return;
    std::remove((directory + "/index.json").c_str());
    std::remove((directory + "/es.json").c_str());
    rmdir(directory.c_str());
}

void the_source_language_is_always_offered() {
    const auto languages = playden::homebrew::i18n::available();
    check(!languages.empty() && languages.front().code == "en",
          "English leads the list even before a catalog is read");
    check(playden::homebrew::i18n::selected() == "en", "English is selected to begin with");
}

void catalogs_load_and_name_themselves() {
    const auto count = playden::homebrew::i18n::load(directory);
    check(count == 2, "the catalog that did not ship is skipped, English and Spanish remain");
    const auto languages = playden::homebrew::i18n::available();
    check(languages.size() == 2 && languages[1].code == "es" && languages[1].name == "Español",
          "a catalog is named by its own endonym");
}

void a_selected_catalog_answers_and_falls_back() {
    check(playden::homebrew::i18n::select("es"), "Spanish can be selected");
    check(playden::homebrew::i18n::selected() == "es", "the selection is remembered");
    check(playden::homebrew::tr("Settings") == "Ajustes", "a known string is translated");
    check(playden::homebrew::tr("Córeldrome II") == "Córeldrome II",
          "a game name the catalog never heard of comes back as it is");
    check(playden::homebrew::tr("Reduced motion") == "Reduced motion",
          "an empty translation leaves the string in English");
}

void a_translation_is_never_translated_again() {
    check(playden::homebrew::tr("Cerrar sesión") == "Cerrar sesión",
          "text already in the player's language passes through untouched");
    const auto& once = playden::homebrew::tr("Sign out");
    const auto& twice = playden::homebrew::tr(once);
    check(once == twice && once == "Cerrar sesión", "fitting then drawing translates only once");
}

void button_prefixes_survive_translation() {
    check(playden::homebrew::tr("Cross Select") == "Cross Seleccionar",
          "the button a legend names stays in English so its glyph is still found");
}

void an_unknown_language_leaves_the_selection_alone() {
    check(!playden::homebrew::i18n::select("fr"), "a catalog that never shipped is refused");
    check(playden::homebrew::i18n::selected() == "es", "the refusal leaves the selection as it was");
}

void references_outlive_a_language_change() {
    const std::string& spanish = playden::homebrew::tr("Settings");
    check(playden::homebrew::i18n::select("en"), "English can be selected again");
    check(spanish == "Ajustes", "text handed out earlier is still readable after the change");
    check(playden::homebrew::tr("Settings") == "Settings", "English answers with the source string");
}

} // namespace

int main() {
    the_source_language_is_always_offered();
    write_fixtures();
    catalogs_load_and_name_themselves();
    a_selected_catalog_answers_and_falls_back();
    a_translation_is_never_translated_again();
    button_prefixes_survive_translation();
    an_unknown_language_leaves_the_selection_alone();
    references_outlive_a_language_change();
    remove_fixtures();

    if (failures != 0) {
        std::fprintf(stderr, "%d translation test assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("homebrew_i18n_test: all assertions passed");
    return EXIT_SUCCESS;
}
