// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/router.h"

namespace playden::web {

void Router::Exact(std::string path, Handler handler) { routes_.push_back({std::move(path), false, false, std::move(handler)}); }

void Router::Prefix(std::string prefix, Handler handler) { routes_.push_back({std::move(prefix), true, false, std::move(handler)}); }

void Router::Post(std::string path, Handler handler) { routes_.push_back({std::move(path), false, true, std::move(handler)}); }

void SendError(Response& response, int status, std::string_view message) {
  std::string body(message);
  body += "\n";
  response.Send(status, "text/plain; charset=utf-8", body, {{"Cache-Control", "no-store"}});
}

void Router::Handle(const Request& request, Response& response) const {
  for (const auto& route : routes_) {
    if (!route.post || request.path != route.path) continue;
    if (request.method == Method::kPost) return route.handler(request, {}, response);
    response.Send(405, "text/plain; charset=utf-8", "Only POST is allowed.\n", {{"Allow", "POST"}, {"Cache-Control", "no-store"}});
    return;
  }
  if (request.method != Method::kGet && request.method != Method::kHead) {
    response.Send(405, "text/plain; charset=utf-8", "Only GET and HEAD are allowed.\n",
                  {{"Allow", "GET, HEAD"}, {"Cache-Control", "no-store"}});
    return;
  }
  for (const auto& route : routes_) {
    if (route.post) continue;
    if (!route.prefix && request.path == route.path) {
      route.handler(request, {}, response);
      return;
    }
    if (route.prefix && request.path.size() > route.path.size() && request.path.compare(0, route.path.size(), route.path) == 0) {
      route.handler(request, request.path.substr(route.path.size()), response);
      return;
    }
  }
  SendError(response, 404, "Not found.");
}

}  // namespace playden::web
