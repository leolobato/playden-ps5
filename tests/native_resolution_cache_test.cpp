#include "steam/backend/native_resolution_cache.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace playden::steam::internal;
int calls = 0;
NativeResolveError next_error = NativeResolveError::kNone;
std::string address = "127.0.0.1";
void Check(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
NativeResolveError Resolve(CURL* easy, const std::string& url, std::uint32_t,
                           const std::atomic<bool>&, curl_slist** entries) {
  ++calls;
  if (next_error == NativeResolveError::kUnusableAddress) {
    // As the console resolver does: the refused answer stays attached for the log, and the caller never performs.
    *entries = curl_slist_append(nullptr, (NativeResolutionCache::Key(url) + ":0.0.0.0").c_str());
    curl_easy_setopt(easy, CURLOPT_RESOLVE, *entries);
    return next_error;
  }
  if (next_error != NativeResolveError::kNone) return next_error;
  const std::string entry = NativeResolutionCache::Key(url) + ":" + address;
  *entries = curl_slist_append(nullptr, entry.c_str());
  return *entries && curl_easy_setopt(easy, CURLOPT_RESOLVE, *entries) == CURLE_OK
             ? NativeResolveError::kNone : NativeResolveError::kFailed;
}
void CheckList(curl_slist* list, const std::string& key,
               const std::string& expected_address) {
  bool removal = false, replacement = false;
  for (; list; list = list->next) {
    if (list->data == "-" + key) removal = true;
    if (list->data == key + ":" + expected_address) {
      Check(removal, "curl DNS removal precedes replacement");
      replacement = true;
    }
  }
  Check(replacement, "resolve list retains numeric replacement");
}
struct Probe {
  int cache_hits = 0;
  int socket_attempts = 0;
};
int Debug(CURL*, curl_infotype type, char* bytes, std::size_t size, void* context) {
  if (type == CURLINFO_TEXT && std::string_view(bytes, size).find("was found in DNS cache") != std::string_view::npos)
    ++static_cast<Probe*>(context)->cache_hits;
  return 0;
}
curl_socket_t BlockSocket(void* context, curlsocktype, curl_sockaddr*) {
  ++static_cast<Probe*>(context)->socket_attempts;
  return CURL_SOCKET_BAD;  // Exercise curl DNS consumption without contacting a server.
}
CURL* Easy(const std::string& url, Probe& probe) {
  CURL* easy = curl_easy_init();
  Check(easy != nullptr, "DNS probe easy initializes");
  Check(curl_easy_setopt(easy, CURLOPT_URL, url.c_str()) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_PROXY, "") == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 100L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, Debug) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_DEBUGDATA, &probe) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, BlockSocket) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_OPENSOCKETDATA, &probe) == CURLE_OK,
        "DNS probe options configure");
  return easy;
}
void Perform(CURLM* multi, CURL* easy) {
  Check(curl_multi_add_handle(multi, easy) == CURLM_OK, "DNS probe joins persistent multi");
  int running = 0;
  do {
    Check(curl_multi_perform(multi, &running) == CURLM_OK, "DNS probe multi performs");
    if (running) Check(curl_multi_poll(multi, nullptr, 0, 10, nullptr) == CURLM_OK, "DNS probe polls");
  } while (running);
  int messages = 0;
  while (curl_multi_info_read(multi, &messages)) {}
  Check(curl_multi_remove_handle(multi, easy) == CURLM_OK, "DNS probe leaves persistent multi");
  curl_easy_cleanup(easy);
}
void NoCachedAddress(CURLM* multi, const std::string& url) {
  Probe probe;
  Perform(multi, Easy(url, probe));
  Check(probe.cache_hits == 0 && probe.socket_attempts == 0,
        "failed origin must not retain a permanent address in curl DNS cache");
}
}  // namespace
int main() {
  Check(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "curl initializes");
  Check(UnusableResolvedAddress("0.0.0.0") && UnusableResolvedAddress("127.0.0.1") &&
            UnusableResolvedAddress("127.255.0.9"),
        "a blocking DNS server's answers are unusable");
  Check(!UnusableResolvedAddress("142.250.203.219") && !UnusableResolvedAddress("10.0.1.9") &&
            !UnusableResolvedAddress("1.27.0.1"),
        "public and private addresses stay usable");
  std::atomic<bool> cancel{false};
  const auto start = NativeResolutionCache::Clock::time_point{};
  const std::string first = "https://CDN.EXAMPLE/chunk";
  Check(NativeResolutionCache::Key(first) == "cdn.example:443", "hostname normalized with default port");
  Check(NativeResolutionCache::Key("https://cdn.example:00443/other") == "cdn.example:443",
        "explicit default port shares normalized key");
  Check(NativeResolutionCache::Key("https://cdn.example:444/other") == "cdn.example:444",
        "different ports have different keys");
  Check(NativeResolutionCache::Key("invalid").empty(), "invalid URL has no cache key");
  {
    NativeResolutionCache cache(Resolve);
    CURLM* multi = NativeResolutionCache::CreateTransport();
    Check(multi != nullptr, "cache test transport initializes");
    curl_slist* entries = nullptr;
    auto attach = [&](const std::string& url, auto now) {
      CURL* easy = curl_easy_init();
      Check(easy != nullptr, "easy initializes");
      auto result = cache.Attach(easy, url, 1000, cancel, &entries, now);
      curl_easy_cleanup(easy);  // caller retains the resolve list until the next request
      return result;
    };
    Check(attach(first, start) == NativeResolveError::kNone && calls == 1, "initial cache miss resolves");
    CheckList(entries, "cdn.example:443", address);
    Check(attach("https://cdn.example:443/other", start + std::chrono::minutes(4)) == NativeResolveError::kNone && calls == 1,
          "cached result skips resolver and does not refresh TTL");
    address = "127.0.0.2";
    Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kNone && calls == 2,
          "entry expires exactly at five minutes");
    CheckList(entries, "cdn.example:443", address);
    cache.ResetTransport(multi);
    Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kNone && calls == 3,
          "failure invalidation forces a new resolution");
    for (auto error : {NativeResolveError::kFailed, NativeResolveError::kTimeout,
                       NativeResolveError::kCanceled}) {
      next_error = error;
      cache.ResetTransport(multi);
      const auto before_failure = calls;
      Check(attach(first, start + std::chrono::minutes(5)) == error && calls == before_failure + 1,
            "native resolver failure, timeout and cancel propagate");
      next_error = NativeResolveError::kNone;
      Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kNone && calls == before_failure + 2,
            "native failure, timeout and cancel are never cached");
    }
    next_error = NativeResolveError::kUnusableAddress;
    cache.ResetTransport(multi);
    const auto before_unusable = calls;
    Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kUnusableAddress &&
              cache.address() == "0.0.0.0",
          "an unusable answer is refused and keeps its address for the log");
    Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kUnusableAddress &&
              calls == before_unusable + 2,
          "an unusable answer is never cached");
    next_error = NativeResolveError::kNone;
    Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kNone && cache.address() == address,
          "the address of an attached answer is kept for the log");
    next_error = NativeResolveError::kFailed;
    cache.ResetTransport(multi);
    Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kFailed && cache.address().empty(),
          "resolution failure propagates");
    next_error = NativeResolveError::kNone;
    const auto before_retry = calls;
    Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kNone && calls == before_retry + 1,
          "failed resolutions are never cached");
    for (int i = 0; i < 32; ++i)
      Check(attach("https://host" + std::to_string(i) + ".example/", start + std::chrono::minutes(5)) == NativeResolveError::kNone,
            "bounded cache insertion succeeds");
    bool removed_evicted = false;
    for (auto* entry = entries; entry; entry = entry->next)
      removed_evicted |= entry->data == std::string("-cdn.example:443");
    Check(removed_evicted, "capacity eviction removes the old key from curl DNS cache");
    const auto before_evicted = calls;
    Check(attach(first, start + std::chrono::minutes(5)) == NativeResolveError::kNone && calls == before_evicted + 1,
          "cache bound evicts oldest entry");
    CheckList(entries, "cdn.example:443", address);
    curl_multi_cleanup(multi);
    curl_slist_free_all(entries);
  }
  {
    NativeResolutionCache cache(Resolve);
    curl_slist* entries = nullptr;
    const auto attach = [&](const std::string& url, auto now) {
      CURL* easy = curl_easy_init();
      const auto result = cache.Attach(easy, url, 1000, cancel, &entries, now);
      curl_easy_cleanup(easy);
      return result;
    };
    Check(attach(first, start) == NativeResolveError::kNone, "expired cache test seeds a name");
    next_error = NativeResolveError::kTimeout;
    Check(attach("https://other.example/", start + std::chrono::minutes(5)) == NativeResolveError::kTimeout,
          "failure during expiry propagates");
    next_error = NativeResolveError::kNone;
    Check(attach("https://other.example/", start + std::chrono::minutes(5)) == NativeResolveError::kNone,
          "resolution after expiry failure recovers");
    bool removed_expired = false;
    for (auto* entry = entries; entry; entry = entry->next)
      removed_expired |= entry->data == std::string("-cdn.example:443");
    Check(removed_expired, "expiry failure retains old curl DNS name for removal on recovery");
    curl_slist_free_all(entries);
  }
  {
    NativeResolutionCache cache(Resolve);
    CURLM* multi = NativeResolutionCache::CreateTransport();
    Check(multi != nullptr, "real DNS cache multi initializes");
    curl_slist* entries = nullptr;
    const std::string failed = "https://permanent-dns.invalid/";
    Probe seed;
    CURL* easy = Easy(failed, seed);
    Check(cache.Attach(easy, failed, 1000, cancel, &entries, start) == NativeResolveError::kNone,
          "real curl cache seeds native address");
    Perform(multi, easy);
    Check(seed.socket_attempts == 1, "curl consumed numeric native address before connection failure");
    cache.ResetTransport(multi);
    Check(multi != nullptr, "failed transport recreates bounded multi");
    NoCachedAddress(multi, failed);

    // A refused (sinkhole) answer is attached to an easy handle that is never performed; the caller resets the
    // transport. The next request resolves again and curl uses only the new address.
    next_error = NativeResolveError::kUnusableAddress;
    const std::string blocked = "https://blocked-dns.invalid/";
    Probe refused;
    CURL* refused_easy = Easy(blocked, refused);
    Check(cache.Attach(refused_easy, blocked, 1000, cancel, &entries, start) == NativeResolveError::kUnusableAddress &&
              cache.address() == "0.0.0.0",
          "a refused answer is reported with its address");
    curl_easy_cleanup(refused_easy);
    cache.ResetTransport(multi);
    NoCachedAddress(multi, blocked);
    next_error = NativeResolveError::kNone;
    Probe retried;
    CURL* retry_easy = Easy(blocked, retried);
    Check(cache.Attach(retry_easy, blocked, 1000, cancel, &entries, start) == NativeResolveError::kNone &&
              cache.address() == address,
          "the next request resolves again after a refused answer");
    Perform(multi, retry_easy);
    Check(retried.socket_attempts == 1, "the retry connects with the newly resolved address");
    cache.ResetTransport(multi);

    // Expiry builds a removal list, but cancellation/deadline before perform never consumes it.
    Probe expired_seed;
    easy = Easy(failed, expired_seed);
    Check(cache.Attach(easy, failed, 1000, cancel, &entries, start) == NativeResolveError::kNone,
          "recreated transport seeds address after failure");
    Perform(multi, easy);
    Probe abandoned;
    const std::string other = "https://prepared-dns.invalid/";
    easy = Easy(other, abandoned);
    Check(cache.Attach(easy, other, 1000, cancel, &entries, start + std::chrono::minutes(5)) == NativeResolveError::kNone,
          "expired address stages a DNS removal");
    bool pending_removal = false;
    for (auto* entry = entries; entry; entry = entry->next)
      pending_removal |= entry->data == std::string("-permanent-dns.invalid:443");
    Check(pending_removal, "abandoned request has not consumed expired DNS removal");
    curl_easy_cleanup(easy);  // Model cancellation/deadline immediately after resolution attachment.
    cache.ResetTransport(multi);
    NoCachedAddress(multi, failed);

    // Repeated failed origins cannot accumulate permanent DNS entries or native resolver results.
    const auto before_failures = calls;
    for (int i = 0; i < 40; ++i) {
      const std::string origin = "https://failed" + std::to_string(i) + ".invalid/";
      Probe failure;
      easy = Easy(origin, failure);
      Check(cache.Attach(easy, origin, 1000, cancel, &entries, start) == NativeResolveError::kNone,
            "distinct failed origin attaches native address");
      Perform(multi, easy);
      Check(failure.socket_attempts == 1, "failed-origin numeric DNS entry was consumed");
      cache.ResetTransport(multi);
    }
    Check(calls == before_failures + 40, "failed origins do not retain native cache results");
    NoCachedAddress(multi, "https://failed0.invalid/");
    NoCachedAddress(multi, "https://failed39.invalid/");

    // Repeated abandonments also clear successfully resolved native results before the next request.
    const auto before_abandoned = calls;
    for (int i = 0; i < 40; ++i) {
      Probe before_start;
      easy = Easy(other, before_start);
      Check(cache.Attach(easy, other, 1000, cancel, &entries, start) == NativeResolveError::kNone,
            "pre-start request attaches native result");
      if (i % 2 == 0) {
        Check(curl_multi_add_handle(multi, easy) == CURLM_OK, "abandoned request joins multi without performing");
        Check(curl_multi_remove_handle(multi, easy) == CURLM_OK, "abandoned request leaves multi before perform");
      }
      curl_easy_cleanup(easy);
      cache.ResetTransport(multi);
    }
    Check(calls == before_abandoned + 40, "pre-perform abandonment clears native cache every time");
    NoCachedAddress(multi, other);
    curl_multi_cleanup(multi);
    curl_slist_free_all(entries);
  }
  {
    NativeResolutionCache host;
    CURL* easy = curl_easy_init();
    curl_slist* entries = nullptr;
    Check(host.Attach(easy, first, 1000, cancel, &entries, start) == NativeResolveError::kNone && !entries,
          "host resolver remains no-op with no synthetic curl DNS entries");
    curl_easy_cleanup(easy);
  }
  curl_global_cleanup();
}
