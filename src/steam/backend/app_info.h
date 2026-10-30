#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playden::steam {

// One branch's manifest of a depot. An encrypted one (encryptedmanifests) needs the branch password
// to read its ID, so it is not usable.
struct BranchManifest {
  std::uint64_t gid = 0;
  std::uint64_t size = 0;
  std::uint64_t download = 0;
  bool encrypted = false;
};

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
  // The public branch's manifest; in SelectDepots' result, the selected branch's.
  std::optional<std::uint64_t> manifest_gid;
  std::uint64_t size = 0;
  std::uint64_t download_size = 0;
  std::uint32_t source_app = 0;  // the app whose appinfo lists the depot
  // Whose content it is: the base app, the depot's dlcappid, or the DLC app that lists it.
  std::uint32_t owner_app = 0;
  // Every other branch's manifest (not "public": the fields above), by branch name as appinfo writes it.
  std::map<std::string, BranchManifest> branches;
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

// A branch (version) of the game, from depots/branches.
struct AppBranch {
  std::string name;
  std::uint64_t build_id = 0;
  std::string description;
  std::int64_t time_updated = 0;  // unix seconds; 0 = unknown
  bool pwd_required = false;
};

struct AppInfo {
  std::uint32_t app_id = 0;
  std::string name;
  std::string type;
  std::string install_dir;
  std::vector<DepotInfo> depots;  // Steam listing order (depot mounting order)
  std::vector<AppBranch> branches;
  std::vector<std::uint32_t> dlc_app_ids;  // extended/listofdlc, which Steam cuts at 64 entries
  // A DLC's game: extended/dlcforappid, else a DLC's common/parent. 0 for anything else.
  std::uint32_t dlc_for_app_id = 0;
  std::vector<AppLaunch> launches;  // sorted by id
  std::string controller_support;
  std::string icon;  // common/icon: the hash of the library icon, as GetOwnedGames img_icon_url
  CloudSaveInfo cloud;
};

// Parses a PICS app buffer (text VDF with optional trailing NULs) whose root is "appinfo".
std::optional<AppInfo> ParseAppInfo(std::uint32_t app_id, std::string_view buffer);

// Steam `oslist`: empty means every platform; otherwise a comma list that must name windows.
bool OsListMatchesWindows(std::string_view os_list);

// What an install asks for: the content language as Steam names it ("german"; empty: English)
// and the branch whose manifests are installed.
struct DepotSelection {
  std::string language;
  std::string branch = "public";
};

struct SelectionResult {
  // A depot left out, and why: os, shared, licence, dlc, branch, language, arch or duplicate.
  struct Rejected {
    std::uint32_t id = 0;
    std::uint32_t source_app = 0;
    std::string reason;
  };
  std::vector<DepotInfo> depots;  // base listing order, then DLC apps in listofdlc order
  std::vector<Rejected> rejected;
  std::string effective_language;      // the language the game installs in
  std::vector<std::string> languages;  // the ones its base depots carry, listing order; empty when none is localised
  std::string reason;                  // short and user-facing, when selection fails
};

// Windows depot selection over the game's depots and those its DLC apps list (`dlc_apps`, the
// appinfo of DLC from the game's listofdlc). Each depot passes, in order: os, shared install,
// licence (an owned depot or depotfromapp; every depot when the licenses are unknown), DLC
// ownership, and a manifest on the branch. A depot listed twice is kept once, from a listing
// with a manifest on the branch; two different manifests for one depot fail the selection.
//
// Then per owner (the game, or one DLC): the content language, then a usable 64-bit Windows
// depot excludes that owner's 32-bit alternates but not architecture-neutral content. A depot
// with no language belongs to every language. The game's language is resolved from its own
// remaining depots: the requested one when they carry it, else English, else the first one
// listed. Depots with no language count as English when some depot is localised, so a game with
// neutral content and a Russian voice depot installs neutral only for French; a game whose depots
// are all neutral installs in the requested language. Each DLC resolves from the requested
// language the same way, and a DLC with a single remaining depot keeps it.
//
// Returns false with `result->reason` when nothing is eligible or available, or when nothing selected is
// the game's own content (only DLC): `result->depots` then still holds that selection.
bool SelectDepots(const AppInfo& app, const std::vector<AppInfo>& dlc_apps, const std::set<std::uint32_t>& owned_apps,
                  const std::optional<std::set<std::uint32_t>>& owned_depots, const DepotSelection& selection,
                  SelectionResult* result);

// What an install in one content language selects on a branch, in bytes.
struct LanguageBytes {
  std::string language;  // as Steam names it ("german")
  std::uint64_t install_bytes = 0;
  std::uint64_t download_bytes = 0;
};

// A version an install can choose: a branch with no password whose manifests give the game ≥ 1 depot
// of its own. Sizes are what an install of it selects, as the details estimate them.
struct BranchOffer {
  std::string name;  // as appinfo writes it; "public" for the default
  std::string description;
  std::int64_t time_updated = 0;
  std::uint64_t build_id = 0;
  std::uint64_t install_bytes = 0;
  std::uint64_t download_bytes = 0;
  // The same for each language the branch's own depots carry (empty when none is localised).
  std::vector<LanguageBytes> languages;
};

// The public branch first (always: it is what an install without a choice gets), then every other
// usable branch, the most recently updated first.
std::vector<BranchOffer> OfferedBranches(const AppInfo& app, const std::vector<AppInfo>& dlc_apps,
                                         const std::set<std::uint32_t>& owned_apps,
                                         const std::optional<std::set<std::uint32_t>>& owned_depots,
                                         const std::string& language);

// What `depots` install and download, in bytes. Some appinfo reports a download larger than the
// install (FINAL FANTASY XIII, issue #5); compressed chunks never are, so the download is capped.
void DepotBytes(const std::vector<DepotInfo>& depots, std::uint64_t* install, std::uint64_t* download);

// Depots that look like the game itself but the account's licenses do not grant: no oslist, no
// language, not DLC, with a public manifest, and together larger than the selected base (not DLC)
// depots. A selected base content depot of at least half the largest missing one holds the game. Empty
// when nothing is missing or the licenses are unknown. Installing the rest would look complete
// but not start (issue #58).
std::vector<std::uint32_t> MissingBaseDepots(const AppInfo& app,
                                             const std::optional<std::set<std::uint32_t>>& owned_depots,
                                             const std::vector<DepotInfo>& selected);

// Steam `betakey`: empty, or a comma list that names "public" or "default".
bool BetaKeyMatchesPublic(std::string_view beta_key);
// Steam `betakey` for `branch` (empty: public): empty, or a comma list that names the branch
// (case-insensitively); for the public branch also "public" or "default".
bool BetaKeyMatchesBranch(std::string_view beta_key, std::string_view branch);

// A dedicated server or relay, by its file name: hlds.exe, hltv.exe, srcds.exe, or a name with the
// word "server" or "dedicated" (DedicatedServer.exe, game_server.exe; not Observer.exe).
bool IsDedicatedServerProgram(std::string_view executable);

// Windows launch entries usable on `branch` (empty: public), default type first, then by id. When no
// entry is for that branch, every branch's entries; `any_branch` then says so. A dedicated server is
// left out while another entry remains.
bool EligibleLaunches(const AppInfo& app, const std::set<std::uint32_t>& owned_apps, std::string_view branch,
                      std::vector<AppLaunch>* launches, std::string* reason, bool* any_branch = nullptr);

// Normalizes a game-relative Windows path to forward slashes. Rejects absolute paths, drive
// colons, control characters, "..", and the download scratch folder.
std::optional<std::string> RelativeGamePath(std::string_view raw, bool allow_root = false);

// Microsoft CRT argument splitting into literal argv values.
std::optional<std::vector<std::string>> ParseWindowsArguments(std::string_view raw);

}  // namespace playden::steam
