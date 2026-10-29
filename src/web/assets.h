// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string_view>

namespace playden::web {

// The page and its fonts, baked into the binary at build time (cmake/EmbedWebAssets.cmake), so the
// page works with no internet. Served only from this table.
struct EmbeddedAsset {
  const char* path;  // "/", "/assets/barlow-regular.ttf", ...
  const char* content_type;
  const unsigned char* data;
  std::size_t size;
};

extern const EmbeddedAsset kEmbeddedAssets[];
extern const std::size_t kEmbeddedAssetCount;

inline const EmbeddedAsset* FindEmbeddedAsset(std::string_view path) {
  for (std::size_t i = 0; i < kEmbeddedAssetCount; ++i)
    if (path == kEmbeddedAssets[i].path) return &kEmbeddedAssets[i];
  return nullptr;
}

}  // namespace playden::web
