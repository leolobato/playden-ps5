// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/address.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace playden::web {

std::string LocalAddress() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return {};
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(53);
  inet_pton(AF_INET, "1.1.1.1", &target.sin_addr);
  sockaddr_in own{};
  socklen_t size = sizeof own;
  std::string address;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&target), sizeof target) == 0 &&
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&own), &size) == 0 && own.sin_family == AF_INET) {
    const std::uint32_t host = ntohl(own.sin_addr.s_addr);
    char text[INET_ADDRSTRLEN] = {};
    // 0.0.0.0, loopback and link-local are not addresses a phone on the network can reach.
    if (host != 0 && (host >> 24) != 127 && (host >> 16) != 0xa9fe && inet_ntop(AF_INET, &own.sin_addr, text, sizeof text))
      address = text;
  }
  ::close(fd);
  return address;
}

std::string PageUrl(const std::string& address, std::uint16_t port) {
  if (address.empty()) return {};
  return "http://" + address + ":" + std::to_string(port) + "/";
}

}  // namespace playden::web
