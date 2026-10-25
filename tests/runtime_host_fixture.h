#pragma once
// The runtime host tests' shared fixture: a temporary console namespace with fake native, registry
// and kit hooks (Fixture), and a synthetic admitted kit (SyntheticKit). Shared by
// homebrew_runtime_host_test.cpp and homebrew_installed_locations_test.cpp.
#include "homebrew/runtime_host.hpp"
#include "platform/ps5/title_allow_list.h"
#include "runtime/artifact_hash.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef CHECK
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
#endif

// Its own namespace: the tests have names of their own (the model's Game, for one).
namespace playden::test::runtime_host {
using namespace playden::homebrew;
using namespace playden::runtime;
using namespace playden::ps5;

// The repository's bundled runtime data (recipes, presets, input): main sets it from argv.
inline std::string g_runtime_dir;

inline void Write(const std::filesystem::path& path, const std::string& bytes, unsigned mode) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
  CHECK(chmod(path.c_str(), mode) == 0);
}
inline std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
struct Fixture {
  std::filesystem::path root;
  unsigned database_reads = 0, registry_mutations = 0;
  bool registered = false;
  // ShadowMountPlus postpones scans while a title runs: with registers false a
  // submission is accepted and listed, but not installed until registered is set.
  bool registers = true, submitted = false;
  std::shared_ptr<std::atomic<int>> activity = std::make_shared<std::atomic<int>>(static_cast<int>(NativeActivity::kStopped));
  Fixture() {
    char name[] = "/tmp/playden-runtime-host-XXXXXX"; CHECK(mkdtemp(name));
    root = std::filesystem::canonical(name);
    std::filesystem::create_directories(root / "data/homebrew");
  }
  ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  RuntimeHostConfig Config() {
    RuntimeHostConfig config;
    config.boundary = root / "data";
    config.state_root = root / "data/runtime-state";
    config.homebrew_root = root / "data/homebrew";
    config.native.database = [this](const std::string& title) {
      ++database_reads;
      if (!registered) return TitleDatabaseObservation{TitleAvailability::kFree, {}};
      return TitleDatabaseObservation{TitleAvailability::kOccupied,
          InstalledTitleIdentity{title, "UP9000-FAKE78001_00-PROSPEROWIN00001", "/user/app/FAKE78001/sce_sys"}};
    };
    config.native.activity = [activity = activity](const std::string&) { return static_cast<NativeActivity>(activity->load()); };
    // ShadowMount loopback API: registration makes the title appear installed.
    config.registry_transport = [this](const std::string& route, const std::string&) -> std::optional<std::string> {
      if (route != "/api/v1/games") { ++registry_mutations; submitted = true; registered = registers; return R"({"status":0})"; }
      if (!registered && submitted)
        return R"({"status":0,"games":[{"title_id":"FAKE78001","installed":false,"managed":true,)"
               R"("source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78001"}]})";
      if (!registered) return R"({"status":0,"games":[]})";
      return R"({"status":0,"games":[{"title_id":"FAKE78001","installed":true,"managed":true,)"
             R"("source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78001"}]})";
    };
    config.admit_kit = [](std::string* reason) -> std::optional<RuntimeKit> {
      *reason = "No verified runtime kit is installed.";
      return std::nullopt;
    };
    config.runtime_dir = g_runtime_dir;
    return config;
  }
};
// Generous: a loaded build machine (parallel ctest, other builds) can stretch a step past 10 s.
inline RuntimeSnapshot WaitFor(const RuntimeHost& host, const std::function<bool(const RuntimeSnapshot&)>& done,
                        int seconds = 30) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  for (;;) {
    auto snapshot = host.Snapshot();
    if (done(snapshot)) return snapshot;
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
inline bool Started(const RuntimeSnapshot& s) { return s.status != RuntimeHostStatus::kStarting; }
// A synthetic admitted kit with the production file layout, real SHA-256 and
// the repository's HL2 recipe. It is not a runnable Wine runtime.
inline RuntimeKit SyntheticKit(const std::filesystem::path& kit, const std::string& eboot = "native",
                        const std::string& kit_id = "kit-1", char files_digest = 'a',
                        const std::string& runtime = "runtime", const std::string& revision = "rev") {
  Write(kit / "titles/variant/eboot.bin", eboot, 0755);
  Write(kit / "titles/variant/sce_sys/param.json",
        "{\n  \"contentId\": \"UP9000-FAKE78001_00-PROSPEROWIN00001\",\n  \"localizedParameters\": {\n"
        "    \"en-US\": {\n      \"titleName\": \"Prospero Win\"\n    }\n  },\n  \"titleId\": \"FAKE78001\"\n}\n", 0644);
  Write(kit / "titles/variant/sce_sys/icon0.png", "kit-icon", 0644);
  Write(kit / ("runtime/wine/" + revision + "/lib/core.prx"), runtime, 0755);
  Write(kit / "template/bottle/system.reg", "WINE REGISTRY Version 2\n", 0644);
  Write(kit / "template/bottle/user.reg", "WINE REGISTRY Version 2\n", 0644);
  Write(kit / "template/bottle/drive_c/windows/win.ini", "", 0644);
  Write(kit / "template/bottle/dosdevices/.pw-symlinks", "c:\t../drive_c\nz:\t/\n", 0644);
  Write(kit / "recipes/hl2.json", Read(std::filesystem::path(g_runtime_dir) / "recipes/steam-220-hl2-v1.json"), 0644);
  RuntimeKit admitted;
  auto& view = admitted.view;
  view.root = kit; view.kit_id = kit_id; view.files_sha256 = std::string(64, files_digest);
  view.runtime = {"wine-wow64", revision, "runtime/wine/" + revision};
  view.template_path = "template/bottle";
  view.template_drive_table = "template/bottle/dosdevices/.pw-symlinks";
  view.recipe = {"steam-220-hl2-v1", "1", "recipes/hl2.json", *HashArtifactFile(kit / "recipes/hl2.json")};
  view.variants.push_back({"variant", "FAKE78001", "titles/variant"});
  for (const auto& [path, mode] : std::vector<std::pair<std::string, unsigned>>{
           {"titles/variant/eboot.bin", 0755}, {"titles/variant/sce_sys/param.json", 0644},
           {"titles/variant/sce_sys/icon0.png", 0644},
           {"runtime/wine/" + revision + "/lib/core.prx", 0755}, {"template/bottle/system.reg", 0644},
           {"template/bottle/user.reg", 0644}, {"template/bottle/drive_c/windows/win.ini", 0644},
           {"template/bottle/dosdevices/.pw-symlinks", 0644}, {"recipes/hl2.json", 0644}})
    view.files.push_back({path, std::filesystem::file_size(kit / path), *HashArtifactFile(kit / path), mode});
  view.artifacts = {
      {"runtime", revision, *HashArtifactFile(kit / ("runtime/wine/" + revision + "/lib/core.prx")),
       ImmutableArtifactLocation::kStagedTitle, "win/wine/lib/core.prx"},
      {"recipe", "steam-220-hl2-v1-1", *HashArtifactFile(kit / "recipes/hl2.json"),
       ImmutableArtifactLocation::kKit, "recipes/hl2.json"}};
  admitted.capabilities = {"dxvk", "game-root-unowned", "offline_bottle", "per_game_title", "wow64_i386"};
  return admitted;
}

}  // namespace playden::test::runtime_host
