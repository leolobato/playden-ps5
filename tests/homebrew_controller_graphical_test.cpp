// homebrew_controller_graphical_test.cpp - Game settings' controller screens on screen: the layout editor with Templates
// first, and the Templates sheet with the focused template's preview beside it.
// Each state is drawn and captured as a PNG beside the checks of what it shows.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/render.hpp"
#include "homebrew/simulation.hpp"
#include "gfx/gl_program.hpp"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace playden::homebrew;
    check(argc == 3, "usage: homebrew_controller_graphical_test <fixtures> <output>");
    const std::filesystem::path fixtures(argv[1]);
    const std::filesystem::path output(argv[2]);
    std::filesystem::create_directories(output);
    check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL starts");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    auto* window = SDL_CreateWindow("Controller", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    check(window != nullptr, "window opens");
    auto context = SDL_GL_CreateContext(window);
    check(context != nullptr, "GL context");
    hui::gfx::set_glsl_prefix("#version 410 core\n");
    {
        const auto profile = std::filesystem::temp_directory_path() /
                             ("playden-controller-graphical-" + std::to_string(getpid()) + ".json");
        std::error_code ignored;
        std::filesystem::remove(profile, ignored);
        Simulation model(fixtures, profile);
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

        // The editor: Templates is the first row, and the editor opens on Cross with its panel.
        check(model.scenario("controller-layout-mouse"), "the layout scenario opens");
        check(model.state().navigation.focus_key == "layout-input-cross", "the editor opens on Cross");
        check(model.view().input_panel.has_value(), "Cross has its panel");
        capture("editor", "controller-layout");

        // Templates: the focused template's preview beside the sheet, every input listed.
        check(model.action("triangle") && model.action("focus:layout-template-wasd-mouse"), "Templates, WASD and mouse");
        check(model.view().comparison && model.view().comparison->compact &&
                  model.view().comparison->rows.size() == 21,
              "the preview lists the controller and all 20 inputs");
        capture("templates-preview", "layout-templates");
        std::filesystem::remove(profile, ignored);
    }
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts("homebrew_controller_graphical_test passed");
    return 0;
}
