#pragma once
// system_keyboard.h - The PS5's own keyboard (the IME dialog) for Playden's text fields.
// Adapted from ProsperoStore's platform/ps5/ime (BlackBearReloaded, GPL-3.0-or-later).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace playden::ps5 {
// Remote Play's keyboard, a USB keyboard and the controller all type into the system keyboard.
// It owns input while open: the app passes nothing on until Poll() stops returning kOpen.
// Open it once the button that asked for it is released, so that press cannot reach the dialog.
class SystemKeyboard {
 public:
  enum class State { kIdle, kOpen, kAccepted, kCancelled, kFailed };
  SystemKeyboard() = default;
  SystemKeyboard(const SystemKeyboard&) = delete;
  SystemKeyboard& operator=(const SystemKeyboard&) = delete;
  ~SystemKeyboard();
  // Loads the dialog's module. Call it first thing in main: once the title's credentials change
  // (the /data grant), loading system modules fails with 0x80020063. Returns the first failure, or 0.
  static int Preload();
  // secret: the dialog hides the text (a password). On failure, error names the refused step and
  // its result code, for the log; Playden's own keyboard is used instead.
  bool Open(std::string_view title, std::string_view value, bool secret, std::string* error);
  State Poll();
  void Close();
  // The accepted text; Take() hands it over and wipes Playden's copies.
  std::string Take();
  bool active() const { return active_; }

 private:
  void Wipe();
  std::array<std::uint16_t, 2049> buffer_{};
  std::array<std::uint16_t, 64> title_{};
  std::string text_;
  std::int64_t started_us_ = 0;
  bool active_ = false;
};
}  // namespace playden::ps5
