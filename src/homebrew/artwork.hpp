#pragma once

#include "library/model.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace playden::homebrew {

inline constexpr std::size_t kMaxArtworkSubscriptions = 64;
inline constexpr std::size_t kMaxArtworkSourceBytes = 2048;
// Large enough for a Steam library hero (1920x620) beside the 600x900 covers.
inline constexpr std::size_t kMaxArtworkImageBytes = 5 * 1024 * 1024;
inline constexpr std::size_t kMaxArtworkResultBytes = 20 * 1024 * 1024;
inline constexpr std::uint32_t kMaxArtworkDimension = 1920;
// A failed load is retried while still subscribed, after a delay that doubles
// per failure, so covers recover once the network comes up after boot.
inline constexpr std::uint32_t kMaxArtworkAttempts = 3;
inline constexpr std::chrono::milliseconds kArtworkRetryDelay{15'000};

// A Steam friend's avatar has no game: its key holds an empty GameId and the avatar's source.
struct ArtworkKey {
  std::uint64_t account_generation = 0;
  GameId game_id;
  std::string source;
};

// The medium avatar for a persona state's avatar hash (40 lowercase hex digits), or empty for any other hash.
std::string AvatarArtworkSource(std::string_view hash);
// Whether `source` is exactly such an avatar; `hash`, when given, receives its hash.
bool IsAvatarArtworkSource(std::string_view source, std::string* hash = nullptr);

bool operator==(const ArtworkKey&, const ArtworkKey&);

struct ArtworkImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
};

struct ArtworkResult {
  ArtworkKey key;
  std::optional<ArtworkImage> image;
};

// Load reports failure as nullopt. An exception that escapes Load on a build
// with exceptions enabled is treated as a failed load, never as a crash.
class ArtworkBackend {
 public:
  virtual ~ArtworkBackend() = default;
  virtual std::optional<ArtworkImage> Load(
      const ArtworkKey&, const std::atomic<bool>& cancel) = 0;
};

// One worker owns backend calls. The backend outlives this service.
class ArtworkService {
 public:
  explicit ArtworkService(ArtworkBackend&,
                          std::chrono::milliseconds retry_delay = kArtworkRetryDelay);
  ~ArtworkService();
  ArtworkService(const ArtworkService&) = delete;
  ArtworkService& operator=(const ArtworkService&) = delete;

  // Replaces the complete visible-key set without waiting for IO. Invalid or
  // excessive sets are rejected without changing accepted work.
  bool ReplaceSubscriptions(std::vector<ArtworkKey>);
  std::vector<ArtworkResult> Pump(std::size_t maximum = 2);
  void Shutdown();

 private:
  struct Attempt {
    ArtworkKey key;
    std::uint32_t failures = 0;
    bool queued = true;  // pending or active; cleared when a load finishes
    std::chrono::steady_clock::time_point retry_at;
  };

  void Run();
  Attempt* FindAttempt(const ArtworkKey&);

  ArtworkBackend& backend_;
  const std::chrono::milliseconds retry_delay_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<ArtworkKey> subscriptions_;
  std::deque<Attempt> attempted_;
  std::deque<ArtworkKey> pending_;
  std::optional<ArtworkKey> active_;
  std::deque<ArtworkResult> results_;
  std::size_t result_bytes_ = 0;
  std::atomic<bool> active_cancel_{false};
  bool stopping_ = false;
  bool joined_ = false;
  std::thread worker_;
};

}  // namespace playden::homebrew
