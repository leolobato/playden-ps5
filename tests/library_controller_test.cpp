#include "app/library_controller.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <utility>
#include <mutex>
#include <thread>
#include "fake_library_source.h"
#include "steam_id.h"

using namespace playden;
using namespace playden::homebrew;
using namespace playden::steam;
using namespace std::chrono_literals;
namespace {
struct Sessions final : SessionRepository {
  mutable std::mutex mutex;
  std::optional<StoredSession> value=StoredSession{123,"Synthetic","refresh","access",{}};
  Result<std::optional<StoredSession>> Load() const override { std::lock_guard<std::mutex> lock(mutex); return Result<std::optional<StoredSession>>::Success(value); }
  Result<void> Save(const StoredSession&) const override { assert(false); return Result<void>::Success(); }
  Result<void> Clear() const override { assert(false); return Result<void>::Success(); }
};
struct Cache final : LibraryRepository {
  mutable std::optional<LibrarySnapshot> value;
  mutable std::uint64_t owner=123;
  bool fail_clear=false;
  Result<LibrarySnapshot> Load(std::uint64_t id) const override {
    if(!value || id!=owner) return Result<LibrarySnapshot>::Failure(StorageError::kNotFound,"missing");
    return Result<LibrarySnapshot>::Success(*value);
  }
  Result<void> Save(std::uint64_t id,const LibrarySnapshot& snapshot) const override { owner=id;value=snapshot;return Result<void>::Success(); }
  Result<void> Clear(std::uint64_t id) const override {
    if(fail_clear) return Result<void>::Failure(StorageError::kRemoveFailed,"failure");
    if(owner==id) value.reset(); return Result<void>::Success();
  }
};
struct Backend final : LibraryBackend {
  std::atomic<bool> block{false},release{false};
  std::atomic<int> calls{0};
  LibraryResult result{LibraryError::kNone,{{SteamId(42),"Synthetic title",{},{},{}}},false};
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>& cancel) override {
    ++calls;
    while(block && !release && !cancel) std::this_thread::sleep_for(1ms);
    return result;
  }
};
template<class Predicate> void Wait(app::LibraryController& controller,Predicate predicate) {
  const auto deadline=std::chrono::steady_clock::now()+2s;
  do { controller.Tick(); if(predicate()) return; std::this_thread::sleep_for(1ms); } while(std::chrono::steady_clock::now()<deadline);
  assert(false && "library presentation deadline");
}
void FreshRefreshAndFailure() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;
  LibraryService service(backend,sessions,&cache,[]{return 12345;});
  app::LibraryController controller(model,service);
  controller.Activate(123,false);
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.size()==1;});
  assert(model.state().games[0].id==SteamId(42) && model.state().account().cache_available);
  assert(model.state().account().last_sync==12345 && model.state().account().authentication==AuthState::SignedIn);
  backend.result={LibraryError::kNetwork,{},false};
  controller.Refresh();
  Wait(controller,[&]{return !model.state().library_loading && !model.state().account().error.empty();});
  assert(model.state().games.size()==1 && model.state().library_offline);
  backend.result={LibraryError::kNone,{},false};
  controller.Refresh();
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.empty();});
  assert(model.state().account().error.empty() && model.state().account().cache_available);
}
void OfflineCacheAndClear() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::Offline,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;
  cache.value=LibrarySnapshot{};cache.value->retrieved_at_unix_seconds=100;
  cache.value->games={{SteamId(7),"Offline title",{},{},{}}};
  LibraryService service(backend,sessions,&cache);
  app::LibraryController controller(model,service);
  controller.Activate(123,true);
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.size()==1;});
  assert(backend.calls==0 && model.state().library_offline && model.state().account().authentication==AuthState::Offline);
  cache.fail_clear=true; controller.Clear();
  std::optional<LibraryError> cleared;
  Wait(controller,[&]{cleared=controller.TakeClearResult();return cleared.has_value();});
  assert(*cleared==LibraryError::kPersistence && model.state().games.size()==1);
  cache.fail_clear=false;controller.Clear();
  Wait(controller,[&]{cleared=controller.TakeClearResult();return cleared.has_value();});
  assert(*cleared==LibraryError::kNone && !cache.value);
}
void RejectedCacheStillAcceptsFreshResult() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;
  cache.value=LibrarySnapshot{};
  cache.value->games={{SteamId(7),"   ",{},{},{}}};
  backend.block=true;
  LibraryService service(backend,sessions,&cache);
  app::LibraryController controller(model,service);
  controller.Activate(123,false);
  Wait(controller,[&]{return backend.calls>0;});
  controller.Tick();backend.release=true;
  Wait(controller,[&]{return model.state().games.size()==1 && !model.state().library_loading;});
  assert(model.state().games.front().id==SteamId(42));
}
void RejectedRequestStopsLoading() {
  Simulation model({}, {},ModelMode::Live);
  Sessions sessions;Cache cache;Backend backend;
  LibraryService service(backend,sessions,&cache);service.Shutdown();
  app::LibraryController controller(model,service);controller.Activate(123,false);
  assert(!model.state().library_loading && !model.state().account().error.empty());
}
void StaleCompletionCannotRepopulate() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;backend.block=true;
  LibraryService service(backend,sessions,&cache);
  app::LibraryController controller(model,service);
  controller.Activate(123,false);
  Wait(controller,[&]{return backend.calls>0;});
  controller.Reset();
  model.UpdateAccount({AuthState::SignedOut,{},{},{},{},true});
  backend.release=true;service.Shutdown();controller.Tick();
  assert(model.state().games.empty() && model.state().account().id.empty());
}
constexpr const char* kImported="0f8fad5b-d9cb-469f-a165-70867728950e";
bool Shows(const Simulation& model,const GameId& id) {
  for(const auto& game:model.state().games) if(game.id==id) return true;
  return false;
}
// Another store's games sit next to Steam's: Steam refreshing, failing or signing out leaves them alone.
void OtherSourceSurvivesSteam() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;
  LibraryService service(backend,sessions,&cache);
  test::FakeLibrarySource imported;imported.games={test::ImportedGame(kImported,"Imported title")};
  app::LibraryController controller(model,service);
  controller.AddSource(imported);
  controller.Activate(123,false);
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.size()==2;});
  const auto local=GameId::Local(kImported);
  assert(model.state().games[0].id==SteamId(42) && model.state().games[0].source=="Steam");
  assert(model.state().games[1].id==local && model.state().games[1].source=="Imported");
  assert(model.state().navigation.route==Route::Home && model.view().subtitle=="Your library");
  // The player looks at the imported game while Steam refreshes.
  assert(model.action("select:"+ToKey(local)));
  const auto selected=model.state().navigation.selected_game_id;
  assert(selected==local);
  backend.result={LibraryError::kNone,{{SteamId(43),"Another title",{},{},{}}},false};
  controller.Refresh();
  Wait(controller,[&]{return !model.state().library_loading && Shows(model,SteamId(43));});
  assert(!Shows(model,SteamId(42)) && Shows(model,local) && model.state().games.size()==2);
  assert(model.state().navigation.selected_game_id==selected);
  backend.result={LibraryError::kNetwork,{},false};
  controller.Refresh();
  Wait(controller,[&]{return !model.state().library_loading && !model.state().account().error.empty();});
  assert(Shows(model,SteamId(43)) && Shows(model,local) && model.state().library_offline);
  assert(model.state().navigation.selected_game_id==selected);
  // The other store failing keeps its games and leaves Steam's status alone.
  imported.fail=true;backend.result={LibraryError::kNone,{{SteamId(43),"Another title",{},{},{}}},false};
  controller.Refresh();
  Wait(controller,[&]{return !model.state().library_loading && !model.state().account(Store::kLocal).error.empty();});
  assert(Shows(model,local) && Shows(model,SteamId(43)) && model.state().account().error.empty());
  // Signing out of Steam: only Steam's games go, and Playden stays on Home with the imported one.
  controller.Reset();
  model.UpdateAccount({AuthState::SignedOut,{},{},{},{},true});
  assert(model.state().games.size()==1 && Shows(model,local));
  assert(model.state().navigation.route==Route::Home);
  { const auto& v=model.view(); assert(v.focused_row>=0 && v.rows[static_cast<std::size_t>(v.focused_row)].key==model.state().navigation.focus_key); }
  const auto& rows=model.view().rows;
  assert(std::any_of(rows.begin(),rows.end(),[](const ViewRow& row) { return row.key=="home-connect" && row.title=="Connect a store"; }));
}
// A Steam answer that arrives after sign-out brings nothing back, and the other store's game stays.
void StaleSteamCompletionKeepsOtherSource() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;Backend backend;backend.block=true;
  LibraryService service(backend,sessions,&cache);
  test::FakeLibrarySource imported;imported.games={test::ImportedGame(kImported,"Imported title")};
  app::LibraryController controller(model,service);
  controller.AddSource(imported);
  controller.Activate(123,false);
  Wait(controller,[&]{return backend.calls>0 && Shows(model,GameId::Local(kImported));});
  controller.Reset();
  model.UpdateAccount({AuthState::SignedOut,{},{},{},{},true});
  backend.release=true;service.Shutdown();controller.Tick();
  assert(model.state().games.size()==1 && Shows(model,GameId::Local(kImported)));
  assert(!model.state().library_loading && model.state().navigation.route==Route::Home);
}
// A library update names its store: games of another store in it are refused.
void UpdateRefusesAnotherStoresGames() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  assert(!model.UpdateLibrary({{test::ImportedGame(kImported,"Imported title")},false,{},false,false,{}}));
  assert(!model.UpdateLibrary({{{SteamId(42),"Synthetic title",{},{},{}}},false,{},false,false,{},Store::kLocal}));
  assert(model.state().games.empty());
}
}
namespace {
// Another store with accounts: its library follows its own signed-in account, next to Steam's. Synthetic GOG games.
GameSummary GogGame(std::uint64_t product,const char* title) {
  GameSummary game;game.id=GameId::Gog(product);game.title=title;return game;
}
bool ShowsStore(const Simulation& model,Store store) {
  for(const auto& game:model.state().games) if(game.id.store==store) return true;
  return false;
}
// A model with Steam signed in and its synthetic library shown, through a fake Steam source.
struct Stores {
  Simulation model{{},{},ModelMode::Live};
  test::FakeLibrarySource steam{Store::kSteam},gog{Store::kGog};
  app::LibraryController controller{model,steam};
  Stores() {
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    steam.games={{SteamId(42),"Synthetic title",{},{},{}}};
    gog.games={GogGame(1207658924,"Synthetic GOG title")};
    controller.Activate(123,false);controller.Tick();
    assert(Shows(model,SteamId(42)) && !model.state().library_loading);
  }
};
void AccountStoreWaitsForActivate() {
  Stores s;
  s.controller.AddSource(s.gog);s.controller.Tick();
  assert(s.gog.refreshes==0 && s.gog.cached_loads==0 && !s.controller.Active(Store::kGog) && !s.controller.Busy(Store::kGog));
  assert(!ShowsStore(s.model,Store::kGog) && !s.model.state().library_loading && s.model.state().games.size()==1);
  // Not refreshed while signed out, nor with account 0.
  s.controller.Refresh();s.controller.Refresh(Store::kGog);s.controller.Activate(Store::kGog,0,false);s.controller.Tick();
  assert(s.gog.refreshes==0 && s.gog.cached_loads==0 && !ShowsStore(s.model,Store::kGog) && !s.model.state().library_loading);
  // A store without accounts added the same way loads at once, with account 0.
  test::FakeLibrarySource local;local.games={test::ImportedGame(kImported,"Imported title")};
  s.controller.AddSource(local);
  assert(local.refreshes==1 && local.last_account==0 && s.controller.Active(Store::kLocal));
  s.controller.Tick();
  assert(Shows(s.model,GameId::Local(kImported)) && !s.model.state().library_loading);
}
void ActivateLoadsTheAccount() {
  Stores s;
  s.controller.AddSource(s.gog);
  s.gog.cache_available=true;
  s.controller.Activate(Store::kGog,0xabc,false);
  assert(s.gog.refreshes==1 && s.gog.cached_loads==0 && s.gog.last_account==0xabc);
  assert(s.controller.Active(Store::kGog) && s.controller.Busy(Store::kGog) && !s.controller.Busy());
  assert(s.model.state().library_loading);
  s.controller.Tick();
  const auto gog=GameId::Gog(1207658924);
  assert(s.model.state().games.size()==2 && s.model.state().games[0].id==SteamId(42));
  assert(s.model.state().games[1].id==gog && s.model.state().games[1].source=="GOG");
  assert(!s.model.state().library_loading && !s.controller.Busy(Store::kGog));
  assert(s.model.state().account(Store::kGog).cache_available && s.model.state().account(Store::kGog).error.empty());
  // Steam's library and status are untouched.
  assert(s.steam.refreshes==1 && s.model.state().account().error.empty() && !s.model.state().account().cache_available);
  assert(!s.model.state().library_offline && s.model.state().account().authentication==AuthState::SignedIn);
  // The saved copy's availability follows each answer.
  s.gog.cache_available=false;
  s.controller.Refresh(Store::kGog);s.controller.Tick();
  assert(s.gog.refreshes==2 && s.gog.last_account==0xabc && !s.model.state().account(Store::kGog).cache_available);
  assert(Shows(s.model,gog) && s.steam.refreshes==1);
  // Activate for Steam is Steam's own.
  s.controller.Activate(Store::kSteam,456,false);
  assert(s.steam.refreshes==2 && s.steam.last_account==456 && s.controller.Active(Store::kSteam));
  s.controller.Tick();
  assert(Shows(s.model,SteamId(42)) && Shows(s.model,gog));
}
void OfflineActivateLoadsCached() {
  Stores s;
  s.controller.AddSource(s.gog);
  s.controller.Activate(Store::kGog,0xabc,true);
  assert(s.gog.cached_loads==1 && s.gog.refreshes==0 && s.gog.last_account==0xabc);
  s.controller.Tick();
  // Offline, the saved copy is the whole answer.
  assert(Shows(s.model,GameId::Gog(1207658924)) && !s.model.state().library_loading && !s.controller.Busy(Store::kGog));
  // Online, a saved copy shown first keeps the store loading until the fresh library arrives.
  s.gog.hold=true;
  s.controller.Activate(Store::kGog,0xabc,false);
  assert(s.gog.refreshes==1 && s.gog.held.size()==1);
  auto cached=s.gog.held.front();cached.kind=LibraryEventKind::kCached;cached.licenses_read=false;
  cached.games={GogGame(1,"Synthetic saved GOG title")};
  s.gog.Push(cached);s.controller.Tick();
  assert(Shows(s.model,GameId::Gog(1)) && s.model.state().library_loading && s.controller.Busy(Store::kGog));
  s.gog.Release();s.controller.Tick();
  assert(!Shows(s.model,GameId::Gog(1)) && Shows(s.model,GameId::Gog(1207658924)));
  assert(!s.model.state().library_loading && !s.controller.Busy(Store::kGog));
}
// GOG's library arrives in batches: each kCached of an online load shows the games so far and keeps the store
// loading; the kComplete ends it, with the whole library.
void BatchesKeepTheStoreLoading() {
  Stores s;
  s.controller.AddSource(s.gog);
  s.gog.hold=true;
  s.controller.Activate(Store::kGog,0xabc,false);
  assert(s.gog.held.size()==1);
  const auto answer=s.gog.held.front();
  auto batch=answer;batch.kind=LibraryEventKind::kCached;batch.licenses_read=false;
  batch.games={GogGame(1,"Synthetic first GOG batch")};
  s.gog.Push(batch);s.controller.Tick();
  assert(Shows(s.model,GameId::Gog(1)) && s.model.state().library_loading && s.controller.Busy(Store::kGog));
  batch.games={GogGame(1,"Synthetic first GOG batch"),GogGame(2,"Synthetic second GOG batch")};
  s.gog.Push(batch);s.controller.Tick();
  assert(Shows(s.model,GameId::Gog(1)) && Shows(s.model,GameId::Gog(2)));
  assert(s.model.state().library_loading && s.controller.Busy(Store::kGog));
  assert(Shows(s.model,SteamId(42)) && s.model.state().account(Store::kGog).error.empty());
  auto complete=answer;
  complete.games={GogGame(1,"Synthetic first GOG batch"),GogGame(2,"Synthetic second GOG batch"),
                  GogGame(3,"Synthetic third GOG batch")};
  s.gog.held.clear();
  s.gog.Push(complete);s.controller.Tick();
  assert(Shows(s.model,GameId::Gog(3)) && s.model.state().games.size()==4);
  assert(!s.model.state().library_loading && !s.controller.Busy(Store::kGog));
}
void StaleAndForeignEventsAreDropped() {
  Stores s;
  s.controller.AddSource(s.gog);
  s.gog.hold=true;
  s.controller.Activate(Store::kGog,0xabc,false);
  assert(s.gog.held.size()==1);
  const auto answer=s.gog.held.front();
  // Another account's answer, same request and generation.
  auto foreign=answer;foreign.account_id=0xdef;foreign.games={GogGame(2,"Synthetic foreign GOG title")};
  s.gog.Push(foreign);s.controller.Tick();
  assert(!ShowsStore(s.model,Store::kGog) && s.controller.Busy(Store::kGog));
  // An older generation's answer, same request and account.
  auto older=answer;older.account_generation=answer.account_generation-1;older.games={GogGame(3,"Synthetic older GOG title")};
  s.gog.Push(older);s.controller.Tick();
  assert(!ShowsStore(s.model,Store::kGog) && s.controller.Busy(Store::kGog));
  // After Reset, the account's own answer is late.
  const int invalidated=s.gog.invalidated;
  s.controller.Reset(Store::kGog);
  assert(s.gog.invalidated==invalidated+1 && s.gog.last_invalidated>answer.account_generation);
  assert(!s.controller.Active(Store::kGog) && !s.controller.Busy(Store::kGog));
  // The dropped load stops loading: nothing else will answer it.
  assert(!s.model.state().library_loading && !s.model.state().library_syncing);
  s.gog.Release();s.controller.Tick();
  assert(!ShowsStore(s.model,Store::kGog) && Shows(s.model,SteamId(42)) && !s.model.state().library_loading);
  // A late answer for the old account, then the new account's: only the new one shows.
  s.controller.Activate(Store::kGog,0xabc,false);
  s.controller.Reset(Store::kGog);
  s.gog.games={GogGame(4,"Synthetic other GOG title")};
  s.controller.Activate(Store::kGog,0x123,false);
  assert(s.gog.held.size()==2 && s.gog.last_account==0x123);
  s.gog.Release();s.controller.Tick();
  assert(Shows(s.model,GameId::Gog(4)) && !Shows(s.model,GameId::Gog(1207658924)) && s.model.state().games.size()==2);
  assert(!s.model.state().library_loading && !s.controller.Busy(Store::kGog));
  // Steam's Reset leaves the GOG library and account alone.
  s.controller.Reset();
  assert(s.controller.Active(Store::kGog) && Shows(s.model,GameId::Gog(4)));
}
// A "Can't play" dialog with the reason.
bool ToldPlayStopped(const Simulation& model,const std::string& reason) {
  for(const auto& modal:model.state().navigation.modals)
    if(modal.title=="Can't play" && modal.body.find(reason)!=std::string::npos) return true;
  return false;
}
// A sign-out while a saved copy shows and the fresh library is awaited: the store stops loading, and its
// games and status stay (what a sign-out removes is the caller's rule). Steam's own load is untouched.
void ResetDuringLoadStopsLoading() {
  Stores s;
  s.controller.AddSource(s.gog);
  // A GOG game that stopped at start waits for GOG's library to name it.
  s.model.NotifyPlayStopped(GameId::Gog(7),"Synthetic stop reason.");
  assert(!ToldPlayStopped(s.model,"Synthetic stop reason."));
  s.gog.hold=true;
  s.controller.Activate(Store::kGog,0xabc,false);
  assert(s.gog.held.size()==1 && s.model.state().library_loading);
  s.controller.Reset(Store::kGog);
  // The load that would have named it is dropped: it is told now, without the name.
  assert(!s.model.state().library_loading && ToldPlayStopped(s.model,"Synthetic stop reason."));
  s.gog.held.clear();
  s.controller.Activate(Store::kGog,0xabc,false);
  assert(s.gog.held.size()==1 && s.model.state().library_loading);
  auto cached=s.gog.held.front();cached.kind=LibraryEventKind::kCached;cached.licenses_read=false;
  cached.games={GogGame(1,"Synthetic saved GOG title")};
  s.gog.Push(cached);s.controller.Tick();
  assert(Shows(s.model,GameId::Gog(1)) && s.model.state().library_loading);
  AccountUpdate expired;expired.authentication=AuthState::Expired;expired.error="Synthetic expiry text.";expired.store=Store::kGog;
  s.model.UpdateAccount(expired);
  s.controller.Reset(Store::kGog);
  assert(!s.model.state().library_loading && !s.model.state().library_syncing && !s.controller.Busy(Store::kGog));
  assert(Shows(s.model,GameId::Gog(1)) && Shows(s.model,SteamId(42)));
  assert(s.model.state().account(Store::kGog).error=="Synthetic expiry text.");
  s.gog.Release();s.controller.Tick();
  assert(!s.model.state().library_loading && Shows(s.model,GameId::Gog(1)) && !Shows(s.model,GameId::Gog(1207658924)));
  // A Steam load running next to it keeps the library loading.
  s.steam.hold=true;
  s.controller.Activate(Store::kGog,0xabc,false);
  s.controller.Refresh();
  assert(s.controller.Busy() && s.controller.Busy(Store::kGog));
  s.controller.Reset(Store::kGog);
  assert(s.model.state().library_loading && s.controller.Busy());
  s.steam.Release();s.controller.Tick();
  assert(!s.model.state().library_loading);
}
void ClearPerStore() {
  Stores s;
  s.controller.AddSource(s.gog);
  // Nothing signed in: nothing to delete.
  s.controller.Clear(Store::kGog);
  assert(s.gog.clears==0 && !s.controller.Busy(Store::kGog));
  auto result=s.controller.TakeClearResult(Store::kGog);
  assert(result && *result==LibraryError::kNone && !s.controller.TakeClearResult(Store::kGog));
  assert(s.model.state().account(Store::kGog).error.empty());
  s.controller.Activate(Store::kGog,0xabc,false);s.controller.Tick();
  s.gog.hold=true;
  s.controller.Clear(Store::kGog);
  assert(s.gog.clears==1 && s.gog.last_account==0xabc && s.controller.Busy(Store::kGog) && !s.controller.Busy());
  assert(!s.controller.TakeClearResult(Store::kGog));
  // No refresh while the saved library is deleted.
  s.controller.Refresh();
  assert(s.gog.refreshes==1 && s.steam.refreshes==2);
  s.gog.Release();s.controller.Tick();
  result=s.controller.TakeClearResult(Store::kGog);
  assert(result && *result==LibraryError::kNone && !s.controller.Busy(Store::kGog));
  assert(!s.controller.TakeClearResult());
  // A failed delete gives its error.
  s.gog.hold=false;s.gog.fail_clear=true;s.gog.error=LibraryError::kPersistence;
  s.controller.Clear(Store::kGog);s.controller.Tick();
  result=s.controller.TakeClearResult(Store::kGog);
  assert(result && *result==LibraryError::kPersistence && !s.controller.TakeClearResult());
  // Steam's Clear is Steam's own.
  s.controller.Clear();s.controller.Tick();
  result=s.controller.TakeClearResult();
  assert(result && *result==LibraryError::kNone && s.steam.clears==1 && s.gog.clears==2);
  assert(!s.controller.TakeClearResult(Store::kGog));
}
// A GOG sign-out: the controller asks the source to delete the saved library while the account is still known, then
// resets the store at once. The source still deletes it (it runs an accepted Clear as a barrier); its late answer
// changes nothing, and a Clear after the reset has no account to delete.
void ClearThenResetStillDeletes() {
  Stores s;
  s.controller.AddSource(s.gog);
  s.controller.Activate(Store::kGog,0xabc,false);s.controller.Tick();
  assert(Shows(s.model,GameId::Gog(1207658924)));
  s.gog.hold=true;
  s.controller.Clear(Store::kGog);
  s.controller.Reset(Store::kGog);
  assert(s.gog.clears==1 && s.gog.last_account==0xabc);
  assert(!s.controller.Busy(Store::kGog) && !s.controller.Active(Store::kGog) && !s.model.state().library_loading);
  s.gog.Release();s.controller.Tick();
  assert(!s.controller.TakeClearResult(Store::kGog) && !s.model.state().library_loading);
  assert(Shows(s.model,GameId::Gog(1207658924)) && Shows(s.model,SteamId(42)));
  s.controller.Clear(Store::kGog);
  assert(s.gog.clears==1);
  const auto result=s.controller.TakeClearResult(Store::kGog);
  assert(result && *result==LibraryError::kNone);
}
void FailureMessagesNameTheStore() {
  Stores s;
  test::FakeLibrarySource local;local.games={test::ImportedGame(kImported,"Imported title")};
  s.controller.AddSource(s.gog);s.controller.AddSource(local);
  s.controller.Activate(Store::kGog,0xabc,false);s.controller.Tick();
  struct Case { LibraryError error;StorageError cache;const char* steam;const char* gog; };
  const Case cases[]={
    {LibraryError::kNetwork,StorageError::kNone,"Steam could not be reached. Retry to refresh.",
     "GOG could not be reached. Retry to refresh."},
    {LibraryError::kProtocol,StorageError::kNone,"Steam returned an incomplete or invalid library. Retry to refresh.",
     "GOG returned an incomplete or invalid library. Retry to refresh."},
    {LibraryError::kCredentialsRevoked,StorageError::kNone,"Your Steam session needs renewal. Reconnect your account.",
     "Your GOG session needs renewal. Reconnect your account."},
    {LibraryError::kNoSession,StorageError::kNone,"Reconnect Steam to refresh your library.",
     "Reconnect GOG to refresh your library."},
    {LibraryError::kPersistence,StorageError::kNotFound,"No cached library is available. Connect to Steam to refresh.",
     "No cached library is available. Connect to GOG to refresh."},
    {LibraryError::kPersistence,StorageError::kRemoveFailed,"Saved library data is unavailable. Retry to refresh.",
     "Saved library data is unavailable. Retry to refresh."},
    {LibraryError::kCanceled,StorageError::kNone,"Library refresh canceled.","Library refresh canceled."},
  };
  for(const auto& c:cases) {
    for(auto* source:{&s.steam,&s.gog,&local}) { source->fail=true;source->error=c.error;source->cache_error=c.cache; }
    s.controller.Refresh();s.controller.Tick();
    assert(s.model.state().account().error==c.steam);
    assert(s.model.state().account(Store::kGog).error==c.gog);
    // Imported keeps its own text.
    assert(s.model.state().account(Store::kLocal).error=="Imported games could not be loaded. Retry to refresh.");
    // The games stay.
    assert(Shows(s.model,SteamId(42)) && Shows(s.model,GameId::Gog(1207658924)) && Shows(s.model,GameId::Local(kImported)));
  }
  // One store failing leaves the other's status alone.
  for(auto* source:{&s.steam,&s.gog,&local}) source->fail=false;
  s.controller.Refresh();s.controller.Tick();
  assert(s.model.state().account().error.empty() && s.model.state().account(Store::kGog).error.empty());
  s.gog.fail=true;s.gog.error=LibraryError::kNetwork;s.gog.cache_error=StorageError::kNone;
  s.controller.Refresh(Store::kGog);s.controller.Tick();
  assert(s.model.state().account(Store::kGog).error=="GOG could not be reached. Retry to refresh.");
  assert(s.model.state().account().error.empty() && !s.model.state().library_offline);
}
void RefreshCoversActiveStores() {
  Stores s;
  test::FakeLibrarySource local,epic{Store::kEpic};
  s.controller.AddSource(s.gog);s.controller.AddSource(local);s.controller.AddSource(epic);
  s.controller.Activate(Store::kGog,0xabc,true);s.controller.Tick();
  assert(s.steam.refreshes==1 && local.refreshes==1 && s.gog.refreshes==0 && s.gog.cached_loads==1 && epic.refreshes==0);
  s.controller.Refresh();
  assert(s.steam.refreshes==2 && local.refreshes==2 && s.gog.refreshes==1 && epic.refreshes==0 && epic.cached_loads==0);
  assert(s.steam.last_account==123 && local.last_account==0 && s.gog.last_account==0xabc);
  s.controller.Tick();
  // Signed out of GOG: skipped.
  s.controller.Reset(Store::kGog);
  s.controller.Refresh();
  assert(s.steam.refreshes==3 && local.refreshes==3 && s.gog.refreshes==1 && epic.refreshes==0);
  s.controller.Tick();
  assert(!s.model.state().library_loading);
}
}
// A backend whose games named after the load arrive through the callback.
struct LateBackend final : LibraryBackend {
  std::mutex mutex;
  std::function<void()> callback;
  std::vector<GameSummary> late;
  std::uint64_t late_account=123;
  std::atomic<int> calls{0};
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>&) override {
    ++calls;
    return {LibraryError::kNone,{{SteamId(42),"Synthetic title",{},{},{}}},false,true};
  }
  std::vector<GameSummary> TakeLateGames(std::uint64_t account) override {
    std::lock_guard<std::mutex> lock(mutex);
    if(account!=late_account) return {};
    return std::exchange(late,{});
  }
  void SetLateGamesCallback(std::function<void()> value) override { std::lock_guard<std::mutex> lock(mutex);callback=std::move(value); }
  void Name(GameSummary game) {
    std::function<void()> notify;
    { std::lock_guard<std::mutex> lock(mutex);late.push_back(std::move(game));notify=callback; }
    if(notify) notify();
  }
};
// Games the backend names after a load completed join the Library and the cache, without a Loading state or
// a status change; a sign-out drops them.
void LateGamesJoinTheLibrary() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;LateBackend backend;
  {
    LibraryService service(backend,sessions,&cache,[]{return 12345;});
    app::LibraryController controller(model,service);
    controller.Activate(123,false);
    Wait(controller,[&]{return !model.state().library_loading && model.state().games.size()==1;});
    GameSummary late{SteamId(43),"Synthetic late",{},{},{}};late.family_shared=true;
    backend.Name(late);
    Wait(controller,[&]{return model.state().games.size()==2;});
    assert(!model.state().library_loading && model.state().account().error.empty());
    assert(cache.value && cache.value->games.size()==2 && cache.value->schema_version==7);
    // The same game again, or one for another account, changes nothing.
    backend.Name(late);
    backend.late_account=456;
    backend.Name({SteamId(44),"Synthetic other",{},{},{}});
    for(int i=0;i<50;++i){controller.Tick();std::this_thread::sleep_for(1ms);}
    assert(model.state().games.size()==2 && cache.value->games.size()==2);
    backend.late_account=123;
    // After a sign-out, late games are not shown.
    controller.Reset();
    backend.Name({SteamId(45),"Synthetic after reset",{},{},{}});
    for(int i=0;i<50;++i){controller.Tick();std::this_thread::sleep_for(1ms);}
    assert(std::none_of(model.state().games.begin(),model.state().games.end(),
                        [](const auto& game){return game.id==SteamId(45);}));
  }
  // The service's shutdown unhooked the callback.
  assert(!backend.callback);
}
// Many late games while the UI isn't pumping: the updates collapse into one, so the completion isn't pushed out.
void ManyLateGamesKeepTheCompletion() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;LateBackend backend;
  LibraryService service(backend,sessions,&cache,[]{return 12345;});
  app::LibraryController controller(model,service);
  controller.Activate(123,false);
  const auto deadline=std::chrono::steady_clock::now()+2s;
  while(backend.calls==0 && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
  std::this_thread::sleep_for(50ms);
  for(std::uint32_t app=100;app<200;++app) {
    backend.Name({SteamId(app),"Synthetic late "+std::to_string(app),{},{},{}});
    std::this_thread::sleep_for(1ms);
  }
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.size()==101;});
}
struct Collections final : CollectionsRepository {
  mutable std::optional<std::vector<LibraryCollection>> value;
  mutable int saves=0;
  Result<std::vector<LibraryCollection>> Load(std::uint64_t) const override {
    if(!value) return Result<std::vector<LibraryCollection>>::Failure(StorageError::kNotFound,"missing");
    return Result<std::vector<LibraryCollection>>::Success(*value);
  }
  Result<void> Save(std::uint64_t,const std::vector<LibraryCollection>& collections) const override { value=collections;++saves;return Result<void>::Success(); }
  Result<void> Clear(std::uint64_t) const override { value.reset();return Result<void>::Success(); }
};
struct CollectionsBackend final : LibraryBackend {
  std::mutex mutex;
  std::optional<std::vector<LibraryCollection>> collections;
  LibraryError error=LibraryError::kNone;
  LibraryResult Fetch(const StoredSession&,const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(mutex);
    return {error,{{SteamId(42),"Synthetic title",{},{},{}},{SteamId(43),"Synthetic other",{},{},{}}},false,true};
  }
  std::optional<std::vector<LibraryCollection>> FetchCollections(const StoredSession&,const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(mutex);
    return collections;
  }
  void Set(std::optional<std::vector<LibraryCollection>> value,LibraryError fetch=LibraryError::kNone) {
    std::lock_guard<std::mutex> lock(mutex);collections=std::move(value);error=fetch;
  }
};
// #536: Steam collections come with the library, are saved, survive a failed read and an offline start, fall back
// when deleted, and go with a sign-out.
void SteamCollectionsReachTheLibrary() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  const auto token=model.BeginOrganizationLoad(kDeviceOrganization);
  OrganizationSnapshot organization;organization.scope="steam-collection:uc-b";
  assert(model.ApplyOrganization(kDeviceOrganization,token,organization));
  Sessions sessions;Cache cache;Collections saved;CollectionsBackend backend;
  backend.Set(std::vector<LibraryCollection>{{"uc-a","Couch",{SteamId(42),SteamId(99)}},{"uc-b","Backlog",{SteamId(43)}}});
  {
    LibraryService service(backend,sessions,&cache,[]{return 12345;},&saved);
    app::LibraryController controller(model,service);
    controller.Activate(123,false);
    Wait(controller,[&]{return !model.state().library_loading && model.state().store_collections.size()==2;});
    assert(model.state().store_collections_known.count(Store::kSteam) && saved.value && saved.value->size()==2);
    assert(model.state().library.scope=="steam-collection:uc-b" && model.visible_games().size()==1);
    // Steam can't give them this time: the saved list stays.
    backend.Set(std::nullopt);
    controller.Refresh();
    Wait(controller,[&]{return !model.state().library_loading && !controller.Busy();});
    assert(model.state().store_collections.size()==2 && saved.value->size()==2);
    // Deleted on the PC while its rail row has the focus: the chosen one falls back to Steam's games, the focus
    // leaves the row, and Cross right away never chooses the deleted collection.
    for(int i=0;i<6 && model.state().navigation.route!=Route::Library;++i) model.action("r1");
    assert(model.state().navigation.route==Route::Library);
    for(int i=0;i<8 && !model.state().navigation.library_rail_focused;++i) model.action("left");
    assert(model.state().navigation.library_rail_focused &&
           model.state().navigation.focus_key=="rail-steam-collection:uc-b");
    backend.Set(std::vector<LibraryCollection>{{"uc-a","Couch renamed",{SteamId(42)}}});
    controller.Refresh();
    Wait(controller,[&]{return !model.state().library_loading && model.state().store_collections.size()==1;});
    assert(model.state().store_collections[0].name=="Couch renamed" && model.state().library.scope=="steam");
    assert(model.state().navigation.focus_key!="rail-steam-collection:uc-b");
    model.action("cross");
    assert(model.state().library.scope.find("uc-b")==std::string::npos);
    // A sign-out clears them with the saved library.
    controller.Clear();
    std::optional<LibraryError> cleared;
    Wait(controller,[&]{cleared=controller.TakeClearResult();return cleared.has_value();});
    assert(*cleared==LibraryError::kNone && !saved.value && model.state().store_collections.empty() &&
           model.state().store_collections_known.count(Store::kSteam));
  }
  // Offline: the saved list arrives with the saved library.
  Simulation offline({}, {},ModelMode::Live);
  offline.UpdateAccount({AuthState::Offline,"123","Synthetic",{},{},true});
  cache.value=LibrarySnapshot{};cache.value->retrieved_at_unix_seconds=100;
  cache.value->games={{SteamId(7),"Offline title",{},{},{}}};
  saved.value=std::vector<LibraryCollection>{{"uc-c","Saved",{SteamId(7)}}};
  LibraryService service(backend,sessions,&cache,{},&saved);
  app::LibraryController controller(offline,service);
  controller.Activate(123,true);
  Wait(controller,[&]{return !offline.state().library_loading && offline.state().store_collections.size()==1;});
  assert(offline.state().store_collections[0].id=="uc-c");
  // The organization loads after the lists: a saved collection Steam no longer has falls back at once.
  const auto late=offline.BeginOrganizationLoad(kDeviceOrganization);
  OrganizationSnapshot gone;gone.scope="steam-collection:uc-gone";
  assert(offline.ApplyOrganization(kDeviceOrganization,late,gone));
  assert(offline.state().library.scope=="steam");
  const auto kept=offline.BeginOrganizationLoad(kDeviceOrganization);
  OrganizationSnapshot present;present.scope="steam-collection:uc-c";
  assert(offline.ApplyOrganization(kDeviceOrganization,kept,present));
  assert(offline.state().library.scope=="steam-collection:uc-c");
}
int main(){AccountStoreWaitsForActivate();ActivateLoadsTheAccount();OfflineActivateLoadsCached();BatchesKeepTheStoreLoading();StaleAndForeignEventsAreDropped();ResetDuringLoadStopsLoading();ClearPerStore();ClearThenResetStillDeletes();FailureMessagesNameTheStore();RefreshCoversActiveStores();
  OtherSourceSurvivesSteam();StaleSteamCompletionKeepsOtherSource();UpdateRefusesAnotherStoresGames();FreshRefreshAndFailure();OfflineCacheAndClear();StaleCompletionCannotRepopulate();RejectedCacheStillAcceptsFreshResult();RejectedRequestStopsLoading();LateGamesJoinTheLibrary();ManyLateGamesKeepTheCompletion();SteamCollectionsReachTheLibrary();}
