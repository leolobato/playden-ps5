// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/http.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace playden::web;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

int main() {
  Request request;
  CHECK(ParseRequestHead("GET / HTTP/1.1\r\nHost: x\r\n", &request) == ParseResult::kIncomplete);
  CHECK(ParseRequestHead("GET /api/logs HTTP/1.1\r\nHost: 10.0.1.68:7031\r\nUser-Agent: curl\r\n\r\n", &request) ==
        ParseResult::kOk);
  CHECK(request.method == Method::kGet && request.method_text == "GET" && request.path == "/api/logs");
  CHECK(request.headers.size() == 2 && request.headers[0].first == "host" && request.headers[0].second == "10.0.1.68:7031");
  // A body after the head is ignored.
  CHECK(ParseRequestHead("HEAD /x?title=FAKE78012 HTTP/1.0\n\nbody", &request) == ParseResult::kOk);
  CHECK(request.method == Method::kHead && request.path == "/x" && request.query == "title=FAKE78012");
  CHECK(ParseRequestHead("POST / HTTP/1.1\r\n\r\n", &request) == ParseResult::kOk && request.method == Method::kPost);
  CHECK(ParseRequestHead("DELETE /api/logs/runtime.log HTTP/1.1\r\n\r\n", &request) == ParseResult::kOk &&
        request.method == Method::kOther && request.method_text == "DELETE");

  // Percent escapes decode in the path; a bad escape or a NUL is refused.
  CHECK(ParseRequestHead("GET /api/logs/title-FAKE78012%2Elog HTTP/1.1\r\n\r\n", &request) == ParseResult::kOk);
  CHECK(request.path == "/api/logs/title-FAKE78012.log");
  CHECK(ParseRequestHead("GET /a%2 HTTP/1.1\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("GET /a%zz HTTP/1.1\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("GET /a%00b HTTP/1.1\r\n\r\n", &request) == ParseResult::kBadRequest);

  // Bad request lines.
  CHECK(ParseRequestHead("GET\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("GET / HTTP/2\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("GET http://evil/ HTTP/1.1\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("GET * HTTP/1.1\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("GET  / HTTP/1.1\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("G(T / HTTP/1.1\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead(std::string("GET /\x7f HTTP/1.1\r\n\r\n"), &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("GET / HTTP/1.1\r\nNo colon\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead("GET / HTTP/1.1\r\n: empty\r\n\r\n", &request) == ParseResult::kBadRequest);
  CHECK(ParseRequestHead(std::string("GET / HTTP/1.1\r\nX: a\x01" "b\r\n\r\n"), &request) == ParseResult::kBadRequest);

  // The head limit: 8 KB, with or without the blank line seen.
  const std::string big_header = "X-Big: " + std::string(kMaxRequestHead, 'a');
  CHECK(ParseRequestHead("GET / HTTP/1.1\r\n" + big_header, &request) == ParseResult::kTooLarge);
  CHECK(ParseRequestHead("GET / HTTP/1.1\r\n" + big_header + "\r\n\r\n", &request) == ParseResult::kTooLarge);
  const std::string fits = "GET / HTTP/1.1\r\nX: " + std::string(kMaxRequestHead - 25, 'a') + "\r\n\r\n";
  CHECK(fits.size() <= kMaxRequestHead && ParseRequestHead(fits, &request) == ParseResult::kOk);

  // Query values.
  CHECK(QueryValue("title=FAKE78012", "title") == std::optional<std::string>("FAKE78012"));
  CHECK(QueryValue("a=1&title=all&b", "title") == std::optional<std::string>("all"));
  CHECK(QueryValue("a=1&b", "b") == std::optional<std::string>(""));
  CHECK(!QueryValue("a=1", "title"));
  CHECK(QueryValue("t%69tle=x+y%21", "title") == std::optional<std::string>("x y!"));
  CHECK(!QueryValue("title=%zz", "title"));

  // Responses: status line, headers, Connection: close; HEAD sends no body.
  std::string sent;
  const auto into = [&sent](const char* data, std::size_t size) { sent.append(data, size); return true; };
  Response get(into, false);
  CHECK(get.Send(200, "text/plain", "hi", {{"Cache-Control", "no-store"}}));
  CHECK(sent.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
  CHECK(sent.find("Content-Length: 2\r\n") != std::string::npos && sent.find("Cache-Control: no-store\r\n") != std::string::npos);
  CHECK(sent.find("Connection: close\r\n\r\nhi") != std::string::npos && get.body_bytes() == 2 && get.status() == 200);
  sent.clear();
  Response head(into, true);
  CHECK(head.Send(404, "text/plain", "missing"));
  CHECK(sent.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0 && sent.find("Content-Length: 7\r\n") != std::string::npos);
  CHECK(sent.size() >= 4 && sent.compare(sent.size() - 4, 4, "\r\n\r\n") == 0 && head.body_bytes() == 0);
  // A client gone mid-answer: the response says so and writes nothing more.
  Response gone([](const char*, std::size_t) { return false; }, false);
  CHECK(!gone.Start(200, "text/plain", std::nullopt) && gone.failed() && !gone.Body("x"));
  std::puts("web_request: ok");
}
