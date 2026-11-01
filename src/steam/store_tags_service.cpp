#include "steam/store_tags_service.h"

#include <chrono>
#include <cctype>
#include <thread>
#include <utility>

namespace playden::steam {
namespace {

constexpr std::size_t kMaximumTags = 32;
constexpr std::size_t kMaximumTagBytes = 64;
constexpr std::size_t kMaximumPageBytes = 512 * 1024;
constexpr std::uint32_t kRequestTimeoutMs = 15'000;
constexpr auto kThrottleBetweenRequests = std::chrono::milliseconds(350);

bool ValidTag(std::string_view tag) {
  return !tag.empty() && tag.size() <= kMaximumTagBytes && tag.find('\0') == std::string_view::npos;
}

// Minimal JSON string-array parser for `"tags":["A","B"]` embedded in HTML.
std::vector<std::string> ParseTagsArray(std::string_view json) {
  std::vector<std::string> tags;
  tags.reserve(8);
  for (std::size_t index = 0; index < json.size();) {
    while (index < json.size() && json[index] != '"') ++index;
    if (index >= json.size()) break;
    ++index;
    std::string value;
    while (index < json.size()) {
      const char c = json[index++];
      if (c == '\\' && index < json.size()) {
        value.push_back(json[index++]);
        continue;
      }
      if (c == '"') break;
      value.push_back(c);
    }
    if (ValidTag(value) && tags.size() < kMaximumTags) tags.push_back(std::move(value));
    while (index < json.size() && json[index] != '"' && json[index] != ']') ++index;
    if (index < json.size() && json[index] == ']') break;
  }
  return tags;
}

}  // namespace

std::string StorePageUrl(std::uint32_t app) {
  return "https://store.steampowered.com/app/" + std::to_string(app) + "/";
}

std::vector<std::string> ParseStoreTagsFromPage(std::string_view body) {
  if (body.size() > kMaximumPageBytes) return {};
  const std::string needle = "\"tags\":";
  const auto start = body.find(needle);
  if (start == std::string_view::npos) return {};
  const auto array_start = body.find('[', start + needle.size());
  if (array_start == std::string_view::npos) return {};
  const auto array_end = body.find(']', array_start);
  if (array_end == std::string_view::npos || array_end <= array_start) return {};
  return ParseTagsArray(body.substr(array_start, array_end - array_start + 1));
}

StoreTagsService::StoreTagsService(Fetch fetch) : fetch_(std::move(fetch)) {
  worker_ = std::thread([this] { Run(); });
}

StoreTagsService::~StoreTagsService() { Shutdown(); }

void StoreTagsService::Request(GameId id) {
  if (!id.steam_app()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || pending_.size() >= kMaxPending || !requested_.insert(id).second) return;
    pending_.push_back(id);
  }
  wake_.notify_one();
}

void StoreTagsService::Forget(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  requested_.erase(id);
}

std::vector<StoreTags> StoreTagsService::Pump() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoreTags> out(std::make_move_iterator(results_.begin()), std::make_move_iterator(results_.end()));
  results_.clear();
  return out;
}

void StoreTagsService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cancel_.store(true);
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void StoreTagsService::Run() {
  for (;;) {
    GameId id;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      id = pending_.front();
      pending_.pop_front();
    }
    const auto app = *id.steam_app();
    const auto body = fetch_ ? fetch_(StorePageUrl(app), cancel_) : std::nullopt;
    StoreTags tags;
    tags.id = id;
    if (body && body->size() <= kMaximumPageBytes) tags.tags = ParseStoreTagsFromPage(*body);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      results_.push_back(std::move(tags));
    }
    if (!stopping_) std::this_thread::sleep_for(kThrottleBetweenRequests);
  }
}

}  // namespace playden::steam
