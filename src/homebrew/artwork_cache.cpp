#include "homebrew/artwork_cache.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace playden::homebrew {
namespace {

bool Contains(const std::vector<ArtworkKey>& values, const ArtworkKey& key) {
  return std::find(values.begin(), values.end(), key) != values.end();
}

std::optional<std::size_t> ImageBytes(const ArtworkImage& image) {
  if (image.width == 0 || image.height == 0 ||
      image.width > kMaxArtworkDimension || image.height > kMaxArtworkDimension)
    return std::nullopt;
  const std::size_t width = image.width;
  const std::size_t height = image.height;
  if (width > std::numeric_limits<std::size_t>::max() / height / 4) return std::nullopt;
  const std::size_t expected = width * height * 4;
  if (expected > kMaxArtworkImageBytes || image.rgba.size() != expected) return std::nullopt;
  return expected;
}

}  // namespace

struct LiveArtworkCache::State {
  struct Entry {
    ArtworkKey key;
    std::uint32_t texture = 0;
    std::size_t bytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t last_seen_frame = 0;
    bool referenced = false;
    bool failed = false;
  };

  ArtworkService& service;
  TextureBackend& textures;
  std::uint64_t generation = 0;
  std::uint64_t frame = 0;
  std::vector<ArtworkKey> visible;
  std::vector<Entry> entries;
  std::deque<ArtworkResult> deferred;
  std::size_t gpu_bytes = 0;
  std::size_t request_count = 0;

  State(ArtworkService& service_value, TextureBackend& texture_value)
      : service(service_value), textures(texture_value) {}

  Entry* Find(const ArtworkKey& key) {
    const auto found = std::find_if(entries.begin(), entries.end(), [&key](const Entry& entry) {
      return entry.key == key;
    });
    return found == entries.end() ? nullptr : &*found;
  }

  const Entry* Find(const ArtworkKey& key) const {
    const auto found = std::find_if(entries.begin(), entries.end(), [&key](const Entry& entry) {
      return entry.key == key;
    });
    return found == entries.end() ? nullptr : &*found;
  }

  void ReleaseAll() {
    for (const auto& entry : entries)
      if (entry.texture != 0) textures.Release(entry.texture);
    entries.clear();
    deferred.clear();
    visible.clear();
    gpu_bytes = 0;
    request_count = 0;
  }

  bool MakeRoom(std::size_t bytes) {
    auto texture_count = [&] {
      return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
                                                    [](const Entry& entry) { return entry.texture != 0; }));
    };
    while (texture_count() >= kMaxArtworkTextures || bytes > kMaxArtworkGpuBytes - gpu_bytes) {
      auto victim = entries.end();
      for (auto current = entries.begin(); current != entries.end(); ++current) {
        if (current->texture == 0 || current->referenced) continue;
        if (victim == entries.end() || current->last_seen_frame < victim->last_seen_frame)
          victim = current;
      }
      if (victim == entries.end()) return false;
      textures.Release(victim->texture);
      gpu_bytes -= victim->bytes;
      entries.erase(victim);
    }
    return true;
  }

  void MarkFailed(const ArtworkKey& key) {
    if (auto* entry = Find(key)) {
      if (entry->texture == 0) entry->failed = true;
      return;
    }
    entries.push_back({key, 0, 0, 0, 0, frame, true, true});
  }
};

LiveArtworkCache::LiveArtworkCache(ArtworkService& service, TextureBackend& textures)
    : state_(std::make_unique<State>(service, textures)) {}

LiveArtworkCache::~LiveArtworkCache() {
  if (!state_) return;
  state_->service.ReplaceSubscriptions({});
  state_->ReleaseAll();
}

void LiveArtworkCache::BeginFrame(std::uint64_t account_generation) {
  auto& state = *state_;
  ++state.frame;
  if (state.generation != account_generation) {
    state.service.ReplaceSubscriptions({});
    state.ReleaseAll();
    state.generation = account_generation;
  }
  state.visible.clear();
  state.request_count = 0;
  for (auto& entry : state.entries) entry.referenced = false;
}

std::uint32_t LiveArtworkCache::TextureFor(GameId game_id, std::string_view source,
                                           std::uint32_t* width, std::uint32_t* height) {
  auto& state = *state_;
  if (state.generation == 0 || !game_id.valid() || source.empty() ||
      source.size() > kMaxArtworkSourceBytes || source.find('\0') != std::string_view::npos)
    return 0;
  ArtworkKey key{state.generation, game_id, std::string(source)};
  if (!Contains(state.visible, key)) {
    if (state.visible.size() >= kMaxArtworkSubscriptions) return 0;
    state.visible.push_back(key);
  }
  auto* entry = state.Find(key);
  if (!entry || entry->texture == 0) return 0;
  entry->referenced = true;
  entry->last_seen_frame = state.frame;
  if (width) *width = entry->width;
  if (height) *height = entry->height;
  return entry->texture;
}

bool LiveArtworkCache::Failed(GameId game_id, std::string_view source) const {
  const auto& state = *state_;
  if (state.generation == 0) return false;
  for (const auto& entry : state.entries)
    if (entry.key.account_generation == state.generation && entry.key.game_id == game_id && entry.key.source == source)
      return entry.failed && entry.texture == 0;
  return false;
}

void LiveArtworkCache::EndFrame() {
  auto& state = *state_;
  state.entries.erase(std::remove_if(state.entries.begin(), state.entries.end(), [&state](const auto& entry) {
                        return entry.texture == 0 && entry.failed && !Contains(state.visible, entry.key);
                      }),
                      state.entries.end());

  std::vector<ArtworkKey> requests;
  requests.reserve(state.visible.size());
  for (const auto& key : state.visible) {
    const auto* entry = state.Find(key);
    if (!entry || entry->texture == 0) requests.push_back(key);
  }
  state.request_count = requests.size();
  if (!state.service.ReplaceSubscriptions(std::move(requests))) state.request_count = 0;

  std::size_t uploads = 0;
  std::size_t upload_bytes = 0;
  auto consume = [&](ArtworkResult& result) -> bool {
    if (result.key.account_generation != state.generation ||
        !Contains(state.visible, result.key) ||
        (state.Find(result.key) && state.Find(result.key)->texture != 0))
      return true;
    if (!result.image) {
      state.MarkFailed(result.key);
      return true;
    }
    const auto bytes = ImageBytes(*result.image);
    if (!bytes) {
      state.MarkFailed(result.key);
      return true;
    }
    if (uploads >= kMaxArtworkUploadsPerFrame ||
        *bytes > kMaxArtworkUploadBytesPerFrame - upload_bytes)
      return false;
    if (!state.MakeRoom(*bytes)) return false;
    const auto texture = state.textures.Upload(*result.image);
    ++uploads;
    upload_bytes += *bytes;
    if (texture == 0) {
      state.MarkFailed(result.key);
      return true;
    }
    if (auto* entry = state.Find(result.key)) {
      entry->texture = texture;
      entry->bytes = *bytes;
      entry->width = result.image->width;
      entry->height = result.image->height;
      entry->last_seen_frame = state.frame;
      entry->referenced = true;
      entry->failed = false;
    } else {
      state.entries.push_back({result.key, texture, *bytes, result.image->width, result.image->height,
                               state.frame, true, false});
    }
    state.gpu_bytes += *bytes;
    return true;
  };

  while (!state.deferred.empty() && uploads < kMaxArtworkUploadsPerFrame) {
    auto result = std::move(state.deferred.front());
    state.deferred.pop_front();
    if (!consume(result)) {
      state.deferred.push_front(std::move(result));
      break;
    }
  }
  if (state.deferred.empty() && uploads < kMaxArtworkUploadsPerFrame) {
    auto results = state.service.Pump(kMaxArtworkUploadsPerFrame - uploads);
    for (auto& result : results)
      if (!consume(result)) state.deferred.push_back(std::move(result));
  }
}

ArtworkCacheStats LiveArtworkCache::Stats() const noexcept {
  const auto& state = *state_;
  ArtworkCacheStats stats;
  stats.texture_count = static_cast<std::size_t>(std::count_if(
      state.entries.begin(), state.entries.end(), [](const auto& entry) { return entry.texture != 0; }));
  stats.gpu_bytes = state.gpu_bytes;
  stats.visible_count = state.visible.size();
  stats.request_count = state.request_count;
  return stats;
}

}  // namespace playden::homebrew
