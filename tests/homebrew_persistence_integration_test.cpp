#include "homebrew/controller.hpp"
#include "app/persistence_controller.h"
#include "app/persistence_service.h"
#include "library/update_check.h"
#include "library/cache.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>
#include "steam_id.h"

using namespace playden;
using namespace playden::homebrew;
using namespace playden::steam;
using namespace std::chrono_literals;
namespace {
struct Sessions final : SessionRepository {
  mutable std::mutex mutex;
  mutable std::optional<StoredSession> value=StoredSession{123,"Synthetic","refresh","access",{}};
  Result<std::optional<StoredSession>> Load() const override {std::lock_guard<std::mutex> lock(mutex);return Result<std::optional<StoredSession>>::Success(value);}
  Result<void> Save(const StoredSession& s) const override {std::lock_guard<std::mutex> lock(mutex);value=s;return Result<void>::Success();}
  Result<void> Clear() const override {std::lock_guard<std::mutex> lock(mutex);value.reset();return Result<void>::Success();}
};
struct Auth final : AuthBackend {
  AuthResult AuthenticateQr(const std::atomic<bool>&,const ChallengeCallback&) override {return {AuthError::kCanceled,{}};}
  AuthResult Restore(const StoredSession& s,const std::atomic<bool>&) override {return {AuthError::kNone,s};}
};
struct Library final : LibraryBackend {
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>&) override {return {LibraryError::kNone,{{SteamId(42),"Synthetic game",{},{},{}}},false};}
};
// Enough played games for Continue playing to end with More.
struct PlayedLibrary final : LibraryBackend {
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>&) override {
    LibraryResult result{LibraryError::kNone,{},false};
    for(std::uint32_t id=1;id<=25;++id) {
      GameSummary game{SteamId(id),"Played "+std::to_string(100+id),{},{},{}};
      game.last_played_unix_seconds=1700000000ULL+id;game.playtime_minutes=id%2 ? 1000+id : id;
      result.games.push_back(game);
    }
    return result;
  }
};
struct Organization final : OrganizationRepository {
  explicit Organization(Storage& storage):store(storage){}
  OrganizationStore store;
  std::atomic<bool> fail_clear{false},block_load{false},release{false};
  mutable std::atomic<bool> entered{false};
  Result<std::optional<OrganizationSnapshot>> Load(std::uint64_t account) const override {
    entered=true;
    while(block_load && !release)std::this_thread::sleep_for(1ms);
    return store.Load(account);
  }
  Result<void> Save(std::uint64_t account,const OrganizationSnapshot& snapshot) const override {return store.Save(account,snapshot);}
  Result<void> Clear(std::uint64_t account) const override {
    return fail_clear ? Result<void>::Failure(StorageError::kRemoveFailed,"synthetic failure") : store.Clear(account);
  }
};
template<class F> void Wait(Controller& controller,F predicate) {
  const auto until=std::chrono::steady_clock::now()+3s;
  do {controller.Tick();if(predicate())return;std::this_thread::sleep_for(1ms);}while(std::chrono::steady_clock::now()<until);
  assert(false && "persistence integration deadline");
}
// Settings: Up and Down stay in a section; another section is reached through the rail.
static void enter_settings_section(playden::homebrew::Simulation& model, int index) {
  using playden::homebrew::Route;
  if (model.state().navigation.route != Route::Settings || !model.state().navigation.modals.empty()) return;
  const auto& rows = model.view().rows;
  const std::string wanted = rows[static_cast<std::size_t>(index)].section;
  const auto here = [&] { return rows[static_cast<std::size_t>(model.view().focused_row)].section; };
  if (here() == wanted) return;
  std::vector<std::string> order;
  for (const auto& row : rows) if (order.empty() || order.back() != row.section) order.push_back(row.section);
  const auto position = [&](const std::string& section) { return std::find(order.begin(), order.end(), section) - order.begin(); };
  model.action("left");
  for (int step = 0; step < 16 && here() != wanted; ++step) model.action(position(here()) < position(wanted) ? "down" : "up");
  model.action("right");
}
void Choose(Simulation& model,const std::string& action) {
  const auto& rows=model.view().rows;
  const auto found=std::find_if(rows.begin(),rows.end(),[&](const ViewRow& r){return r.action==action;});assert(found!=rows.end());
  const int index=static_cast<int>(found-rows.begin());
  enter_settings_section(model,index);
  for(int n=0;n<80 && model.view().focused_row!=index;++n)assert(model.action(model.view().focused_row<index?"down":"up"));
  assert(model.view().focused_row==index && model.action("cross"));
}
void Settings(Simulation& model) {
  for(int n=0;n<4 && model.state().navigation.route!=Route::Settings;++n)assert(model.action("r1"));
  assert(model.state().navigation.route==Route::Settings);
}
void SignOut(Simulation& model) {Settings(model);Choose(model,"signout-confirm");Choose(model,"confirm-destructive");}
// Organization belongs to the device: it outlives sign-out, and edits made signed out are saved.
void RestartAndSignOut(const std::filesystem::path& root) {
  Storage storage(root.string());PreferencesStore preferences(storage);Organization organization(storage);LibraryCache cache(storage);
  // An upgrade: the account's version 1 file is what the device has.
  OrganizationSnapshot seed;seed.favorites.insert(SteamId(42));seed.notes[SteamId(42)]="Saved note";seed.selected_game_id= SteamId(42);
  assert(organization.store.SaveV1(123,seed).ok());assert(preferences.Save({true}).ok());
  Sessions sessions;Auth auth;Library backend;
  {
    Simulation model({}, {},ModelMode::Live);
    LibraryService library(backend,sessions,&cache);
    app::PersistenceService persistence(preferences,organization);
    Controller controller(model,auth,sessions,&library,&persistence);
    Wait(controller,[&]{return model.state().games.size()==1 && model.state().library.favorites.contains(SteamId(42)) && model.state().reduced_motion;});
    assert(model.state().library.notes.at(SteamId(42))=="Saved note");
    assert(model.state().navigation.selected_game_id==SteamId(42));
    assert(model.action("r1"));if(model.state().navigation.library_rail_focused)assert(model.action("right"));
    assert(model.action("square"));
    Settings(model);Choose(model,"toggle-motion");
    controller.Shutdown();
    auto saved=organization.Load(kDeviceOrganization);assert(saved.ok() && saved.value() && saved.value()->favorites.empty());
    assert(saved.value()->notes.at(SteamId(42))=="Saved note" && !preferences.Load().preferences.reduce_motion);
    // The version 1 file is left as it was.
    const auto legacy=organization.store.LoadV1(123);
    assert(legacy.ok() && legacy.value() && legacy.value()->favorites.contains(SteamId(42)));
  }
  {
    Simulation model({}, {},ModelMode::Live);LibraryService library(backend,sessions,&cache);
    app::PersistenceService persistence(preferences,organization);Controller controller(model,auth,sessions,&library,&persistence);
    Wait(controller,[&]{return model.state().library.notes.count(SteamId(42))>0 && model.state().games.size()==1;});
    assert(model.state().library.favorites.empty() && !model.state().reduced_motion);
    // A failing Clear would not matter: sign-out deletes no organization.
    organization.fail_clear=true;SignOut(model);controller.Tick();
    Wait(controller,[&]{return model.state().account().authentication==AuthState::SignedOut;});
    assert(!sessions.Load().value());
    // Signed out, the organization is still there, in memory and on disk.
    assert(model.state().library.notes.at(SteamId(42))=="Saved note");
    const auto kept=organization.Load(kDeviceOrganization);
    assert(kept.ok() && kept.value() && kept.value()->notes.at(SteamId(42))=="Saved note");
    assert(preferences.Load().status==PreferencesLoadStatus::kLoaded);
    controller.Shutdown();
  }
  // Signed out from the start: a favorite of a game of no store account is saved and restored.
  const GameId local=GameId::Local("0b8f6c1e-4d2a-4f3b-9a1c-2e5d7f8a9b0c");
  {
    Simulation model({}, {},ModelMode::Live);LibraryService library(backend,sessions,&cache);
    app::PersistenceService persistence(preferences,organization);Controller controller(model,auth,sessions,&library,&persistence);
    persistence.WaitIdle();controller.Tick();
    model.UpdateLibrary({{{local,"Imported game",{},{},{},{},{}}},false,{},false,false,{},Store::kLocal});
    assert(model.action("select:"+ToKey(local)) && model.action("square"));
    Wait(controller,[&]{const auto saved=organization.Load(kDeviceOrganization);return saved.ok() && saved.value() && saved.value()->favorites.count(local)>0;});
    controller.Shutdown();
  }
  {
    Simulation model({}, {},ModelMode::Live);LibraryService library(backend,sessions,&cache);
    app::PersistenceService persistence(preferences,organization);Controller controller(model,auth,sessions,&library,&persistence);
    Wait(controller,[&]{return model.state().library.favorites.count(local)>0;});
    assert(model.state().library.notes.at(SteamId(42))=="Saved note");
    controller.Shutdown();
  }
}
// The sort chosen in Sort and filters is the one the Library opens with after a restart, also when
// "More" on Continue playing was used last.
void SortSurvivesRestart(const std::filesystem::path& root) {
  Storage storage(root.string());PreferencesStore preferences(storage);Organization organization(storage);LibraryCache cache(storage);
  Sessions sessions;Auth auth;PlayedLibrary backend;
  {
    Simulation model({}, {},ModelMode::Live);LibraryService library(backend,sessions,&cache);
    app::PersistenceService persistence(preferences,organization);Controller controller(model,auth,sessions,&library,&persistence);
    Wait(controller,[&]{return model.state().games.size()==25 && !model.state().library_loading;});
    assert(model.action("r1"));if(model.state().navigation.library_rail_focused)assert(model.action("right"));
    assert(model.action("triangle"));Choose(model,"sort-playtime");
    assert(model.state().library.sort=="playtime");
    assert(model.action("l1") && model.state().navigation.route==Route::Home);
    for(int step=0;step<30 && model.state().navigation.focus_key!="shelf-continue-more";++step) model.action("right");
    assert(model.action("cross") && model.state().navigation.route==Route::Library);
    assert(model.visible_games().front()->id==SteamId(25) && model.visible_games()[1]->id==SteamId(24));
    Wait(controller,[&]{const auto saved=organization.Load(kDeviceOrganization);return saved.ok() && saved.value() && saved.value()->sort=="playtime";});
    controller.Shutdown();
  }
  const auto saved=organization.Load(kDeviceOrganization);
  assert(saved.ok() && saved.value() && saved.value()->sort=="playtime");
  {
    Simulation model({}, {},ModelMode::Live);LibraryService library(backend,sessions,&cache);
    app::PersistenceService persistence(preferences,organization);Controller controller(model,auth,sessions,&library,&persistence);
    Wait(controller,[&]{return model.state().games.size()==25 && !model.state().library_loading && model.state().library.sort=="playtime";});
    assert(model.action("r1") && model.state().navigation.route==Route::Library);
    const auto games=model.visible_games();
    assert(games.front()->id==SteamId(25) && games[1]->id==SteamId(23));
  }
}
void EditWhileLoading(const std::filesystem::path& root) {
  Storage storage(root.string());PreferencesStore preferences(storage);Organization organization(storage);LibraryCache cache(storage);
  OrganizationSnapshot seed;seed.favorites.insert(SteamId(77));seed.notes[SteamId(77)]="Keep unrelated note";
  assert(organization.Save(kDeviceOrganization,seed).ok());organization.block_load=true;
  Sessions sessions;Auth auth;Library backend;Simulation model({}, {},ModelMode::Live);
  LibraryService library(backend,sessions,&cache);app::PersistenceService persistence(preferences,organization);
  Controller controller(model,auth,sessions,&library,&persistence);
  Wait(controller,[&]{return organization.entered && model.state().games.size()==1;});
  assert(model.action("r1"));if(model.state().navigation.library_rail_focused)assert(model.action("right"));
  assert(model.action("square") && model.state().library.favorites.contains(SteamId(42)));
  organization.release=true;
  Wait(controller,[&]{return model.state().library.favorites.contains(SteamId(77)) && model.state().library.favorites.contains(SteamId(42));});
  controller.Shutdown();
  const auto loaded=organization.Load(kDeviceOrganization);
  assert(loaded.ok() && loaded.value() && loaded.value()->favorites==std::set<GameId>({SteamId(42),SteamId(77)}));
  assert(loaded.value()->notes.at(SteamId(77))=="Keep unrelated note");
}

}
// Steam's and GOG's update checks go to their own files, for the same account number too, and a restart reads both
// back. Steam's file never names a GOG game: an older Playden would refuse all of it.
void UpdateChecksSurviveRestart(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  Storage storage(root.string());PreferencesStore preferences(storage);Organization organization(storage);
  UpdateCheckStore steam(storage),gog(storage,"updates-gog-");
  constexpr std::uint64_t kAccount=123;
  const auto witcher=GameId::Gog(1207658924);
  const auto wait=[](app::PersistenceController& controller,const auto& predicate){
    const auto until=std::chrono::steady_clock::now()+3s;
    do {controller.Tick();if(predicate())return;std::this_thread::sleep_for(1ms);}while(std::chrono::steady_clock::now()<until);
    assert(false && "update check deadline");
  };
  {
    Simulation model({}, {},ModelMode::Live);model.SetWallClock(1760000000);
    app::PersistenceService persistence(preferences,organization,&steam);persistence.SetUpdateChecks(Store::kGog,&gog);
    app::PersistenceController controller(model,persistence);
    controller.Activate(kAccount);controller.Activate(Store::kGog,kAccount);
    wait(controller,[&]{return model.state().update_check.loaded;});
    model.UpdateAppDetails(SteamId(42),1,1,true,{},{},{},GameUpdate{true,false,5});
    model.UpdateAppDetails(witcher,1,1,true,{},{},{},GameUpdate{false,true,6});
    controller.FlushAndShutdown();
  }
  const auto text=[&](const std::string& path){const auto read=storage.Read(path,1<<20);assert(read.ok());return std::string(read.value().begin(),read.value().end());};
  const auto steam_file=text("updates-123.v2"),gog_file=text("updates-gog-123.v2");
  assert(steam_file.find("\n42 1 ")!=std::string::npos && steam_file.find("gog-")==std::string::npos);
  assert(gog_file.find("\ngog-1207658924 2 ")!=std::string::npos && gog_file.find("\n42 ")==std::string::npos);
  {
    Simulation model({}, {},ModelMode::Live);model.SetWallClock(1760000000);
    app::PersistenceService persistence(preferences,organization,&steam);persistence.SetUpdateChecks(Store::kGog,&gog);
    app::PersistenceController controller(model,persistence);
    controller.Activate(kAccount);controller.Activate(Store::kGog,kAccount);
    wait(controller,[&]{return model.state().update_check.loaded;});
    const auto& games=model.state().update_check.games;
    assert(games.size()==2 && games.at(SteamId(42)).newer && games.at(witcher).new_dlc);
    // Steam signs out: GOG's update stays.
    controller.Reset();
    assert(games.size()==1 && games.count(witcher) && model.state().update_check.loaded);
    controller.FlushAndShutdown();
  }
}
int main() {
  const auto root=std::filesystem::temp_directory_path()/("playden-persistence-integration-"+std::to_string(getpid()));
  std::filesystem::create_directories(root);RestartAndSignOut(root);EditWhileLoading(root/"delayed");SortSurvivesRestart(root/"sort");UpdateChecksSurviveRestart(root/"updates");std::filesystem::remove_all(root);
}
