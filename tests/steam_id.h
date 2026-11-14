#pragma once
#include "library/store.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

// Tests name Steam games by their app ID.
inline playden::GameId SteamId(std::uint32_t app) { return playden::GameId::Steam(app); }
// Steam DLC by their app IDs, in the order given.
inline std::vector<playden::GameId> SteamIds(std::initializer_list<std::uint32_t> apps) {
  std::vector<playden::GameId> ids;
  for (const auto app : apps) ids.push_back(playden::GameId::Steam(app));
  return ids;
}
