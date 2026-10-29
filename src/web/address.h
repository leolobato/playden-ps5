// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace playden::web {

// This device's address on the local network, as dotted IPv4 ("192.168.1.50"), or empty. A UDP
// socket is "connected" to a public address and asked for its own address: the system picks the
// interface it would route through, and no packet is sent.
std::string LocalAddress();

// The page's address for an IPv4 address and port: "http://192.168.1.50:7031/". Empty without an address.
std::string PageUrl(const std::string& address, std::uint16_t port);

}  // namespace playden::web
