#pragma once
// title_remover.h - Playden's side of the one-shot title remover payload.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace playden::ps5 {
// What one run of the remover reported. sent: the payload reached the loader.
// code: its answer for this nonce (0 = the console accepted the uninstall).
struct TitleRemoverOutcome {
  bool sent = false;
  std::optional<int> code;
  std::string detail;
};

struct TitleRemoverConfig {
  // Exchange directory the payload reads (0755, owned by Playden; see
  // tools/title-remover/main.c) and the payload itself, shipped in Playden's title.
  std::filesystem::path request_directory = "/data/playden/title-remover";
  std::filesystem::path payload = "/data/homebrew/FAKE77031/title-remover.elf";
  // Sends the payload's bytes to the payload loader; false with a reason on failure.
  std::function<bool(const std::string& elf, std::string* error)> send;
  int result_polls = 60;
  std::chrono::milliseconds result_interval{250};
};

// Writes the request for title_id (FAKE78xxx) and nonce, sends the payload and
// waits (bounded) for its answer. Worker only. The answer only reports what the
// payload saw; the caller confirms removal in the console inventory.
TitleRemoverOutcome RunTitleRemover(const TitleRemoverConfig& config, const std::string& title_id,
                                    const std::string& nonce);

// Streams an ELF to the payload loader on 127.0.0.1:9021, as the web file manager does.
bool SendToPayloadLoader(const std::string& elf, std::string* error);
}  // namespace playden::ps5
