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
// Also empty when a stick or trigger action of an action set can't be converted (a mouse mode, for example): a game
// that reads the controller without Steam Input too (Euro Truck Simulator 2) stops seeing it once Steam Input reports
// one, and would lose that stick. `lost_actions`, when set, gets those actions as "<set>/<action>".
std::vector<SteamInputActionSet> ConvertSteamInputConfig(std::string_view controller_vdf,
                                                         std::vector<std::string>* lost_actions = nullptr);

// The configuration named by a Steam Input action manifest ("Action Manifest" VDF) for the controller the emulator
// stands in for, relative to the manifest's folder. Xbox first: its button names are the emulator's. Empty: none.
std::string SteamInputConfigPath(std::string_view manifest_vdf);

// Finds the game's Steam Input action manifest (steam_input_manifest.vdf, in any case) in its install and converts
// the configuration it names. Empty when the game has none or the map loses a stick or trigger: the emulator then
// reports no Steam Input controller.
std::vector<SteamInputActionSet> SteamInputActionSets(const std::string& install_root,
                                                      std::vector<std::string>* lost_actions = nullptr);

}  // namespace playden::runtime
