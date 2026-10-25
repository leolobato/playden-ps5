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

std::string Header(const HttpResponse& response, const std::string& name) {
  for (const auto& header : response.headers)
    if (header.first == name) return header.second;
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  Check(argc == 5, "expected BASE_URL CA_BUNDLE WRONG_CA_BUNDLE PLAIN_URL");
  const std::string base_url = argv[1];
  const std::string ca_bundle = argv[2];
  const std::string wrong_ca_bundle = argv[3];
  const std::string plain_url = argv[4];
  std::atomic<bool> cancel{false};
  HttpClient client(ca_bundle);


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

  HttpRequest invalid_get_body = get;
  invalid_get_body.body = {'x'};
  response = client.Perform(invalid_get_body, cancel);
  Check(response.error == HttpError::kInvalidRequest, "GET body should be rejected explicitly");

  HttpClient untrusted(wrong_ca_bundle);
  response = untrusted.Perform(get, cancel);
  Check(response.error == HttpError::kTlsVerificationFailed, "wrong CA should fail verification");

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
