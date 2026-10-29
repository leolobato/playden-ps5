#pragma once
// version.h - Playden versions: "0.2.0", tags "v0.2.0", param.json "00.002.000" and nightly builds.
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
  // A nightly test build (YYYYMMDD and that day's build number), never a release. A version's nightlies
  // come before its betas and its stable release, so an update to either is offered.
  unsigned nightly_date = 0;
  unsigned nightly_build = 0;

  bool nightly() const { return nightly_date != 0; }
  std::string ToString() const;  // "0.2.0", "0.7.1-beta.1", "0.8.0-nightly.20261027.1"
  friend bool operator==(const Version& a, const Version& b) {
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch && a.beta == b.beta &&
           a.nightly_date == b.nightly_date && a.nightly_build == b.nightly_build;
  }
  friend bool operator!=(const Version& a, const Version& b) { return !(a == b); }
  friend bool operator<(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major;
    if (a.minor != b.minor) return a.minor < b.minor;
    if (a.patch != b.patch) return a.patch < b.patch;
    // Nightly, then beta, then stable.
    const auto rank = [](const Version& v) { return v.nightly() ? 0 : v.beta ? 1 : 2; };
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    if (a.nightly()) return a.nightly_date != b.nightly_date ? a.nightly_date < b.nightly_date : a.nightly_build < b.nightly_build;
    return a.beta < b.beta;
  }
};

// Three dot-separated numbers, with an optional leading "v". Each part is 1-6 digits (param.json
// pads them with zeros). Optional suffix: -beta.N, N is 1-999999 with no leading zero; or
// -nightly.YYYYMMDD.N, N is 1-999 with no leading zero.
std::optional<Version> ParseVersion(std::string_view text);

// The version this build was made from (CMake's PROJECT_VERSION and pre-release suffix).
Version CurrentVersion();

// The commit this build was made from (10 hex digits), or empty when the source was not a git checkout.
std::string BuildCommit();

}  // namespace playden::update
