// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web/file_store.h"
#include "web/pairing.h"
#include "web/router.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace playden::web {

// A game the Files tab offers, as the controller reports it. No path comes from a request: a request names a game's
// id and one of its roots, and these fields say where those are.
struct FileGame {
  std::string id;    // "steam-220", "local-<uuid>"
  std::string name;
  std::string kind;  // "steam" or "imported"
  std::uint32_t app = 0;  // a Steam game's app ID; 0: none
  std::string bottle;       // the private bottle (holding drive_c), as this process opens it; empty: none yet
  std::string game_folder;  // the game's folder, as this process opens it; empty: not installed
  std::string game_folder_shown;  // the same, as the console names it
  std::string drive;  // where the game folder is: "console", "usb", "m2"; empty: none
  bool drive_absent = false;  // its drive isn't connected
  bool installed = true;      // false: uninstalled, its bottle kept
  bool running = false;       // its PS5 title runs or is suspended
  bool setting_up = false;    // this game's setup is what's running
  bool cloud = false;         // Cloud sync is on for it
  std::optional<std::int64_t> last_played;
  std::optional<std::uint64_t> windows_user_bytes;  // all of drive_c/users, measured earlier
  std::optional<std::string> physx_needed;          // needs PhysX: the version, empty when it isn't known
};

struct FileCatalog {
  std::vector<FileGame> games;
  bool setting_up = false;  // a game's setup holds the path lock: file actions answer 503 meanwhile
  bool known = false;       // false until the controller has reported the library
};

struct FilesConfig {
  std::function<std::shared_ptr<void>()> try_hold;  // ps5::TryHoldEffectiveUid; empty on the host
  std::function<std::optional<std::uint64_t>(const std::string& path)> free_space;
  std::vector<std::string> private_profile_paths;   // runtime::PrivateProfilePaths()
  Pairing::Options pairing;
  FileStore::Options store;  // tests; try_hold and free_space above take its fields' place
  std::function<std::int64_t()> now;  // seconds since 1970, stamped on zip entries
};

// How many downloads, zips and uploads may run at once. The rest answer 503, so lists stay served.
inline constexpr int kMaxTransfers = 2;

// The Files tab's routes. Every route but pairing's needs a paired browser's cookie; everything that changes
// something also needs X-Playden-Action: files (Service refuses another Origin, and a name in Host, before this).
class FilesApi {
 public:
  explicit FilesApi(FilesConfig config);
  void Register(Router& router);
  void SetCatalog(FileCatalog catalog);
  std::string PairingCode() const;
  void CancelPairing();

 private:
  class Transfer;
  bool Paired(const Request& request);
  bool RequirePaired(const Request& request, Response& response);
  bool RequireAction(const Request& request, Response& response);
  // The game and root a request names. False: answered.
  bool Find(const Request& request, Response& response, FileGame* game, FileRoot* root);
  std::optional<FileGame> Game(const std::string& id) const;
  std::vector<std::pair<std::string, FileRoot>> Roots(const FileGame& game) const;

  FilesConfig config_;
  mutable Pairing pairing_;
  FileStore store_;
  mutable std::mutex mutex_;
  FileCatalog catalog_;
  std::atomic<int> transfers_{0};
};

}  // namespace playden::web
