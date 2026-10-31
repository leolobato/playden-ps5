#pragma once

#include "steam/backend/native_network.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string>
#include <vector>

namespace playden::steam::internal {

// Single-thread-owned native DNS cache. The caller owns the CURLOPT_RESOLVE list and must
// retain it until the easy handle has been cleaned up, before calling Attach again.
class NativeResolutionCache {
 public:
  using Clock = std::chrono::steady_clock;
  using Resolver = decltype(&AttachNativeResolution);
  explicit NativeResolutionCache(Resolver resolver = AttachNativeResolution) : resolver_(resolver) {}

  static CURLM* CreateTransport() {
    CURLM* multi = curl_multi_init();
    if (multi && curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, 4L) != CURLM_OK) {
      curl_multi_cleanup(multi);
      return nullptr;
    }
    return multi;
  }

  // Call only after removing/cleaning the easy handle. CURLOPT_RESOLVE entries are permanent
  // in the multi DNS cache; invalidation or unconsumed removal lists require discarding that
  // cache as well as our native results. A failed recreation leaves the client unavailable.
  void ResetTransport(CURLM*& multi) {
    if (multi) curl_multi_cleanup(multi);
    multi = CreateTransport();
    entries_.clear();
  }

  static std::string Key(const std::string& url) {
    CURLU* parsed = curl_url();
    if (!parsed) return {};
    char* host = nullptr;
    char* port = nullptr;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) == CURLUE_OK;
    std::string key;
    if (valid && host && port) {
      unsigned int number = 0;
      const char* end = port + std::char_traits<char>::length(port);
      const auto converted = std::from_chars(port, end, number);
      if (converted.ec == std::errc{} && converted.ptr == end) {
        key = host;
        for (char& c : key)
          if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        key += ":" + std::to_string(number);
      }
    }
    curl_free(host);
    curl_free(port);
    curl_url_cleanup(parsed);
    return key;
  }

  NativeResolveError Attach(CURL* easy, const std::string& url, std::uint32_t timeout_ms,
                            const std::atomic<bool>& cancel, curl_slist** active,
                            Clock::time_point now) {
    address_.clear();
    if (!easy || !active) return NativeResolveError::kFailed;
    curl_slist_free_all(*active);
    *active = nullptr;
    const auto key = Key(url);
    if (key.empty()) return NativeResolveError::kFailed;
    std::vector<std::string> removals;
    for (const auto& entry : entries_)
      if (now >= entry.expires) removals.push_back("-" + entry.key);
    auto cached = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry& entry) { return entry.key == key && now < entry.expires; });
    std::string replacement;
    std::string evicted_key;
    const bool cache_miss = cached == entries_.end();
    if (cached != entries_.end()) {
      replacement = cached->resolution;
    } else {
      const auto error = resolver_(easy, url, timeout_ms, cancel, active);
      if (error == NativeResolveError::kUnusableAddress && *active) {
        // Never cached; the next request asks the DNS server again.
        const std::string refused = (*active)->data;
        address_ = refused.substr(refused.rfind(':') + 1);
      }
      if (error != NativeResolveError::kNone) return error;
      // Host builds deliberately leave the list null and use curl's normal resolver.
      if (!*active) return NativeResolveError::kNone;
      const std::string native = (*active)->data;
      const auto colon = native.rfind(':');
      if (colon == std::string::npos) return NativeResolveError::kFailed;
      replacement = key + native.substr(colon);
      if (entries_.size() == kCapacity && removals.empty()) {
        evicted_key = entries_.front().key;
        removals.push_back("-" + evicted_key);
      }
    }
    // Native entries are permanent in curl's multi DNS cache. Explicit removal before every
    // replacement also handles an address change, expired entries and capacity eviction.
    removals.push_back("-" + key);
    curl_slist* updated = nullptr;
    for (const auto& removal : removals) {
      auto* appended = curl_slist_append(updated, removal.c_str());
      if (!appended) { curl_slist_free_all(updated); return NativeResolveError::kFailed; }
      updated = appended;
    }
    auto* appended = curl_slist_append(updated, replacement.c_str());
    if (!appended) { curl_slist_free_all(updated); return NativeResolveError::kFailed; }
    updated = appended;
    if (curl_easy_setopt(easy, CURLOPT_RESOLVE, updated) != CURLE_OK) {
      curl_slist_free_all(updated);
      return NativeResolveError::kFailed;
    }
    curl_slist_free_all(*active);
    *active = updated;
    // Commit pruning only after the removal list is attached. A resolver failure must not lose
    // the expired names that still need removal from curl's DNS cache on the next request.
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
      return now >= entry.expires || entry.key == evicted_key;
    }), entries_.end());
    if (cache_miss) entries_.push_back({key, replacement, now + std::chrono::minutes(5)});
    address_ = replacement.substr(replacement.rfind(':') + 1);
    return NativeResolveError::kNone;
  }

  // The numeric address the last Attach gave curl, or the unusable one it refused; empty when the
  // resolver left the choice to curl (host builds) or failed.
  const std::string& address() const { return address_; }

 private:
  struct Entry { std::string key, resolution; Clock::time_point expires; };
  static constexpr std::size_t kCapacity = 32;
  Resolver resolver_;
  std::vector<Entry> entries_;
  std::string address_;
};

}  // namespace playden::steam::internal
