// SPDX-License-Identifier: GPL-3.0-or-later
// An HttpClient made while the native network check fails checks again on its next request (#809). The test links
// http_client.cpp with this file in place of native_network.cpp, so the check can fail on the host.

#include "steam/backend/http_client.h"
#include "steam/backend/native_network.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failing_checks = 0;
int checks = 0;
int resolves = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

}  // namespace

namespace playden::steam::internal {

bool InitializeNativeNetworkForBootstrap() { return true; }

bool InitializeNativeNetwork() {
  ++checks;
  if (failing_checks > 0) {
    --failing_checks;
    return false;
  }
  return true;
}

bool UnusableResolvedAddress(const std::string& numeric) {
  return numeric == "0.0.0.0" || numeric.rfind("127.", 0) == 0;
}

// Every request that gets past the client's checks reaches the resolver, which fails it without opening a socket.
NativeResolveError AttachNativeResolution(CURL*, const std::string&, std::uint32_t, const std::atomic<bool>&,
                                          curl_slist**) {
  ++resolves;
  return NativeResolveError::kFailed;
}

}  // namespace playden::steam::internal

int main() {
  using playden::steam::HttpClient;
  using playden::steam::HttpError;
  using playden::steam::HttpRequest;
  std::atomic<bool> cancel{false};
  HttpRequest request;
  request.url = "https://steam.invalid/";
  request.timeout_ms = 2'000;

  // The check fails when the client is made and on its first request, then passes.
  failing_checks = 2;
  HttpClient client("unused-ca-bundle.pem");
  Check(checks == 1, "the client checks the native network when it is made");
  Check(client.Perform(request, cancel).error == HttpError::kInitializationFailed,
        "a request fails while the check fails");
  Check(checks == 2, "a request on a client that isn't ready checks again");
  Check(resolves == 0, "a request on a client that isn't ready doesn't reach the resolver");
  Check(client.Perform(request, cancel).error == HttpError::kNameResolutionFailed && resolves == 1,
        "a request after the check passes reaches the resolver on the same client");
  Check(checks == 3, "the passing check runs once");

  // A ready client doesn't check again.
  Check(client.Perform(request, cancel).error == HttpError::kNameResolutionFailed && resolves == 2,
        "a ready client keeps reaching the resolver");
  Check(checks == 3, "a ready client does not check again");

  // A canceled request on a client that isn't ready doesn't run the check.
  failing_checks = 1;
  HttpClient unready("unused-ca-bundle.pem");
  const int before = checks;
  cancel.store(true);
  Check(unready.Perform(request, cancel).error == HttpError::kCanceled, "a canceled request reports canceled");
  Check(checks == before, "a canceled request doesn't run the network check");
  return 0;
}
