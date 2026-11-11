// experimental_features.cpp - Features a player turns on with a line in features.txt.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/experimental_features.h"

#include <cctype>
#include <cstdio>

namespace playden::app {

ExperimentalFeatures DefaultExperimentalFeatures(bool all_on) {
  ExperimentalFeatures features;
  features.community_recipes = all_on;
  return features;
}

ExperimentalFeatures ParseExperimentalFeatures(std::string_view text, ExperimentalFeatures defaults,
                                               std::vector<std::string>* unknown) {
  ExperimentalFeatures features = defaults;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
    if (line.empty() || line.front() == '#') continue;
    if (line == "community-recipes") features.community_recipes = true;
    else if (line == "online-steam") features.online_steam = true;
    else if (unknown) unknown->emplace_back(line);
  }
  return features;
}

ExperimentalFeatures ReadExperimentalFeatures(const std::string& path, ExperimentalFeatures defaults,
                                              std::vector<std::string>* unknown) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return defaults;
  std::string text;
  char chunk[1024];
  for (std::size_t count; text.size() < kExperimentalFeaturesMaxBytes && (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;)
    text.append(chunk, count);
  std::fclose(file);
  if (text.size() > kExperimentalFeaturesMaxBytes) text.resize(kExperimentalFeaturesMaxBytes);
  return ParseExperimentalFeatures(text, defaults, unknown);
}

}  // namespace playden::app
