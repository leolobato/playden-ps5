// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/router.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

using namespace playden::web;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
struct Answer {
  std::string bytes;
  int status = 0;
};

Answer Ask(const Router& router, const std::string& head) {
  Request request;
  Answer answer;
  if (ParseRequestHead(head + "\r\n\r\n", &request) != ParseResult::kOk) return answer;
  Response response([&answer](const char* data, std::size_t size) { answer.bytes.append(data, size); return true; },
                    request.method == Method::kHead);
  router.Handle(request, response);
  answer.status = response.status();
  return answer;
}

std::string Body(const Answer& answer) {
  const auto end = answer.bytes.find("\r\n\r\n");
  return end == std::string::npos ? std::string() : answer.bytes.substr(end + 4);
}
}  // namespace

int main() {
  // The log route as the web layer registers it: the name must be one the index lists.
  const std::set<std::string> index = {"runtime.log", "title-FAKE78012.log"};
  Router router;
  router.Exact("/", [](const Request&, const std::string&, Response& response) { response.Send(200, "text/html", "page"); });
  router.Exact("/api/logs", [](const Request&, const std::string&, Response& response) { response.Send(200, "application/json", "{}"); });
  router.Prefix("/api/logs/", [&index](const Request&, const std::string& name, Response& response) {
    if (!index.count(name)) return SendError(response, 404, "No such log.");
    response.Send(200, "text/plain; charset=utf-8", name);
  });

  CHECK(Ask(router, "GET / HTTP/1.1").status == 200);
  CHECK(Body(Ask(router, "GET / HTTP/1.1")) == "page");
  CHECK(Ask(router, "GET /index.html HTTP/1.1").status == 404);
  CHECK(Ask(router, "GET /api/logs HTTP/1.1").status == 200);
  CHECK(Ask(router, "GET /api/logs/ HTTP/1.1").status == 404);
  CHECK(Body(Ask(router, "GET /api/logs/runtime.log HTTP/1.1")) == "runtime.log");
  CHECK(Ask(router, "GET /api/logs/title-FAKE78012.log HTTP/1.1").status == 200);
  // HEAD answers like GET without the body.
  const auto head = Ask(router, "HEAD /api/logs/runtime.log HTTP/1.1");
  CHECK(head.status == 200 && Body(head).empty() && head.bytes.find("Content-Length: 11\r\n") != std::string::npos);

  // Names not in the index, paths and escapes: never a file outside it.
  CHECK(Ask(router, "GET /api/logs/missing.log HTTP/1.1").status == 404);
  CHECK(Ask(router, "GET /api/logs/../user/session.v1 HTTP/1.1").status == 404);
  CHECK(Ask(router, "GET /api/logs/..%2Fuser%2Fsession.v1 HTTP/1.1").status == 404);
  CHECK(Ask(router, "GET /api/logs/%2E%2E HTTP/1.1").status == 404);
  CHECK(Ask(router, "GET /api/logs//etc/passwd HTTP/1.1").status == 404);
  CHECK(Ask(router, "GET /api/logs/sub/runtime.log HTTP/1.1").status == 404);
  CHECK(Ask(router, "GET /API/LOGS HTTP/1.1").status == 404);

  // Only GET and HEAD.
  for (const char* method : {"POST", "PUT", "DELETE", "OPTIONS", "PATCH", "TRACE"}) {
    const auto answer = Ask(router, std::string(method) + " /api/logs HTTP/1.1");
    CHECK(answer.status == 405 && answer.bytes.find("Allow: GET, HEAD\r\n") != std::string::npos);
  }
  CHECK(Ask(router, "POST /nowhere HTTP/1.1").status == 405);

  // An action takes POST only; it is not reachable by GET, HEAD or a prefix.
  int actions = 0;
  router.Post("/api/quit", [&actions](const Request&, const std::string&, Response& response) {
    ++actions;
    response.Send(202, "application/json", "{}");
  });
  CHECK(Ask(router, "POST /api/quit HTTP/1.1").status == 202 && actions == 1);
  for (const char* method : {"GET", "HEAD", "PUT", "DELETE"}) {
    const auto answer = Ask(router, std::string(method) + " /api/quit HTTP/1.1");
    CHECK(answer.status == 405 && answer.bytes.find("Allow: POST\r\n") != std::string::npos);
  }
  CHECK(Ask(router, "POST /api/quit/now HTTP/1.1").status == 405 && Ask(router, "POST /api/logs HTTP/1.1").status == 405);
  CHECK(actions == 1);
  std::puts("web_router: ok");
}
