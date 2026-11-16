#pragma once
// system_notification.h - The PS5 system notification (the bubble over any screen, Home included).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include <cstddef>
#include <string>
#include <string_view>

namespace playden::ps5 {
// The longest message the notification request holds, in bytes (its buffer less the terminator).
inline constexpr std::size_t kSystemNotificationMaxBytes = 3074;

// text cut to at most max_bytes without splitting a UTF-8 sequence.
std::string TruncateUtf8(std::string_view text, std::size_t max_bytes);

// Shows text as a PS5 system notification. Any thread; it does not wait for the bubble. The kernel
// call is looked up at run time: false when it is unavailable (always on the host) or refused.
bool PostSystemNotification(std::string_view text);
}  // namespace playden::ps5
