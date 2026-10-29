// i18n.cpp - Translation catalogs for the user-facing strings of the homebrew UI.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/i18n.hpp"

#include "homebrew/json.hpp"

#include <atomic>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace playden::homebrew::i18n {
namespace {

struct Catalog {
    std::string code;
    std::string name;
    std::map<std::string, std::string, std::less<>> entries;
    // The translations themselves. Text already in this set is returned untouched, so a string
    // may pass through translate() more than once (fitted, wrapped, then drawn) without a
    // translation that happens to read like a source string being translated again.
    std::map<std::string_view, const std::string*, std::less<>> translated;
};

// Catalogs are never dropped once published: translate() hands out references into them and the
// UI keeps those across frames, so a language change may only move the pointer below.
std::deque<Catalog>& catalogs() {
    static std::deque<Catalog> storage;
    return storage;
}

std::mutex& catalog_lock() {
    static std::mutex lock;
    return lock;
}

std::atomic<const Catalog*>& active() {
    static std::atomic<const Catalog*> selected{nullptr};
    return selected;
}

// Strings a catalog has no entry for. Kept forever so translate() can answer with a reference
// even when it is only echoing its argument back.
std::shared_mutex& fallback_lock() {
    static std::shared_mutex lock;
    return lock;
}

std::map<std::string, std::string, std::less<>>& fallbacks() {
    static std::map<std::string, std::string, std::less<>> entries;
    return entries;
}

const std::string& remember_fallback(std::string_view source) {
    {
        const std::shared_lock<std::shared_mutex> shared(fallback_lock());
        const auto found = fallbacks().find(source);
        if (found != fallbacks().end()) return found->second;
    }
    const std::unique_lock<std::shared_mutex> unique(fallback_lock());
    return fallbacks().emplace(std::string(source), std::string(source)).first->second;
}

std::string read_file(const std::string& path) {
    // The console libc has no locale support for iostreams, so every read here goes through stdio.
    std::FILE* input = std::fopen(path.c_str(), "rb");
    if (!input) return std::string();
    std::string result;
    char buffer[4096];
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), input)) result.append(buffer, count);
    std::fclose(input);
    return result;
}

const Catalog* find(std::string_view code) {
    for (const auto& catalog : catalogs())
        if (catalog.code == code) return &catalog;
    return nullptr;
}

// English is the language the literals in the source are written in, so its catalog may stay
// empty: translate() echoes whatever it is given.
const Catalog& source_catalog() {
    if (const auto* existing = find(kSourceLanguage)) return *existing;
    catalogs().push_back(Catalog{std::string(kSourceLanguage), "English", {}});
    return catalogs().back();
}

bool read_catalog(const std::string& directory, const std::string& code, const std::string& name) {
    if (find(code)) return true;
    if (code == kSourceLanguage) {
        source_catalog();
        return true;
    }

    const auto source = read_file(directory + "/" + code + ".json");
    if (source.empty()) return false;
    const auto parsed = json::parse(source);
    if (!parsed) return false;
    const auto* strings = parsed->get("strings");
    const auto* object = strings ? strings->object() : nullptr;
    if (!object) return false;

    Catalog catalog;
    catalog.code = code;
    const auto* label = parsed->get("name");
    const auto* label_text = label ? label->string() : nullptr;
    catalog.name = label_text && !label_text->empty() ? *label_text : name;
    for (const auto& [key, value] : *object) {
        const auto* text = value.string();
        // An empty value marks a string that is deliberately left in English.
        if (text && !text->empty()) catalog.entries.emplace(key, *text);
    }
    catalogs().push_back(std::move(catalog));
    // The reverse index points into the stored entries, so it is built after the move.
    auto& stored = catalogs().back();
    for (const auto& entry : stored.entries) stored.translated.emplace(entry.second, &entry.second);
    return true;
}

} // namespace

std::size_t load(const std::string& directory) {
    const std::lock_guard<std::mutex> guard(catalog_lock());
    source_catalog();

    const auto manifest = json::parse(read_file(directory + "/index.json"));
    if (manifest) {
        if (const auto* languages = manifest->get("languages"); languages && languages->array())
            for (const auto& entry : *languages->array()) {
                const auto* code = entry.get("code");
                const auto* code_text = code ? code->string() : nullptr;
                if (!code_text || code_text->empty()) continue;
                const auto* name = entry.get("name");
                const auto* name_text = name ? name->string() : nullptr;
                read_catalog(directory, *code_text, name_text ? *name_text : *code_text);
            }
    }

    if (!active().load(std::memory_order_acquire))
        active().store(&source_catalog(), std::memory_order_release);
    return catalogs().size();
}

std::vector<Language> available() {
    const std::lock_guard<std::mutex> guard(catalog_lock());
    source_catalog();
    std::vector<Language> languages;
    languages.reserve(catalogs().size());
    for (const auto& catalog : catalogs()) languages.push_back(Language{catalog.code, catalog.name});
    return languages;
}

bool select(std::string_view code) {
    const std::lock_guard<std::mutex> guard(catalog_lock());
    const auto* catalog = find(code);
    if (!catalog) return false;
    active().store(catalog, std::memory_order_release);
    return true;
}

std::string selected() {
    const auto* catalog = active().load(std::memory_order_acquire);
    return catalog ? catalog->code : std::string(kSourceLanguage);
}

const std::string& translate(std::string_view source) {
    const auto* catalog = active().load(std::memory_order_acquire);
    if (catalog) {
        const auto found = catalog->entries.find(source);
        if (found != catalog->entries.end()) return found->second;
        const auto done = catalog->translated.find(source);
        if (done != catalog->translated.end()) return *done->second;
    }
    return remember_fallback(source);
}

} // namespace playden::homebrew::i18n
