#pragma once
// gog_wiring.hpp - GOG in the Mac and PS5 apps, behind features.txt's gog-store line (host.cpp and ps5.cpp make
// it the same way): the session file's store, the token owner, the account, the saved library, the library source,
// the details source (install sizes), the install runner and the phone relay, and how each reaches the registry, the
// install service, web access, the controller and "Stay signed in".
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/http.h"
#include "gog/session.h"
#include "gog/session_store.h"
#include "installs/install_record.h"
#include "library/store.h"
#include "library/cache.h"
#include "library/collections_cache.h"
#include "platform/storage.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace playden::installs {
class InstallRunner;
}  // namespace playden::installs
namespace playden::stores {
class GogAccount;
class GogDetailsSource;
class GogInstallRunner;
class GogLibrarySource;
class Registry;
}  // namespace playden::stores
namespace playden::web {
class GogRelay;
struct ServiceConfig;
}  // namespace playden::web

namespace playden::homebrew {

class Controller;
class Simulation;

struct GogWiringConfig {
  bool enabled = false;         // features.txt's gog-store line
  bool stay_signed_in = true;   // "Stay signed in" as saved at startup (steam_stay_signed_in: one setting for both)
  std::string user_root;        // absolute, no link: <user root>/gog holds the session and the saved products
  std::string ca_bundle;        // the pinned CA bundle every GOG host is checked against
  std::string log_directory;    // gog.log, web.log (the relay) and auth.log ("Stay signed in")
  Storage* library_storage = nullptr;  // library-gog-<account>.cache, beside Steam's library cache
  // Settings › Game language as a Steam code, read when a GOG download starts; empty: English.
  std::function<std::string()> content_language;
  std::string install_log_directory;  // install.log (the GOG runner's lines); empty: log_directory
  // An installed GOG game's record (none: not installed or its drive is away) and folder, for its updates and their
  // sizes; passed to the details source (GogDetailsSource::SetInstalled, SetInstallRoot). Called on its worker, which
  // the controller's Shutdown joins before the install service stops; the apps read the install service's snapshot,
  // which is made after these services. Empty: no GOG game is ever seen as installed, so none shows an update.
  std::function<std::optional<installs::InstalledRecord>(const GameId&)> installed_record;
  std::function<std::optional<std::string>(const GameId&)> install_root;
  // Tests: free bytes of the drive holding a folder, for the install runner. Empty: steam::QueryVolumeSpace.
  std::function<std::optional<std::uint64_t>(const std::string&)> free_bytes;
  // Tests: each call makes one transport, the session's first, then the library's, then the details source's (made on
  // its worker at its first request) and one for each thread of a GOG download (made while it runs). Empty:
  // gog::ClientTransport.
  std::function<gog::Transport()> transport;
  // Tests: the relay's tokens. Empty: /dev/urandom.
  std::function<std::optional<std::string>()> random_token;
};

// Every GOG object the apps keep, in the order they are made; destroyed in reverse. The registry owns the account and
// the library source once Register has run, so this must be declared before the registry (and the controller after
// both): the account and the library use the session, the session store and the saved library until they stop. The
// install service runs install_runner(), so this must also be declared before the install service: the runners
// outlive the service.
class GogServices {
 public:
  GogServices(const GogServices&) = delete;
  GogServices& operator=(const GogServices&) = delete;
  ~GogServices();

  // Adds the account, the library source and the details source to the registry, turns the line on in the model and
  // lets GOG games install (Simulation::SetStoreInstalls). Before the controller is made: it restores every registered
  // account and loads every library when it starts.
  void Register(stores::Registry& registry, Simulation& model);
  // GET /gog and POST /api/gog/sign-in, and the "Open GOG sign-in" link. Before web::Service is made.
  void ConfigureWeb(web::ServiceConfig& config) const;
  // The TV's GOG sign-in opens the relay. The controller's Shutdown closes it and stops the account (answering a
  // phone post still waiting) before web access's workers are joined.
  void Attach(Controller& controller) const;
  // "Stay signed in" changed: on the account worker, the GOG session file is written (on) or deleted with its
  // temporary files (off), and auth.log gets "stay signed in gog moved=<0|1>". Returns at once.
  void SetStaySignedIn(bool stay);

  gog::SessionStore& session_store() { return *store_; }
  gog::Session& session() { return *session_; }
  stores::GogAccount* account() const { return account_; }
  stores::GogLibrarySource* library() const { return library_; }
  stores::GogDetailsSource* details() const { return details_; }
  // The install service's runner for Store::kGog. Made with the services; used before the controller exists.
  installs::InstallRunner* install_runner() const;
  const std::shared_ptr<web::GogRelay>& relay() const { return relay_; }

 private:
  friend std::unique_ptr<GogServices> MakeGogServices(GogWiringConfig config);
  explicit GogServices(GogWiringConfig config);

  GogWiringConfig config_;
  std::unique_ptr<gog::SessionStore> store_;
  std::unique_ptr<gog::Session> session_;
  std::unique_ptr<LibraryCache> cache_;
  std::unique_ptr<CollectionsCache> tags_;  // the library tags (#536), next to the saved library
  std::unique_ptr<stores::GogInstallRunner> install_runner_;
  // Made here, handed to the registry by Register; the pointers stay valid while the registry keeps them.
  std::unique_ptr<stores::GogAccount> owned_account_;
  std::unique_ptr<stores::GogLibrarySource> owned_library_;
  std::unique_ptr<stores::GogDetailsSource> owned_details_;
  stores::GogAccount* account_ = nullptr;
  stores::GogLibrarySource* library_ = nullptr;
  stores::GogDetailsSource* details_ = nullptr;
  std::shared_ptr<web::GogRelay> relay_;
};

// With the line on: every GOG object, nothing registered yet. With it off: nothing (no folder, no route, no log
// line). Either way, when "Stay signed in" is off, a GOG session file and its temporary files left from an earlier
// run are deleted first (gog::SessionStore::ForgetIfNotPersisted), so they never outlive the setting.
std::unique_ptr<GogServices> MakeGogServices(GogWiringConfig config);

}  // namespace playden::homebrew
