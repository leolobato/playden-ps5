#include "steam/backend/http_client.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using playden::steam::HttpClient;
using playden::steam::HttpError;
using playden::steam::HttpMethod;
using playden::steam::HttpRequest;
using playden::steam::HttpResponse;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::string Text(const std::vector<std::uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// The fixture's streamed bodies: byte i is (i * 31 + 7) & 0xff.
std::uint8_t PatternByte(std::uint64_t index) {
  return static_cast<std::uint8_t>((index * 31 + 7) & 0xff);
}

std::string Header(const HttpResponse& response, const std::string& name) {
  for (const auto& header : response.headers)
    if (header.first == name) return header.second;
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  Check(argc == 6, "expected BASE_URL CA_BUNDLE WRONG_CA_BUNDLE PLAIN_URL TLS12_URL");
  const std::string base_url = argv[1];
  const std::string ca_bundle = argv[2];
  const std::string wrong_ca_bundle = argv[3];
  const std::string plain_url = argv[4];
  const std::string tls12_url = argv[5];
  std::atomic<bool> cancel{false};
  HttpClient client(ca_bundle);

  {
    // A TLS 1.2-only server that signs its key exchange with RSA-PSS, as Alibaba Cloud storage does (#155).
    HttpRequest request;
    request.url = tls12_url + "/ok";
    const auto response = client.Perform(request, cancel);
    if (!response.ok()) std::cerr << "TLS 1.2 RSA-PSS GET: " << playden::steam::HttpFailureName(response) << '\n';
    Check(response.ok() && Text(response.body) == "fixture-ok",
          "TLS 1.2 server with an RSA-PSS key exchange signature should be accepted");
  }

  {
    HttpClient persistent(ca_bundle);
    HttpRequest keep;
    keep.url = base_url + "/keep/marked";
    auto first = persistent.Perform(keep, cancel);
    Check(first.ok() && first.timing.new_connections == 1, "first keepalive GET opens connection");
    Check(Header(first, "X-Fixture-Accepted") == "1", "server accepts first connection");
    const auto connection = Header(first, "X-Fixture-Connection");
    keep.url = base_url + "/keep/ok";
    auto second = persistent.Perform(keep, cancel);
    Check(second.ok() && Text(second.body) == "fixture-ok", "second keepalive GET succeeds");
    Check(second.timing.new_connections == 0 && second.timing.tls_ms == 0,
          "second keepalive GET reuses connection without TLS handshake");
    Check(Header(second, "X-Fixture-Connection") == connection &&
          Header(second, "X-Fixture-Accepted") == "1", "server sees one connection for two GETs");
    Check(Header(second, "X-Fixture-Marker").empty(), "response headers do not leak");

    HttpRequest other = keep;
    other.url.replace(other.url.find("localhost"), 9, "127.0.0.1");
    auto different = persistent.Perform(other, cancel);
    Check(different.ok() && different.timing.new_connections == 1 &&
          Header(different, "X-Fixture-Connection") != connection, "different origins do not share connections");

    keep.url = base_url + "/keep/close";
    Check(persistent.Perform(keep, cancel).ok(), "server closes idle connection after a reply");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    keep.url = base_url + "/keep/ok";
    auto reconnected = persistent.Perform(keep, cancel);
    Check(reconnected.ok() && reconnected.timing.new_connections == 1 &&
          Text(reconnected.body) == "fixture-ok", "stale idle connection reconnects");

    const auto check_recovery = [&] {
      auto recovered = persistent.Perform(keep, cancel);
      Check(recovered.ok() && Text(recovered.body) == "fixture-ok" &&
            Header(recovered, "X-Fixture-Marker").empty() && recovered.timing.new_connections == 1,
            "failed half-read connection is closed and next GET has clean body and headers");
    };
    HttpRequest slow = keep;
    slow.url = base_url + "/keep/slow";
    slow.timeout_ms = 100;
    Check(persistent.Perform(slow, cancel).error == HttpError::kTimeout, "keepalive partial body times out");
    check_recovery();
    slow.timeout_ms = 3000;
    std::thread cancel_keep([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      cancel.store(true);
    });
    auto canceled_keep = persistent.Perform(slow, cancel);
    cancel_keep.join();
    Check(canceled_keep.error == HttpError::kCanceled, "keepalive partial body cancels");
    cancel.store(false);
    check_recovery();
    HttpRequest large = keep;
    large.url = base_url + "/keep/large";
    large.max_response_bytes = 64;
    Check(persistent.Perform(large, cancel).error == HttpError::kResponseTooLarge, "keepalive body limit enforced");
    check_recovery();

    for (auto method : {HttpMethod::kPost, HttpMethod::kPut}) {
      HttpRequest upload;
      upload.method = method;
      upload.url = base_url + "/keep/echo";
      upload.body = {'x'};
      std::string previous = Header(persistent.Perform(keep, cancel), "X-Fixture-Connection");
      for (int attempt = 0; attempt < 2; ++attempt) {
        auto sent = persistent.Perform(upload, cancel);
        Check(sent.ok() && Text(sent.body) == ":x" && sent.timing.new_connections == 1 &&
              Header(sent, "X-Fixture-Connection") != previous, "POST and PUT always open fresh connections");
        previous = Header(sent, "X-Fixture-Connection");
      }
      auto after = persistent.Perform(keep, cancel);
      Check(after.ok() && Text(after.body) == "fixture-ok" &&
            Header(after, "X-Fixture-Connection") != previous, "GET after upload does not reuse upload connection");
    }
  }

  HttpRequest get;
  get.url = base_url + "/ok";
  auto response = client.Perform(get, cancel);
  Check(response.ok(), "verified GET should succeed");
  Check(response.status_code == 200, "GET should retain HTTP status");
  Check(Text(response.body) == "fixture-ok", "GET should retain response bytes");
  Check(response.timing.new_connections == 1, "GET should measure a fresh connection");
  Check(response.timing.resolve_ms == 0, "host has no native resolution phase");
  Check(response.timing.total_ms > 0 && response.timing.body_bytes == 10, "GET should measure time and bytes");
  bool found_result = false;
  for (const auto& header : response.headers)
    found_result |= header.first == "X-ErEsUlT" && header.second == "42";
  Check(found_result, "response should retain bounded header names, casing, and values");

  HttpRequest post;
  post.method = HttpMethod::kPost;
  post.url = base_url + "/echo";
  post.headers.emplace_back("X-Playden-Test", "present");
  post.body = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
  response = client.Perform(post, cancel);
  Check(response.ok() && response.status_code == 201, "verified POST should succeed");
  Check(response.transport_code == 0 && playden::steam::HttpFailureName(response) == "none",
        "a successful transfer has no transport code");
  Check(Text(response.body) == "present:payload", "POST should send headers and bytes");

  HttpRequest timeout;
  timeout.url = base_url + "/slow";
  timeout.timeout_ms = 100;
  auto started = std::chrono::steady_clock::now();
  response = client.Perform(timeout, cancel);
  Check(response.timing.total_ms > 0, "failure should preserve timing");
  Check(response.error == HttpError::kTimeout, "slow response should time out");
  Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
        "timeout should be enforced near its deadline");

  HttpRequest canceled = timeout;
  canceled.timeout_ms = 3000;
  cancel.store(false);
  std::thread canceler([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancel.store(true);
  });
  started = std::chrono::steady_clock::now();
  response = client.Perform(canceled, cancel);
  canceler.join();
  Check(response.error == HttpError::kCanceled, "cancel flag should abort active request");
  Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
        "cancellation should interrupt the multi loop promptly");
  cancel.store(false);

  HttpRequest capped;
  capped.url = base_url + "/large";
  capped.max_response_bytes = 64;
  response = client.Perform(capped, cancel);
  Check(response.error == HttpError::kResponseTooLarge, "response cap should fail closed");
  Check(response.body.empty(), "oversized response bytes should not escape");

  HttpRequest header_capped;
  header_capped.url = base_url + "/large-headers";
  response = client.Perform(header_capped, cancel);
  Check(response.error == HttpError::kResponseHeadersTooLarge,
        "response headers should have an independent count/byte cap");
  Check(response.headers.empty(), "oversized headers should not escape");

  HttpRequest malformed;
  malformed.url = "http://localhost/not-https";
  response = client.Perform(malformed, cancel);
  Check(response.error == HttpError::kInvalidRequest, "non-HTTPS URL should be rejected");
  malformed.url = plain_url + "/ok";
  response = client.Perform(malformed, cancel);
  Check(response.error == HttpError::kInvalidRequest, "plain HTTP needs the request's opt-in");
  HttpRequest plain;
  plain.url = plain_url + "/ok";
  plain.allow_plain_http = true;
  response = client.Perform(plain, cancel);
  Check(response.ok() && response.status_code == 200 && Text(response.body) == "fixture-ok",
        "an opted-in request reaches a plain HTTP server");
  plain.url = "ftp://localhost/ok";
  response = client.Perform(plain, cancel);
  Check(response.error == HttpError::kInvalidRequest, "the opt-in allows http only, not other schemes");
  malformed.url = base_url + std::string("/ok\0ignored", 11);
  response = client.Perform(malformed, cancel);
  Check(response.error == HttpError::kInvalidRequest, "embedded NUL in URL should be rejected");

  HttpRequest redirect;
  redirect.url = base_url + "/redirect";
  response = client.Perform(redirect, cancel);
  Check(response.ok() && response.status_code == 302,
        "redirect should be returned without being followed");

  HttpRequest invalid_bounds = get;
  invalid_bounds.timeout_ms = playden::steam::kMaxHttpTimeoutMs + 1;
  response = client.Perform(invalid_bounds, cancel);
  Check(response.error == HttpError::kInvalidRequest, "timeout should have a hard upper bound");

  // Only a depot manifest may ask for more than the general cap (#451), and only up to its own.
  HttpRequest large_cap = get;
  large_cap.max_response_bytes = playden::steam::kMaxHttpResponseBytes + 1;
  response = client.Perform(large_cap, cancel);
  Check(response.error == HttpError::kInvalidRequest, "a response cap above 16 MiB needs the manifest opt-in");
  large_cap.depot_manifest = true;
  large_cap.max_response_bytes = playden::steam::kMaxHttpManifestResponseBytes;
  response = client.Perform(large_cap, cancel);
  Check(response.ok() && response.status_code == 200, "a depot manifest may be up to 64 MiB");
  large_cap.max_response_bytes = playden::steam::kMaxHttpManifestResponseBytes + 1;
  response = client.Perform(large_cap, cancel);
  Check(response.error == HttpError::kInvalidRequest, "a depot manifest's cap has an upper bound");

  {
    // A body sink streams a 2xx body block by block and never buffers it.
    HttpRequest stream;
    stream.url = base_url + "/stream/5242880";
    stream.max_response_bytes = 8 * 1024 * 1024;
    stream.stall_timeout_ms = 5000;
    std::uint64_t received = 0;
    bool in_order = true;
    stream.body_sink = [&](const std::uint8_t* data, std::size_t size) {
      for (std::size_t index = 0; index < size; ++index)
        in_order = in_order && data[index] == PatternByte(received + index);
      received += size;
      return true;
    };
    response = client.Perform(stream, cancel);
    Check(response.ok() && response.status_code == 200, "a streamed GET succeeds");
    Check(received == 5242880 && in_order, "the sink gets every byte in order");
    Check(response.body.empty() && response.timing.body_bytes == 5242880,
          "a streamed body is counted but not kept");

    // A limit that is not a multiple of curl's block size: the sink never sees a byte past it.
    received = 0;
    stream.max_response_bytes = 1000 * 1000;
    response = client.Perform(stream, cancel);
    Check(response.error == HttpError::kResponseTooLarge, "a streamed body past its limit fails");
    Check(received > 0 && received <= 1000 * 1000, "the sink got part of the body, and no more than the limit");
    Check(response.body.empty() && response.status_code == 0, "a failed stream keeps no body");
    stream.max_response_bytes = 8 * 1024 * 1024;

    int refusals = 0;
    stream.body_sink = [&](const std::uint8_t*, std::size_t) {
      ++refusals;
      return false;
    };
    response = client.Perform(stream, cancel);
    Check(response.error == HttpError::kSinkFailed && refusals == 1, "a sink that refuses a block fails the request");
    Check(std::string(playden::steam::HttpErrorName(HttpError::kSinkFailed)) == "sink_failed",
          "the sink failure has a stable name");

    // Cancel from inside the sink after the first block: no further block reaches it.
    HttpRequest slow_stream = stream;
    slow_stream.url = base_url + "/stream-slow";
    int calls_after_cancel = 0;
    slow_stream.body_sink = [&](const std::uint8_t*, std::size_t) {
      if (cancel.load()) ++calls_after_cancel;
      cancel.store(true);
      return true;
    };
    started = std::chrono::steady_clock::now();
    response = client.Perform(slow_stream, cancel);
    Check(response.error == HttpError::kCanceled, "a stream cancels");
    Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500), "a stream cancels promptly");
    Check(calls_after_cancel == 0, "the sink is not called after cancel");
    cancel.store(false);

    // No body byte for the stall limit fails, even with no total deadline.
    HttpRequest stalled = stream;
    stalled.url = base_url + "/stall";
    stalled.stall_timeout_ms = 500;
    stalled.timeout_ms = 0;
    received = 0;
    stalled.body_sink = [&](const std::uint8_t*, std::size_t size) {
      received += size;
      return true;
    };
    started = std::chrono::steady_clock::now();
    response = client.Perform(stalled, cancel);
    Check(response.error == HttpError::kTimeout && received > 0, "a stalled stream times out mid-body");
    Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1500),
          "a stalled stream fails near its stall limit");

    // A slow but steady body runs past 2 s: with timeout_ms 0 there is no total cap.
    HttpRequest trickle = stalled;
    trickle.url = base_url + "/trickle";
    received = 0;
    started = std::chrono::steady_clock::now();
    response = client.Perform(trickle, cancel);
    Check(response.ok() && response.status_code == 200 && received == 25 * 4096, "a steady trickle completes");
    Check(std::chrono::steady_clock::now() - started > std::chrono::milliseconds(2000),
          "the trickle ran longer than 2 s");

    // An error status keeps its body for the caller and never reaches the sink.
    HttpRequest missing = stalled;
    missing.url = base_url + "/stream-404";
    missing.stall_timeout_ms = 5000;
    int sink_calls = 0;
    missing.body_sink = [&](const std::uint8_t*, std::size_t) {
      ++sink_calls;
      return true;
    };
    response = client.Perform(missing, cancel);
    Check(response.ok() && response.status_code == 404 && sink_calls == 0, "an error status skips the sink");
    Check(Text(response.body) == "{\"error\":\"not_found\"}", "an error status keeps its body");

    // An error body longer than max_response_bytes (sized for the 2xx body) still reads as its status: a refused
    // link to a 68-byte chunk must not look like an overlong chunk.
    HttpRequest refused = missing;
    refused.url = base_url + "/stream-403/500";
    refused.max_response_bytes = 69;
    sink_calls = 0;
    response = client.Perform(refused, cancel);
    Check(response.ok() && response.status_code == 403 && sink_calls == 0 && response.body.size() == 500,
          "an error body over max_response_bytes keeps its status and body");
    refused.url = base_url + "/stream-403/" + std::to_string(playden::steam::kMaxHttpStreamErrorBodyBytes + 3 * 4096);
    response = client.Perform(refused, cancel);
    Check(response.ok() && response.status_code == 403 && sink_calls == 0 &&
              response.body.size() == playden::steam::kMaxHttpStreamErrorBodyBytes,
          "an error body over the cap is cut there and keeps its status");
    Check(Text(client.Perform(missing, cancel).body) == "{\"error\":\"not_found\"}", "the client works after a cut");

    HttpRequest invalid_stream = missing;
    invalid_stream.method = HttpMethod::kPost;
    Check(client.Perform(invalid_stream, cancel).error == HttpError::kInvalidRequest, "a sink is GET only");
    invalid_stream = missing;
    invalid_stream.stall_timeout_ms = 0;
    Check(client.Perform(invalid_stream, cancel).error == HttpError::kInvalidRequest, "a sink needs a stall limit");
    invalid_stream.stall_timeout_ms = playden::steam::kMaxHttpStallTimeoutMs + 1;
    Check(client.Perform(invalid_stream, cancel).error == HttpError::kInvalidRequest, "a stall limit has a cap");
    invalid_stream = missing;
    invalid_stream.max_response_bytes = playden::steam::kMaxHttpStreamResponseBytes + 1;
    Check(client.Perform(invalid_stream, cancel).error == HttpError::kInvalidRequest, "a stream's size has a cap");
    invalid_stream = missing;
    invalid_stream.depot_manifest = true;
    Check(client.Perform(invalid_stream, cancel).error == HttpError::kInvalidRequest,
          "the manifest opt-in does not apply to a stream");
    invalid_stream = missing;
    invalid_stream.timeout_ms = playden::steam::kMaxHttpStreamTimeoutMs + 1;
    Check(client.Perform(invalid_stream, cancel).error == HttpError::kInvalidRequest, "a stream's deadline has a cap");
  }

  HttpRequest invalid_get_body = get;
  invalid_get_body.body = {'x'};
  response = client.Perform(invalid_get_body, cancel);
  Check(response.error == HttpError::kInvalidRequest, "GET body should be rejected explicitly");

  HttpClient untrusted(wrong_ca_bundle);
  response = untrusted.Perform(get, cancel);
  Check(response.error == HttpError::kTlsVerificationFailed, "wrong CA should fail verification");
  // 60 is CURLE_PEER_FAILED_VERIFICATION.
  Check(response.transport_code == 60 &&
            playden::steam::HttpFailureName(response) == "tls_verification_failed curl=60",
        "a transport failure should keep curl's code for diagnostics");
  {
    // The console's resolver records the address; a failure names it, a success doesn't.
    playden::steam::HttpResponse blocked;
    blocked.error = HttpError::kConnectionFailed;
    blocked.transport_code = 7;
    blocked.address = "0.0.0.0";
    Check(playden::steam::HttpFailureName(blocked) == "connection_failed curl=7 addr=0.0.0.0",
          "a failure should name the server address when it is known");
    blocked.error = HttpError::kNone;
    blocked.transport_code = 0;
    Check(playden::steam::HttpFailureName(blocked) == "none", "a success names no address");
  }

  HttpRequest invalid_header = get;
  invalid_header.headers.emplace_back("Bad\r\nInjected", "value");
  response = client.Perform(invalid_header, cancel);
  Check(response.error == HttpError::kInvalidRequest, "header injection should be rejected");
  invalid_header = get;
  invalid_header.headers.emplace_back(std::string("Bad\0Name", 8), "value");
  response = client.Perform(invalid_header, cancel);
  Check(response.error == HttpError::kInvalidRequest,
        "embedded NUL in header name should be rejected");
  Check(std::string(playden::steam::HttpErrorName(HttpError::kCanceled)) == "canceled",
        "error names should be stable and safe for diagnostics");

  {
    std::atomic<int> ready{0};
    std::atomic<bool> begin{false};
    std::vector<std::thread> workers;
    for (int index = 0; index < 8; ++index) workers.emplace_back([&] {
      HttpClient independent(ca_bundle);
      ++ready;
      while (!begin.load()) std::this_thread::yield();
      HttpRequest request;
      request.url = base_url + "/keep/ok";
      for (int iteration = 0; iteration < 10; ++iteration) {
        const auto response = independent.Perform(request, cancel);
        Check(response.ok() && Text(response.body) == "fixture-ok", "parallel verified HTTPS clients");
      }
    });
    while (ready.load() != 8) std::this_thread::yield();
    begin.store(true);
    for (auto& worker : workers) worker.join();
  }

  return 0;
}
