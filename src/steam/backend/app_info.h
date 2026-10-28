#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playden::steam {

struct DepotInfo {
  std::uint32_t id = 0;
  std::string name;
  std::string os_list;  // "windows", "macos,linux", "" = all
  std::string os_arch;
  std::string language;  // "" = all languages
  bool shared_install = false;
  std::optional<std::uint32_t> depot_from_app;
  bool is_dlc = false;
  std::optional<std::uint32_t> dlc_app_id;
  std::optional<std::uint64_t> manifest_gid;  // public branch manifest
  std::uint64_t size = 0;
  std::uint64_t download_size = 0;
};

struct AppLaunch {
  std::string id;
  std::string executable;
  std::string arguments;
  std::string working_directory;
  std::string os_list;
  std::string os_arch;
  std::string type;
  std::optional<std::uint32_t> required_dlc;
  std::optional<std::string> beta_key;
  std::optional<std::string> description;
};

// Steam Cloud ("ufs") save roots. Steam writes them as %Token% in appinfo and in Cloud file names.
enum class SavePathRoot {
  kNone, kGameInstall, kSteamUserData, kWinMyDocuments, kWinAppDataLocal, kWinAppDataLocalLow,
  kWinAppDataRoaming, kWinSavedGames, kWinProgramData, kRoot,
  kLinuxHome, kLinuxXdgDataHome, kLinuxXdgConfigHome, kMacHome, kMacAppSupport,
};
// Accepts the token with or without '%', case-insensitively, including Steam's aliases.
SavePathRoot ParseSavePathRoot(std::string_view token);
// The canonical token name without '%' ("WinMyDocuments"); empty for kNone.
const char* SavePathRootName(SavePathRoot root);
bool IsWindowsSavePathRoot(SavePathRoot root);

// One Windows save location, with any root override applied. Cloud file names are built from the
// root and path as Steam declared them (upload_root, upload_path), before overrides.
struct SaveFilePattern {
  SavePathRoot root = SavePathRoot::kNone;
  std::string path;     // forward slashes
  std::string pattern;  // file name glob
  int recursive = 0;
  SavePathRoot upload_root = SavePathRoot::kNone;
  std::string upload_path;
};

struct CloudSaveInfo {
  bool declared = false;  // the app has a ufs section
  std::uint64_t quota = 0;
  std::uint64_t max_num_files = 0;
  std::vector<SaveFilePattern> patterns;
};

struct AppInfo {
  std::uint32_t app_id = 0;
  std::string name;
  std::string type;
  std::string install_dir;
  std::vector<DepotInfo> depots;  // Steam listing order (depot mounting order)
  std::vector<std::pair<std::string, std::uint64_t>> branches;
  std::vector<std::uint32_t> dlc_app_ids;
  std::vector<AppLaunch> launches;  // sorted by id
  std::string controller_support;
  std::string icon;  // common/icon: the hash of the library icon, as GetOwnedGames img_icon_url
  CloudSaveInfo cloud;
};

// Parses a PICS app buffer (text VDF with optional trailing NULs) whose root is "appinfo".
std::optional<AppInfo> ParseAppInfo(std::uint32_t app_id, std::string_view buffer);

// Steam `oslist`: empty means every platform; otherwise a comma list that must name windows.
bool OsListMatchesWindows(std::string_view os_list);

// Windows/English depot selection: eligible by OS, language, shared install, access and DLC
// ownership, then only those with a public manifest. A usable 64-bit Windows depot excludes
// 32-bit alternates but not architecture-neutral content. Results are sorted by id. Returns
// false with a short reason when nothing is eligible or available.
bool SelectDepots(const AppInfo& app, const std::set<std::uint32_t>& owned_apps,
                  const std::optional<std::set<std::uint32_t>>& owned_depots,
                  std::vector<DepotInfo>* selected, std::string* reason);

// Depots that look like the game itself but the account's licenses do not grant: no oslist, no
// language, not DLC, with a public manifest, and together larger than the selected depots. Empty
// when nothing is missing or the licenses are unknown. Installing the rest would look complete
// but not start (issue #58).
std::vector<std::uint32_t> MissingBaseDepots(const AppInfo& app,
                                             const std::optional<std::set<std::uint32_t>>& owned_depots,
                                             const std::vector<DepotInfo>& selected);

// Steam `betakey`: empty, or a comma list that names "public" or "default".
bool BetaKeyMatchesPublic(std::string_view beta_key);

// Windows launch entries usable on the public branch, default type first, then by id. When no entry
// is for the public branch, every branch's entries; `any_branch` then says so.
bool EligibleLaunches(const AppInfo& app, const std::set<std::uint32_t>& owned_apps,
                      std::vector<AppLaunch>* launches, std::string* reason, bool* any_branch = nullptr);

// Normalizes a game-relative Windows path to forward slashes. Rejects absolute paths, drive
// colons, control characters, "..", and the download scratch folder.
std::optional<std::string> RelativeGamePath(std::string_view raw, bool allow_root = false);

// Microsoft CRT argument splitting into literal argv values.
std::optional<std::vector<std::string>> ParseWindowsArguments(std::string_view raw);

}  // namespace playden::steam
