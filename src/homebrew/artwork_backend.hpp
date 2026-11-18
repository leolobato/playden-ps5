#pragma once

#include "homebrew/artwork.hpp"
#include "steam/backend/http_client.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace playden::homebrew {

inline constexpr std::size_t kMaxArtworkTransferBytes = 4 * 1024 * 1024;
// Bounds stb's simultaneous internal allocations. The validated returned RGBA
// vector is separately bounded by kMaxArtworkImageBytes.
inline constexpr std::size_t kMaxArtworkDecodeAllocationBytes = 16 * 1024 * 1024;

std::optional<ArtworkImage> DecodeArtwork(
    const std::vector<std::uint8_t>& encoded);

// A picture a player put beside a game (homebrew/folder_art.hpp): a PNG, up to 4K, larger than store art can be.
inline constexpr std::size_t kMaxPlayerArtBytes = 32 * 1024 * 1024;
inline constexpr std::uint32_t kMaxPlayerArtSide = 8192;
inline constexpr std::uint64_t kMaxPlayerArtPixels = 3840ULL * 2160;
enum class PlayerArtCheck { kOk, kNotPng, kTooLarge, kUnreadable };
// Its header only: a PNG of at most kMaxPlayerArtSide on each side and kMaxPlayerArtPixels in all.
PlayerArtCheck CheckPlayerArt(const std::vector<std::uint8_t>& encoded);
// The whole picture, as 8-bit RGBA, for a file CheckPlayerArt accepts (8- or 16-bit, interlaced or not).
std::optional<ArtworkImage> DecodePlayerArt(const std::vector<std::uint8_t>& encoded);

namespace artwork_internal {
// Deterministic budget seam for allocator-bound tests. Production callers use
// DecodeArtwork and its fixed kMaxArtworkDecodeAllocationBytes ceiling.
std::optional<ArtworkImage> DecodeArtworkWithAllocationBudget(
    const std::vector<std::uint8_t>& encoded, std::size_t allocation_limit);
}

class HttpArtworkBackend final : public ArtworkBackend {
 public:
  using Perform = std::function<steam::HttpResponse(const steam::HttpRequest&, const std::atomic<bool>&)>;

  explicit HttpArtworkBackend(std::string ca_bundle, std::string cache_dir = "");
  HttpArtworkBackend(Perform perform, std::string cache_dir);  // tests
  ~HttpArtworkBackend() override;
  HttpArtworkBackend(const HttpArtworkBackend&) = delete;
  HttpArtworkBackend& operator=(const HttpArtworkBackend&) = delete;

  std::optional<ArtworkImage> Load(
      const ArtworkKey&, const std::atomic<bool>& cancel) override;

  const std::string& cache_dir() const noexcept { return cache_dir_; }

 private:
  struct Cache;
  std::string cache_dir_;
  std::unique_ptr<steam::HttpClient> http_;
  Perform perform_;
  std::unique_ptr<Cache> cache_;
};

}  // namespace playden::homebrew
