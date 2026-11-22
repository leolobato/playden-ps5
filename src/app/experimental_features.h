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

// A line that names a retired feature (community-recipes, part of every build since 0.11.0-beta.4) is accepted and
// ignored: it turns nothing on and is not reported as unknown.
struct ExperimentalFeatures {
  // Line online-steam: Steam features = Online (Steam's client beside the game), Steam sign-in and Steam network
  // (runtime::SetOnlineSteamEnabled). On by default in every build since 0.12.0-beta.1; the line is still accepted.
  bool online_steam = false;
  // Line gog-store: GOG as a second store (sign-in, library, installs). On by default in every build since
  // 0.12.0-beta.1; the line is still accepted. Going back to a build older than the first release that reads GOG
  // records (Phase 1 of the GOG plan) breaks Play for every game until the GOG titles are removed: that build refuses
  // the whole runtime install records file when one record names GOG.
  bool gog_store = false;
  // Line publisher-pack-download: Playden downloads and installs the Rockstar Games pack (runtime/publisher_pack_install)
  // from builds.playden.club when the pinned one is not in place. On by default in every build since 0.12.0-beta.4;
  // the line is still accepted.
  bool publisher_pack_download = false;
  // Line nightly-updates: with Beta updates on, Playden also offers the nightly builds listed on builds.playden.club
  // (update::UpdateConfig::nightly_site), whichever of stable, beta and nightly is newest, and a nightly downloads its
  // own runtime kit from there. Off in every build until the line turns it on.
  bool nightly_updates = false;
};

// What a build has on before features.txt. Online, GOG and the publisher pack download are on in every build; all_on
// (nightlies and local dev builds) turns on nothing more today.
ExperimentalFeatures DefaultExperimentalFeatures(bool all_on);
// defaults with the features text names turned on. unknown: each line that names no feature, as written.
ExperimentalFeatures ParseExperimentalFeatures(std::string_view text, ExperimentalFeatures defaults = {},
                                               std::vector<std::string>* unknown = nullptr);
// The same for the file at path; defaults when it is missing or can't be read.
ExperimentalFeatures ReadExperimentalFeatures(const std::string& path, ExperimentalFeatures defaults = {},
                                              std::vector<std::string>* unknown = nullptr);

}  // namespace playden::app
