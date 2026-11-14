// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "library/store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace playden::stores {

// One Windows launch option from the store's game details, as the store wrote it.
struct LaunchOption {
  std::string executable;
  std::string arguments;
  std::string working_directory;
  std::string description;
};

// A version (Steam branch) an install can choose, with what installing it takes.
struct AppVersion {
  std::string name;  // the branch as Steam names it; "public" for the default
  std::string description;
  std::int64_t time_updated = 0;  // unix seconds; 0 = unknown
  std::uint64_t build_id = 0;
  std::uint64_t download_bytes = 0;  // 0 = unknown
  std::uint64_t install_bytes = 0;
  // The sizes for each content language the version carries, when an install picks another one.
  struct LanguageSize {
    std::string language;
    std::uint64_t download_bytes = 0;
    std::uint64_t install_bytes = 0;
  };
  std::vector<LanguageSize> languages;
};

// A DLC an install can include or leave out. Its sizes are for the public branch in the Settings language.
struct DlcChoice {
  GameId id;  // the DLC's own ID in the game's store
  std::string name;  // empty: the store doesn't describe it
  std::uint64_t download_bytes = 0;
  std::uint64_t install_bytes = 0;
  bool free = false;  // not owned yet: an install claims it
  // Where another version or language differs. No install bytes: this choice doesn't have the DLC.
  struct OtherSize {
    std::string branch;
    std::string language;
    std::uint64_t download_bytes = 0;
    std::uint64_t install_bytes = 0;
    bool optional = false;  // the store doesn't download it by itself on this choice
  };
  std::vector<OtherSize> other_sizes;
  bool optional = false;  // the store doesn't download it by itself: an install starts with it off, if it is owned
};

// What a store says about installing one game on this console: sizes, whether it installs here, its launch options.
struct GameDetails {
  GameId id;
  std::uint64_t account_generation = 0;
  bool available = false;  // false: the store had no usable details (see reason)
  std::optional<std::uint64_t> download_bytes;
  std::optional<std::uint64_t> install_bytes;
  bool windows_installable = false;  // depots and a launch entry exist for Windows/English
  std::string reason;                // short, user-facing, no identifiers
  std::vector<LaunchOption> launches;  // Windows launch options, default first
  // No option was for the public branch, so `launches` are every branch's: Steam doesn't say
  // which program starts the game.
  bool launches_any_branch = false;
  // The game's folder in a Steam library (config/installdir), where a runtime shows it to Wine; empty if unknown.
  std::string install_dir;
  // The content languages the game's Windows depots carry, as Steam names them ("german"), in
  // listing order; empty when nothing is localised. An install can pick one of them.
  std::vector<std::string> languages;
  // The DLC with files an install can leave out, owned or free to claim (OfferedDlc). An installed game's are its
  // record's branch and language, with no other sizes.
  std::vector<DlcChoice> dlc;
  // The versions an install can choose: the public branch first, then branches with no password
  // that carry the game's own content, the most recently updated first. One entry: nothing to choose.
  std::vector<AppVersion> versions;
  // An installed game: what the store has now against its install record (selected with the record's branch
  // and language). `newer`: a depot has another manifest, is new, or left the selection; `new_dlc`: DLC
  // the account owns now that the install doesn't hold. `target` names the selection updated to
  // (UpdateTarget): its download size is measured from the manifests (a SizeRequest with `update`). All
  // false when the licenses or the record's depots are unknown.
  struct Update {
    bool newer = false;
    bool new_dlc = false;
    std::uint64_t target = 0;
    bool available() const { return newer || new_dlc; }
  };
  Update update;
  // The store gave no answer for the game (no session, the network): asking again may work. Not set when the
  // store answered that it has no usable details.
  bool failed = false;
};

}  // namespace playden::stores
