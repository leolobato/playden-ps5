#pragma once

#include "homebrew/artwork.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace playden::homebrew {

inline constexpr std::size_t kMaxArtworkTextures = 64;
inline constexpr std::size_t kMaxArtworkGpuBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxArtworkUploadBytesPerFrame = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxArtworkUploadsPerFrame = 2;

class TextureBackend {
 public:
  virtual ~TextureBackend() = default;
  virtual std::uint32_t Upload(const ArtworkImage&) = 0;
  virtual void Release(std::uint32_t texture) = 0;
};

struct ArtworkCacheStats {
  std::size_t texture_count = 0;
  std::size_t gpu_bytes = 0;
  std::size_t visible_count = 0;
  std::size_t request_count = 0;
};

// Lives on the graphics thread. ArtworkService and TextureBackend outlive it.
class LiveArtworkCache {
 public:
  LiveArtworkCache(ArtworkService&, TextureBackend&);
  ~LiveArtworkCache();
  LiveArtworkCache(const LiveArtworkCache&) = delete;
  LiveArtworkCache& operator=(const LiveArtworkCache&) = delete;

  void BeginFrame(std::uint64_t account_generation);
  // A texture once loaded, else 0; `width`/`height`, when given, receive its image size.
  [[nodiscard]] std::uint32_t TextureFor(GameId, std::string_view source,
                                         std::uint32_t* width = nullptr,
                                         std::uint32_t* height = nullptr);
  // Its load failed (it may still be retried): the caller can show something else meanwhile.
  [[nodiscard]] bool Failed(GameId, std::string_view source) const;
  void EndFrame();
  [[nodiscard]] ArtworkCacheStats Stats() const noexcept;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace playden::homebrew
