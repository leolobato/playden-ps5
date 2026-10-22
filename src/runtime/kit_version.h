#pragma once
// kit_version.h - Runtime kit versions ("0.12", "0.12-dev.20261022.1") and their order.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <optional>
#include <string>
#include <string_view>

namespace playden::runtime {

// A kit version is <generation>.<revision>: 0.9 < 0.10 < 0.12 < 1.0. A development kit
// (<generation>.<revision>-dev.YYYYMMDD.N) leads to that release and counts as it, so it compares
// equal to it. Kits before 0.10 (kit H r1-r9) have no such version.
struct KitVersion {
  unsigned generation = 0;
  unsigned revision = 0;
};

inline bool operator==(KitVersion left, KitVersion right) {
  return left.generation == right.generation && left.revision == right.revision;
}
inline bool operator!=(KitVersion left, KitVersion right) { return !(left == right); }
inline bool operator<(KitVersion left, KitVersion right) {
  return left.generation != right.generation ? left.generation < right.generation : left.revision < right.revision;
}
inline bool operator>(KitVersion left, KitVersion right) { return right < left; }
inline bool operator<=(KitVersion left, KitVersion right) { return !(right < left); }
inline bool operator>=(KitVersion left, KitVersion right) { return !(left < right); }

// "0.12" or "0.12-dev.20261022.1": decimal numbers without a sign or leading zero (at most 4
// digits each), and for a dev version an 8-digit date and a build number from 1. Anything else
// ("kit-h", "r9", "0.12.1", " 0.12") is nothing.
std::optional<KitVersion> ParseKitVersion(std::string_view text);

// "0.12": the release a version counts as.
std::string FormatKitVersion(KitVersion version);

// The version in a kit manifest's runtime.artifact_revision, "<wine revision>-kit-<version>"
// ("f729534-kit-0.12", "f729534-kit-0.12-dev.20261019.1"); nothing for older kits
// ("bceed2d-kit-h-r1"). The whole version text is returned so a dev kit keeps its name.
std::optional<std::string> KitVersionTextFromArtifactRevision(std::string_view artifact_revision);
std::optional<KitVersion> KitVersionFromArtifactRevision(std::string_view artifact_revision);

}  // namespace playden::runtime
