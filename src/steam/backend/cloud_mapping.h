#pragma once

#include "steam/backend/app_info.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::steam::cloud {

// Where a save lives: inside the installed game, or inside the game's Wine prefix (the folder
// holding drive_c, laid out like a prospero-win prefix with the Windows user "prospero").
enum class SaveRoot { kGame, kPrefix };

inline constexpr const char* kPrefixUserFolder = "drive_c/users/prospero";
// The Steam emulator's per-account storage; bare Cloud names (no %Root%) live under <appid>/remote.
inline constexpr const char* kEmulatorUserData = "drive_c/Program Files (x86)/Steam/userdata/0";

struct SaveRule {
  SaveRoot root = SaveRoot::kPrefix;
  std::string directory;   // relative to the root, forward slashes, may be empty
  std::string pattern = "*";
  bool recursive = false;
  std::string cloud_prefix;  // "" for bare names, else "%Root%path" as Steam names files
};

struct SaveMapping {
  std::vector<SaveRule> rules;
  std::vector<std::string> unresolved;  // short reasons a location could not be mapped
  bool usable() const { return unresolved.empty() && !rules.empty(); }
};

// Builds the Windows save rules for an app. Account tokens ({64BitSteamID}, {Steam3AccountID}) in
// paths are expanded with steam_id; with steam_id 0 such locations are unresolved.
SaveMapping BuildSaveMapping(const CloudSaveInfo& info, std::uint32_t app_id, std::uint64_t steam_id);

// The folders of the app's Windows Cloud save locations that lie inside the Windows user folder (kPrefixUserFolder),
// relative to it ("AppData/LocalLow/Eternity/Leaf it Alone/Saves"): on Windows Steam makes them before a game starts,
// and some games watch theirs before they write any save. Locations in the game folder, ProgramData, the emulator's
// storage or the user folder itself are left out, and so are those that name the account (they need a signed-in
// account) or that BuildSaveMapping can't map. Each folder once, whatever its case, in mapping order.
std::vector<std::string> ProfileSaveFolders(const CloudSaveInfo& info, std::uint32_t app_id);

struct LocalSavePath {
  SaveRoot root = SaveRoot::kPrefix;
  std::string path;  // relative to the root
  // Case-insensitive identity, so "Save.dat" and "save.dat" are the same slot.
  std::string key() const;
};

// Case-insensitive glob with '*' and '?'.
bool GlobMatch(std::string_view pattern, std::string_view text);

// Bidirectional mapping between Steam Cloud names and local save paths for a usable mapping.
class SavePaths {
 public:
  explicit SavePaths(SaveMapping mapping);
  // The local slot for a remote name; nothing when no rule covers it. An ambiguous or unsafe
  // name sets `error`.
  std::optional<LocalSavePath> LocalFor(const std::string& remote_name, std::string* error = nullptr) const;
  // The remote name for a local slot, when exactly one rule covers it and maps back to it.
  std::optional<std::string> RemoteFor(const LocalSavePath& local) const;
  const std::vector<SaveRule>& rules() const { return rules_; }

 private:
  std::vector<SaveRule> rules_;
};

}  // namespace playden::steam::cloud
