#pragma once

#include "runtime/publisher_pack.h"

#include <string>
#include <vector>

namespace playden::runtime {

// Steam games that start through Rockstar Games' launcher (the macOS PublisherPreparation.publisher
// rules for Rockstar, after EA's).
struct PublisherDetection {
  bool found = false;
  Publisher publisher = Publisher::kRockstar;
  std::string title_directory;  // the folder with title.rgl or Social Club, relative ("" = root)
  std::string evidence;         // for logs: what matched, or why a game is not Rockstar's
};

// install_root: the game folder. launch_targets: the Steam launch entries' executables as
// listed (relative paths with '/' or '\', or publisher URIs such as link2ea://...).
// A game EA's launcher starts (a link2ea:// or steam2ea:// entry, or __Installer/installerdata.xml) is
// not found, whatever else it holds. Rockstar: title.rgl starting with "RGLM", or socialclub.dll /
// socialclub64.dll, in the root or a direct child folder (hidden entries and links skipped, folders in case-insensitive order).
// Names are compared without case; links are never followed.
PublisherDetection DetectPublisher(const std::string& install_root, const std::vector<std::string>& launch_targets);

}  // namespace playden::runtime
