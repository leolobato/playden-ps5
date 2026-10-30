#include "homebrew/artwork_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "steam_id.h"

using namespace playden::homebrew;
using playden::GameId;
using namespace std::chrono_literals;

namespace {

ArtworkImage Image(std::uint32_t side, std::uint8_t value) {
  return {side, side, std::vector<std::uint8_t>(static_cast<std::size_t>(side) * side * 4, value)};
}

class ControlledArtwork final : public ArtworkBackend {
 public:
  std::atomic<int> calls{0};
  std::atomic<bool> block_old{false};
  std::atomic<bool> release_old{false};
  std::atomic<bool> old_started{false};
  std::map<GameId, std::uint32_t> sides;
  std::set<GameId> malformed;
  std::set<GameId> failed;

  std::optional<ArtworkImage> Load(const ArtworkKey& key,
                                   const std::atomic<bool>&) override {
    ++calls;
    if (key.source == "old" && block_old) {
      old_started = true;
      while (!release_old) std::this_thread::sleep_for(1ms);
    }
    if (failed.contains(key.game_id)) return std::nullopt;
    const auto side = sides.contains(key.game_id) ? sides[key.game_id] : 1U;
    auto image = Image(side, key.source == "old" ? 1 : key.source == "new" ? 2
                                                                  : static_cast<std::uint8_t>(*key.game_id.steam_app()));
    if (malformed.contains(key.game_id)) image.rgba.pop_back();
    return image;
  }
};

class FakeTextures final : public TextureBackend {
 public:
  std::uint32_t next = 1;
  std::set<std::uint8_t> fail_values;
  std::vector<std::uint8_t> uploads;
  std::vector<std::uint32_t> releases;
  std::thread::id owner = std::this_thread::get_id();

  std::uint32_t Upload(const ArtworkImage& image) override {
    assert(std::this_thread::get_id() == owner);
    const auto value = image.rgba.empty() ? 0 : image.rgba.front();
    uploads.push_back(value);
    return fail_values.contains(value) ? 0 : next++;
  }
  void Release(std::uint32_t texture) override {
    assert(std::this_thread::get_id() == owner && texture != 0);
    releases.push_back(texture);
  }
};

void WaitFor(const std::atomic<bool>& value) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!value && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(value);
}

void WaitCalls(const std::atomic<int>& value, int count) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (value.load() < count && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(value.load() >= count);
}

std::uint32_t AwaitTexture(LiveArtworkCache& cache, std::uint64_t generation,
                           GameId game, const std::string& source) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    cache.BeginFrame(generation);
    const auto texture = cache.TextureFor(game, source);
    cache.EndFrame();
    if (texture != 0) return texture;
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "artwork texture deadline exceeded");
  return 0;
}

void DeduplicatesAndUploadsOnlyOnUiThread() {
  ControlledArtwork backend;
  ArtworkService service(backend);
  FakeTextures textures;
  LiveArtworkCache cache(service, textures);
  cache.BeginFrame(1);
  assert(cache.TextureFor(SteamId(10), "a") == 0);
  assert(cache.TextureFor(SteamId(10), "a") == 0);
  cache.EndFrame();
  assert(cache.Stats().visible_count == 1 && cache.Stats().request_count == 1);
  const auto texture = AwaitTexture(cache, 1, SteamId(10), "a");
  assert(texture != 0 && cache.Stats().texture_count == 1 && cache.Stats().gpu_bytes == 4);
  const int calls = backend.calls;
  cache.BeginFrame(1);
  cache.EndFrame();
  cache.BeginFrame(1);
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  assert(cache.TextureFor(SteamId(10), "a", &width, &height) == texture);
  assert(width == 1 && height == 1);  // the size is reported so a logo can be fitted
  cache.EndFrame();
  assert(backend.calls == calls);
}

void UploadCountIsBoundedPerFrame() {
  ControlledArtwork backend;
  ArtworkService service(backend);
  FakeTextures textures;
  LiveArtworkCache cache(service, textures);
  cache.BeginFrame(1);
  for (std::uint32_t id = 1; id <= 4; ++id) (void)cache.TextureFor(SteamId(id), "batch");
  cache.EndFrame();
  WaitCalls(backend.calls, 4);
  cache.BeginFrame(1);
  for (std::uint32_t id = 1; id <= 4; ++id) (void)cache.TextureFor(SteamId(id), "batch");
  const auto before = textures.uploads.size();
  cache.EndFrame();
  assert(textures.uploads.size() - before == kMaxArtworkUploadsPerFrame);
}

void LateSourceAndGenerationCannotPopulateCurrentIdentity() {
  ControlledArtwork backend;
  backend.block_old = true;
  ArtworkService service(backend);
  FakeTextures textures;
  LiveArtworkCache cache(service, textures);
  cache.BeginFrame(1);
  (void)cache.TextureFor(SteamId(20), "old");
  cache.EndFrame();
  WaitFor(backend.old_started);
  cache.BeginFrame(1);
  (void)cache.TextureFor(SteamId(20), "new");
  cache.EndFrame();
  backend.release_old = true;
  assert(AwaitTexture(cache, 1, SteamId(20), "new") != 0);
  assert(std::find(textures.uploads.begin(), textures.uploads.end(), 1) == textures.uploads.end());
  assert(std::find(textures.uploads.begin(), textures.uploads.end(), 2) != textures.uploads.end());

  const auto releases = textures.releases.size();
  cache.BeginFrame(2);
  // The generation change releases everything before this frame draws; the new generation's
  // load may then finish within this same EndFrame, so check before it.
  assert(textures.releases.size() == releases + 1 && cache.Stats().texture_count == 0);
  assert(cache.TextureFor(SteamId(20), "new") == 0);
  cache.EndFrame();
  assert(AwaitTexture(cache, 2, SteamId(20), "new") != 0);
}

void InvalidAndFailedUploadsStayPlaceholders() {
  ControlledArtwork backend;
  backend.malformed.insert(SteamId(30));
  ArtworkService service(backend);
  FakeTextures textures;
  textures.fail_values.insert(31);
  LiveArtworkCache cache(service, textures);
  for (int frame = 0; frame < 30; ++frame) {
    cache.BeginFrame(1);
    assert(cache.TextureFor(SteamId(30), "bad") == 0);
    assert(cache.TextureFor(SteamId(31), "upload-fails") == 0);
    cache.EndFrame();
    std::this_thread::sleep_for(1ms);
  }
  assert(cache.Stats().texture_count == 0 && backend.calls == 2);
  cache.BeginFrame(1);
  cache.EndFrame();
  textures.fail_values.clear();
  backend.malformed.clear();
  assert(AwaitTexture(cache, 1, SteamId(30), "bad") != 0);
  assert(AwaitTexture(cache, 1, SteamId(31), "upload-fails") != 0);

  cache.BeginFrame(1);
  assert(cache.TextureFor(SteamId(0), "x") == 0);
  assert(cache.TextureFor(SteamId(1), "") == 0);
  assert(cache.TextureFor(SteamId(1), std::string(kMaxArtworkSourceBytes + 1, 'x')) == 0);
  const std::string embedded_nul{"bad\0source", 10};
  assert(cache.TextureFor(SteamId(1), embedded_nul) == 0);
  assert(cache.TextureFor(SteamId(99), "valid-alongside-invalid") == 0);
  for (std::uint32_t id = 1; id <= 65; ++id) (void)cache.TextureFor(SteamId(100 + id), "bounded");
  cache.EndFrame();
  assert(cache.Stats().visible_count == kMaxArtworkSubscriptions);
  assert(AwaitTexture(cache, 1, SteamId(99), "valid-alongside-invalid") != 0);
}

void UploadAndEvictionBudgetsProtectCurrentFrameTextures() {
  ControlledArtwork backend;
  for (std::uint32_t id = 1; id <= 17; ++id) backend.sides[SteamId(id)] = 1024;
  ArtworkService service(backend);
  FakeTextures textures;
  LiveArtworkCache cache(service, textures);
  for (std::uint32_t id = 1; id <= 16; ++id) assert(AwaitTexture(cache, 1, SteamId(id), "large") != 0);
  assert(cache.Stats().texture_count == 16 && cache.Stats().gpu_bytes == kMaxArtworkGpuBytes);

  // Keep all 17 referenced until the 17th result has certainly been loaded and
  // offered for upload; none of the 16 referenced textures may be evicted.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  for (int frames_after_load = 0; frames_after_load < 50;) {
    assert(std::chrono::steady_clock::now() < deadline);
    cache.BeginFrame(1);
    for (std::uint32_t id = 1; id <= 17; ++id) (void)cache.TextureFor(SteamId(id), "large");
    cache.EndFrame();
    if (backend.calls.load() >= 17) ++frames_after_load;
    std::this_thread::sleep_for(1ms);
  }
  assert(textures.releases.empty() && cache.Stats().texture_count == 16);

  const auto seventeenth = AwaitTexture(cache, 1, SteamId(17), "large");
  assert(seventeenth != 0 && !textures.releases.empty());
  assert(cache.Stats().texture_count == 16 && cache.Stats().gpu_bytes <= kMaxArtworkGpuBytes);
}

void DestructorReleasesEveryTexture() {
  ControlledArtwork backend;
  ArtworkService service(backend);
  FakeTextures textures;
  {
    LiveArtworkCache cache(service, textures);
    assert(AwaitTexture(cache, 1, SteamId(40), "a") != 0);
    assert(AwaitTexture(cache, 1, SteamId(41), "b") != 0);
  }
  assert(textures.releases.size() == 2);
}

void TextureCountEvictsLeastRecentlySeenUnusedEntry() {
  ControlledArtwork backend;
  ArtworkService service(backend);
  FakeTextures textures;
  LiveArtworkCache cache(service, textures);
  for (std::uint32_t id = 1; id <= 65; ++id)
    assert(AwaitTexture(cache, 1, SteamId(1000 + id), "count") != 0);
  assert(cache.Stats().texture_count == kMaxArtworkTextures);
  assert(!textures.releases.empty());
}

}  // namespace

// A cover that failed to load says so, so a tile can show the program's icon meanwhile (#213).
void FailedLoadIsReported() {
  ControlledArtwork backend;
  backend.failed.insert(SteamId(30));
  ArtworkService service(backend, 1h);
  FakeTextures textures;
  LiveArtworkCache cache(service, textures);
  cache.BeginFrame(1);
  assert(!cache.Failed(SteamId(30), "cover") && cache.TextureFor(SteamId(30), "cover") == 0);
  assert(cache.TextureFor(SteamId(31), "cover") == 0);
  cache.EndFrame();
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  bool failed = false;
  while (!failed && std::chrono::steady_clock::now() < deadline) {
    cache.BeginFrame(1);
    (void)cache.TextureFor(SteamId(30), "cover");
    (void)cache.TextureFor(SteamId(31), "cover");
    cache.EndFrame();
    failed = cache.Failed(SteamId(30), "cover");
    std::this_thread::sleep_for(1ms);
  }
  assert(failed && !cache.Failed(SteamId(30), "other") && !cache.Failed(SteamId(32), "cover"));
  assert(AwaitTexture(cache, 1, SteamId(31), "cover") != 0 && !cache.Failed(SteamId(31), "cover"));
  // Another account's cache knows nothing of it.
  cache.BeginFrame(2);
  assert(!cache.Failed(SteamId(30), "cover"));
  cache.EndFrame();
}

int main() {
  FailedLoadIsReported();
  DeduplicatesAndUploadsOnlyOnUiThread();
  UploadCountIsBoundedPerFrame();
  LateSourceAndGenerationCannotPopulateCurrentIdentity();
  InvalidAndFailedUploadsStayPlaceholders();
  UploadAndEvictionBudgetsProtectCurrentFrameTextures();
  DestructorReleasesEveryTexture();
  TextureCountEvictsLeastRecentlySeenUnusedEntry();
}
