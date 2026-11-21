// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime/publisher_pack.h"

#include <functional>
#include <string>
#include <vector>

namespace playden::runtime {

// Where builds.playden.club serves each pinned pack: <base><version>/<archive>. Only this prefix is downloaded from.
inline constexpr const char* kPublisherPackBaseUrl = "https://builds.playden.club/packs/";
std::string PublisherArchiveUrl(const std::string& version, const PublisherRelease& release);

// The release.json Playden writes beside the archives it installs: the version, and each archive's pinned SHA-256
// and tree hash (the trimmed file builds.playden.club hosts). CheckPublisherPack reads it like a full helper-tools one.
std::string PublisherReleaseJson(const std::string& version, const std::vector<PublisherRelease>& releases);

struct PublisherPackInstall {
  std::string pack_root;  // /data/playden/publisher
  std::string version;    // kPublisherPackVersion
  PublisherRelease release;
  // Unpacked copies of archives (PreparePublisherRelease); every one but this release's is removed. Empty: kept.
  std::string cache_root;
  // Runs work with root's effective user id: the pack folder may be root's (an FTP upload). Empty: runs it as is.
  std::function<bool(const std::function<bool()>& work)> as_root;
  std::function<void(const std::string& line)> log;
};

// Puts a downloaded archive in place, as tools/console_publisher_upload.py and the builds page's Install button do:
// the archive (copied, then read back against the pin) and release.json go into <pack_root>/.<version>.download
// (not their ".upload", which one of them may be writing), which then replaces <pack_root>/<version>. Every other
// version, and Playden's own staging folder left behind, is removed.
// The archive must match the release's pinned SHA-256. Folders are 0755 and files 0644. On failure the installed
// pack, if any, is left as it was, unless it was this version's (damaged: it is the one being replaced).
bool InstallPublisherPack(const PublisherPackInstall& install, const std::string& archive, std::string* reason);

}  // namespace playden::runtime
