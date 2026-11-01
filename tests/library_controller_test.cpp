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
// The refreshing pill clears when the owned list arrives. LicensesRead waits for the follow-up.
void OwnedLibraryClearsThePillBeforeLicenses() {
  Simulation model({}, {},ModelMode::Live);
  model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
  Sessions sessions;Cache cache;
  struct Progress final : LibraryBackend {
    std::atomic<bool> release{false},progressed{false};
    LibraryResult Fetch(const StoredSession&,const std::atomic<bool>& cancel) override {
      ReportFetchProgress({LibraryError::kNone,{{SteamId(42),"Owned title",{},{},{}}},false,false});
      progressed=true;
      while(!release && !cancel) std::this_thread::sleep_for(1ms);
      return {LibraryError::kNone,{{SteamId(42),"Owned title",{},{},{}},{SteamId(7),"Family title",{},{},{}}},false,true};
    }
  } backend;
  LibraryService service(backend,sessions,&cache,[]{return 12345;});
  app::LibraryController controller(model,service);
  controller.Activate(123,false);
  Wait(controller,[&]{return backend.progressed && !model.state().library_loading && model.state().games.size()==1 && controller.LicensesRead()==0;});
  assert(model.state().games[0].title=="Owned title");
  backend.release=true;
  Wait(controller,[&]{return !model.state().library_loading && model.state().games.size()==2 && controller.LicensesRead()==1;});
  assert(model.state().games[1].title=="Family title");
}
int main(){OtherSourceSurvivesSteam();StaleSteamCompletionKeepsOtherSource();UpdateRefusesAnotherStoresGames();FreshRefreshAndFailure();OfflineCacheAndClear();StaleCompletionCannotRepopulate();RejectedCacheStillAcceptsFreshResult();RejectedRequestStopsLoading();LateGamesJoinTheLibrary();ManyLateGamesKeepTheCompletion();OwnedLibraryClearsThePillBeforeLicenses();}
