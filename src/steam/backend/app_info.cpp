#include "steam/backend/app_info.h"

#include <algorithm>
#include <charconv>
#include <iterator>

#include "steam/backend/content_language.h"
#include "steam/backend/vdf.h"

namespace playden::steam {
namespace {

constexpr std::size_t kMaxArgumentBytes = 32767;

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string LowerString(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = Lower(c);
  return out;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::optional<std::uint64_t> ParseU64(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::uint64_t value = 0;
  const char* end = s.data() + s.size();
  const auto result = std::from_chars(s.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
  return value;
}

std::optional<std::uint32_t> ParseU32(std::string_view s) {
  const auto value = ParseU64(s);
  if (!value || *value > 0xFFFFFFFFULL) return std::nullopt;
  return static_cast<std::uint32_t>(*value);
}

std::optional<std::uint32_t> ChildU32(const VdfNode* node, std::string_view key) {
  if (node == nullptr) return std::nullopt;
  const auto text = node->String(key);
  if (!text) return std::nullopt;
  return ParseU32(*text);
}

std::string ChildString(const VdfNode* node, std::string_view key) {
  if (node == nullptr) return std::string();
  const auto text = node->String(key);
  return text ? std::string(*text) : std::string();
}

std::optional<std::string> ChildOptString(const VdfNode* node, std::string_view key) {
  if (node == nullptr) return std::nullopt;
  const auto text = node->String(key);
  if (!text) return std::nullopt;
  return std::string(*text);
}

// Natural order: digit runs compare by numeric value, everything else ASCII case-insensitively.
int NaturalCompare(std::string_view a, std::string_view b) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (IsDigit(a[i]) && IsDigit(b[j])) {
      std::size_t ie = i;
      std::size_t je = j;
      while (ie < a.size() && IsDigit(a[ie])) ++ie;
      while (je < b.size() && IsDigit(b[je])) ++je;
      std::string_view da = a.substr(i, ie - i);
      std::string_view db = b.substr(j, je - j);
      while (da.size() > 1 && da.front() == '0') da.remove_prefix(1);
      while (db.size() > 1 && db.front() == '0') db.remove_prefix(1);
      if (da.size() != db.size()) return da.size() < db.size() ? -1 : 1;
      const int cmp = da.compare(db);
      if (cmp != 0) return cmp < 0 ? -1 : 1;
      i = ie;
      j = je;
      continue;
    }
    const char x = Lower(a[i]);
    const char y = Lower(b[j]);
    if (x != y) {
      return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    ++i;
    ++j;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

bool Fail(std::string* reason, std::string_view text) {
  if (reason != nullptr) *reason = text;
  return false;
}

AppLaunch ParseLaunch(const std::string& id, const VdfNode& value) {
  AppLaunch launch;
  launch.id = id;
  launch.executable = ChildString(&value, "executable");
  launch.arguments = ChildString(&value, "arguments");
  launch.working_directory = ChildString(&value, "workingdir");
  launch.type = ChildString(&value, "type");
  const VdfNode* config = value.Find("config");
  launch.os_list = ChildString(config, "oslist");
  launch.os_arch = ChildString(config, "osarch");
  launch.required_dlc = ChildU32(config, "ownsdlc");
  launch.beta_key = ChildOptString(config, "betakey");
  launch.description = ChildOptString(&value, "description");
  return launch;
}

DepotInfo ParseDepot(std::uint32_t id, const VdfNode& value) {
  DepotInfo depot;
  depot.id = id;
  const auto name = value.String("name");
  depot.name = name ? std::string(*name) : "depot " + std::to_string(id);
  const VdfNode* config = value.Find("config");
  depot.os_list = ChildString(config, "oslist");
  depot.os_arch = ChildString(config, "osarch");
  depot.language = ChildString(config, "language");
  depot.shared_install = value.String("sharedinstall") == std::string_view("1");
  depot.depot_from_app = ChildU32(&value, "depotfromapp");
  depot.dlc_app_id = ChildU32(&value, "dlcappid");
  depot.is_dlc = depot.dlc_app_id.has_value();
  // Every branch's manifest. Old appinfo format: the gid directly as a string; new: a dict {gid, size, download}.
  if (const VdfNode* manifests = value.Find("manifests"); manifests != nullptr && manifests->is_dict) {
    for (const auto& [branch, entry] : manifests->children) {
      BranchManifest manifest;
      manifest.gid = entry.is_dict ? entry.Uint64("gid").value_or(0) : ParseU64(entry.value).value_or(0);
      manifest.size = entry.is_dict ? entry.Uint64("size").value_or(0) : 0;
      manifest.download = entry.is_dict ? entry.Uint64("download").value_or(0) : 0;
      if (LowerString(branch) == "public") {
        if (manifest.gid != 0) depot.manifest_gid = manifest.gid;
        depot.size = manifest.size;
        depot.download_size = manifest.download;
      } else {
        depot.branches[branch] = manifest;
      }
    }
  }
  // A password branch's manifest is encrypted with the branch password: listed, but not usable.
  if (const VdfNode* encrypted = value.Find("encryptedmanifests"); encrypted != nullptr && encrypted->is_dict) {
    for (const auto& entry : encrypted->children) {
      if (LowerString(entry.first) == "public") continue;
      auto& manifest = depot.branches[entry.first];
      if (manifest.gid == 0) manifest.encrypted = true;
    }
  }
  return depot;
}

// The depot's usable manifest on `branch` (matched case-insensitively), if any: the public one from
// the depot's own fields, another branch's from its branches.
std::optional<BranchManifest> UsableManifest(const DepotInfo& depot, const std::string& branch) {
  const auto wanted = LowerString(branch);
  if (wanted == "public") {
    if (!depot.manifest_gid || *depot.manifest_gid == 0) return std::nullopt;
    return BranchManifest{*depot.manifest_gid, depot.size, depot.download_size, false};
  }
  auto found = depot.branches.find(branch);
  if (found == depot.branches.end())
    found = std::find_if(depot.branches.begin(), depot.branches.end(),
                         [&](const auto& entry) { return LowerString(entry.first) == wanted; });
  if (found == depot.branches.end() || found->second.encrypted || found->second.gid == 0) return std::nullopt;
  return found->second;
}

std::vector<std::uint32_t> ParseDlcList(std::string_view list) {
  std::vector<std::uint32_t> out;
  while (!list.empty()) {
    const std::size_t comma = list.find(',');
    const std::string_view part = Trim(list.substr(0, comma));
    if (const auto id = ParseU32(part)) out.push_back(*id);
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return out;
}

}  // namespace

namespace {

bool SameWord(std::string_view value, std::string_view wanted) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  return LowerString(std::string(value)) == wanted;
}

bool ListContains(std::string_view list, std::string_view wanted) {
  while (!list.empty()) {
    const auto comma = list.find(',');
    if (SameWord(list.substr(0, comma), wanted)) return true;
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return false;
}

std::string ReplaceAll(std::string text, const std::string& find, const std::string& replace) {
  if (find.empty()) return text;
  for (std::size_t at = text.find(find); at != std::string::npos; at = text.find(find, at + replace.size()))
    text.replace(at, find.size(), replace);
  return text;
}

std::string TrimSlashes(std::string text) {
  while (!text.empty() && text.front() == '/') text.erase(text.begin());
  while (!text.empty() && text.back() == '/') text.pop_back();
  return text;
}

// Windows save patterns from appinfo/ufs. Steam names platforms "Windows", "MacOS" and "Linux".
CloudSaveInfo ParseCloudSaves(const VdfNode* ufs) {
  CloudSaveInfo info;
  if (ufs == nullptr || !ufs->is_dict) return info;
  info.declared = true;
  info.quota = ufs->Uint64("quota").value_or(0);
  info.max_num_files = ufs->Uint64("maxnumfiles").value_or(0);
  struct Override {
    SavePathRoot from = SavePathRoot::kNone;
    SavePathRoot to = SavePathRoot::kNone;
    std::string add_path;
    std::vector<std::pair<std::string, std::string>> transforms;
  };
  std::vector<Override> overrides;
  if (const VdfNode* list = ufs->Find("rootoverrides"); list != nullptr && list->is_dict) {
    for (const auto& [key, value] : list->children) {
      if (!value.is_dict) continue;
      const std::string os = ChildString(&value, "os");
      // "oscompare" "!=" applies the override to every platform other than "os".
      const bool matches = os.empty() ? ListContains(ChildString(&value, "oslist"), "windows")
                                      : ((ChildString(&value, "oscompare") == "!=") != SameWord(os, "windows"));
      if (!matches) continue;
      Override item;
      item.from = ParseSavePathRoot(ChildString(&value, "root"));
      item.to = ParseSavePathRoot(ChildString(&value, "useinstead"));
      item.add_path = ChildString(&value, "addpath");
      if (const VdfNode* transforms = value.Find("pathtransforms"); transforms != nullptr && transforms->is_dict)
        for (const auto& [index, transform] : transforms->children)
          if (transform.is_dict) item.transforms.emplace_back(ChildString(&transform, "find"), ChildString(&transform, "replace"));
      overrides.push_back(std::move(item));
    }
  }
  const VdfNode* files = ufs->Find("savefiles");
  if (files == nullptr || !files->is_dict) return info;
  for (const auto& [key, value] : files->children) {
    if (!value.is_dict) continue;
    bool for_windows = true;
    if (const VdfNode* platforms = value.Find("platforms"); platforms != nullptr && platforms->is_dict && !platforms->children.empty()) {
      for_windows = false;
      for (const auto& [index, platform] : platforms->children)
        if (!platform.is_dict && SameWord(platform.value, "windows")) for_windows = true;
    }
    if (!for_windows) continue;
    SaveFilePattern pattern;
    pattern.upload_root = ParseSavePathRoot(ChildString(&value, "root"));
    std::string raw_path = ChildString(&value, "path");
    std::replace(raw_path.begin(), raw_path.end(), '\\', '/');
    pattern.upload_path = raw_path == "." || raw_path == "/" ? std::string() : raw_path;
    pattern.root = pattern.upload_root;
    pattern.path = pattern.upload_path;
    const auto override = std::find_if(overrides.begin(), overrides.end(),
                                       [&](const Override& item) { return item.from == pattern.upload_root; });
    if (override != overrides.end()) {
      pattern.root = override->to;
      if (!override->add_path.empty()) {
        std::string prefix = override->add_path;
        std::replace(prefix.begin(), prefix.end(), '\\', '/');
        prefix = TrimSlashes(prefix);
        pattern.path = pattern.upload_path.empty() ? prefix : prefix + "/" + TrimSlashes(pattern.upload_path);
      }
      for (const auto& [find, replace] : override->transforms) pattern.path = ReplaceAll(pattern.path, find, replace);
    }
    pattern.pattern = ChildString(&value, "pattern");
    pattern.recursive = static_cast<int>(value.Uint64("recursive").value_or(0));
    info.patterns.push_back(std::move(pattern));
  }
  return info;
}

}  // namespace

SavePathRoot ParseSavePathRoot(std::string_view token) {
  std::string key = LowerString(std::string(token));
  while (!key.empty() && key.front() == '%') key.erase(key.begin());
  while (!key.empty() && key.back() == '%') key.pop_back();
  if (key == "gameinstall") return SavePathRoot::kGameInstall;
  if (key == "steamuserdata" || key == "steamuserbasestorage") return SavePathRoot::kSteamUserData;
  if (key == "winmydocuments" || key == "steamclouddocuments") return SavePathRoot::kWinMyDocuments;
  if (key == "winappdatalocal") return SavePathRoot::kWinAppDataLocal;
  if (key == "winappdatalocallow") return SavePathRoot::kWinAppDataLocalLow;
  if (key == "winappdataroaming") return SavePathRoot::kWinAppDataRoaming;
  if (key == "winsavedgames") return SavePathRoot::kWinSavedGames;
  if (key == "winprogramdata") return SavePathRoot::kWinProgramData;
  if (key == "linuxhome") return SavePathRoot::kLinuxHome;
  if (key == "linuxxdgdatahome") return SavePathRoot::kLinuxXdgDataHome;
  if (key == "linuxxdgconfighome") return SavePathRoot::kLinuxXdgConfigHome;
  if (key == "machome") return SavePathRoot::kMacHome;
  if (key == "macappsupport") return SavePathRoot::kMacAppSupport;
  if (key == "root" || key == "windowshome" || key == "root_mod") return SavePathRoot::kRoot;
  return SavePathRoot::kNone;
}

const char* SavePathRootName(SavePathRoot root) {
  switch (root) {
    case SavePathRoot::kGameInstall: return "GameInstall";
    case SavePathRoot::kSteamUserData: return "SteamUserData";
    case SavePathRoot::kWinMyDocuments: return "WinMyDocuments";
    case SavePathRoot::kWinAppDataLocal: return "WinAppDataLocal";
    case SavePathRoot::kWinAppDataLocalLow: return "WinAppDataLocalLow";
    case SavePathRoot::kWinAppDataRoaming: return "WinAppDataRoaming";
    case SavePathRoot::kWinSavedGames: return "WinSavedGames";
    case SavePathRoot::kWinProgramData: return "WinProgramData";
    case SavePathRoot::kRoot: return "Root";
    case SavePathRoot::kLinuxHome: return "LinuxHome";
    case SavePathRoot::kLinuxXdgDataHome: return "LinuxXdgDataHome";
    case SavePathRoot::kLinuxXdgConfigHome: return "LinuxXdgConfigHome";
    case SavePathRoot::kMacHome: return "MacHome";
    case SavePathRoot::kMacAppSupport: return "MacAppSupport";
    case SavePathRoot::kNone: break;
  }
  return "";
}

bool IsWindowsSavePathRoot(SavePathRoot root) {
  switch (root) {
    case SavePathRoot::kGameInstall: case SavePathRoot::kSteamUserData: case SavePathRoot::kWinMyDocuments:
    case SavePathRoot::kWinAppDataLocal: case SavePathRoot::kWinAppDataLocalLow: case SavePathRoot::kWinAppDataRoaming:
    case SavePathRoot::kWinSavedGames: case SavePathRoot::kWinProgramData: case SavePathRoot::kRoot:
      return true;
    default:
      return false;
  }
}

std::optional<AppInfo> ParseAppInfo(std::uint32_t app_id, std::string_view buffer) {
  while (!buffer.empty() && buffer.back() == '\0') buffer.remove_suffix(1);
  const auto doc = ParseVdf(buffer);
  if (!doc) return std::nullopt;
  const VdfNode* root = doc->Find("appinfo");
  if (root == nullptr || !root->is_dict) return std::nullopt;

  AppInfo app;
  app.app_id = app_id;
  const VdfNode* common = root->Find("common");
  const VdfNode* config = root->Find("config");
  const auto name = common != nullptr ? common->String("name") : std::nullopt;
  app.name = name ? std::string(*name) : "app_" + std::to_string(app_id);
  const auto type = common != nullptr ? common->String("type") : std::nullopt;
  app.type = type ? std::string(*type) : "?";
  app.install_dir = ChildString(config, "installdir");
  app.controller_support = ChildString(common, "controller_support");
  app.icon = ChildString(common, "icon");

  std::optional<std::string_view> dlc_list;
  if (const VdfNode* extended = root->Find("extended")) dlc_list = extended->String("listofdlc");
  if (!dlc_list && common != nullptr) {
    if (const VdfNode* extended = common->Find("extended")) dlc_list = extended->String("listofdlc");
  }
  if (dlc_list) app.dlc_app_ids = ParseDlcList(*dlc_list);
  // listofdlc stops at 64 entries (Monster Hunter: World's leaves out Iceborne): a DLC names its game itself,
  // in dlcforappid (whatever its type says, as GameNative reads it), else in common/parent when it is a DLC.
  {
    std::optional<std::uint32_t> game;
    if (const VdfNode* extended = root->Find("extended")) game = ChildU32(extended, "dlcforappid");
    if ((!game || !*game) && common != nullptr)
      if (const VdfNode* extended = common->Find("extended")) game = ChildU32(extended, "dlcforappid");
    if ((!game || !*game) && LowerString(app.type) == "dlc") game = ChildU32(common, "parent");
    if (game && *game != 0 && *game != app_id) app.dlc_for_app_id = *game;
  }
  app.cloud = ParseCloudSaves(root->Find("ufs"));

  if (config != nullptr) {
    if (const VdfNode* launch = config->Find("launch"); launch != nullptr && launch->is_dict) {
      for (const auto& entry : launch->children) {
        app.launches.push_back(ParseLaunch(entry.first, entry.second));
      }
      std::stable_sort(app.launches.begin(), app.launches.end(),
                       [](const AppLaunch& a, const AppLaunch& b) {
                         return NaturalCompare(a.id, b.id) < 0;
                       });
    }
  }

  const VdfNode* depots = root->Find("depots");
  if (depots != nullptr && depots->is_dict) {
    for (const auto& entry : depots->children) {
      if (LowerString(entry.first) == "branches") {
        if (!entry.second.is_dict) continue;
        for (const auto& child : entry.second.children) {
          const std::string& name = child.first;
          const VdfNode& value = child.second;
          AppBranch branch;
          branch.name = name;
          branch.build_id = value.Uint64("buildid").value_or(0);
          branch.description = ChildString(&value, "description");
          branch.time_updated = static_cast<std::int64_t>(value.Uint64("timeupdated").value_or(0));
          branch.pwd_required = value.String("pwdrequired") == std::string_view("1");
          auto existing = std::find_if(app.branches.begin(), app.branches.end(),
                                       [&](const AppBranch& b) { return b.name == name; });
          if (existing != app.branches.end()) {
            *existing = std::move(branch);
          } else {
            app.branches.push_back(std::move(branch));
          }
        }
        continue;
      }
      const auto id = ParseU32(entry.first);
      if (!id) continue;  // baselanguages, overridescddb, ...
      DepotInfo depot = ParseDepot(*id, entry.second);
      depot.source_app = app.app_id;
      depot.owner_app = depot.dlc_app_id.value_or(app.app_id);
      app.depots.push_back(std::move(depot));
    }
  }
  return app;
}

bool OsListMatchesWindows(std::string_view os_list) {
  if (os_list.empty()) return true;
  while (!os_list.empty()) {
    const std::size_t comma = os_list.find(',');
    if (LowerString(Trim(os_list.substr(0, comma))) == "windows") return true;
    if (comma == std::string_view::npos) break;
    os_list.remove_prefix(comma + 1);
  }
  return false;
}

namespace {

// The languages `depots` carry, in listing order, lower case. Content with no language is the
// game's English (Portal ships its English beside a Russian voice depot), so English is carried
// when any depot has no language.
std::vector<std::string> CarriedLanguages(const std::vector<const DepotInfo*>& depots) {
  std::vector<std::string> carried;
  bool neutral = false;
  for (const DepotInfo* depot : depots) {
    if (depot->language.empty()) {
      neutral = true;
      continue;
    }
    const auto code = LowerString(Trim(depot->language));
    if (std::find(carried.begin(), carried.end(), code) == carried.end()) carried.push_back(code);
  }
  const std::string english(kDefaultContentLanguage);
  if (neutral && !carried.empty() && std::find(carried.begin(), carried.end(), english) == carried.end())
    carried.insert(carried.begin(), english);
  return carried;
}

bool InLanguage(const DepotInfo& depot, const std::string& language) {
  return depot.language.empty() || LowerString(Trim(depot.language)) == language;
}

}  // namespace

bool SelectDepots(const AppInfo& app, const std::vector<AppInfo>& dlc_apps, const std::set<std::uint32_t>& owned_apps,
                  const std::optional<std::set<std::uint32_t>>& owned_depots, const DepotSelection& selection,
                  SelectionResult* result) {
  SelectionResult local;
  SelectionResult& out = result != nullptr ? *result : local;
  out = SelectionResult{};
  if (owned_apps.count(app.app_id) == 0) {
    out.reason = "This account does not own the selected game.";
    return false;
  }
  // The branch as appinfo names it, matched case-insensitively.
  std::string branch = selection.branch.empty() ? std::string("public") : std::string(Trim(selection.branch));
  for (const AppBranch& listed_branch : app.branches)
    if (LowerString(listed_branch.name) == LowerString(branch)) branch = listed_branch.name;
  const auto reject = [&out](const DepotInfo& depot, const char* why) {
    out.rejected.push_back({depot.id, depot.source_app, why});
  };

  // Every listed depot once: the game's in listing order, then each DLC app's in listofdlc order.
  std::vector<DepotInfo> listed;
  const auto add = [&](const DepotInfo& raw, std::uint32_t source, std::uint32_t owner) -> bool {
    DepotInfo depot = raw;
    depot.source_app = source;
    depot.owner_app = owner;
    // The chosen branch's manifest, and nothing else: no fallback to the public one.
    const auto manifest = UsableManifest(raw, branch);
    depot.manifest_gid = manifest ? std::optional<std::uint64_t>(manifest->gid) : std::nullopt;
    depot.size = manifest ? manifest->size : 0;
    depot.download_size = manifest ? manifest->download : 0;
    const auto same = std::find_if(listed.begin(), listed.end(), [&](const DepotInfo& seen) { return seen.id == depot.id; });
    if (same == listed.end()) {
      listed.push_back(std::move(depot));
      return true;
    }
    const std::uint64_t a = same->manifest_gid.value_or(0), b = depot.manifest_gid.value_or(0);
    if (a != 0 && b != 0 && a != b) return false;
    // The listing with a manifest on the branch is the one installed (a DLC app can list a copy of the
    // game's depot with the branch's manifest when the game's own listing has none). The first listing
    // keeps its place, the mounting order, and says whose content it is.
    if (a == 0 && b != 0) {
      const auto owner = same->owner_app;
      std::swap(*same, depot);
      same->owner_app = owner;
    }
    reject(depot, "duplicate");
    return true;
  };
  bool consistent = true;
  for (const DepotInfo& depot : app.depots) consistent &= add(depot, app.app_id, depot.dlc_app_id.value_or(app.app_id));
  std::vector<const AppInfo*> dlc_order;
  for (const auto id : app.dlc_app_ids)
    for (const AppInfo& dlc : dlc_apps)
      if (dlc.app_id == id && std::find(dlc_order.begin(), dlc_order.end(), &dlc) == dlc_order.end()) dlc_order.push_back(&dlc);
  for (const AppInfo& dlc : dlc_apps)
    if (dlc.app_id != app.app_id && std::find(dlc_order.begin(), dlc_order.end(), &dlc) == dlc_order.end())
      dlc_order.push_back(&dlc);
  for (const AppInfo* dlc : dlc_order)
    for (const DepotInfo& depot : dlc->depots) consistent &= add(depot, dlc->app_id, dlc->app_id);
  if (!consistent) {
    out.rejected.clear();
    out.reason = "This game lists conflicting depots.";
    return false;
  }

  // The per-depot gates, in order.
  std::set<std::uint32_t> from_app;
  for (const DepotInfo& depot : listed)
    if (depot.depot_from_app) from_app.insert(depot.id);
  std::vector<const DepotInfo*> passed;
  bool eligible = false;
  for (const DepotInfo& depot : listed) {
    if (!OsListMatchesWindows(depot.os_list)) { reject(depot, "os"); continue; }
    if (depot.shared_install) { reject(depot, "shared"); continue; }
    if (owned_depots && owned_depots->count(depot.id) == 0 && from_app.count(depot.id) == 0) { reject(depot, "licence"); continue; }
    if (depot.owner_app != app.app_id && owned_apps.count(depot.owner_app) == 0) { reject(depot, "dlc"); continue; }
    eligible = true;
    if (!depot.manifest_gid || *depot.manifest_gid == 0) { reject(depot, "branch"); continue; }
    passed.push_back(&depot);
  }
  if (!eligible) {
    out.reason = "This game has no downloadable Windows content.";
    return false;
  }

  // Per owner: the language, then the architecture. The game's own group comes first.
  std::vector<std::uint32_t> owners{app.app_id};
  for (const DepotInfo* depot : passed)
    if (std::find(owners.begin(), owners.end(), depot->owner_app) == owners.end()) owners.push_back(depot->owner_app);
  const std::string requested = selection.language.empty() ? std::string(kDefaultContentLanguage)
                                                           : LowerString(Trim(selection.language));
  std::set<const DepotInfo*> kept;
  for (const auto owner : owners) {
    std::vector<const DepotInfo*> group;
    for (const DepotInfo* depot : passed)
      if (depot->owner_app == owner) group.push_back(depot);
    // The game's language comes from its own installable content: a Mac-only or DLC-only
    // localisation does not make the game carry that language (issue #45). Each DLC resolves from
    // the requested language, not the game's.
    const auto carried = CarriedLanguages(group);
    const std::string language = ResolveContentLanguage(carried, requested);
    if (owner == app.app_id) {
      out.effective_language = language;
      out.languages = carried;
    }
    std::vector<const DepotInfo*> in_language;
    for (const DepotInfo* depot : group) {
      if (group.size() == 1 || InLanguage(*depot, language)) in_language.push_back(depot);
      else reject(*depot, "language");
    }
    bool has_win64 = false;
    for (const DepotInfo* depot : in_language)
      if (LowerString(Trim(depot->os_arch)) == "64") has_win64 = true;
    for (const DepotInfo* depot : in_language) {
      if (has_win64 && LowerString(Trim(depot->os_arch)) == "32") reject(*depot, "arch");
      else kept.insert(depot);
    }
  }
  for (const DepotInfo& depot : listed)
    if (kept.count(&depot)) out.depots.push_back(depot);
  if (out.depots.empty()) {
    out.reason = "The game's Windows content is not available on its " + branch + " branch.";
    return false;
  }
  // DLC alone is no version of the game: an update to it would delete the game. depotfromapp content the
  // game lists counts as the game's.
  if (std::none_of(out.depots.begin(), out.depots.end(),
                   [&app](const DepotInfo& depot) { return depot.owner_app == app.app_id; })) {
    out.reason = "This version has no content of the game itself.";
    return false;
  }
  return true;
}

void DepotBytes(const std::vector<DepotInfo>& depots, std::uint64_t* install, std::uint64_t* download) {
  std::uint64_t total = 0;
  std::uint64_t compressed = 0;
  for (const auto& depot : depots) {
    total += depot.size;
    compressed += depot.size && depot.download_size > depot.size ? depot.size : depot.download_size;
  }
  if (install != nullptr) *install = total;
  if (download != nullptr) *download = compressed;
}

std::vector<BranchOffer> OfferedBranches(const AppInfo& app, const std::vector<AppInfo>& dlc_apps,
                                         const std::set<std::uint32_t>& owned_apps,
                                         const std::optional<std::set<std::uint32_t>>& owned_depots,
                                         const std::string& language) {
  const auto offer = [&](const std::string& name, const AppBranch* info) -> std::optional<BranchOffer> {
    BranchOffer result;
    result.name = name;
    if (info != nullptr) {
      result.description = info->description;
      result.time_updated = info->time_updated;
      result.build_id = info->build_id;
    }
    SelectionResult selected;
    if (!SelectDepots(app, dlc_apps, owned_apps, owned_depots, DepotSelection{language, name}, &selected)) return std::nullopt;
    // A branch that only carries DLC or another platform's content is no version of the game.
    if (std::none_of(selected.depots.begin(), selected.depots.end(),
                     [&](const DepotInfo& depot) { return depot.owner_app == app.app_id; }))
      return std::nullopt;
    DepotBytes(selected.depots, &result.install_bytes, &result.download_bytes);
    // A language swaps depots in and out, so an install sheet that changes it needs that selection's size.
    for (const auto& carried : selected.languages) {
      LanguageBytes bytes{carried, result.install_bytes, result.download_bytes};
      SelectionResult other;
      if (carried != selected.effective_language) {
        if (!SelectDepots(app, dlc_apps, owned_apps, owned_depots, DepotSelection{carried, name}, &other)) continue;
        DepotBytes(other.depots, &bytes.install_bytes, &bytes.download_bytes);
      }
      result.languages.push_back(std::move(bytes));
    }
    return result;
  };
  const auto is_public = [](const AppBranch& branch) { return LowerString(branch.name) == "public"; };
  const auto listed_public = std::find_if(app.branches.begin(), app.branches.end(), is_public);
  const AppBranch* public_info = listed_public != app.branches.end() ? &*listed_public : nullptr;
  // The public branch is what an install without a choice gets: always offered, first.
  std::vector<BranchOffer> offers;
  if (auto pub = offer("public", public_info)) offers.push_back(std::move(*pub));
  else offers.push_back(BranchOffer{"public", public_info ? public_info->description : std::string(),
                                    public_info ? public_info->time_updated : 0, public_info ? public_info->build_id : 0, 0, 0});
  std::vector<BranchOffer> others;
  for (const AppBranch& branch : app.branches) {
    if (is_public(branch) || branch.pwd_required) continue;
    if (auto found = offer(branch.name, &branch)) others.push_back(std::move(*found));
  }
  std::stable_sort(others.begin(), others.end(), [](const BranchOffer& a, const BranchOffer& b) {
    if (a.time_updated != b.time_updated) return a.time_updated > b.time_updated;
    return NaturalCompare(a.name, b.name) < 0;
  });
  offers.insert(offers.end(), std::make_move_iterator(others.begin()), std::make_move_iterator(others.end()));
  return offers;
}

std::vector<std::uint32_t> MissingBaseDepots(const AppInfo& app,
                                             const std::optional<std::set<std::uint32_t>>& owned_depots,
                                             const std::vector<DepotInfo>& selected) {
  if (!owned_depots) return {};
  const auto base_content = [](const DepotInfo& depot) {
    return !depot.depot_from_app && !depot.shared_install && !depot.is_dlc && depot.os_list.empty() &&
           depot.language.empty() && depot.manifest_gid && *depot.manifest_gid != 0;
  };
  std::vector<std::uint32_t> missing;
  std::uint64_t missing_size = 0, largest_missing = 0;
  for (const DepotInfo& depot : app.depots) {
    if (owned_depots->count(depot.id) != 0 || !base_content(depot)) continue;
    missing.push_back(depot.id);
    missing_size += depot.size;
    largest_missing = std::max(largest_missing, depot.size);
  }
  // A license that grants base content of its own holds the game: the base depots it leaves out
  // are alternates for other licenses (Marvel Rivals lists a second copy of its content), and
  // Steam installs only what the license grants. Only content of a size near the largest one left
  // out counts: Counter-Strike: Source's 51 KB depot 209 is not its 4.5 GB content depot 241.
  // Only the game's own content: DLC in the selection, also a DLC app's own depot, does not stand in for it.
  const auto own = [&app](const DepotInfo& depot) {
    return !depot.dlc_app_id && (depot.owner_app == 0 || depot.owner_app == app.app_id);
  };
  for (const DepotInfo& depot : selected)
    if (own(depot) && base_content(depot) && depot.size && depot.size >= largest_missing / 2) return {};
  std::uint64_t selected_size = 0;
  for (const DepotInfo& depot : selected)
    if (own(depot)) selected_size += depot.size;
  // An unowned extra (a bonus or edition depot) is smaller than the game; old appinfo has no sizes.
  if (missing_size == 0 || missing_size <= selected_size) return {};
  return missing;
}

bool BetaKeyMatchesBranch(std::string_view beta_key, std::string_view branch) {
  if (Trim(beta_key).empty()) return true;
  const auto wanted = LowerString(Trim(branch));
  const bool is_public = wanted.empty() || wanted == "public";
  // A comma list of branches (Minecraft Dungeons II lists "public, build_debug, ..."). "default"
  // tags the public branch's option when other branches have their own (Half-Life 2, issue #10).
  while (!beta_key.empty()) {
    const std::size_t comma = beta_key.find(',');
    const auto listed = LowerString(Trim(beta_key.substr(0, comma)));
    if (is_public ? (listed == "public" || listed == "default") : listed == wanted) return true;
    if (comma == std::string_view::npos) break;
    beta_key.remove_prefix(comma + 1);
  }
  return false;
}

bool BetaKeyMatchesPublic(std::string_view beta_key) { return BetaKeyMatchesBranch(beta_key, "public"); }

bool IsDedicatedServerProgram(std::string_view executable) {
  const auto slash = executable.find_last_of("/\\");
  if (slash != std::string_view::npos) executable.remove_prefix(slash + 1);
  if (executable.size() > 4 && LowerString(executable.substr(executable.size() - 4)) == ".exe")
    executable.remove_suffix(4);
  // Words split at punctuation, digits and case changes: GameServer, HLServer, game_server_x64.
  const auto upper = [&](std::size_t i) { return i < executable.size() && executable[i] >= 'A' && executable[i] <= 'Z'; };
  const auto lower = [&](std::size_t i) { return i < executable.size() && executable[i] >= 'a' && executable[i] <= 'z'; };
  std::vector<std::string> words;
  std::string word;
  for (std::size_t i = 0; i <= executable.size(); ++i) {
    const char c = i < executable.size() ? executable[i] : '\0';
    const bool letter = upper(i) || lower(i);
    const bool word_start = upper(i) && i > 0 && (lower(i - 1) || (upper(i - 1) && lower(i + 1)));
    if (!letter || word_start) {
      if (!word.empty()) words.push_back(std::move(word));
      word.clear();
    }
    if (letter) word.push_back(Lower(c));
  }
  if (words.empty()) return false;
  // GoldSrc and Source servers and the HLTV relay (Cry of Fear lists hlds.exe and hltv.exe).
  if (words.front() == "hlds" || words.front() == "hltv" || words.front() == "srcds") return true;
  for (const std::string& part : words)
    if (part == "server" || part.find("dedicated") != std::string::npos) return true;
  return false;
}

bool EligibleLaunches(const AppInfo& app, const std::set<std::uint32_t>& owned_apps, std::string_view branch,
                      std::vector<AppLaunch>* launches, std::string* reason, bool* any_branch) {
  const auto eligible_for = [&](bool ignore_branch) {
    std::vector<AppLaunch> eligible;
    for (const AppLaunch& launch : app.launches) {
      const bool beta_ok = ignore_branch || !launch.beta_key || BetaKeyMatchesBranch(*launch.beta_key, branch);
      const bool dlc_ok = !launch.required_dlc || *launch.required_dlc == 0 ||
                          owned_apps.count(*launch.required_dlc) != 0;
      // A level editor or a dedicated server is not the game (Selaco lists its .NET editor).
      const auto type = LowerString(launch.type);
      const bool game = type != "editor" && type != "server";
      if (OsListMatchesWindows(launch.os_list) && beta_ok && dlc_ok && game) eligible.push_back(launch);
    }
    return eligible;
  };
  auto eligible = eligible_for(false);
  // Steam still starts a game whose only options name other branches (GUILTY GEAR -STRIVE- lists
  // one for "debug"): every branch's options are then candidates.
  const bool fallback = eligible.empty();
  if (fallback) eligible = eligible_for(true);
  if (any_branch != nullptr) *any_branch = fallback && !eligible.empty();
  // A dedicated server listed without the "server" type is not the game either, unless it is all
  // Steam lists.
  const auto server = [](const AppLaunch& launch) {
    return IsDedicatedServerProgram(launch.executable) ||
           (launch.description && LowerString(*launch.description).find("dedicated server") != std::string::npos);
  };
  if (std::any_of(eligible.begin(), eligible.end(), [&](const AppLaunch& launch) { return !server(launch); }))
    eligible.erase(std::remove_if(eligible.begin(), eligible.end(), server), eligible.end());
  std::set<std::string> ids;
  for (const AppLaunch& launch : eligible) ids.insert(launch.id);
  if (eligible.empty() || ids.size() != eligible.size()) {
    return Fail(reason, "The game has no valid Windows launch configuration.");
  }
  std::stable_sort(eligible.begin(), eligible.end(), [](const AppLaunch& a, const AppLaunch& b) {
    const bool a_default = LowerString(a.type) == "default";
    const bool b_default = LowerString(b.type) == "default";
    if (a_default != b_default) return a_default;
    return NaturalCompare(a.id, b.id) < 0;
  });
  if (launches != nullptr) *launches = std::move(eligible);
  return true;
}

std::optional<std::vector<std::string>> ParseWindowsArguments(std::string_view raw) {
  if (raw.find('\0') != std::string_view::npos || raw.size() > kMaxArgumentBytes) {
    return std::nullopt;
  }
  const auto is_blank = [](char c) { return c == ' ' || c == '\t'; };
  std::vector<std::string> result;
  std::size_t index = 0;
  const std::size_t count = raw.size();
  while (index < count) {
    while (index < count && is_blank(raw[index])) ++index;
    if (index >= count) break;
    std::string argument;
    bool quoted = false;
    while (index < count) {
      if (!quoted && is_blank(raw[index])) break;
      std::size_t slashes = 0;
      while (index < count && raw[index] == '\\') {
        ++slashes;
        ++index;
      }
      if (index < count && raw[index] == '"') {
        argument.append(slashes / 2, '\\');
        if (slashes % 2 == 1) {
          argument.push_back('"');
          ++index;
        } else if (quoted && index + 1 < count && raw[index + 1] == '"') {
          argument.push_back('"');
          index += 2;
        } else {
          quoted = !quoted;
          ++index;
        }
      } else {
        argument.append(slashes, '\\');
        if (index < count) {
          if (!quoted && is_blank(raw[index])) break;
          argument.push_back(raw[index]);
          ++index;
        }
      }
    }
    result.push_back(std::move(argument));
  }
  return result;
}

}  // namespace playden::steam
