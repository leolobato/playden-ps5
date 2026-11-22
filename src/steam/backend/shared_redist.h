// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/cm_client.h"
#include "steam/backend/depot_installer.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace playden::steam {

// Steam's shared redistributables: app 228980, "Steamworks Common Redistributables". A game lists the depots it
// needs (Visual C++, DirectX, .NET, PhysX) with depotfromapp 228980 and sharedinstall 1, and Steam installs each one
// once for every game. Playden keeps a depot the same way: downloaded once per console into a cache, with its
// manifest and chunks checked as a game's are (DepotInstaller), and never part of a game's own install.
inline constexpr std::uint32_t kCommonRedistApp = 228980;
// The largest depot the cache takes: the Visual C++ depots are 6 to 30 MB; .NET's reach 270 MB.
inline constexpr std::uint64_t kMaxSharedDepotBytes = 64ull << 20;

struct SharedDepotResult {
  enum class Status {
    kReady,        // folder holds the depot's files
    kNotCached,    // not downloaded yet, and this call doesn't download
    kRefused,      // Steam refused the depot's key or manifest to this account
    kUnavailable,  // no session, Steam or its servers can't be reached now: try again later
    kFailed,       // anything else (the depot isn't listed, is too large, or its download failed)
    kCanceled,
  };
  Status status = Status::kFailed;
  std::string folder;
  std::string file;         // kReady with a file name: that file's path in folder
  bool downloaded = false;  // kReady: fetched by this call, not the kept copy
  std::string detail;       // short and identifier-free
};
const char* SharedDepotStatusName(SharedDepotResult::Status status);

// The kept copy of a depot: <cache_root>/<depot>, once DepotInstaller's record there names that depot of app
// 228980. A download that stopped part way leaves no record (its journal resumes it) and isn't a copy.
std::optional<std::string> CachedSharedDepot(const std::string& cache_root, std::uint32_t depot);

// A file of a kept depot, by name (any case): its path when the file tree DepotInstaller kept lists it and the file
// has that size and SHA-1, else nullopt with the reason in *why.
std::optional<std::string> VerifiedSharedDepotFile(const std::string& folder, const std::string& name, std::string* why);

class SharedRedistCache {
 public:
  using AppInfoFetch = std::function<AppInfoBuffers(const StoredSession&, const std::vector<std::uint32_t>&,
                                                    const std::atomic<bool>&)>;
  // installer downloads the depots; app_info reads 228980's appinfo for each depot's public manifest.
  SharedRedistCache(AppInfoFetch app_info, DepotInstaller& installer);

  // The depot's folder: the kept copy, else (with download and a session) its public manifest's files, downloaded
  // into <cache_root>/<depot> under app 228980. A depot larger than kMaxSharedDepotBytes is not downloaded. With a
  // file_name, also that file (by name, any case), which must match the size and SHA-1 the depot's file tree lists:
  // a kept copy without it, or with a changed one, is downloaded again (and is kNotCached without download).
  SharedDepotResult Get(const StoredSession* session, std::uint32_t depot, const std::string& cache_root, bool download,
                        const std::atomic<bool>& cancel, const std::string& file_name = {});

 private:
  AppInfoFetch app_info_;
  DepotInstaller& installer_;
};

}  // namespace playden::steam
