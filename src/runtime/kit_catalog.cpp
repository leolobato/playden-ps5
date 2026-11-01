#include "runtime/kit_catalog.h"

#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace playden::runtime {
namespace {

bool RemoveTreeNoFollow(const std::string& path) {
  struct stat info {};
  if (lstat(path.c_str(), &info) != 0) return true;
  if (S_ISLNK(info.st_mode)) return unlink(path.c_str()) == 0;
  if (!S_ISDIR(info.st_mode)) return unlink(path.c_str()) == 0;
  DIR* folder = opendir(path.c_str());
  if (!folder) return false;
  bool ok = true;
  while (const dirent* entry = readdir(folder)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
    ok = RemoveTreeNoFollow(path + "/" + entry->d_name) && ok;
  }
  closedir(folder);
  return rmdir(path.c_str()) == 0 && ok;
}

bool Pinned(const std::vector<std::string>& pinned, const std::string& version) {
  for (const auto& pin : pinned)
    if (pin == version) return true;
  return false;
}

}  // namespace

std::vector<std::string> PruneUnpinnedKits(const std::string& kits_root, std::string_view kit_id,
                                           const std::vector<std::string>& pinned,
                                           const std::function<bool(const std::string& path)>& admits) {
  std::vector<std::string> removed;
  DIR* folder = opendir(kits_root.c_str());
  if (!folder) return removed;
  std::vector<std::string> names;
  while (const dirent* entry = readdir(folder)) {
    if (entry->d_name[0] == '.') continue;
    names.emplace_back(entry->d_name);
  }
  closedir(folder);
  for (const auto& name : names) {
    std::string version;
    if (!ParkedKitVersion(kit_id, name, &version) || Pinned(pinned, version)) continue;
    const std::string path = kits_root + "/" + name;
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) continue;
    if (!admits || !admits(path)) continue;
    if (RemoveTreeNoFollow(path)) removed.push_back(path);
  }
  return removed;
}

}  // namespace playden::runtime
