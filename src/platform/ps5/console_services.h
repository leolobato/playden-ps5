#pragma once
// console_services.h - Small PS5 console adapters for the runtime host.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <optional>
#include <string>

namespace playden::ps5 {
// Running kernel firmware version as "MM.mm" (for example "13.60"), or nothing.
std::optional<std::string> ConsoleFirmwareVersion();
// Raw values from the firmware sources, for diagnostics.
std::string ConsoleFirmwareDiagnostics();
// POSTs JSON to the console-local ShadowMountPlus API (127.0.0.1:10101). Returns
// the response body, including error bodies, or nothing on transport failure.
// Worker-only: it blocks for at most five seconds.
std::optional<std::string> ShadowMountPost(const std::string& route, const std::string& body);

// The console's processor as Windows' CentralProcessor keys describe it: the time-stamp counter's rate
// (machdep.tsc_freq) in MHz, the CPUs this process may run on (counted as the kit's Wine counts them), and CPUID's
// vendor, family/model/stepping and brand string. Nothing when the rate can't be read.
struct ConsoleProcessor {
  std::uint32_t tsc_mhz = 0;
  unsigned count = 0;
  std::string vendor;
  std::string identifier;
  std::string name;
};
std::optional<ConsoleProcessor> ConsoleProcessorInfo();
}  // namespace playden::ps5
