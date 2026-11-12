// experimental_features.h - Features a player turns on with a line in features.txt.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playden::app {

// features.txt in Playden's data folder (/data/playden/features.txt on the console, which FTP and the file manager
// can write): one feature name per line; blank lines and lines starting with # are skipped. Read once at startup.
// A line can only turn a feature on: a feature on by default for the build stays on.
inline constexpr const char* kExperimentalFeaturesFileName = "features.txt";
// The longest file read; the rest is ignored.
inline constexpr std::size_t kExperimentalFeaturesMaxBytes = 16384;

struct ExperimentalFeatures {
  // Line community-recipes: community recipes, sharing, votes, test reports and the Playden account
  // (docs/community-recipes.md). My presets is not part of it. On in every build (DefaultExperimentalFeatures); the
  // line is still accepted.
  bool community_recipes = false;
  // Line online-steam: Steam features = Online (Steam's client beside the game), Steam sign-in and Steam network
  // (runtime::SetOnlineSteamEnabled). Off in every build unless the file names it.
  bool online_steam = false;
};

// What a build has on before features.txt: community recipes in every build. all_on (nightlies and local dev builds)
// turns on no other feature today: Online stays off in every build.
ExperimentalFeatures DefaultExperimentalFeatures(bool all_on);
// defaults with the features text names turned on. unknown: each line that names no feature, as written.
ExperimentalFeatures ParseExperimentalFeatures(std::string_view text, ExperimentalFeatures defaults = {},
                                               std::vector<std::string>* unknown = nullptr);
// The same for the file at path; defaults when it is missing or can't be read.
ExperimentalFeatures ReadExperimentalFeatures(const std::string& path, ExperimentalFeatures defaults = {},
                                              std::vector<std::string>* unknown = nullptr);

}  // namespace playden::app
