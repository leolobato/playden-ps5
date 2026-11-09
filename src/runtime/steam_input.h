// steam_input.h - A game's Steam Input action map, for the offline Steam emulator.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// One action set of a game's Steam Input map, as the offline Steam emulator reads it from
// steam_settings/controller/<name>.txt: one "action=BUTTON[,BUTTON][=mode]" line per action.
struct SteamInputActionSet {
  std::string name;
  std::string text;
};

// Converts a game's Steam Input controller configuration (a "controller_mappings" VDF, as the game ships it for the
// Xbox controller) to the emulator's action sets. Only the action sets' own presets: action layers and mode shifts
// stay out. Empty when the text is not such a configuration or binds nothing.
std::vector<SteamInputActionSet> ConvertSteamInputConfig(std::string_view controller_vdf);

// The configuration named by a Steam Input action manifest ("Action Manifest" VDF) for the controller the emulator
// stands in for, relative to the manifest's folder. Xbox first: its button names are the emulator's. Empty: none.
std::string SteamInputConfigPath(std::string_view manifest_vdf);

// Finds the game's Steam Input action manifest (steam_input_manifest.vdf, in any case) in its install and converts
// the configuration it names. Empty when the game has none: the emulator then reports no Steam Input controller.
std::vector<SteamInputActionSet> SteamInputActionSets(const std::string& install_root);

}  // namespace playden::runtime
