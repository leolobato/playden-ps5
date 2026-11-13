// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/http.h"

#include <memory>
#include <utility>

namespace playden::gog {

Transport ClientTransport(std::string ca_bundle) {
  // std::function must be copyable, so the client is shared; it still belongs to one worker.
  auto client = std::make_shared<steam::HttpClient>(std::move(ca_bundle));
  return [client](const steam::HttpRequest& request, const std::atomic<bool>& cancel) {
    return client->Perform(request, cancel);
  };
}

}  // namespace playden::gog
