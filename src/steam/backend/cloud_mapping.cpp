#include "steam/backend/cloud_mapping.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace playden::steam::cloud {
namespace {

std::string Lower(std::string_view text) {
  std::string result(text);
  for (auto& value : result) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return result;
}

std::string Join(const std::string& left, const std::string& right) {
  if (left.empty()) return right;
  if (right.empty()) return left;
  return left + "/" + right;
}

std::string Replace(std::string text, const std::string& find, const std::string& replace) {
  for (std::size_t at = text.find(find); at != std::string::npos; at = text.find(find, at + replace.size()))
    text.replace(at, find.size(), replace);
  return text;
}

// Path segments must be plain names: no empty, "." or ".." parts and no characters that would
// leave a placeholder unresolved or point outside the root.
bool SafeComponents(std::string_view path) {
  if (path.empty()) return true;
  if (path.front() == '/') return false;
  for (const char value : path)
    if (value == '%' || value == '{' || value == '}' || value == '*' || value == '?' || value == '<' ||
        value == '>' || value == '|' || value == '\r' || value == '\n' || value == '\0' || value == ':' || value == '\\')
      return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = std::min(path.find('/', start), path.size());
    const auto part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    start = end + 1;
  }
  return true;
}

// A declared path: backslashes become slashes, trailing separators go, absolute paths are refused.
std::optional<std::string> RelativePath(std::string path, std::uint64_t steam_id, bool* needs_account) {
  std::replace(path.begin(), path.end(), '\\', '/');
  if (!path.empty() && path.front() == '/') return std::nullopt;
  while (!path.empty() && path.back() == '/') path.pop_back();
  if (path.find("{64BitSteamID}") != std::string::npos || path.find("{Steam3AccountID}") != std::string::npos) {
    if (steam_id == 0) {
      *needs_account = true;
      return std::nullopt;
    }
    path = Replace(path, "{64BitSteamID}", std::to_string(steam_id));
    path = Replace(path, "{Steam3AccountID}", std::to_string(steam_id & 0xffffffffULL));
  }
  if (!SafeComponents(path)) return std::nullopt;
  return path;
}

bool ValidPattern(std::string_view pattern) {
  if (pattern.empty() || pattern == "." || pattern == "..") return false;
  for (const char value : pattern)
    if (value == '/' || value == '\\' || value == '\0' || value == ':' || value == '[' || value == ']' || value == '{' ||
        value == '}' || value == '\r' || value == '\n')
      return false;
  return true;
}

// Where each Windows root lives in a prospero-win style prefix.
std::optional<std::pair<SaveRoot, std::string>> Base(SavePathRoot root) {
  const std::string user = kPrefixUserFolder;
  switch (root) {
    case SavePathRoot::kGameInstall: return std::make_pair(SaveRoot::kGame, std::string());
    case SavePathRoot::kWinMyDocuments: return std::make_pair(SaveRoot::kPrefix, user + "/Documents");
    case SavePathRoot::kWinAppDataLocal: return std::make_pair(SaveRoot::kPrefix, user + "/AppData/Local");
    case SavePathRoot::kWinAppDataLocalLow: return std::make_pair(SaveRoot::kPrefix, user + "/AppData/LocalLow");
    case SavePathRoot::kWinAppDataRoaming: return std::make_pair(SaveRoot::kPrefix, user + "/AppData/Roaming");
    case SavePathRoot::kWinSavedGames: return std::make_pair(SaveRoot::kPrefix, user + "/Saved Games");
    case SavePathRoot::kWinProgramData: return std::make_pair(SaveRoot::kPrefix, std::string("drive_c/ProgramData"));
    case SavePathRoot::kRoot: return std::make_pair(SaveRoot::kPrefix, user);
    default: return std::nullopt;
  }
}

// Steam writes both "%Root%/folder" and "%Root%folder"; normalize only that separator and the
// root aliases. Bare user-data names lose their token.
std::optional<std::string> Normalized(const std::string& raw) {
  std::string name = raw;
  std::replace(name.begin(), name.end(), '\\', '/');
  if (!name.empty() && name.front() == '%') {
    const auto end = name.find('%', 1);
    if (end == std::string::npos || end == 1) return std::nullopt;
    const auto token = name.substr(1, end - 1);
    for (const char value : token)
      if (!(std::isalnum(static_cast<unsigned char>(value)) || value == '_')) return std::nullopt;
    std::string remainder = name.substr(end + 1);
    if (!remainder.empty() && remainder.front() == '/') remainder.erase(remainder.begin());
    if (!SafeComponents(remainder)) return std::nullopt;
    const auto lower = Lower(token);
    std::string root = "%" + token + "%";
    if (lower == "steamuserdata" || lower == "steamuserbasestorage") return remainder;
    if (lower == "steamclouddocuments") root = "%WinMyDocuments%";
    if (lower == "windowshome" || lower == "root_mod") root = "%Root%";
    return root + (remainder.empty() ? "" : "/" + remainder);
  }
  if (!SafeComponents(name)) return std::nullopt;
  return name;
}

std::optional<std::string> Suffix(const std::string& name, const std::string& prefix) {
  if (prefix.empty()) return name.empty() ? std::nullopt : std::optional<std::string>(name);
  const std::string start = prefix.back() == '/' ? prefix : prefix + "/";
  if (name.size() <= start.size() || Lower(name.substr(0, start.size())) != Lower(start)) return std::nullopt;
  return name.substr(start.size());
}

bool Matches(const std::string& suffix, const SaveRule& rule) {
  if (suffix.empty() || (!rule.recursive && suffix.find('/') != std::string::npos)) return false;
  const auto slash = suffix.rfind('/');
  return GlobMatch(rule.pattern, slash == std::string::npos ? suffix : suffix.substr(slash + 1));
}

bool SameRule(const SaveRule& left, const SaveRule& right) {
  return left.root == right.root && left.directory == right.directory && left.pattern == right.pattern &&
         left.recursive == right.recursive && left.cloud_prefix == right.cloud_prefix;
}

// A macOS or Linux save location listed beside the Windows ones without "platforms" (Cat Quest lists one per OS):
// its root is still the other OS's, or a Windows override sends it to the folder of a Windows entry, whose saves the
// Windows client keeps under that entry's Cloud names. The Mac or Linux client syncs its files; the sync leaves them
// alone (they match no rule), so progress kept only under the Mac names stays in the Cloud, not on the console. One a
// Windows override sends to a folder of its own stays unresolved. Paths and patterns match without case, as files do.
bool OtherPlatformCopy(const CloudSaveInfo& info, const SaveFilePattern& item) {
  if (item.upload_root == SavePathRoot::kNone || IsWindowsSavePathRoot(item.upload_root)) return false;
  if (!IsWindowsSavePathRoot(item.root)) return true;
  const auto folder = [](const std::string& path) {
    auto lower = Lower(path);
    while (!lower.empty() && lower.back() == '/') lower.pop_back();
    return lower;
  };
  return std::any_of(info.patterns.begin(), info.patterns.end(), [&](const SaveFilePattern& other) {
    return IsWindowsSavePathRoot(other.upload_root) && other.root == item.root && folder(other.path) == folder(item.path) &&
           Lower(other.pattern) == Lower(item.pattern) && other.recursive == item.recursive;
  });
}

}  // namespace

std::string LocalSavePath::key() const { return (root == SaveRoot::kGame ? "game/" : "prefix/") + Lower(path); }

bool GlobMatch(std::string_view pattern, std::string_view text) {
  // Iterative wildcard matching with single-star backtracking.
  std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
  const auto same = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  };
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], text[t]))) { ++p; ++t; }
    else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
    else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
    else return false;
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

SaveMapping BuildSaveMapping(const CloudSaveInfo& info, std::uint32_t app_id, std::uint64_t steam_id) {
  SaveMapping mapping;
  if (!info.declared) {
    mapping.unresolved.push_back("This game does not use Steam Cloud.");
    return mapping;
  }
  // The Steam emulator keeps ISteamRemoteStorage files under <appid>/remote: bare Cloud names land there.
  const std::string remote = std::string(kEmulatorUserData) + "/" + std::to_string(app_id) + "/remote";
  if (info.quota > 0 || info.max_num_files > 0 || !info.patterns.empty())
    mapping.rules.push_back({SaveRoot::kPrefix, remote, "*", true, ""});
  for (const auto& item : info.patterns) {
    bool needs_account = false;
    const auto path = RelativePath(item.path, steam_id, &needs_account);
    const auto upload_path = RelativePath(item.upload_path, steam_id, &needs_account);
    const auto base = item.root == SavePathRoot::kSteamUserData ? std::make_optional(std::make_pair(SaveRoot::kPrefix, remote))
                                                                : Base(item.root);
    if (OtherPlatformCopy(info, item)) continue;
    if (needs_account) {
      mapping.unresolved.push_back("Sign in to resolve this game's account save folder.");
      continue;
    }
    if (!base || !path || !upload_path || !IsWindowsSavePathRoot(item.upload_root) || !ValidPattern(item.pattern) ||
        (item.recursive != 0 && item.recursive != 1)) {
      mapping.unresolved.push_back("A save location is not supported yet.");
      continue;
    }
    SaveRule rule;
    rule.root = base->first;
    rule.directory = Join(base->second, *path);
    rule.pattern = item.pattern;
    rule.recursive = item.recursive == 1;
    rule.cloud_prefix = item.upload_root == SavePathRoot::kSteamUserData
                            ? *upload_path
                            : "%" + std::string(SavePathRootName(item.upload_root)) + "%" + *upload_path;
    if (std::none_of(mapping.rules.begin(), mapping.rules.end(), [&](const SaveRule& other) { return SameRule(other, rule); }))
      mapping.rules.push_back(std::move(rule));
  }
  std::sort(mapping.unresolved.begin(), mapping.unresolved.end());
  mapping.unresolved.erase(std::unique(mapping.unresolved.begin(), mapping.unresolved.end()), mapping.unresolved.end());
  return mapping;
}

SavePaths::SavePaths(SaveMapping mapping) {
  for (auto& rule : mapping.rules)
    if (SafeComponents(rule.directory) && ValidPattern(rule.pattern) && Normalized(rule.cloud_prefix))
      rules_.push_back(std::move(rule));
}

std::optional<LocalSavePath> SavePaths::LocalFor(const std::string& remote_name, std::string* error) const {
  const auto fail = [&](const char* why) -> std::optional<LocalSavePath> {
    if (error) *error = why;
    return std::nullopt;
  };
  const auto name = Normalized(remote_name);
  if (!name) return fail("A Cloud save name is not supported.");
  // The most specific (longest) matching prefix wins; equally specific rules must agree.
  std::vector<std::pair<std::size_t, LocalSavePath>> matches;
  for (const auto& rule : rules_) {
    const auto prefix = Normalized(rule.cloud_prefix).value_or("");
    // The bare-name fallback must never swallow an unknown %Root% name.
    if (prefix.empty() && !name->empty() && name->front() == '%') continue;
    const auto suffix = Suffix(*name, prefix);
    if (!suffix || !Matches(*suffix, rule)) continue;
    const auto local = Join(rule.directory, *suffix);
    if (SafeComponents(local)) matches.push_back({prefix.size(), {rule.root, local}});
  }
  if (matches.empty()) return std::nullopt;
  std::size_t longest = 0;
  for (const auto& match : matches) longest = std::max(longest, match.first);
  std::set<std::string> keys;
  const LocalSavePath* chosen = nullptr;
  for (const auto& match : matches) {
    if (match.first != longest) continue;
    keys.insert(match.second.key());
    chosen = &match.second;
  }
  if (keys.size() != 1) return fail("A Cloud save matches more than one local folder.");
  return *chosen;
}

std::optional<std::string> SavePaths::RemoteFor(const LocalSavePath& local) const {
  if (!SafeComponents(local.path)) return std::nullopt;
  std::set<std::string> names;
  std::string chosen;
  for (const auto& rule : rules_) {
    if (rule.root != local.root) continue;
    const auto suffix = Suffix(local.path, rule.directory);
    if (!suffix || !Matches(*suffix, rule)) continue;
    std::string prefix = rule.cloud_prefix;
    std::replace(prefix.begin(), prefix.end(), '\\', '/');
    const std::string name = prefix + (prefix.empty() || prefix.back() == '/' || prefix.back() == '%' ? "" : "/") + *suffix;
    const auto back = LocalFor(name);
    if (back && back->key() == local.key()) {
      names.insert(Lower(name));
      chosen = name;
    }
  }
  if (names.size() != 1) return std::nullopt;
  return chosen;
}

}  // namespace playden::steam::cloud
