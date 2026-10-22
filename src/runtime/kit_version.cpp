// kit_version.cpp - Runtime kit versions and their order.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/kit_version.h"

namespace playden::runtime {
namespace {

bool Digits(std::string_view text) {
  if (text.empty()) return false;
  for (const char c : text)
    if (c < '0' || c > '9') return false;
  return true;
}

// A decimal number without a sign or leading zero, at most max_digits long.
bool Number(std::string_view text, std::size_t max_digits, unsigned* out) {
  if (!Digits(text) || text.size() > max_digits || (text.size() > 1 && text.front() == '0')) return false;
  unsigned value = 0;
  for (const char c : text) value = value * 10 + static_cast<unsigned>(c - '0');
  *out = value;
  return true;
}

}  // namespace

std::optional<KitVersion> ParseKitVersion(std::string_view text) {
  std::string_view release = text;
  const auto dev = text.find("-dev.");
  if (dev != std::string_view::npos) {
    release = text.substr(0, dev);
    const auto suffix = text.substr(dev + 5);
    const auto dot = suffix.find('.');
    if (dot != 8 || !Digits(suffix.substr(0, 8))) return std::nullopt;
    unsigned build = 0;
    if (!Number(suffix.substr(dot + 1), 4, &build) || build == 0) return std::nullopt;
  }
  const auto dot = release.find('.');
  if (dot == std::string_view::npos) return std::nullopt;
  KitVersion version;
  if (!Number(release.substr(0, dot), 4, &version.generation) ||
      !Number(release.substr(dot + 1), 4, &version.revision))
    return std::nullopt;
  return version;
}

std::string FormatKitVersion(KitVersion version) {
  return std::to_string(version.generation) + "." + std::to_string(version.revision);
}

std::optional<std::string> KitVersionTextFromArtifactRevision(std::string_view artifact_revision) {
  static constexpr std::string_view kMarker = "-kit-";
  const auto marker = artifact_revision.find(kMarker);
  if (marker == std::string_view::npos || marker == 0) return std::nullopt;
  for (const char c : artifact_revision.substr(0, marker))
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
  const auto version = artifact_revision.substr(marker + kMarker.size());
  if (!ParseKitVersion(version)) return std::nullopt;
  return std::string(version);
}

std::optional<KitVersion> KitVersionFromArtifactRevision(std::string_view artifact_revision) {
  const auto text = KitVersionTextFromArtifactRevision(artifact_revision);
  return text ? ParseKitVersion(*text) : std::nullopt;
}

}  // namespace playden::runtime
