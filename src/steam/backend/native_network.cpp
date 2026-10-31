#include "steam/backend/native_network.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
int* sceNetErrnoLoc(void);
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int pool_id);
int sceNetRecv(int socket, void* data, std::size_t size, int flags);
int sceNetResolverCreate(const char* name, int pool_id, int flags);
int sceNetResolverAbort(int resolver_id, int flags);
int sceNetResolverDestroy(int resolver_id);
int sceNetResolverStartNtoa(int resolver_id, const char* hostname, in_addr_t* address,
                            int timeout, int retries, int flags);
int sceNetSetsockopt(int socket, int level, int option, const void* value,
                     std::uint32_t size);
}
#endif

namespace playden::steam::internal {
namespace {

#if defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5
using Clock = std::chrono::steady_clock;

bool NativeNonblockingPreflight() {
  constexpr int kSceNetSolSocket = 0xffff;
  constexpr int kSceNetSoNonblock = 0x1200;
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  socklen_t address_size = sizeof(address);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener, 1) != 0 ||
      getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
    close(listener);
    return false;
  }

  const int client = socket(AF_INET, SOCK_STREAM, 0);
  const bool connected = client >= 0 &&
                         connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  if (!connected) {
    if (client >= 0) close(client);
    close(listener);
    return false;
  }
  // A failed or ignored nonblocking option must not leave either receive blocked. The watchdog
  // operates on the actual connected client, independent of server accept scheduling.
  std::atomic<bool> release_watchdog{false};
  std::thread watchdog([client, &release_watchdog] {
    const auto deadline = Clock::now() + std::chrono::milliseconds(50);
    while (!release_watchdog.load(std::memory_order_acquire) && Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!release_watchdog.load(std::memory_order_acquire)) shutdown(client, SHUT_RDWR);
  });
  int enabled = 1;
  const int option_result = connected
                                ? sceNetSetsockopt(client, kSceNetSolSocket, kSceNetSoNonblock,
                                                   &enabled, sizeof(enabled))
                                : -1;
  unsigned char byte = 0;
  const auto native_start = Clock::now();
  const int native_result = option_result == 0 ? sceNetRecv(client, &byte, 1, 0) : 0;
  const int native_error = native_result < 0 && sceNetErrnoLoc() ? *sceNetErrnoLoc() : 0;
  const auto native_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - native_start).count();
  errno = 0;
  const auto posix_start = Clock::now();
  const ssize_t posix_result = option_result == 0 ? recv(client, &byte, 1, 0) : 0;
  const int posix_error = posix_result < 0 ? errno : 0;
  const auto posix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - posix_start).count();

  release_watchdog.store(true, std::memory_order_release);
  watchdog.join();
  close(client);
  close(listener);
  return option_result == 0 && native_result < 0 && native_error == EWOULDBLOCK &&
         native_ms < 20 && posix_result < 0 &&
         (posix_error == EAGAIN || posix_error == EWOULDBLOCK) && posix_ms < 20;
}

bool InitializeNativeNetworkOnce() {
  // Do not call sceNetTerm: ownership may be shared with rendering or artwork subsystems, and the
  // process-wide runtime is needed for the full application lifetime.
  return sceNetInit() == 0;
}
#endif

}  // namespace

bool InitializeNativeNetworkForBootstrap() {
#if defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5
  static std::once_flag once;
  static bool ready = false;
  std::call_once(once, [] { ready = InitializeNativeNetworkOnce(); });
  return ready;
#else
  return true;
#endif
}

bool InitializeNativeNetwork() {
  if (!InitializeNativeNetworkForBootstrap()) return false;
#if defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5
  static std::once_flag once;
  static bool ready = false;
  std::call_once(once, [] { ready = NativeNonblockingPreflight(); });
  return ready;
#else
  return true;
#endif
}

bool UnusableResolvedAddress(const std::string& numeric) {
  return numeric == "0.0.0.0" || numeric.rfind("127.", 0) == 0;
}

NativeResolveError AttachNativeResolution(CURL* easy, const std::string& url,
                                          std::uint32_t timeout_ms,
                                          const std::atomic<bool>& cancel,
                                          curl_slist** entries) {
#if defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5
  if (!easy || !entries || timeout_ms == 0) return NativeResolveError::kFailed;
  CURLU* parsed = curl_url();
  if (!parsed) return NativeResolveError::kFailed;
  char* host = nullptr;
  char* port = nullptr;
  bool ok = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
            curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK;
  if (ok && curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) != CURLUE_OK) ok = false;

  int pool = -1;
  int resolver = -1;
  in_addr_t address = 0;
  char numeric[INET_ADDRSTRLEN]{};
  if (ok) pool = sceNetPoolCreate("playden-http-resolver", 0x4000, 0);
  if (pool >= 0) resolver = sceNetResolverCreate("playden-http-resolver", pool, 0);
  NativeResolveError resolve_error = NativeResolveError::kFailed;
  if (resolver >= 0) {
    const std::uint32_t bounded_ms = std::min<std::uint32_t>(timeout_ms, 5'000);
    const int timeout_microseconds = static_cast<int>(bounded_ms * 1000);
    std::atomic<bool> done{false};
    int native_result = -1;
    std::thread worker([&] {
      native_result = sceNetResolverStartNtoa(resolver, host, &address, timeout_microseconds, 0, 0);
      done.store(true, std::memory_order_release);
    });
    const auto deadline = Clock::now() + std::chrono::milliseconds(bounded_ms);
    while (!done.load(std::memory_order_acquire) && !cancel.load(std::memory_order_acquire) &&
           Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!done.load(std::memory_order_acquire)) sceNetResolverAbort(resolver, 0);
    worker.join();
    if (cancel.load(std::memory_order_acquire)) {
      resolve_error = NativeResolveError::kCanceled;
      ok = false;
    } else if (Clock::now() >= deadline && native_result != 0) {
      resolve_error = NativeResolveError::kTimeout;
      ok = false;
    } else {
      ok = native_result == 0 && inet_ntop(AF_INET, &address, numeric, sizeof(numeric));
      resolve_error = !ok ? NativeResolveError::kFailed
                          : UnusableResolvedAddress(numeric) ? NativeResolveError::kUnusableAddress
                                                             : NativeResolveError::kNone;
    }
  } else {
    ok = false;
  }
  if (ok) {
    const std::string entry = std::string(host) + ":" + port + ":" + numeric;
    curl_slist* appended = curl_slist_append(*entries, entry.c_str());
    ok = appended != nullptr;
    if (ok) {
      *entries = appended;
      ok = curl_easy_setopt(easy, CURLOPT_RESOLVE, *entries) == CURLE_OK;
    }
    if (!ok) resolve_error = NativeResolveError::kFailed;
    // Attached for the log only: the caller refuses the request before curl runs it.
    if (resolve_error == NativeResolveError::kUnusableAddress) ok = false;
  }
  if (resolver >= 0) sceNetResolverDestroy(resolver);
  if (pool >= 0) sceNetPoolDestroy(pool);
  curl_free(host);
  curl_free(port);
  curl_url_cleanup(parsed);
  return ok ? NativeResolveError::kNone : resolve_error;
#else
  (void)easy;
  (void)url;
  (void)timeout_ms;
  (void)cancel;
  (void)entries;
  return NativeResolveError::kNone;
#endif
}

}  // namespace playden::steam::internal
