#include "app/library_controller.h"
#include "stores/steam.h"
#include <algorithm>
#include <limits>
#include <string>

namespace playden::app {
namespace {
// The status line for a failed load of a store with accounts. Steam's texts name Steam.
std::string Message(const LibraryEvent& event,Store store) {
  using E=LibraryError;
  const std::string name(Traits(store).name);
  switch(event.error) {
    case E::kCanceled:return "Library refresh canceled.";
    case E::kNetwork:return name+" could not be reached. Retry to refresh.";
    case E::kProtocol:return name+" returned an incomplete or invalid library. Retry to refresh.";
    case E::kCredentialsRevoked:return "Your "+name+" session needs renewal. Reconnect your account.";
    case E::kNoSession:return "Reconnect "+name+" to refresh your library.";
    case E::kPersistence:return event.cache_error==StorageError::kNotFound
        ? "No cached library is available. Connect to "+name+" to refresh."
        : "Saved library data is unavailable. Retry to refresh.";
    case E::kNone:return "";
  }
  return "Library refresh failed.";
}
std::optional<std::int64_t> Time(const LibraryEvent& event) {
  return event.retrieved_at_unix_seconds<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
      ? std::optional<std::int64_t>(static_cast<std::int64_t>(event.retrieved_at_unix_seconds)) : std::nullopt;
}
}
LibraryController::LibraryController(homebrew::Simulation& model,stores::LibrarySource& steam)
    :model_(model),steam_(steam) {}
LibraryController::LibraryController(homebrew::Simulation& model,LibraryService& service)
    :model_(model),owned_steam_(std::make_unique<stores::SteamLibrarySource>(service)),steam_(*owned_steam_) {}
void LibraryController::AddSource(stores::LibrarySource& source) {
  if(source.store()==Store::kSteam) return;
  for(const auto& other:others_) if(other.source==&source || other.source->store()==source.store()) return;
  others_.push_back({&source});
  // A store with accounts is skipped here: it loads once one is signed in (Activate).
  RefreshOther(others_.back());
}
LibraryController::Other* LibraryController::Find(Store store) {
  for(auto& other:others_) if(other.source->store()==store) return &other;
  return nullptr;
}
const LibraryController::Other* LibraryController::Find(Store store) const {
  for(const auto& other:others_) if(other.source->store()==store) return &other;
  return nullptr;
}
bool LibraryController::Active(const Other& other) {
  return !Traits(other.source->store()).needs_account || other.account!=0;
}
void LibraryController::LoadingOther(const Other& other) {
  const auto store=other.source->store();
  model_.UpdateLibrary({{},true,{},false,model_.state().account(store).cache_available,{},store});
}
void LibraryController::BusyOther(const Other& other) {
  const auto store=other.source->store();
  model_.UpdateLibrary({{},false,"Library is busy. Retry to refresh.",false,
                        model_.state().account(store).cache_available,{},store});
}
void LibraryController::RefreshOther(Other& other) {
  if(!Active(other) || other.clearing) return;
  LoadingOther(other);
  other.offline=false;
  other.request=other.source->Refresh(other.account,other.generation);
  if(!other.request) BusyOther(other);
}
bool LibraryController::SteamGamesShown() const {
  const auto& games=model_.state().games;
  return std::any_of(games.begin(),games.end(),[](const homebrew::Game& game) { return game.id.store==Store::kSteam; });
}
void LibraryController::Shutdown() {
  steam_.Shutdown();
  for(auto& other:others_) other.source->Shutdown();
}
void LibraryController::Loading() {
  model_.UpdateLibrary({{},true,{},model_.state().library_offline,model_.state().account().cache_available,{}});
}
void LibraryController::Activate(std::uint64_t account,bool offline) {
  Reset();account_=account;offline_=offline;needs_renewal_=false;
  if(!account_) return;
  Loading();
  request_=offline ? steam_.LoadCached(account_,generation_) : steam_.Refresh(account_,generation_);
  if(!request_) model_.UpdateLibrary({{},false,"Library is busy. Retry to refresh.",model_.state().library_offline,model_.state().account().cache_available,{}});
}
void LibraryController::Activate(Store store,std::uint64_t account,bool offline) {
  if(store==Store::kSteam) { Activate(account,offline);return; }
  auto* other=Find(store);
  if(!other || !Traits(store).needs_account) return;
  Reset(store);other->account=account;other->offline=offline;
  if(!account) return;
  LoadingOther(*other);
  other->request=offline ? other->source->LoadCached(account,other->generation)
                         : other->source->Refresh(account,other->generation);
  if(!other->request) BusyOther(*other);
}
void LibraryController::Refresh() {
  for(auto& other:others_) RefreshOther(other);
  if(!account_ || clearing_) return;
  Loading();request_=steam_.Refresh(account_,generation_);offline_=false;
  if(!request_) model_.UpdateLibrary({{},false,"Library is busy. Retry to refresh.",model_.state().library_offline,model_.state().account().cache_available,{}});
}
void LibraryController::Refresh(Store store) {
  for(auto& other:others_) if(other.source->store()==store) RefreshOther(other);
}
void LibraryController::Reset() {
  steam_.Invalidate(++generation_);request_=0;completed_request_=0;account_=0;clearing_=false;clear_result_.reset();offline_=false;needs_renewal_=false;
}
void LibraryController::Reset(Store store) {
  if(store==Store::kSteam) { Reset();return; }
  auto* other=Find(store);
  if(!other) return;
  // The store's games stay: what a sign-out removes is up to the caller. A load it drops stops loading.
  other->source->Invalidate(++other->generation);
  if(other->request) model_.EndLibraryLoading(store);
  other->request=0;other->account=0;other->offline=false;other->clearing=false;other->clear_result.reset();
}
void LibraryController::Clear(Store store) {
  if(store==Store::kSteam) { Clear();return; }
  auto* other=Find(store);
  if(!other) return;
  other->clear_result.reset();
  if(!other->account) { other->clear_result=LibraryError::kNone;return; }
  other->clearing=true;
  model_.UpdateLibrary({{},false,"Clearing saved library…",false,model_.state().account(store).cache_available,{},store});
  other->request=other->source->Clear(other->account,++other->generation);
  if(!other->request) { other->clear_result=LibraryError::kPersistence;other->clearing=false; }
}
std::optional<LibraryError> LibraryController::TakeClearResult(Store store) {
  if(store==Store::kSteam) return TakeClearResult();
  auto* other=Find(store);
  if(!other) return std::nullopt;
  auto value=other->clear_result;other->clear_result.reset();return value;
}
bool LibraryController::Busy(Store store) const {
  if(store==Store::kSteam) return Busy();
  const auto* other=Find(store);
  return other && (other->request!=0 || other->clearing);
}
bool LibraryController::Active(Store store) const {
  if(store==Store::kSteam) return account_!=0;
  const auto* other=Find(store);
  return other && Active(*other);
}
void LibraryController::Clear() {
  clear_result_.reset();clearing_=true;
  model_.UpdateLibrary({{},false,"Clearing saved library…",model_.state().library_offline,model_.state().account().cache_available,{}});
  if(!account_) { clear_result_=LibraryError::kNone;clearing_=false;return; }
  request_=steam_.Clear(account_,++generation_);
  if(!request_) { clear_result_=LibraryError::kPersistence;clearing_=false; }
}
std::optional<LibraryError> LibraryController::TakeClearResult() {
  auto value=clear_result_;clear_result_.reset();return value;
}
void LibraryController::Tick() {
  for(auto& event:steam_.Pump(16)) TickSteam(event);
  for(auto& other:others_)
    for(auto& event:other.source->Pump(16)) TickOther(other,event);
}
void LibraryController::TickOther(Other& other,LibraryEvent& event) {
  const auto store=other.source->store();
  const bool accounts=Traits(store).needs_account;
  if(!other.request || event.request_id!=other.request || event.account_generation!=other.generation ||
     (accounts && event.account_id!=other.account)) return;
  if(other.clearing) {
    if(event.kind==LibraryEventKind::kCleared || event.kind==LibraryEventKind::kFailure) {
      other.clear_result=event.kind==LibraryEventKind::kCleared ? LibraryError::kNone : event.error;
      other.clearing=false;other.request=0;
      if(*other.clear_result==LibraryError::kNone) {
        model_.UpdateLibrary({{},false,"Clearing saved session…",false,false,{},store});
        model_.UpdateStoreCollections(store,{});  // they went with the saved library
      }
    }
    return;
  }
  if(event.kind==LibraryEventKind::kLoading) { LoadingOther(other);return; }
  if(event.kind==LibraryEventKind::kFailure) {
    // The store's games stay; only its status changes.
    model_.UpdateLibrary({{},false,accounts ? Message(event,store)
                              : std::string(Traits(store).name)+" games could not be loaded. Retry to refresh.",
                          false,event.cache_available,{},store});
    other.request=0;return;
  }
  if(event.kind!=LibraryEventKind::kCached && event.kind!=LibraryEventKind::kComplete) return;
  // A saved copy shown while the store is reached keeps the store loading until the fresh library arrives.
  const bool awaiting_fresh=event.kind==LibraryEventKind::kCached && !other.offline;
  if(!model_.UpdateLibrary({std::move(event.games),false,{},false,event.cache_available,Time(event),store})) {
    model_.UpdateLibrary({{},awaiting_fresh,"Library data could not be displayed.",false,event.cache_available,{},store});
    if(!awaiting_fresh) other.request=0;
    return;
  }
  if(event.collections_known) model_.UpdateStoreCollections(store,std::move(event.collections));
  if(awaiting_fresh) model_.UpdateLibrary({{},true,{},false,event.cache_available,{},store});
  else other.request=0;
}
void LibraryController::TickSteam(LibraryEvent& event) {
  if(event.kind==LibraryEventKind::kUpdated) {
    // Games named in the background after the last load completed: the whole library again, status untouched.
    if(request_ || clearing_ || !completed_request_ || event.request_id!=completed_request_ ||
       event.account_id!=account_ || event.account_generation!=generation_) return;
    // Keeps the status line, unless saving the bigger library failed.
    const std::string status=event.cache_error!=StorageError::kNone ? "Library loaded, but saving failed. Retry to save."
        : model_.state().account().error;
    model_.UpdateLibrary({std::move(event.games),false,status,false,event.cache_available,Time(event)});
    if(event.collections_known) model_.UpdateStoreCollections(Store::kSteam,std::move(event.collections));
    return;
  }
  if(!request_ || event.request_id!=request_ || event.account_id!=account_ || event.account_generation!=generation_) return;
  if(clearing_) {
    if(event.kind==LibraryEventKind::kCleared || event.kind==LibraryEventKind::kFailure) {
      clear_result_=event.kind==LibraryEventKind::kCleared ? LibraryError::kNone : event.error;
      clearing_=false;request_=0;
      if(*clear_result_==LibraryError::kNone) {
        model_.UpdateLibrary({{},false,"Clearing saved session…",false,false,{}});
        model_.UpdateStoreCollections(Store::kSteam,{});  // they went with the saved library
      }
    }
    return;
  }
  if(event.kind==LibraryEventKind::kLoading) { Loading();return; }
  if(event.kind==LibraryEventKind::kFailure) {
    needs_renewal_=event.error==LibraryError::kCredentialsRevoked || event.error==LibraryError::kNoSession;
    model_.UpdateLibrary({{},false,Message(event,Store::kSteam),model_.state().account().cache_available || SteamGamesShown(),
                          event.cache_available,{}});
    request_=0;return;
  }
  if(event.kind!=LibraryEventKind::kCached && event.kind!=LibraryEventKind::kComplete) return;
  const auto time=Time(event);
  const bool cached=event.kind==LibraryEventKind::kCached;
  if(!model_.UpdateLibrary({std::move(event.games),false,{},cached,event.cache_available,time})) {
    const bool awaiting_fresh=cached && !offline_;
    model_.UpdateLibrary({{},awaiting_fresh,"Library data could not be displayed.",model_.state().library_offline,
                          model_.state().account().cache_available,{}});
    if(!awaiting_fresh) request_=0;
    return;
  }
  if(event.collections_known) model_.UpdateStoreCollections(Store::kSteam,std::move(event.collections));
  // Only a load that read the account's licenses can tell which games have updates.
  if(!cached && event.licenses_read) { licenses_generation_=model_.state().account_generation;++licenses_read_; }
  if(cached && !offline_) Loading();
  else {
    const char* warning=event.cache_error!=StorageError::kNone ? "Library loaded, but saving failed. Retry to save."
        : event.metadata_partial ? "Some game details are unavailable." : "";
    if(*warning) model_.UpdateLibrary({{},false,warning,cached,event.cache_available,time});
    completed_request_=cached ? 0 : request_;
    request_=0;
  }
}
}
