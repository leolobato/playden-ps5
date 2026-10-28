#include "steam/backend/app_info.h"

#include <algorithm>
#include <charconv>

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

bool Fail(std::string* reason, const char* text) {
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
  const VdfNode* manifests = value.Find("manifests");
  const VdfNode* pub = manifests != nullptr ? manifests->Find("public") : nullptr;
  if (pub != nullptr) {
    // Old appinfo format: gid directly as a string; new: dict {gid, size, download}.
    depot.manifest_gid = pub->Uint64("gid");
    if (!depot.manifest_gid && !pub->is_dict) depot.manifest_gid = ParseU64(pub->value);
    depot.size = pub->Uint64("size").value_or(0);
    depot.download_size = pub->Uint64("download").value_or(0);
  }
  return depot;
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
        for (const auto& branch : entry.second.children) {
          const std::uint64_t build = branch.second.Uint64("buildid").value_or(0);
          auto existing = std::find_if(app.branches.begin(), app.branches.end(),
                                       [&](const auto& b) { return b.first == branch.first; });
          if (existing != app.branches.end()) {
            existing->second = build;
          } else {
            app.branches.emplace_back(branch.first, build);
          }
        }
        continue;
      }
      const auto id = ParseU32(entry.first);
      if (!id) continue;  // baselanguages, overridescddb, ...
      app.depots.push_back(ParseDepot(*id, entry.second));
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

bool SelectDepots(const AppInfo& app, const std::set<std::uint32_t>& owned_apps,
                  const std::optional<std::set<std::uint32_t>>& owned_depots,
                  std::vector<DepotInfo>* selected, std::string* reason) {
  if (owned_apps.count(app.app_id) == 0) {
    return Fail(reason, "This account does not own the selected game.");
  }
  std::set<std::uint32_t> accessible;
  if (owned_depots) {
    accessible = *owned_depots;
  } else {
    for (const DepotInfo& depot : app.depots) accessible.insert(depot.id);
  }
  for (const DepotInfo& depot : app.depots) {
    if (depot.depot_from_app) accessible.insert(depot.id);
  }

  std::vector<DepotInfo> eligible;
  for (const DepotInfo& depot : app.depots) {
    const bool language_ok = depot.language.empty() || LowerString(depot.language) == "english";
    const bool dlc_ok = (!depot.is_dlc && !depot.dlc_app_id) ||
                        (depot.dlc_app_id && owned_apps.count(*depot.dlc_app_id) != 0);
    if (OsListMatchesWindows(depot.os_list) && language_ok && !depot.shared_install &&
        accessible.count(depot.id) != 0 && dlc_ok) {
      eligible.push_back(depot);
    }
  }
  if (eligible.empty()) {
    return Fail(reason, "This game has no downloadable Windows content for English.");
  }
  bool has_win64 = false;
  for (const DepotInfo& depot : eligible) {
    if (LowerString(Trim(depot.os_arch)) == "64" && depot.manifest_gid && *depot.manifest_gid != 0) {
      has_win64 = true;
      break;
    }
  }
  std::vector<DepotInfo> result;
  std::set<std::uint32_t> ids;
  for (const DepotInfo& depot : eligible) {
    if (has_win64 && LowerString(Trim(depot.os_arch)) == "32") continue;
    if (depot.manifest_gid && *depot.manifest_gid != 0) {
      result.push_back(depot);
      ids.insert(depot.id);
    }
  }
  if (result.empty() || ids.size() != result.size()) {
    return Fail(reason, "The game's Windows content is not available on its public branch.");
  }
  std::sort(result.begin(), result.end(),
            [](const DepotInfo& a, const DepotInfo& b) { return a.id < b.id; });
  if (selected != nullptr) *selected = std::move(result);
  return true;
}

std::vector<std::uint32_t> MissingBaseDepots(const AppInfo& app,
                                             const std::optional<std::set<std::uint32_t>>& owned_depots,
                                             const std::vector<DepotInfo>& selected) {
  if (!owned_depots) return {};
  const auto base_content = [](const DepotInfo& depot) {
    return !depot.depot_from_app && !depot.shared_install && !depot.is_dlc && depot.os_list.empty() &&
           depot.language.empty() && depot.manifest_gid && *depot.manifest_gid != 0;
  };
  // A license that grants base content of its own holds the game: the base depots it leaves out
  // are alternates for other licenses (Marvel Rivals lists a second copy of its content), and
  // Steam installs only what the license grants.
  for (const DepotInfo& depot : selected)
    if (base_content(depot) && depot.size) return {};
  std::uint64_t selected_size = 0;
  for (const DepotInfo& depot : selected) selected_size += depot.size;
  std::vector<std::uint32_t> missing;
  std::uint64_t missing_size = 0;
  for (const DepotInfo& depot : app.depots) {
    if (owned_depots->count(depot.id) != 0 || !base_content(depot)) continue;
    missing.push_back(depot.id);
    missing_size += depot.size;
  }
  // An unowned extra (a bonus or edition depot) is smaller than the game; old appinfo has no sizes.
  if (missing_size == 0 || missing_size <= selected_size) return {};
  return missing;
}

bool BetaKeyMatchesPublic(std::string_view beta_key) {
  if (Trim(beta_key).empty()) return true;
  // A comma list of branches (Minecraft Dungeons II lists "public, build_debug, ..."). "default"
  // tags the public branch's option when other branches have their own (Half-Life 2, issue #10).
  while (!beta_key.empty()) {
    const std::size_t comma = beta_key.find(',');
    const auto branch = LowerString(Trim(beta_key.substr(0, comma)));
    if (branch == "public" || branch == "default") return true;
    if (comma == std::string_view::npos) break;
    beta_key.remove_prefix(comma + 1);
  }
  return false;
}

bool EligibleLaunches(const AppInfo& app, const std::set<std::uint32_t>& owned_apps,
                      std::vector<AppLaunch>* launches, std::string* reason, bool* any_branch) {
  const auto eligible_for = [&](bool ignore_branch) {
    std::vector<AppLaunch> eligible;
    for (const AppLaunch& launch : app.launches) {
      const bool beta_ok = ignore_branch || !launch.beta_key || BetaKeyMatchesPublic(*launch.beta_key);
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
