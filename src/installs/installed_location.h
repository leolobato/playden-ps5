#pragma once

#include "installs/install_service.h"
#include "library/store.h"

#include <algorithm>
#include <string>
#include <vector>

namespace playden::installs {

// Where an installed game's files are, for preparing and playing it. Managed installs are Playden's
// own (the install service downloaded or copied them, and uninstalling deletes them); external ones
// are folders a source found and plays in place (an imported game on a USB drive): Playden never
// deletes them, so they never reach a managed uninstall or the Installed games list.
struct InstalledLocation {
  enum class Ownership { kManaged, kExternal };
  GameId game;
  std::string root;
  std::string volume_id;
  bool available = true;  // the folder is there now
  Ownership ownership = Ownership::kManaged;
  // Why an available drive's folder still can't be played from (a name the PS5 can't open); empty otherwise.
  std::string reason;
  // Another USB drive that holds the same folder too; empty when none.
  std::string also_on;

  bool managed() const { return ownership == Ownership::kManaged; }
};

inline bool operator==(const InstalledLocation& left, const InstalledLocation& right) {
  return left.game == right.game && left.root == right.root && left.volume_id == right.volume_id &&
         left.available == right.available && left.ownership == right.ownership && left.reason == right.reason &&
         left.also_on == right.also_on;
}
inline bool operator!=(const InstalledLocation& left, const InstalledLocation& right) { return !(left == right); }

// One location per game, ordered by game: the install service's games, then external ones for
// games it doesn't manage (a managed install of the same game wins).
inline std::vector<InstalledLocation> ProjectInstalledLocations(const std::vector<InstalledGame>& managed,
                                                                const std::vector<InstalledLocation>& external) {
  std::vector<InstalledLocation> locations;
  for (const auto& game : managed)
    locations.push_back({game.id, game.root, game.volume_id, game.available, InstalledLocation::Ownership::kManaged, {}, game.also_on});
  for (const auto& location : external) {
    if (!location.game.valid() || location.root.empty()) continue;
    if (std::any_of(locations.begin(), locations.end(), [&](const InstalledLocation& known) { return known.game == location.game; }))
      continue;
    auto added = location;
    added.ownership = InstalledLocation::Ownership::kExternal;
    locations.push_back(std::move(added));
  }
  std::stable_sort(locations.begin(), locations.end(),
                   [](const InstalledLocation& left, const InstalledLocation& right) { return left.game < right.game; });
  return locations;
}

}  // namespace playden::installs
