// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace playden::ps5 {

// The console's IPv4 address from the network control service (sceNetCtlGetInfo), or empty.
// diagnostic: the calls' results, for web.log.
std::string ConsoleAddress(std::string* diagnostic = nullptr);

}  // namespace playden::ps5
