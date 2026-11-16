// SPDX-License-Identifier: GPL-3.0-or-later
// GOG in the apps (homebrew/gog_wiring), as host.cpp and ps5.cpp make it: with the gog-store line, the account and
// the library join the registry, the relay reaches web access and the controller, a phone post signs in, and "Stay
// signed in" moves the session file; without it nothing is made and no route exists. "Stay signed in" off deletes a
// GOG session left from an earlier run either way. Shutdown answers a phone post still waiting before web access's
// workers are joined, and keeps a sign-out asked for in the last frame. A sign-in the relay ends leaves the QR code; a
// restart whose saved session expired shows the saved GOG library; Refresh library signs an offline session in once
// GOG answers. An installed GOG game whose record names an older build shows "Update…": the install service's
// records reach the details source. An installed GOG game installed without its DLC offers Manage DLC, which turns the
// DLC on through the install service. Synthetic tokens and accounts only.
#include "gog/account_key.h"
#include "gog/auth.h"
#include "gog/session_store.h"
#include "gog_cdn_stub.h"
#include "gog_stub_transport.h"
#include "gog_synthetic_game.h"
#include "homebrew/controller.hpp"
#include "homebrew/gog_wiring.hpp"
#include "installs/install_record.h"
#include "installs/install_service.h"
#include "library/cache.h"
#include "stores/gog.h"
#include "stores/gog_library.h"
#include "stores/gog_details.h"
#include "stores/registry.h"
#include "stores/steam.h"
#include "web/gog_relay.h"
#include "web/service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace playden;
using namespace playden::homebrew;
using namespace gog_test;
namespace fs = std::filesystem;

namespace {

std::string g_fixtures;
fs::path g_base;
int g_roots = 0;

constexpr const char* kHost = "192.168.1.20:7031";
const std::string kToken = "0123456789abcdef0123456789abcdef";
const std::string kPaste = "https://embed.gog.com/on_login_success?origin=client&code=SyntheticCode-wiring-0123456789";

fs::path FreshRoot() {
  const fs::path root = g_base / ("root-" + std::to_string(++g_roots));
  fs::create_directories(root / "logs");
  CHECK(::chmod(root.c_str(), 0755) == 0);
  return root;
}

bool Exists(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0;
}

fs::path SessionFile(const fs::path& root) { return root / "gog" / "session"; }

std::string ReadText(const fs::path& path) {
  if (!Exists(path)) return {};
  return ReadFile(path.string());
}

std::size_t LogFiles(const fs::path& root) {
  std::size_t count = 0;
  for (const auto& entry : fs::directory_iterator(root / "logs")) count += entry.is_regular_file();
  return count;
}

void SaveSession(const fs::path& root) {
  gog::SessionStore::Options options;
  options.user_root = root.string();
  gog::SessionStore store(options);
  gog::StoredSession session;
  session.refresh_token = RefreshText(9);
  session.user_id = kUserId;
  session.username = kUsername;
  CHECK(store.Save(session).ok);
}

// Steam with nothing saved: the controller needs a Steam account to run.
class NoSteamSession final : public SessionRepository {
 public:
  Result<std::optional<StoredSession>> Load() const override {
    return Result<std::optional<StoredSession>>::Success(std::nullopt);
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};
class NoSteamSignIn final : public steam::AuthBackend {
 public:
  steam::AuthResult AuthenticateQr(const std::atomic<bool>&, const steam::ChallengeCallback&) override {
    return {steam::AuthError::kCanceled, std::nullopt};
  }
  steam::AuthResult Restore(const StoredSession&, const std::atomic<bool>&) override {
    return {steam::AuthError::kCanceled, std::nullopt};
  }
};

// Steam's library with no account: the apps always have one, and the controller runs the other stores' libraries
// beside it.
class NoSteamLibrary final : public stores::LibrarySource {
 public:
  Store store() const override { return Store::kSteam; }
  std::uint64_t Refresh(std::uint64_t, std::uint64_t) override { return 0; }
  std::uint64_t LoadCached(std::uint64_t, std::uint64_t) override { return 0; }
  std::uint64_t Clear(std::uint64_t, std::uint64_t) override { return 0; }
  void Invalidate(std::uint64_t) override {}
  void Shutdown() override {}
  std::vector<LibraryEvent> Pump(std::size_t) override { return {}; }
};

web::ServiceConfig WebConfig(const fs::path& root) {
  web::ServiceConfig config;
  config.paths = {(root / "logs").string(), (root / "homebrew").string(), (root / "kits").string(),
                  (root / "homebrew/FAKE77031").string()};
  config.server.first_port = config.server.last_port = 0;
  config.server.loopback_only = true;
  return config;
}

struct Reply {
  int status = 0;
  std::string bytes;
};

Reply Ask(web::Service& service, web::Method method, const std::string& path, const std::string& body = {}) {
  web::Request request;
  request.method = method;
  request.method_text = method == web::Method::kPost ? "POST" : "GET";
  request.path = path;
  request.headers.push_back({"host", kHost});
  request.headers.push_back({"origin", std::string("http://") + kHost});
  request.headers.push_back({"x-playden-action", "gog-sign-in"});
  request.client = "192.168.1.50";
  request.content_length = body.size();
  auto offset = std::make_shared<std::size_t>(0);
  request.read_body = [body, offset](char* data, std::size_t size) -> std::ptrdiff_t {
    const auto count = std::min(size, body.size() - *offset);
    std::memcpy(data, body.data() + *offset, count);
    *offset += count;
    return static_cast<std::ptrdiff_t>(count);
  };
  Reply reply;
  web::Response response([&reply](const char* data, std::size_t size) { reply.bytes.append(data, size); return true; },
                         false);
  service.Handle(request, response);
  reply.status = response.status();
  return reply;
}

Reply PostCode(web::Service& service, const std::string& token) {
  return Ask(service, web::Method::kPost, "/api/gog/sign-in", "{\"token\":\"" + token + "\",\"address\":\"" + kPaste + "\"}");
}

void Wait(Controller& controller, const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!done()) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    controller.Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// The apps' GOG, on stub transports (the session's, then the library's, then the details source's CDN), with Steam
// signed out beside it. With installs, an install service over <root>/games runs GOG's runner, and the wiring's
// installed_record and install_root read its snapshot, as host.cpp's do.
struct App {
  fs::path root = FreshRoot();
  Stub session_stub;
  Stub library_stub;
  gog_test::CdnStub cdn;
  int transports = 0;
  bool with_installs = false;
  std::unique_ptr<Storage> library_storage = std::make_unique<Storage>(root.string());
  std::unique_ptr<GogServices> gog;
  // After the services: GOG's install runner outlives the install service.
  std::unique_ptr<installs::InstallService> installs;
  NoSteamSession steam_sessions;
  NoSteamSignIn steam_sign_in;
  Simulation model{{}, {}, ModelMode::Live};
  stores::Registry registry;
  std::unique_ptr<web::Service> web;
  std::unique_ptr<Controller> controller;

  std::atomic<bool> random_works{true};

  // seed runs first, before anything restores: saved files, scripted answers.
  explicit App(bool stay_signed_in = true, const std::function<void(App&)>& seed = {}) {
    if (seed) seed(*this);
    GogWiringConfig config;
    config.enabled = true;
    config.stay_signed_in = stay_signed_in;
    config.user_root = root.string();
    config.ca_bundle = "unused";
    config.log_directory = (root / "logs").string();
    config.library_storage = library_storage.get();
    config.transport = [this] {
      ++transports;
      return transports == 1 ? session_stub.Transport() : transports == 2 ? library_stub.Transport() : cdn.Transport();
    };
    if (with_installs) {
      config.installed_record = [this](const GameId& id) -> std::optional<installs::InstalledRecord> {
        if (!installs) return std::nullopt;
        for (const auto& game : installs->Snapshot().installed)
          if (game.id == id && game.available) return game.record;
        return std::nullopt;
      };
      config.install_root = [this](const GameId& id) -> std::optional<std::string> {
        if (!installs) return std::nullopt;
        for (const auto& game : installs->Snapshot().installed)
          if (game.id == id && game.available) return game.root;
        return std::nullopt;
      };
    }
    config.random_token = [this] {
      return random_works ? std::optional<std::string>(kToken) : std::optional<std::string>();
    };
    gog = MakeGogServices(std::move(config));
    CHECK(gog != nullptr && transports == 2);
    registry.Add(std::make_unique<stores::SteamAccount>(steam_sign_in, steam_sessions));
    registry.Add(std::make_unique<NoSteamLibrary>());
    gog->Register(registry, model);
    auto web_config = WebConfig(root);
    gog->ConfigureWeb(web_config);
    CHECK(web_config.gog == gog->relay() && web_config.gog_auth_url == gog::AuthorizeUrl());
    web = std::make_unique<web::Service>(std::move(web_config));
    if (with_installs)
      installs = std::make_unique<installs::InstallService>(
          installs::InstallService::Runners{{Store::kGog, gog->install_runner()}},
          std::vector<installs::InstallVolume>{{"internal", "Console storage", (root / "games").string(), true}});
    controller = std::make_unique<Controller>(model, registry, nullptr, installs.get());
    controller->SetWebAccess(web.get());
    gog->Attach(*controller);
  }
  ~App() {
    controller->Shutdown();
    if (installs) installs->Shutdown();
    web->Shutdown();
  }
  const Account& Gog() const { return model.state().account(Store::kGog); }
  // GOG's restore found nothing; the TV's sign-in opens the relay.
  void OpenSignIn() {
    Wait(*controller, [&] { return Gog().authentication == AuthState::SignedOut; });
    model.RequestServiceAction({ServiceAction::BeginQr, Store::kGog});
    controller->Tick();
    CHECK(gog->relay()->request() != 0 && gog->relay()->Token() == kToken);
  }
  void ScriptSignIn() {
    session_stub.Script(Endpoint::kToken, Tokens(1));
    session_stub.Script(Endpoint::kUser, Answer(200, ReadFile(g_fixtures + "/user-data.json")));
    library_stub.Script(Endpoint::kOwned, Answer(200, "{\"owned\":[]}"));
  }
  void SignIn() {
    OpenSignIn();
    ScriptSignIn();
    const auto reply = PostCode(*web, kToken);
    CHECK(reply.status == 200);
    Wait(*controller, [&] { return Gog().authentication == AuthState::SignedIn && !model.state().library_loading; });
  }
};

// Nothing in the logs names the account, a token or the code.
void CheckLogs(const fs::path& root) {
  for (const auto& entry : fs::directory_iterator(root / "logs")) {
    const auto text = ReadText(entry.path());
    CHECK(text.find(kUsername) == std::string::npos && text.find(kUserId) == std::string::npos);
    CHECK(text.find(AccessText(1)) == std::string::npos && text.find(RefreshText(1)) == std::string::npos);
    CHECK(text.find("SyntheticCode") == std::string::npos && text.find(kToken) == std::string::npos);
  }
}

// The line off: nothing made, no folder, no log line, and no route.
void LineOffMakesNothing() {
  const auto root = FreshRoot();
  Storage storage(root.string());
  GogWiringConfig config;
  config.user_root = root.string();
  config.log_directory = (root / "logs").string();
  config.library_storage = &storage;
  int transports = 0;
  config.transport = [&transports] {
    ++transports;
    return gog::Transport{};
  };
  CHECK(MakeGogServices(config) == nullptr);
  CHECK(transports == 0 && !Exists(root / "gog") && LogFiles(root) == 0);
  // No runner: GOG games can't install, as the apps' model says without the line.
  Simulation model{{}, {}, ModelMode::Live};
  CHECK(!model.StoreInstalls(Store::kGog) && model.StoreInstalls(Store::kSteam));
  // "Stay signed in" off with no GOG folder: still nothing is made.
  config.stay_signed_in = false;
  CHECK(MakeGogServices(config) == nullptr);
  CHECK(transports == 0 && !Exists(root / "gog") && LogFiles(root) == 0);
  // Web access without the relay: neither GOG route exists.
  web::Service web(WebConfig(root));
  CHECK(Ask(web, web::Method::kGet, "/gog").status == 404);
  CHECK(PostCode(web, kToken).status == 404);
  CHECK(Ask(web, web::Method::kGet, "/").status == 200);
  web.Shutdown();
}

// "Stay signed in" off deletes a GOG session and its temporary files left from an earlier run, line on or off. On, a
// saved session is kept while the line is off.
void StaySignedInOffForgetsTheSession() {
  for (const bool line : {false, true}) {
    const auto root = FreshRoot();
    SaveSession(root);
    const auto leftover = root / "gog" / (std::string(gog::kSessionLeftoverPrefix) + "x");
    {
      FILE* file = std::fopen(leftover.c_str(), "wb");
      CHECK(file != nullptr);
      std::fputs("synthetic-leftover", file);
      std::fclose(file);
      CHECK(::chmod(leftover.c_str(), 0600) == 0);
    }
    Storage storage(root.string());
    GogWiringConfig config;
    config.enabled = line;
    config.user_root = root.string();
    config.log_directory = (root / "logs").string();
    config.library_storage = &storage;
    if (!line) {
      // On: nothing is touched.
      CHECK(MakeGogServices(config) == nullptr);
      CHECK(Exists(SessionFile(root)) && Exists(leftover));
    }
    config.stay_signed_in = false;
    config.transport = [] {
      return gog::Transport([](const steam::HttpRequest&, const std::atomic<bool>&) {
        std::fprintf(stderr, "unexpected request\n");
        std::exit(1);
        return steam::HttpResponse{};
      });
    };
    auto made = MakeGogServices(config);
    CHECK((made != nullptr) == line);
    CHECK(!Exists(SessionFile(root)) && !Exists(leftover));
    if (!line) CHECK(LogFiles(root) == 0);
  }
}

// The line on: the account and the library join the registry, the model shows GOG, the relay reaches web access and
// the controller, and a phone post signs in and saves the session. "Stay signed in" moves the file both ways.
void LineOnSignsInThroughThePhone() {
  App app;
  CHECK(app.model.GogStoreEnabled());
  // GOG games install through the queue: the runner is made (its transports only when a download runs).
  CHECK(app.gog->install_runner() != nullptr && app.model.StoreInstalls(Store::kGog));
  CHECK(app.transports == 2);
  CHECK(app.registry.account(Store::kGog) == app.gog->account());
  const auto libraries = app.registry.libraries();
  CHECK(std::find(libraries.begin(), libraries.end(), app.gog->library()) != libraries.end());
  // The details source (install sizes) too; its transport waits for its first request.
  CHECK(app.gog->details() != nullptr && app.registry.details(Store::kGog) == app.gog->details());
  const auto sources = app.registry.details_sources();
  CHECK(std::find(sources.begin(), sources.end(), app.gog->details()) != sources.end());
  CHECK(Ask(*app.web, web::Method::kGet, "/gog").status == 200);
  app.SignIn();
  CHECK(app.Gog().display_name == kUsername);
  CHECK(Exists(SessionFile(app.root)));
  CHECK(app.gog->relay()->request() == 0);
  // "Stay signed in" off: the file goes, the session stays in memory. On: it is written again.
  // Both on the account worker: the main thread never waits for the store.
  app.gog->SetStaySignedIn(false);
  Wait(*app.controller, [&] { return !Exists(SessionFile(app.root)); });
  CHECK(app.gog->session().signed_in());
  app.gog->SetStaySignedIn(true);
  Wait(*app.controller, [&] {
    const auto text = ReadText(app.root / "logs" / "auth.log");
    return Exists(SessionFile(app.root)) && std::count(text.begin(), text.end(), '\n') == 2;
  });
  const auto auth = ReadText(app.root / "logs" / "auth.log");
  // Two lines, each "<time> stay signed in gog moved=1".
  const std::string line = " stay signed in gog moved=1\n";
  const auto first = auth.find(line);
  CHECK(first != std::string::npos && auth.find(line, first + 1) != std::string::npos);
  CHECK(std::count(auth.begin(), auth.end(), '\n') == 2);
  CHECK(!ReadText(app.root / "logs" / "gog.log").empty());
  app.controller->Shutdown();
  CheckLogs(app.root);
}

// Started with "Stay signed in" off: a sign-in lives in memory only, and turning it on writes the file.
void StaySignedInOffKeepsTheSessionInMemory() {
  App app(false);
  app.SignIn();
  CHECK(!Exists(SessionFile(app.root)) && app.gog->session().signed_in());
  app.gog->SetStaySignedIn(true);
  Wait(*app.controller, [&] { return Exists(SessionFile(app.root)); });
}

// Shutdown with a phone post waiting for GOG's answer: the post is answered and web access's workers can be joined.
void ShutdownAnswersAWaitingPost() {
  App app;
  app.OpenSignIn();
  auto gate = std::make_shared<Gate>();
  app.session_stub.Script(Endpoint::kToken, Tokens(1), gate);
  std::atomic<int> status{0};
  std::thread phone([&] { status = PostCode(*app.web, kToken).status; });
  gate->WaitEntered(1);
  const auto started = std::chrono::steady_clock::now();
  app.controller->Shutdown();
  phone.join();
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
  CHECK(status != 0 && status != 200);
  CHECK(app.gog->relay()->request() == 0 && !app.gog->relay()->Token());
  CHECK(!Exists(SessionFile(app.root)));
  CheckLogs(app.root);
}

// A GOG sign-out asked for in the last frame still deletes the session at shutdown.
void ShutdownKeepsALastSignOut() {
  App app;
  app.SignIn();
  CHECK(Exists(SessionFile(app.root)));
  app.model.RequestServiceAction({ServiceAction::SignOut, Store::kGog});
  app.controller->Shutdown();
  CHECK(!Exists(SessionFile(app.root)));
  CheckLogs(app.root);
}

// A failed code with no randomness left for a new token ends the sign-in on the relay: the TV leaves the QR code and
// offers Try again, though no account event follows.
void EndedSignInLeavesTheCode() {
  App app;
  app.OpenSignIn();
  app.session_stub.Script(Endpoint::kToken, InvalidGrant());
  app.random_works = false;
  CHECK(PostCode(*app.web, kToken).status == 503);
  Wait(*app.controller, [&] { return app.Gog().authentication == AuthState::Failed; });
  CHECK(app.Gog().error.find("has ended") != std::string::npos && app.Gog().challenge_url.empty());
  CHECK(app.gog->relay()->request() == 0 && !app.gog->relay()->Ended());
  CheckLogs(app.root);
}

void SaveLibrary(App& app) {
  LibraryCache cache(*app.library_storage, "library-gog-");
  LibrarySnapshot snapshot;
  snapshot.schema_version = 8;
  snapshot.retrieved_at_unix_seconds = 5;
  GameSummary game;
  game.id = GameId::Gog(1207658924);
  game.title = "Saved Synthetic Game";
  snapshot.games.push_back(game);
  CHECK(cache.Save(gog::AccountKey(kUserId), snapshot).ok());
}

bool GameShown(const Simulation& model, const GameId& id) {
  const auto& games = model.state().games;
  return std::any_of(games.begin(), games.end(), [&](const auto& game) { return game.id == id; });
}
bool GogGameShown(const Simulation& model) { return GameShown(model, GameId::Gog(1207658924)); }

// A restart whose saved refresh token GOG refuses: signed out with "GOG sign-in expired", and the saved GOG library is
// still shown while the player signs in again.
void ExpiredAtStartupKeepsTheLibrary() {
  App app(true, [](App& app) {
    SaveSession(app.root);
    SaveLibrary(app);
    app.session_stub.Script(Endpoint::kToken, InvalidGrant());
  });
  Wait(*app.controller, [&] {
    return app.Gog().authentication == AuthState::Expired && GogGameShown(app.model) && !app.model.state().library_loading;
  });
  CHECK(app.Gog().error.find("expired") != std::string::npos);
  CHECK(!Exists(SessionFile(app.root)));
  CheckLogs(app.root);
}

// The same with no saved library: Settings still says the sign-in expired, not that no library is saved, also after
// Steam's Refresh library (which refreshes the other stores' libraries too).
void ExpiredAtStartupWithoutALibrary() {
  App app(true, [](App& app) {
    SaveSession(app.root);
    app.session_stub.Script(Endpoint::kToken, InvalidGrant());
  });
  Wait(*app.controller, [&] {
    return app.Gog().authentication == AuthState::Expired && !app.model.state().library_loading;
  });
  for (int i = 0; i < 20; ++i) {
    app.controller->Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(app.Gog().error == "GOG sign-in expired. Sign in again.");
  app.model.RequestServiceAction({ServiceAction::RefreshLibrary, Store::kSteam});
  for (int i = 0; i < 20; ++i) {
    app.controller->Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Wait(*app.controller, [&] { return !app.model.state().library_loading; });
  CHECK(app.Gog().error == "GOG sign-in expired. Sign in again.");
  CheckLogs(app.root);
}

// A restart with no answer from GOG: offline. Refresh library asks GOG again: no answer keeps it offline; an answer
// signs in and loads the library.
void OfflineRefreshSignsIn() {
  App app(true, [](App& app) {
    SaveSession(app.root);
    SaveLibrary(app);
    app.session_stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
  });
  Wait(*app.controller, [&] { return app.Gog().authentication == AuthState::Offline && GogGameShown(app.model); });
  app.session_stub.Script(Endpoint::kToken, Failure(HttpError::kTimeout));
  app.model.RequestServiceAction({ServiceAction::RefreshLibrary, Store::kGog});
  Wait(*app.controller, [&] { return app.session_stub.Calls(Endpoint::kToken) == 2; });
  for (int i = 0; i < 20; ++i) {
    app.controller->Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(app.Gog().authentication == AuthState::Offline && GogGameShown(app.model));
  app.session_stub.Script(Endpoint::kToken, Tokens(2));
  app.library_stub.Script(Endpoint::kOwned, Answer(200, "{\"owned\":[]}"));
  app.model.RequestServiceAction({ServiceAction::RefreshLibrary, Store::kGog});
  Wait(*app.controller, [&] {
    return app.Gog().authentication == AuthState::SignedIn && app.library_stub.Calls(Endpoint::kOwned) == 1 &&
           !app.model.state().library_loading;
  });
  CHECK(app.session_stub.Calls(Endpoint::kToken) == 3 && app.session_stub.Unused() == 0);
  CheckLogs(app.root);
}

void WriteText(const std::string& path, const std::string& text) {
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  CHECK(std::fwrite(text.data(), 1, text.size(), file) == text.size());
  CHECK(std::fclose(file) == 0);
}

const ViewRow* FindRow(const Simulation& model, const std::string& key) {
  for (const auto& row : model.view().rows)
    if (row.key == key) return &row;
  return nullptr;
}

// An installed GOG game whose record names an older build than GOG's default one: its page shows "Update…". The
// details source learns of the install only through the wiring's installed_record.
void InstalledOlderBuildShowsAnUpdate() {
  const auto id = GameId::Gog(gog_test::kBase);
  App app(true, [](App& app) {
    app.with_installs = true;
    const auto folder = app.root / "games" / "gog-1900000001";
    fs::create_directories(folder);
    installs::GogRecord record;
    record.build_id = "1000000000000000101";
    record.products = {gog_test::kBase};
    record.language = "en-US";
    record.meta_id = std::string(32, 'a');
    WriteText((folder / installs::kInstallRecordName).string(),
              installs::FormatGogInstallRecord(GameId::Gog(gog_test::kBase), 4096, 1, "english", record,
                                               {{"bin/Game.exe", "", "", "Synthetic Game"}}));
    WriteText((app.root / "games" / ".playden-games").string(), "gog-1900000001\tinternal\t4096\n");
    // GOG's default build is a newer one.
    gog_test::Game game;
    game.build_id = "1000000000000000202";
    gog_test::GameDepot depot;
    depot.files.push_back({"DepotFile", "bin\\Game.exe", gog_test::RandomBytes(4096, 7)});
    game.depots.push_back(depot);
    gog_test::Publish(app.cdn, game);
    app.cdn.SetOwned("{\"owned\":[1900000001]}");
  });
  app.OpenSignIn();
  app.session_stub.Script(Endpoint::kToken, Tokens(1));
  app.session_stub.Script(Endpoint::kUser, Answer(200, ReadFile(g_fixtures + "/user-data.json")));
  app.library_stub.Script(Endpoint::kOwned, Answer(200, "{\"owned\":[1900000001]}"));
  app.library_stub.SetProducts([](std::uint64_t product, const std::string&) {
    const auto text = std::to_string(product);
    return Stub::Step{Answer(200, R"({"external_id":")" + text +
                                      R"(","type":"game","title":{"*":"Synthetic Game"},"summary":{"*":"Synthetic"},)"
                                      R"("supported_operating_systems":[{"slug":"windows","name":"Windows"}],"dlcs":[],)"
                                      R"("game":{"visible_in_library":true}})"),
                      nullptr};
  });
  CHECK(PostCode(*app.web, kToken).status == 200);
  Wait(*app.controller, [&] {
    const auto& state = app.model.state();
    return app.Gog().authentication == AuthState::SignedIn && GameShown(app.model, id) &&
           std::any_of(state.installations.begin(), state.installations.end(),
                       [&](const Installation& item) { return item.game_id == id; });
  });
  CHECK(app.model.action("details:" + ToKey(id)));
  Wait(*app.controller, [&] {
    const auto* update = FindRow(app.model, "details-update");
    return update && update->title == "Update…";
  });
  app.controller->Shutdown();
  CheckLogs(app.root);
}

const Installation* FindInstallation(const Simulation& model, const GameId& id) {
  for (const auto& item : model.state().installations)
    if (item.game_id == id) return &item;
  return nullptr;
}

const homebrew::Game* FindGame(const Simulation& model, const GameId& id) {
  for (const auto& game : model.state().games)
    if (game.id == id) return &game;
  return nullptr;
}

void Choose(App& app, const std::string& key) {
  CHECK(app.model.action("focus:" + key) && app.model.state().navigation.focus_key == key);
  CHECK(app.model.action("cross"));
  app.controller->Tick();
}

// An installed GOG game whose record leaves its owned DLC out: the controller reads the record's products and
// "excluded" list into the model, the details list the DLC, and Maintenance › Manage DLC turns it on through the
// install service's update, which finishes with the DLC in the record. The logs name no account, token or address.
void InstalledGameWithDlcOffersManageDlc() {
  const auto id = GameId::Gog(gog_test::kBase);
  const auto dlc = GameId::Gog(gog_test::kDlc);
  // The DLC's chunks wait while `hold` is set, so the change is seen running.
  auto hold = std::make_shared<std::atomic<bool>>(false);
  auto held = std::make_shared<std::atomic<int>>(0);
  App app(true, [hold, held](App& app) {
    app.with_installs = true;
    gog_test::Game game;
    gog_test::GameDepot base;
    base.files.push_back({"DepotFile", "bin\\Game.exe", gog_test::RandomBytes(4096, 7)});
    game.depots.push_back(base);
    gog_test::GameDepot info;
    info.gog = true;
    info.files.push_back({"DepotFile", "goggame-1900000001.info", ReadFile(g_fixtures + "/goggame-1900000001.info")});
    game.depots.push_back(info);
    gog_test::GameDepot extra;
    extra.product = gog_test::kDlc;
    extra.files.push_back({"DepotFile", "dlc\\extra.dat", gog_test::RandomBytes(8192, 8)});
    game.depots.push_back(extra);
    gog_test::Publish(app.cdn, game);
    app.cdn.SetOwned("{\"owned\":[1900000001,1900000002]}");
    app.cdn.SetSecureLink(ReadFile(g_fixtures + "/secure-link-200.json"));
    app.cdn.SetFault([hold, held](const gog_test::Request& request) -> std::optional<gog_test::Fault> {
      if (request.kind == gog_test::Kind::kChunk && request.product == gog_test::kDlc) {
        ++*held;
        while (*hold) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      return std::nullopt;
    });
  });
  app.OpenSignIn();
  app.session_stub.Script(Endpoint::kToken, Tokens(1));
  app.session_stub.Script(Endpoint::kUser, Answer(200, ReadFile(g_fixtures + "/user-data.json")));
  // The library lists the game only: a DLC is no game of its own.
  app.library_stub.Script(Endpoint::kOwned, Answer(200, "{\"owned\":[1900000001]}"));
  app.library_stub.SetProducts([](std::uint64_t product, const std::string&) {
    const auto text = std::to_string(product);
    return Stub::Step{Answer(200, R"({"external_id":")" + text +
                                      R"(","type":"game","title":{"*":"Synthetic Game"},"summary":{"*":"Synthetic"},)"
                                      R"("supported_operating_systems":[{"slug":"windows","name":"Windows"}],"dlcs":[],)"
                                      R"("game":{"visible_in_library":true}})"),
                      nullptr};
  });
  CHECK(PostCode(*app.web, kToken).status == 200);
  Wait(*app.controller, [&] { return app.Gog().authentication == AuthState::SignedIn && GameShown(app.model, id); });
  CHECK(!GameShown(app.model, dlc));
  // Installed with the DLC turned off, as the install sheet does.
  installs::InstallOptions options;
  options.excluded_dlc = {dlc};
  app.installs->Install(id, std::nullopt, {}, options);
  Wait(*app.controller, [&] {
    const auto* installation = FindInstallation(app.model, id);
    return installation && installation->dlc_known && app.installs->Snapshot().jobs.empty();
  });
  {
    const auto* installation = FindInstallation(app.model, id);
    CHECK(installation->installed_dlc.empty() && (installation->excluded_dlc == std::vector<GameId>{dlc}));
    const auto snapshot = app.installs->Snapshot();
    CHECK(snapshot.installed.size() == 1 && snapshot.installed[0].record && snapshot.installed[0].record->gog);
    CHECK((snapshot.installed[0].record->gog->products == std::vector<std::uint64_t>{gog_test::kBase}));
    CHECK((snapshot.installed[0].record->gog->excluded == std::vector<std::uint64_t>{gog_test::kDlc}));
  }
  CHECK(*held == 0);
  // The page's details bring the DLC.
  CHECK(app.model.action("details:" + ToKey(id)));
  Wait(*app.controller, [&] {
    const auto* game = FindGame(app.model, id);
    return game && std::any_of(game->dlc.begin(), game->dlc.end(), [&](const GameDlc& item) {
             return item.id == dlc && item.install_bytes != 0;
           });
  });
  Choose(app, "details-more");
  const auto* manage = FindRow(app.model, "manage-dlc");
  CHECK(manage && manage->enabled && manage->value == "0 of 1");
  Choose(app, "manage-dlc");
  const auto key = "manage-dlc-" + ToKey(dlc);
  CHECK(FindRow(app.model, key) && !FindRow(app.model, key)->selected);
  Choose(app, key);
  CHECK(FindRow(app.model, key)->selected && FindRow(app.model, key)->subtitle == "Downloads");
  *hold = true;
  Choose(app, "manage-dlc-apply");
  // The install service runs it as an update with the DLC change, and Queue names it.
  bool changing = false, named = false;
  Wait(*app.controller, [&] {
    const auto snapshot = app.installs->Snapshot();
    for (const auto& job : snapshot.jobs)
      if (job.id == id && job.kind == installs::InstallJobKind::kUpdate && job.dlc_change) changing = true;
    for (const auto& job : app.model.state().jobs)
      if (job.game_id == id && job.stage_text == "Changing DLC") named = true;
    return changing && named && *held > 0;
  });
  *hold = false;
  Wait(*app.controller, [&] {
    const auto* installation = FindInstallation(app.model, id);
    return app.installs->Snapshot().jobs.empty() && installation && installation->dlc_known &&
           installation->installed_dlc == std::vector<GameId>{dlc};
  });
  {
    const auto* installation = FindInstallation(app.model, id);
    CHECK(installation->excluded_dlc.empty());
    const auto snapshot = app.installs->Snapshot();
    CHECK(snapshot.installed.size() == 1 && snapshot.installed[0].record && snapshot.installed[0].record->gog);
    CHECK((snapshot.installed[0].record->gog->products == std::vector<std::uint64_t>{gog_test::kBase, gog_test::kDlc}));
    CHECK(snapshot.installed[0].record->gog->excluded.empty());
    CHECK(Exists(fs::path(snapshot.installed[0].root) / "dlc" / "extra.dat"));
  }
  app.controller->Shutdown();
  CheckLogs(app.root);
  // gog.log and install.log say what ran, without an address.
  for (const char* name : {"gog.log", "install.log"}) {
    const auto text = ReadText(app.root / "logs" / name);
    CHECK(!text.empty() && text.find("://") == std::string::npos);
  }
  CHECK(ReadText(app.root / "logs" / "install.log").find("gog update game=gog-1900000001") != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  g_fixtures = argv[1];
  std::error_code ignored;
  fs::remove_all(argv[2], ignored);
  fs::create_directories(argv[2]);
  g_base = fs::canonical(argv[2]);
  CHECK(::chmod(g_base.c_str(), 0755) == 0);
  LineOffMakesNothing();
  StaySignedInOffForgetsTheSession();
  LineOnSignsInThroughThePhone();
  StaySignedInOffKeepsTheSessionInMemory();
  ShutdownAnswersAWaitingPost();
  ShutdownKeepsALastSignOut();
  EndedSignInLeavesTheCode();
  ExpiredAtStartupKeepsTheLibrary();
  ExpiredAtStartupWithoutALibrary();
  OfflineRefreshSignsIn();
  InstalledOlderBuildShowsAnUpdate();
  InstalledGameWithDlcOffersManageDlc();
  fs::remove_all(g_base, ignored);
  std::puts("homebrew_gog_wiring: ok");
  return 0;
}
