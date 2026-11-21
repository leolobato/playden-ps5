#include "runtime/publisher_detect.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
  return text;
}

struct Listing {
  std::vector<std::string> files;    // regular files
  std::vector<std::string> folders;  // directories, not links
};

Listing List(const std::string& directory) {
  Listing listing;
  DIR* dir = ::opendir(directory.c_str());
  if (!dir) return listing;
  while (const dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    struct stat info {};
    if (::lstat((directory + "/" + name).c_str(), &info) != 0) continue;
    if (S_ISREG(info.st_mode)) listing.files.push_back(name);
    else if (S_ISDIR(info.st_mode)) listing.folders.push_back(name);
  }
  ::closedir(dir);
  return listing;
}

// The real name of a regular file in listing whose name matches without case.
std::optional<std::string> FindFile(const Listing& listing, const std::string& name) {
  const auto wanted = Lower(name);
  for (const auto& file : listing.files)
    if (Lower(file) == wanted) return file;
  return std::nullopt;
}

bool StartsWithMagic(const std::string& path, const char* magic, std::size_t size) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  char head[8] = {};
  ssize_t got;
  do got = ::read(fd, head, size);
  while (got < 0 && errno == EINTR);
  ::close(fd);
  return got == static_cast<ssize_t>(size) && std::equal(head, head + size, magic);
}

bool StartsWithNoCase(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && Lower(text.substr(0, prefix.size())) == prefix;
}

}  // namespace

PublisherDetection DetectPublisher(const std::string& install_root, const std::vector<std::string>& launch_targets) {
  PublisherDetection detection;
  const auto found = [&](std::string evidence, std::string title) {
    detection.found = true;
    detection.evidence = std::move(evidence);
    detection.title_directory = std::move(title);
    return detection;
  };
  struct stat info {};
  if (::lstat(install_root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return detection;
  const auto root = List(install_root);

  // EA's launcher comes first: a game it starts is not Rockstar's, even with Social Club files beside it.
  for (const auto& target : launch_targets)
    if (StartsWithNoCase(target, "link2ea://") || StartsWithNoCase(target, "steam2ea://")) {
      detection.evidence = "EA: " + target.substr(0, target.find("://") + 3);
      return detection;
    }
  for (const auto& folder : root.folders)
    if (Lower(folder) == "__installer" && FindFile(List(install_root + "/" + folder), "installerdata.xml")) {
      detection.evidence = "EA: __Installer/installerdata.xml";
      return detection;
    }

  // Rockstar: the root, then direct child folders in case-insensitive order.
  std::vector<std::string> candidates{""};
  auto children = root.folders;
  children.erase(std::remove_if(children.begin(), children.end(), [](const std::string& name) { return name[0] == '.'; }),
                 children.end());
  std::sort(children.begin(), children.end(), [](const std::string& a, const std::string& b) {
    const auto la = Lower(a), lb = Lower(b);
    return la != lb ? la < lb : a < b;
  });
  candidates.insert(candidates.end(), children.begin(), children.end());
  for (const auto& candidate : candidates) {
    const auto folder = candidate.empty() ? install_root : install_root + "/" + candidate;
    const auto listing = candidate.empty() ? root : List(folder);
    if (const auto rgl = FindFile(listing, "title.rgl"); rgl && StartsWithMagic(folder + "/" + *rgl, "RGLM", 4))
      return found("title.rgl", candidate);
    for (const char* name : {"socialclub.dll", "socialclub64.dll"})
      if (FindFile(listing, name)) return found(name, candidate);
  }

  return detection;
}

}  // namespace playden::runtime
