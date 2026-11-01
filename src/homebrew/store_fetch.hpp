// store_fetch.hpp - HTTPS GET for the Steam store details service.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "steam/backend/http_client.h"
#include "steam/store_details_service.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace playden::homebrew {

// The client is made on first use on the service's worker, which is its only caller.
// The default cap fits one appdetails page. Store browse passes kStoreBrowseResponseBytes.
inline steam::StoreDetailsService::Fetch StoreFetch(std::string ca_bundle,
                                                    std::size_t max_response_bytes = 2 * 1024 * 1024) {
    auto client = std::make_shared<std::unique_ptr<steam::HttpClient>>();
    return [client, ca_bundle = std::move(ca_bundle), max_response_bytes](const std::string& url,
                                                      const std::atomic<bool>& cancel) -> std::optional<std::string> {
        if (!*client) *client = std::make_unique<steam::HttpClient>(ca_bundle);
        steam::HttpRequest request;
        request.url = url;
        request.timeout_ms = 15'000;
        request.max_response_bytes = max_response_bytes;
        const auto response = (*client)->Perform(request, cancel);
        if (!response.ok() || response.status_code != 200) return std::nullopt;
        return std::string(response.body.begin(), response.body.end());
    };
}

}  // namespace playden::homebrew
