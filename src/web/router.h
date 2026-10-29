// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web/http.h"

#include <functional>
#include <string>
#include <vector>

namespace playden::web {

// Routes GET and HEAD requests by exact path or by a "/prefix/", and POST only to the few exact paths
// registered with Post; every other method or path gets 405, and an unknown path 404. A prefix handler
// receives the rest of the path after the prefix.
class Router {
 public:
  using Handler = std::function<void(const Request& request, const std::string& rest, Response& response)>;
  void Exact(std::string path, Handler handler);
  void Prefix(std::string prefix, Handler handler);
  // An action (POST only): GET and HEAD on it get 405.
  void Post(std::string path, Handler handler);
  void Handle(const Request& request, Response& response) const;

 private:
  struct Route {
    std::string path;
    bool prefix = false;
    bool post = false;
    Handler handler;
  };
  std::vector<Route> routes_;
};

// A plain-text error answer, as every route sends it.
void SendError(Response& response, int status, std::string_view message);

}  // namespace playden::web
