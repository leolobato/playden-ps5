#pragma once

#include "runtime/publisher_pack.h"

#include <string>
#include <vector>

namespace playden::runtime {

// Steam games that start through their publisher's launcher (the macOS
// PublisherPreparation.publisher rules, in the same order).
struct PublisherDetection {
  bool found = false;
  Publisher publisher = Publisher::kSteamHost;  // kEa, kRockstar or kUbisoft when found
  std::string title_directory;  // Rockstar: the folder with title.rgl or Social Club, relative ("" = root)
  std::string evidence;         // for logs: what matched
};

// install_root: the game folder. launch_targets: the Steam launch entries' executables as
// listed (relative paths with '/' or '\', or publisher URIs such as link2ea://...).
// EA: a link2ea:// or steam2ea:// entry, or __Installer/installerdata.xml.
// Rockstar: title.rgl starting with "RGLM", or socialclub.dll / socialclub64.dll, in the root or a
// direct child folder (hidden entries and links skipped, folders in case-insensitive order).
// Ubisoft: an Ubisoft Connect loader DLL in the root or beside a launch program.
// Names are compared without case; links are never followed.
PublisherDetection DetectPublisher(const std::string& install_root, const std::vector<std::string>& launch_targets);

}  // namespace playden::runtime
