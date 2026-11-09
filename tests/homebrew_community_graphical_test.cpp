// homebrew_community_graphical_test.cpp - Community recipes on screen: the chooser (no recipe, one,
// three, one the kit refuses, an offline copy), the Install sheet's Settings row, the switch
// confirmation, Game settings with an updated recipe and a fallback, the "Recipe available" tag, and sharing
// and votes (the Share sheet's cases, the sent dialog with its QR code, the vote prompt).
// Each scenario is drawn and captured as a PNG beside the checks of what it shows.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/render.hpp"
#include "homebrew/simulation.hpp"
#include "gfx/gl_program.hpp"
#include "runtime/game_settings.h"

#include <SDL.h>

#include <algorithm>
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
    check(argc == 3, "usage: homebrew_community_graphical_test <fixtures> <output>");
    const std::filesystem::path fixtures(argv[1]);
    const std::filesystem::path output(argv[2]);
    std::filesystem::create_directories(output);
    check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL starts");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    auto* window = SDL_CreateWindow("Community recipes", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    check(window != nullptr, "window opens");
    auto context = SDL_GL_CreateContext(window);
    check(context != nullptr, "GL context");
    hui::gfx::set_glsl_prefix("#version 410 core\n");
    {
        const auto profile = std::filesystem::temp_directory_path() /
                             ("playden-community-graphical-" + std::to_string(getpid()) + ".json");
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
        const auto capture = [&](const std::string& scenario, const std::string& surface, const std::string& focus) {
            check(model.scenario(scenario), "scenario loads");
            for (int frame = 0; frame < 16; ++frame) renderer.draw(model, 1920, 1080);
            check(renderer.capture_png(output / (scenario + ".png"), 1920, 1080), "capture written");
            if (model.view().surface != surface || model.state().navigation.focus_key.find(focus) == std::string::npos) {
                std::fprintf(stderr, "%s: surface %s focus %s\n", scenario.c_str(), model.view().surface.c_str(),
                             model.state().navigation.focus_key.c_str());
                check(false, "scenario shows its surface and focus");
            }
        };
        const std::string one = "recipe-" + std::string(64, '1');
        capture("recipes-none", "preset-picker", "preset-playden-default");
        check(find_row(model, "recipes-none") && !find_row(model, one), "no recipe: the chooser says so");
        capture("recipes-one", "install", "install-confirm");
        check(find_row(model, "install-settings") && find_row(model, "install-settings")->subtitle == "★ 1 community recipe available",
              "one recipe: the Install sheet offers it");
        capture("recipes-three", "preset-picker", "preset-playden-default");
        check(find_row(model, one) && find_row(model, "recipe-" + std::string(64, '3')), "three recipes are listed");
        capture("recipes-refused", "preset-picker", "preset-playden-default");
        check(find_row(model, "recipe-" + std::string(64, '5')) && !find_row(model, "recipe-" + std::string(64, '5'))->enabled,
              "a recipe the kit refuses is greyed out");
        capture("recipes-offline", "preset-picker", "preset-playden-default");
        check(model.view().subtitle.find("Offline") != std::string::npos && find_row(model, one), "the offline copy is offered");
        capture("recipes-switch", "confirmation", "recipe-switch-confirm");
        capture("recipes-settings", "game-settings", "preset-picker");
        check(find_row(model, "recipe-update") != nullptr, "an updated recipe is offered in Game settings");
        capture("recipes-fallback", "game-settings", "preset-picker");
        check(find_row(model, "recipe-status") != nullptr, "the fallback reason is in Game settings");
        capture("recipes-details", "details", "details");
        check(find_row(model, "details-recipe") && find_row(model, "details-recipe")->title == "Recipe available",
              "an installed game with recipes carries the mark");
        // The community's compatibility list, apart from the player's own rating.
        check(find_row(model, "details-compat") && find_row(model, "details-compat")->title == "Works" &&
                  find_row(model, "details-compat")->value == "Confirmed by 14 players",
              "a game listed as Works says how many players confirmed it");
        check(model.action("details:900005") && find_row(model, "details-compat") &&
                  find_row(model, "details-compat")->title == "Refused" && find_row(model, "details-compat")->value.empty(),
              "a game listed as Refused says so, without a count");
        check(model.action("details:900004") && find_row(model, "details-compat") &&
                  find_row(model, "details-compat")->title == "Not listed",
              "a game the list doesn't have is Not listed");
        {
            // Without the list (not fetched yet, or an API that has none) the page says nothing about the community.
            auto catalog = model.state().community;
            catalog.compat.reset();
            model.UpdateCommunityCatalog(catalog);
            check(!find_row(model, "details-compat"), "no list: no Compatibility from the community");
        }
        // Search by game name and linked games.
        capture("recipes-imported-none", "preset-picker", "preset-playden-default");
        check(find_row(model, "recipes-none") && find_row(model, "recipes-search"), "no recipes of its own: None yet and the search");
        capture("recipes-search", "keyboard", "keyboard-done");
        check(model.state().editor.value == "Night Sky", "the search opens prefilled with the game's name");
        capture("recipes-search-results", "preset-picker", "search-game-gog/1453375253");
        check(find_row(model, "search-game-imported/night-sky") &&
                  find_row(model, "search-game-gog/1453375253")->subtitle == "GOG \u00b7 2 recipes",
              "the results name each game's store and recipe count");
        capture("recipes-other-game", "preset-picker", "recipe-" + std::string(64, '7'));
        check(model.view().comparison && model.view().comparison->note.find("isn't in this copy") != std::string::npos,
              "another game's recipe: the side panel notes Playden's program");
        // Chosen, another game's recipe is the one in use: the details tag says so, with no recipe of the game's own.
        check(model.action("cross"), "choose another game's recipe");
        if (model.view().surface == "confirmation") check(model.action("cross"), "switch to it");
        for (int back = 0; back < 4 && model.view().surface != "details"; ++back) model.action("circle");
        check(model.view().surface == "details", "back on the game's details");
        check(find_row(model, "details-recipe") && find_row(model, "details-recipe")->title == "Recipe in use",
              "another game's recipe in use carries the mark");
        capture("recipes-linked", "preset-picker", "recipe-" + std::string(64, '6'));
        check(find_row(model, "chooser-heading-linked") && find_row(model, "recipes-unlink") && find_row(model, one),
              "the game's own recipes, the linked game's, and Unlink");
        // The chooser with a recipe focused: its comparison beside the sheet.
        check(model.scenario("recipes-three") && model.action("focus:" + one), "focus the first recipe");
        for (int frame = 0; frame < 16; ++frame) renderer.draw(model, 1920, 1080);
        check(renderer.capture_png(output / "recipes-three-focused.png", 1920, 1080), "capture written");
        check(model.view().comparison && model.view().comparison->column == "This recipe", "the recipe's comparison shows");
        // Sharing and votes: the Share sheet's cases, the first share's notice, the sent dialog with its
        // QR code, the vote prompt, a thumbs down with a reason, and votes turned off.
        capture("share-new", "share-recipe", "share-title");
        check(find_row(model, "share-what") && find_row(model, "share-send"), "a new recipe: what is sent, Send");
        capture("share-ask", "share-recipe", "share-choice-update");
        check(find_row(model, "share-choice-update")->selected, "Suggest changes is pre-selected");
        capture("share-own", "share-recipe", "share-title");
        check(find_row(model, "share-send") && find_row(model, "share-send")->title == "Update the pull request",
              "an update to a share in review");
        capture("share-nothing", "share-recipe", "share-vote");
        check(model.action("circle") && model.view().surface == "game-settings" && model.action("focus:recipe-vote"),
              "Game settings has Your vote for the recipe in use");
        for (int frame = 0; frame < 16; ++frame) renderer.draw(model, 1920, 1080);
        check(renderer.capture_png(output / "share-nothing-settings.png", 1920, 1080), "capture written");
        check(model.scenario("share-nothing"), "scenario loads again");
        check(model.view().title == "Nothing new to share", "nothing new: vote instead");
        capture("share-privacy", "confirmation", "share-send-confirm");
        check(model.state().navigation.modals.back().title == "Before you share", "the first share says what is sent");
        capture("share-sent", "confirmation", "alert-ok");
        check(!model.state().navigation.modals.back().link.empty(), "the sent dialog shows the pull request's code");
        // Back in Game settings: the shared recipe's state and the vote for the recipe in use.
        check(model.action("cross") && model.view().surface == "game-settings", "OK goes back to Game settings");
        check(model.action("focus:shared-recipe"), "Game settings lists the shared recipe");
        for (int frame = 0; frame < 16; ++frame) renderer.draw(model, 1920, 1080);
        check(renderer.capture_png(output / "share-sent-settings.png", 1920, 1080), "capture written");
        check(find_row(model, "shared-recipe")->value == "In review", "the shared recipe is in review");
        capture("vote-prompt", "vote-recipe", "vote-up");
        check(model.view().title.rfind("How did", 0) == 0, "the vote prompt asks how the game ran");
        capture("vote-reason", "vote-recipe", "vote-reason-slow");
        check(find_row(model, "vote-reason-slow")->selected, "a thumbs down with a reason");
        capture("votes-off", "share-recipe", "share-vote");
        check(find_row(model, "share-vote")->subtitle.find("Recipe votes are off") != std::string::npos, "votes off says so");
        std::filesystem::remove(profile, ignored);
    }
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts("homebrew_community_graphical_test passed");
    return 0;
}
