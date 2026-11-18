// local_folder_recipe_test.cpp - The recipe.json a packager puts at the root of a game's folder: the Local store reads
// it when the game is added and hands it to keep_folder_recipe, and the Add's outcome carries what became of it (#1034).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/local/library_source.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace playden::stores::local;
namespace fs = std::filesystem;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {

struct TemporaryDirectory {
  TemporaryDirectory() {
    auto pattern = (fs::temp_directory_path() / "playden-folder-recipe-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    CHECK(::mkdtemp(writable.data()) != nullptr);
    path = fs::canonical(writable.data()).string();
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  std::string path;
};

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  CHECK(std::fclose(file) == 0);
}

// The smallest file the program check takes as a 64-bit Windows program.
std::string PeProgram() {
  std::string pe(0x80 + 26, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40; pe[0x40] = 'P'; pe[0x41] = 'E';
  pe[0x44] = 0x64; pe[0x45] = static_cast<char>(0x86); pe[0x40 + 24] = 0x0b; pe[0x40 + 25] = 0x02;
  return pe + std::string(4096, 'x');
}

struct Keeper {
  std::mutex mutex;
  std::vector<std::string> files;  // what it was given
  std::string problem;             // set: it refuses with this
};

ImportOutcome AddAndWait(LocalLibrarySource& source, const std::string& folder) {
  const auto request = source.Add("usb0", folder, "Game.exe");
  CHECK(request != 0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  for (;;) {
    const auto snapshot = source.Snapshot();
    for (const auto& outcome : snapshot->outcomes)
      if (outcome.request == request) return outcome;
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}  // namespace

int main() {
  TemporaryDirectory directory;
  const fs::path root = directory.path;
  const auto import = root / "mnt/usb0/playden/import";
  const std::string recipe = "{\"recipe_id\": \"community-imported-minecraft-1-20-1\"}";
  Write(import / "Plain/Game.exe", PeProgram());
  Write(import / "Packaged/Game.exe", PeProgram());
  Write(import / "Packaged/recipe.json", recipe);
  Write(import / "Refused/Game.exe", PeProgram());
  Write(import / "Refused/recipe.json", "{}");
  Write(import / "Large/Game.exe", PeProgram());
  Write(import / "Large/recipe.json", std::string(kMaxFolderRecipeBytes + 1, ' '));
  Write(import / "Linked/Game.exe", PeProgram());
  Write(root / "outside.json", recipe);
  CHECK(::symlink((root / "outside.json").c_str(), (import / "Linked/recipe.json").c_str()) == 0);
  Write(import / "Folder/Game.exe", PeProgram());
  fs::create_directories(import / "Folder/recipe.json");
  Write(import / "Unkept/Game.exe", PeProgram());
  Write(import / "Unkept/recipe.json", recipe);

  fs::create_directories(root / "state");
  auto storage = playden::Storage((root / "state").string()).OpenTree();
  ImportStore store(storage);
  Keeper keeper;
  LocalLibraryConfig config;
  config.volumes = {{"usb0", "/mnt/usb0", "playden/import", false}};
  config.host_prefix = root.string();
  config.present = [](const ImportVolume&) { return true; };
  config.keep_folder_recipe = [&keeper](std::string bytes) -> FolderRecipeKept {
    std::lock_guard<std::mutex> lock(keeper.mutex);
    keeper.files.push_back(bytes);
    if (bytes == "{}") return {{}, "This file is not a recipe."};
    if (!keeper.problem.empty()) return {};
    return {"sha256-of-the-recipe", {}};
  };
  LocalLibrarySource source(store, config);

  // No recipe.json: nothing is offered and the keeper is never asked.
  {
    const auto outcome = AddAndWait(source, "Plain");
    if (!outcome.ok) std::fprintf(stderr, "add refused: %s\n", outcome.message.c_str());
    CHECK(outcome.ok && !outcome.id.empty());
    CHECK(outcome.recipe_revision.empty() && outcome.recipe_problem.empty());
    CHECK(keeper.files.empty());
  }
  // A recipe.json at the folder's root: its bytes are kept, the outcome names the revision, and the file stays.
  {
    const auto outcome = AddAndWait(source, "Packaged");
    CHECK(outcome.ok && !outcome.id.empty());
    CHECK(outcome.recipe_revision == "sha256-of-the-recipe" && outcome.recipe_problem.empty());
    CHECK(keeper.files.size() == 1 && keeper.files.back() == recipe);
    CHECK(fs::is_regular_file(import / "Packaged/recipe.json"));
  }
  // One the keeper refuses: the game is added, and the outcome says why the recipe isn't used.
  {
    const auto outcome = AddAndWait(source, "Refused");
    CHECK(outcome.ok && !outcome.id.empty());
    CHECK(outcome.recipe_revision.empty() && outcome.recipe_problem == "This file is not a recipe.");
  }
  // Too large, a link, a folder: refused before the keeper sees anything.
  const auto before = keeper.files.size();
  {
    const auto outcome = AddAndWait(source, "Large");
    CHECK(outcome.ok && outcome.recipe_revision.empty() && outcome.recipe_problem.find("16 KB") != std::string::npos);
  }
  {
    const auto outcome = AddAndWait(source, "Linked");
    CHECK(outcome.ok && outcome.recipe_revision.empty() && !outcome.recipe_problem.empty());
  }
  {
    const auto outcome = AddAndWait(source, "Folder");
    CHECK(outcome.ok && outcome.recipe_revision.empty() && outcome.recipe_problem == "The game's recipe.json is not a file.");
  }
  CHECK(keeper.files.size() == before);
  // A keeper that answers nothing still gives the player a reason.
  {
    std::lock_guard<std::mutex> lock(keeper.mutex);
    keeper.problem = "unkept";
  }
  {
    const auto outcome = AddAndWait(source, "Unkept");
    CHECK(outcome.ok && outcome.recipe_revision.empty() && !outcome.recipe_problem.empty());
  }
  source.Shutdown();
  std::puts("local folder recipe tests passed");
  return 0;
}
