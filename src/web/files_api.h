// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web/file_store.h"
#include "web/pairing.h"
#include "web/router.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
  std::optional<std::string> physx_needed;          // needs PhysX: the version, empty when it isn't known
};

struct FileCatalog {
  std::vector<FileGame> games;
  bool setting_up = false;  // a game's setup holds the path lock: file actions answer 503 meanwhile
  bool known = false;       // false until the controller has reported the library
};

// What became of a recipe a paired browser sent for a game: an HTTP status and a sentence for the player.
struct RecipeImportAnswer {
  int status = 500;
  std::string message;
  std::string game;  // the FileGame id of the game that uses it now; empty when none does
};
// The largest recipe file the page may send (the recipe service's limit).
inline constexpr std::size_t kMaxRecipeUploadBytes = 16 << 10;

struct FilesConfig {
  std::function<std::shared_ptr<void>()> try_hold;  // ps5::TryHoldEffectiveUid; empty on the host
  std::function<std::optional<std::uint64_t>(const std::string& path)> free_space;
  std::vector<std::string> private_profile_paths;   // runtime::PrivateProfilePaths()
  Pairing::Options pairing;
  FileStore::Options store;  // tests; try_hold and free_space above take its fields' place
  std::function<std::int64_t()> now;  // seconds since 1970, stamped on zip entries
  // Uses a recipe file for a game (its FileGame id, or empty: the game the recipe is for), on the calling server worker;
  // the controller answers. Empty: the route answers 503.
  std::function<RecipeImportAnswer(const std::string& game, std::string file)> import_recipe;
  // How long POST /api/files/move waits for a move across roots before it answers 202 and the move goes on alone.
  int move_wait_ms = 1000;
};

// How many downloads, zips and uploads may run at once. The rest answer 503, so lists stay served.
inline constexpr int kMaxTransfers = 2;

// How long a finished move's answer stays for the page to ask for, and how many finished moves are kept at most.
inline constexpr int kMoveAnswerSeconds = 600;
inline constexpr std::size_t kMaxKeptMoves = 16;

// The Files tab's routes, and POST /api/files/recipe, which makes a game use a recipe file. Every route but pairing's needs a paired browser's cookie; everything that changes
// something also needs X-Playden-Action: files (Service refuses another Origin, and a name in Host, before this).
class FilesApi {
 public:
  explicit FilesApi(FilesConfig config);
  // Stops a move still running at the next chunk (the file it copies stays where it was) and waits for it.
  ~FilesApi();
  // The same, for the service's shutdown: no move starts after it. Safe to call again.
  void StopMoves();
  FilesApi(const FilesApi&) = delete;
  FilesApi& operator=(const FilesApi&) = delete;
  void Register(Router& router);
  void SetCatalog(FileCatalog catalog);
  std::string PairingCode() const;
  void CancelPairing();
  // A paired browser asked for something in the last half a minute, or a move it started still runs.
  bool InUse() const;
  // Browsers paired now, remembered or not (no file access), and Forget all.
  std::size_t PairedBrowsers() const;
  void ForgetBrowsers();
  // The remembered browsers' file: changed tells the caller's worker to call DoStoreWork, which reads and writes it.
  void SetStoreChanged(std::function<void()> changed);
  Pairing::StoreWork DoStoreWork(std::string* log);

 private:
  class Transfer;
  struct MoveJob;
  // Finished moves past their time, and their threads. Holds jobs_mutex_.
  void ReapMoves();
  bool Paired(const Request& request);
  Pairing::Browser Check(const std::string& token);
  bool Remembers() const;
  std::string SessionCookie(const std::string& token, bool remembered) const;
  bool RequirePaired(const Request& request, Response& response);
  bool RequireAction(const Request& request, Response& response);
  // The game and root a request names. False: answered.
  bool Find(const Request& request, Response& response, FileGame* game, FileRoot* root);
  std::optional<FileGame> Game(const std::string& id) const;
  bool SettingUp() const;
  std::vector<std::pair<std::string, FileRoot>> Roots(const FileGame& game) const;

  FilesConfig config_;
  mutable Pairing pairing_;
  FileStore store_;
  mutable std::mutex mutex_;
  FileCatalog catalog_;
  std::atomic<int> transfers_{0};
  std::atomic<std::int64_t> last_used_{0};  // steady seconds; 0: never
  // Moves across roots run on their own thread: they take as long as an upload, and the page asks how far they are.
  std::mutex jobs_mutex_;
  std::condition_variable jobs_changed_;
  std::map<std::string, std::shared_ptr<MoveJob>> jobs_;
  std::uint64_t next_job_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<int> running_moves_{0};
};

}  // namespace playden::web
