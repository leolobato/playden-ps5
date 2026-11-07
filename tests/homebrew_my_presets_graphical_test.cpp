// homebrew_my_presets_graphical_test.cpp - My presets on screen: Choose settings with the player's presets and a
// comparison, a preset's Triangle menu, the Rename keyboard, the Delete question, Game settings with Save as my preset…
// and Update “<name>”, the Save keyboard and its dialog, and the dialog when all 50 presets are made.
// Each state is drawn and captured as a PNG beside the checks of what it shows.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/render.hpp"
#include "homebrew/simulation.hpp"
#include "gfx/gl_program.hpp"
#include "runtime/game_settings.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const playden::homebrew::ViewRow* find_row(const playden::homebrew::Simulation& simulation, const std::string& key) {
    for (const auto& row : simulation.view().rows)
        if (row.key == key) return &row;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace playden::homebrew;
    check(argc == 3, "usage: homebrew_my_presets_graphical_test <fixtures> <output>");
    const std::filesystem::path fixtures(argv[1]);
    const std::filesystem::path output(argv[2]);
    std::filesystem::create_directories(output);
    check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL starts");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    auto* window = SDL_CreateWindow("My presets", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    check(window != nullptr, "window opens");
    auto context = SDL_GL_CreateContext(window);
    check(context != nullptr, "GL context");
    hui::gfx::set_glsl_prefix("#version 410 core\n");
    {
        const auto profile = std::filesystem::temp_directory_path() /
                             ("playden-my-presets-graphical-" + std::to_string(getpid()) + ".json");
        std::error_code ignored;
        std::filesystem::remove(profile, ignored);
        Simulation model(fixtures, profile);
        std::vector<playden::runtime::SettingsPreset> presets;
        std::string reason;
        check(playden::runtime::ParseSettingsPresets(read_text(fixtures.parent_path().parent_path() / "runtime/presets.json"),
                                                     &presets, &reason),
              "presets load");
        model.SetSettingsData(std::move(presets), {});
        Render renderer(PLAYDEN_HOMEBREW_ASSETS_DIR);
        const auto capture = [&](const std::string& name, const std::string& surface) {
            for (int frame = 0; frame < 16; ++frame) renderer.draw(model, 1920, 1080);
            check(renderer.capture_png(output / (name + ".png"), 1920, 1080), "capture written");
            if (model.view().surface != surface) {
                std::fprintf(stderr, "%s: surface %s focus %s\n", name.c_str(), model.view().surface.c_str(),
                             model.state().navigation.focus_key.c_str());
                check(false, "the capture shows its surface");
            }
        };
        const std::string mine1 = "mypreset-my-0000000000000001", mine3 = "mypreset-my-0000000000000003";
        check(model.scenario("my-presets") && model.action("focus:" + mine3), "focus a preset of mine");
        capture("picker-mine", "preset-picker");
        check(model.view().comparison && model.view().comparison->title == "Couch at 60 Hz", "the comparison is the focused preset's");
        check(model.action("focus:" + mine1) && model.action("triangle"), "Triangle on the game's preset");
        capture("picker-menu", "mypreset-menu");
        check(find_row(model, "my-preset-update") && find_row(model, "my-preset-update")->enabled, "the menu offers Update");
        check(model.action("focus:my-preset-rename") && model.action("cross"), "Rename…");
        capture("rename-keyboard", "keyboard");
        check(model.state().editor.value == "Point and click", "the keyboard has the name");
        check(model.action("circle") && model.action("focus:my-preset-delete") && model.action("cross"), "Delete…");
        capture("delete-confirm", "confirmation");
        check(model.state().navigation.modals.back().title == "Delete “Point and click”?", "Delete asks first");
        check(model.scenario("my-presets") && model.action("circle") && model.action("focus:update-my-preset"), "Game settings");
        capture("settings-update-row", "game-settings");
        check(find_row(model, "update-my-preset")->title == "Update “Point and click”", "Update names the preset");
        check(model.scenario("my-presets-save"), "my-presets-save scenario loads");
        capture("settings-save-row", "game-settings");
        check(model.state().navigation.focus_key == "save-my-preset", "Save as my preset… is focused");
        check(model.action("cross"), "Save as my preset…");
        capture("save-keyboard", "keyboard");
        model.SubmitEditorText("Overlay and 720p");
        capture("saved-notice", "confirmation");
        check(model.state().navigation.modals.back().title == "Saved “Overlay and 720p”", "the save says so");
        check(model.scenario("my-presets-full") && model.action("cross"), "Save with 50 presets");
        capture("full-error", "confirmation");
        check(model.state().navigation.modals.back().body == "You have 50 presets. Delete one to save another.", "50 is the most");
        std::filesystem::remove(profile, ignored);
    }
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts("homebrew_my_presets_graphical_test passed");
    return 0;
}
