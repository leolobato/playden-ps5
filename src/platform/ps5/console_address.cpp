// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/console_address.h"

#include <cstdio>
#include <vector>

extern "C" {
int sceNetCtlInit(void);
int sceNetCtlGetInfo(int code, void* info);
}

namespace playden::ps5 {

std::string ConsoleAddress(std::string* diagnostic) {
  constexpr int kInfoIpAddress = 14;  // SCE_NET_CTL_INFO_IP_ADDRESS: char[16] at the start of the info union
  // The union's largest member is well under this; on the heap, so a small worker stack is spared.
  std::vector<char> info(1024, '\0');
  const int init = sceNetCtlInit();
  const int result = sceNetCtlGetInfo(kInfoIpAddress, info.data());
  if (diagnostic) {
    char line[96];
    std::snprintf(line, sizeof line, "netctl init=0x%08x info=0x%08x", static_cast<unsigned>(init),
                  static_cast<unsigned>(result));
    *diagnostic = line;
  }
  if (result != 0) return {};
  info[15] = '\0';
  std::string address(info.data());
  // Only a dotted IPv4 address; anything else is unknown rather than wrong.
  int dots = 0;
  for (const char c : address) {
    if (c == '.') ++dots;
    else if (c < '0' || c > '9') return {};
  }
  if (dots != 3 || address == "0.0.0.0") return {};
  return address;
}

}  // namespace playden::ps5
