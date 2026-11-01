#pragma once
// kit_catalog.h - Which installed kit a pin names, and which parked copies to keep.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "update/release.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Preferences and per-game settings store this when the player chose Latest.
inline constexpr const char* kLatestKitPin = "latest";

inline std::string FormatKitVersion(const update::KitVersion& version) {
  std::string text = std::to_string(version.generation) + "." + std::to_string(version.revision);
  if (version.development)
    text += "-dev." + std::to_string(version.date) + "." + std::to_string(version.build);
  return text;
}

// A version the player saved ("0.17", "0.13-dev.20261023.1"). "latest" and empty are not one.
inline bool IsConcreteKitPin(std::string_view pin) { return update::ParseKitVersion(pin).has_value(); }

// What is written to preferences and game settings: "latest", or a version. Anything else is Latest.
inline std::string NormalizedKitPin(std::string_view pin) {
  if (pin == kLatestKitPin || IsConcreteKitPin(pin)) return std::string(pin);
  return kLatestKitPin;
}

// The folder a replaced kit is parked in, beside the Latest slot: "<kit_id>-<version>".
inline std::string ParkedKitFolder(std::string_view kit_id, std::string_view version) {
  return std::string(kit_id) + "-" + std::string(version);
}

// `name` is that folder (not the Latest slot, not a dot-folder). Writes the version when it is.
inline bool ParkedKitVersion(std::string_view kit_id, std::string_view name, std::string* version) {
  const std::string prefix = std::string(kit_id) + "-";
  if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return false;
  const auto parsed = name.substr(prefix.size());
  if (!IsConcreteKitPin(parsed)) return false;
  if (version) *version = std::string(parsed);
  return true;
}

enum class ResolvedKitKind {
  kUnavailable,  // nothing to run this game with
  kLatestSlot,   // the kit at <kits>/<kit_id>
  kVersion,      // a parked copy of this version
};

struct ResolvedKit {
  ResolvedKitKind kind = ResolvedKitKind::kUnavailable;
  std::string version;  // the version in use, when one was read
  bool fell_back = false;  // a saved version was not available, so Latest is in use
};

// `saved` is empty, "latest", or a version. `version_folders` are parked versions that admit.
// The Latest slot is used for a pin of its own version, so one copy of that version is enough.
inline ResolvedKit ResolveKitPin(std::string_view saved, bool latest_admits, std::string_view latest_version,
                                 const std::vector<std::string>& version_folders) {
  const auto parked = [&](std::string_view version) {
    for (const auto& folder : version_folders)
      if (folder == version) return true;
    return false;
  };
  if (IsConcreteKitPin(saved)) {
    if (latest_admits && latest_version == saved)
      return {ResolvedKitKind::kLatestSlot, std::string(latest_version), false};
    if (parked(saved)) return {ResolvedKitKind::kVersion, std::string(saved), false};
    if (latest_admits) return {ResolvedKitKind::kLatestSlot, std::string(latest_version), true};
    return {ResolvedKitKind::kUnavailable, {}, true};
  }
  if (latest_admits) return {ResolvedKitKind::kLatestSlot, std::string(latest_version), false};
  return {ResolvedKitKind::kUnavailable, {}, false};
}

// A game pin wins when it is set. Empty follows the default pin (itself empty or "latest" for the slot).
inline ResolvedKit ResolveGameKit(std::string_view game_pin, std::string_view default_pin, bool latest_admits,
                                  std::string_view latest_version, const std::vector<std::string>& version_folders) {
  return ResolveKitPin(game_pin.empty() ? default_pin : game_pin, latest_admits, latest_version, version_folders);
}

// Deletes parked copies that admit and that no pin names. The Latest slot, dot-folders, and a folder
// that does not admit are left alone. `admits` is the folder path. Returns the folders removed.
std::vector<std::string> PruneUnpinnedKits(const std::string& kits_root, std::string_view kit_id,
                                           const std::vector<std::string>& pinned,
                                           const std::function<bool(const std::string& path)>& admits);

}  // namespace playden::runtime
