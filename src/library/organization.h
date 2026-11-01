#pragma once

#include "library/model.h"
#include "platform/storage.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace playden {

enum class CompatibilityRating : std::uint8_t {
  kUntested = 0,
  kWorks = 1,
  kPlayable = 2,
  kBroken = 3,
};

struct OrganizationCollection {
  std::string id;
  std::string name;
  bool pinned = false;
  std::set<GameId> members;
};

// The one organization scope since version 2: organization belongs to the device, not to a
// store account. Version 1 files were per Steam account and are only read, to migrate.
constexpr std::uint64_t kDeviceOrganization = 1;

struct OrganizationSnapshot {
  // 2: device-wide, any store's games. 1: a Steam account's `.v1` file, Steam games only.
  std::uint16_t schema_version = 2;
  std::set<GameId> favorites;
  std::set<GameId> hidden;
  std::vector<OrganizationCollection> collections;
  std::map<GameId, CompatibilityRating> ratings;
  std::map<GameId, std::string> notes;
  std::string query;
  std::string scope = "all";
  std::string sort = "name";
  std::set<std::string> filters;
  // Active Steam user-tag filter (display spelling); empty means no tag filter.
  std::string tag_filter;
  GameId selected_game_id;
};

bool operator==(const OrganizationCollection& left,
                const OrganizationCollection& right);
bool operator==(const OrganizationSnapshot& left,
                const OrganizationSnapshot& right);

// Pure validation shared by persistence and presentation, including the version 2 encoded size.
bool OrganizationSnapshotIsValid(const OrganizationSnapshot& snapshot);

// `scope` is kDeviceOrganization; the persistence pipeline still carries it as a scope number.
class OrganizationRepository {
 public:
  virtual ~OrganizationRepository() = default;
  virtual Result<std::optional<OrganizationSnapshot>> Load(
      std::uint64_t account_id) const = 0;
  virtual Result<void> Save(std::uint64_t account_id,
                            const OrganizationSnapshot& snapshot) const = 0;
  virtual Result<void> Clear(std::uint64_t account_id) const = 0;
};

// `organization.v2` in the storage root. Without it, Load merges every `organization-<id>.v1`
// (favorites, hidden and collections by name united; ratings and notes per game from the most
// recently modified file that has one; selection and view from the newest file), writes the
// result to `organization.v2` and returns it. `.v1` files are never modified or removed.
// A damaged `organization.v2` fails the load (kInvalidData); it never falls back to the merge.
class OrganizationStore final : public OrganizationRepository {
 public:
  explicit OrganizationStore(Storage& storage) : storage_(storage) {}
  Result<std::optional<OrganizationSnapshot>> Load(std::uint64_t scope) const override;
  Result<void> Save(std::uint64_t scope, const OrganizationSnapshot& snapshot) const override;
  // Removes `organization.v2` only; the next load merges the `.v1` files again.
  Result<void> Clear(std::uint64_t scope) const override;

  // Version 1, one Steam account's file. Playden only reads it (the merge); the writer exists
  // for tests and fixtures.
  Result<std::optional<OrganizationSnapshot>> LoadV1(std::uint64_t account_id) const;
  Result<void> SaveV1(std::uint64_t account_id, const OrganizationSnapshot& snapshot) const;

 private:
  Storage& storage_;
};

}  // namespace playden
