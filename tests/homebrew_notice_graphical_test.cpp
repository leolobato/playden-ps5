// Playden - A corner notification shows its whole text: a long reason wraps and the box grows, and a short
// notice keeps its one-line box.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/render.hpp"
#include "homebrew/simulation.hpp"
#include "gfx/gl_program.hpp"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "steam_id.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using namespace playden::homebrew;

namespace {

struct Box {
  int top;
  int height;
};

struct Capture {
  int width = 0, height = 0;
  unsigned char* pixels = nullptr;
  explicit Capture(const std::filesystem::path& png) {
    int channels = 0;
    pixels = stbi_load(png.string().c_str(), &width, &height, &channels, 3);
    CHECK(pixels != nullptr);
  }
  ~Capture() { stbi_image_free(pixels); }
  const unsigned char* at(int x, int y) const { return pixels + (static_cast<std::size_t>(y) * width + x) * 3; }
  bool panel(int x, int y) const {
    const unsigned char* p = at(x, y);
    return std::abs(p[0] - 0x16) <= 2 && std::abs(p[1] - 0x14) <= 2 && std::abs(p[2] - 0x0F) <= 2;
  }
  // The notification boxes, top to bottom: runs of rows whose pixel at column 1300 is the box's colour. That
  // column is inside the box (1288..1824), clear of its rounded corners and left of the status dot. A run that
  // reaches the last row counts too.
  std::vector<Box> boxes() const {
    std::vector<Box> runs;
    int run = 0;
    for (int y = 100; y <= height; ++y) {
      if (y < height && panel(1300, y)) {
        ++run;
      } else if (run > 0) {
        if (run > 20) runs.push_back({y - run, run});
        run = 0;
      }
    }
    return runs;
  }
  // Whether a row band of the box's text column holds text: pixels far brighter than the panel.
  bool text_in(int from, int to) const {
    for (int y = from; y < to; ++y)
      for (int x = 1342; x < 1800; ++x)
        if (at(x, y)[0] > 0x60) return true;
    return false;
  }
};

const std::string kLongReason =
    "This file isn't a Playden recipe: community recipe needs community.format_version 1 to 2, "
    "and the copy stopped before anything on the drive changed.";

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  auto* window = SDL_CreateWindow("Notices", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  CHECK(window != nullptr);
  auto context = SDL_GL_CreateContext(window);
  CHECK(context != nullptr);
  SDL_GL_SetSwapInterval(0);
  hui::gfx::set_glsl_prefix("#version 410 core\n");
  const std::filesystem::path output(argv[1]);
  std::filesystem::create_directories(output);
  {
    Render renderer(PLAYDEN_HOMEBREW_ASSETS_DIR);
    Simulation model({}, {}, ModelMode::Live);
    model.SetWallClock(1700000000);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic account", {}, {}, true});
    CHECK(model.UpdateLibrary({{
        {SteamId(701), "Recipe Bad", {}, {}, {}},
        {SteamId(702), "Short Game", {}, {}, {}},
    }, false, {}, false, true, 1700000000}));
    // A reason far longer than one line of the box (the console cut "Recipe not used" at "This file is…").
    model.NotifyMoveRefused(GameId::Steam(701), kLongReason);
    model.NotifyMoveRefused(GameId::Steam(702), "No room.");
    for (int i = 0; i < 16; ++i) renderer.draw(model, 1920, 1080);
    const auto png = output / "notices.png";
    CHECK(renderer.capture_png(png, 1920, 1080));
    {
      const Capture capture(png);
      const auto boxes = capture.boxes();
      for (const Box& box : boxes) std::fprintf(stderr, "notice box at %d, height %d\n", box.top, box.height);
      CHECK(boxes.size() == 2);
      // The long reason takes at least three lines under the message, so the box grows by two lines or more...
      CHECK(boxes[0].height >= 84 + 2 * 24 - 2);
      // ...and its last line is drawn in the grown part, not cut.
      CHECK(capture.text_in(boxes[0].top + boxes[0].height - 34, boxes[0].top + boxes[0].height - 12));
      // A notice whose text fits keeps the one-line box.
      CHECK(boxes[1].height >= 82 && boxes[1].height <= 86);
      CHECK(capture.text_in(boxes[1].top + 44, boxes[1].top + 68));
    }

    // Eight long notices outgrow the screen: the oldest give way at the top, the newest (a short one) shows, and
    // no box runs past the bottom.
    std::vector<playden::GameSummary> games;
    for (std::uint32_t app = 801; app <= 808; ++app) {
      playden::GameSummary game;
      game.id = GameId::Steam(app);
      game.title = "Game " + std::to_string(app);
      games.push_back(game);
    }
    CHECK(model.UpdateLibrary({games, false, {}, false, true, 1700000000}));
    for (std::uint32_t app = 801; app <= 807; ++app) model.NotifyMoveRefused(GameId::Steam(app), kLongReason);
    model.NotifyMoveRefused(GameId::Steam(808), "No room.");
    for (int i = 0; i < 16; ++i) renderer.draw(model, 1920, 1080);
    const auto crowded = output / "crowded.png";
    CHECK(renderer.capture_png(crowded, 1920, 1080));
    {
      const Capture capture(crowded);
      const auto boxes = capture.boxes();
      for (const Box& box : boxes) std::fprintf(stderr, "crowded box at %d, height %d\n", box.top, box.height);
      CHECK(boxes.size() >= 2 && boxes.size() < 8);
      CHECK(boxes.back().height >= 82 && boxes.back().height <= 86);
      CHECK(boxes.back().top + boxes.back().height <= 1080 - 96 + 2);
    }
  }
  SDL_GL_DeleteContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::puts("notice graphical tests passed");
  return 0;
}
