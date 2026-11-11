#include "homebrew/artwork.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace playden::homebrew {
namespace {

constexpr std::size_t kMaxArtworkResults = kMaxArtworkSubscriptions;

bool Contains(const std::deque<ArtworkKey>& values, const ArtworkKey& key) {
  return std::find(values.begin(), values.end(), key) != values.end();
}

bool ValidKey(const ArtworkKey& key) {
  return key.account_generation != 0 &&
         (key.game_id.valid() || (key.game_id == GameId{} && IsAvatarArtworkSource(key.source))) &&
         !key.source.empty() && key.source.size() <= kMaxArtworkSourceBytes &&
         key.source.find('\0') == std::string::npos;
}

bool ValidImage(const ArtworkImage& image) {
  if (image.width == 0 || image.height == 0 ||
      image.width > kMaxArtworkDimension ||
      image.height > kMaxArtworkDimension) return false;
  const std::uint64_t bytes = static_cast<std::uint64_t>(image.width) *
                              static_cast<std::uint64_t>(image.height) * 4u;
  return bytes <= kMaxArtworkImageBytes && image.rgba.size() == bytes;
}

std::size_t ResultBytes(const ArtworkResult& result) {
  return result.image ? result.image->rgba.size() : 0;
}

// Backends report ordinary transport/decode failures as nullopt. Where
// exceptions exist (host builds), an unexpected throw is also reduced to a
// failed load so the worker survives; PS5 builds compile without exceptions.
std::optional<ArtworkImage> LoadGuarded(ArtworkBackend& backend,
                                        const ArtworkKey& key,
                                        const std::atomic<bool>& cancel) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  try {
    return backend.Load(key, cancel);
  } catch (...) {
    return std::nullopt;
  }
#else
  return backend.Load(key, cancel);
#endif
}

constexpr std::string_view kAvatarPrefix = "https://avatars.steamstatic.com/";
constexpr std::string_view kAvatarSuffix = "_medium.jpg";

bool AvatarHash(std::string_view hash) {
  return hash.size() == 40 &&
         std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

std::string AvatarArtworkSource(std::string_view hash) {
  if (!AvatarHash(hash)) return {};
  return std::string(kAvatarPrefix) + std::string(hash) + std::string(kAvatarSuffix);
}

bool IsAvatarArtworkSource(std::string_view source, std::string* hash) {
  if (source.size() != kAvatarPrefix.size() + 40 + kAvatarSuffix.size() || source.substr(0, kAvatarPrefix.size()) != kAvatarPrefix ||
      source.substr(source.size() - kAvatarSuffix.size()) != kAvatarSuffix)
    return false;
  const auto found = source.substr(kAvatarPrefix.size(), 40);
  if (!AvatarHash(found)) return false;
  if (hash) *hash = std::string(found);
  return true;
}

bool operator==(const ArtworkKey& left, const ArtworkKey& right) {
  return left.account_generation == right.account_generation &&
         left.game_id == right.game_id && left.source == right.source;
}

ArtworkService::ArtworkService(ArtworkBackend& backend,
                               std::chrono::milliseconds retry_delay)
    : backend_(backend), retry_delay_(retry_delay), worker_([this] { Run(); }) {}

ArtworkService::Attempt* ArtworkService::FindAttempt(const ArtworkKey& key) {
  for (auto& attempt : attempted_)
    if (attempt.key == key) return &attempt;
  return nullptr;
}

ArtworkService::~ArtworkService() { Shutdown(); }

bool ArtworkService::ReplaceSubscriptions(std::vector<ArtworkKey> replacement) {
  if (replacement.size() > kMaxArtworkSubscriptions) return false;
  for (std::size_t index = 0; index < replacement.size(); ++index) {
    if (!ValidKey(replacement[index])) return false;
    if (std::find(replacement.begin(), replacement.begin() + index,
                  replacement[index]) != replacement.begin() + index)
      return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return false;
  const std::deque<ArtworkKey> next(replacement.begin(), replacement.end());

  if (active_ && !Contains(next, *active_))
    active_cancel_.store(true, std::memory_order_release);

  for (auto attempted = attempted_.begin(); attempted != attempted_.end();) {
    if (!Contains(next, attempted->key)) attempted = attempted_.erase(attempted);
    else ++attempted;
  }
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [&next](const ArtworkKey& key) {
                                  return !Contains(next, key);
                                }), pending_.end());
  for (auto result = results_.begin(); result != results_.end();) {
    if (!Contains(next, result->key)) {
      result_bytes_ -= ResultBytes(*result);
      result = results_.erase(result);
    } else {
      ++result;
    }
  }
  subscriptions_ = next;
  const auto now = std::chrono::steady_clock::now();
  for (const auto& key : subscriptions_) {
    auto* attempt = FindAttempt(key);
    if (!attempt) {
      attempted_.push_back({key, 0, true, {}});
      pending_.push_back(key);
    } else if (!attempt->queued && attempt->failures > 0 &&
               attempt->failures < kMaxArtworkAttempts && now >= attempt->retry_at) {
      attempt->queued = true;
      pending_.push_back(key);
    }
  }
  wake_.notify_all();
  return true;
}

std::vector<ArtworkResult> ArtworkService::Pump(std::size_t maximum) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto count = std::min(maximum, results_.size());
  std::vector<ArtworkResult> output;
  output.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    result_bytes_ -= ResultBytes(results_.front());
    output.push_back(std::move(results_.front()));
    results_.pop_front();
  }
  if (count) wake_.notify_all();
  return output;
}

void ArtworkService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (joined_) return;
    stopping_ = true;
    pending_.clear();
    active_cancel_.store(true, std::memory_order_release);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  joined_ = true;
}

void ArtworkService::Run() {
  for (;;) {
    ArtworkKey key;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] {
        const bool result_capacity = results_.size() < kMaxArtworkResults &&
            result_bytes_ <= kMaxArtworkResultBytes - kMaxArtworkImageBytes;
        return stopping_ || (!pending_.empty() && result_capacity);
      });
      if (stopping_) return;
      key = std::move(pending_.front());
      pending_.pop_front();
      active_ = key;
      active_cancel_.store(false, std::memory_order_release);
    }

    auto image = LoadGuarded(backend_, key, active_cancel_);
    if (image && !ValidImage(*image)) image.reset();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      const bool current = !stopping_ && Contains(subscriptions_, key) &&
                           !active_cancel_.load(std::memory_order_acquire);
      active_.reset();
      if (auto* attempt = FindAttempt(key)) {
        attempt->queued = false;
        if (current && image) {
          attempt->failures = 0;  // loaded: never retried while subscribed
        } else if (current) {
          ++attempt->failures;
          attempt->retry_at = std::chrono::steady_clock::now() +
                              retry_delay_ * (1u << (attempt->failures - 1));
        }
      }
      if (current) {
        ArtworkResult result{std::move(key), std::move(image)};
        result_bytes_ += ResultBytes(result);
        results_.push_back(std::move(result));
      }
    }
    wake_.notify_all();
  }
}

}  // namespace playden::homebrew
