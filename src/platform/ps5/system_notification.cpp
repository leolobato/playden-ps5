// system_notification.cpp - The PS5 system notification.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "platform/ps5/system_notification.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#if defined(__PROSPERO__)
extern "C" int sceKernelDlsym(int, const char*, void**);
#endif

namespace playden::ps5 {

std::string TruncateUtf8(std::string_view text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) return std::string(text);
  std::size_t end = max_bytes;
  // Back over continuation bytes to the start of the sequence the cut falls in, which goes too.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return std::string(text.substr(0, end));
}

bool PostSystemNotification(std::string_view text) {
#if defined(__PROSPERO__)
  struct Request {
    std::uint8_t reserved[45];
    char message[kSystemNotificationMaxBytes + 1];
  };
  using Send = int (*)(int, void*, std::size_t, int);
  // libkernel's own export, resolved once; a missing one leaves the bubble out quietly.
  static const Send send = [] {
    Send found = nullptr;
    if (sceKernelDlsym(0x2001, "sceKernelSendNotificationRequest", reinterpret_cast<void**>(&found)) != 0) found = nullptr;
    return found;
  }();
  if (!send) return false;
  static std::mutex mutex;
  static Request request;  // 3 KB: kept off the caller's (small) stack
  static_assert(sizeof(request.message) == 3075);
  std::lock_guard<std::mutex> lock(mutex);
  std::memset(&request, 0, sizeof request);
  const auto message = TruncateUtf8(text, kSystemNotificationMaxBytes);
  std::memcpy(request.message, message.data(), message.size());
  return send(0, &request, sizeof request, 0) == 0;
#else
  (void)text;
  return false;
#endif
}

}  // namespace playden::ps5
