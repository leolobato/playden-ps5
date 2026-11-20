// publisher_refresh_fetch.hpp - HTTPS transport for a publisher pack's sign-in refresh.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime/publisher_account.h"
#include "steam/backend/http_client.h"
#include "steam/secret.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace playden::homebrew {

// The client is made on first use on the runtime worker, its only caller. The request and answer hold the
// sign-in's secrets: HttpClient never logs them, and the copies made here are wiped.
inline runtime::PublisherRefreshTransport PublisherRefreshFetch(std::string ca_bundle) {
    auto client = std::make_shared<std::unique_ptr<steam::HttpClient>>();
    return [client, ca_bundle = std::move(ca_bundle)](const runtime::PublisherRefreshRequest& refresh) {
        if (!*client) *client = std::make_unique<steam::HttpClient>(ca_bundle);
        steam::HttpRequest request;
        request.url = refresh.url;
        request.headers = refresh.headers;
        request.timeout_ms = 15'000;
        request.connect_timeout_ms = 8'000;
        request.max_response_bytes = 256 * 1024;
        const std::atomic<bool> cancel{false};
        auto response = (*client)->Perform(request, cancel);
        for (auto& [name, value] : request.headers) steam::ZeroizeSecret(value);
        runtime::PublisherRefreshAnswer answer;
        answer.reached = response.ok();
        answer.status = response.status_code;
        answer.headers = std::move(response.headers);
        answer.body.assign(response.body.begin(), response.body.end());
        std::fill(response.body.begin(), response.body.end(), std::uint8_t{0});
        return answer;
    };
}

}  // namespace playden::homebrew
