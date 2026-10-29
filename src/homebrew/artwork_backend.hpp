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
