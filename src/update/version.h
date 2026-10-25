#pragma once
// version.h - Playden release versions: "0.2.0", tags "v0.2.0" and param.json "00.002.000".
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include <optional>
#include <string>
#include <string_view>

namespace playden::update {

struct Version {
  unsigned major = 0;
  unsigned minor = 0;
  unsigned patch = 0;
  unsigned beta = 0;  // 0 is stable, newer than every beta of the same version.

  std::string ToString() const;  // "0.2.0"
  friend bool operator==(const Version& a, const Version& b) {
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch && a.beta == b.beta;
  }
  friend bool operator!=(const Version& a, const Version& b) { return !(a == b); }
  friend bool operator<(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major;
    if (a.minor != b.minor) return a.minor < b.minor;
    if (a.patch != b.patch) return a.patch < b.patch;
    if (a.beta == b.beta) return false;
    return a.beta != 0 && (b.beta == 0 || a.beta < b.beta);
  }
};

// Three dot-separated numbers, with an optional leading "v". Each part is 1-6 digits (param.json
// pads them with zeros). Optional suffix: -beta.N, N is 1-999999 with no leading zero.
std::optional<Version> ParseVersion(std::string_view text);

// The version this build was made from (CMake's PROJECT_VERSION).
Version CurrentVersion();

}  // namespace playden::update
