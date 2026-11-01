#include "app/library_controller.h"
#include "stores/steam.h"
#include <algorithm>
#include <limits>

namespace playden::app {
namespace {
const char* Message(const LibraryEvent& event) {
  using E=steam::LibraryError;
  switch(event.error) {
    case E::kCanceled:return "Library refresh canceled.";
    case E::kNetwork:return "Steam could not be reached. Retry to refresh.";
    case E::kProtocol:return "Steam returned an incomplete or invalid library. Retry to refresh.";
    case E::kCredentialsRevoked:return "Your Steam session needs renewal. Reconnect your account.";
    case E::kNoSession:return "Reconnect Steam to refresh your library.";
    case E::kPersistence:return event.cache_error==StorageError::kNotFound
        ? "No cached library is available. Connect to Steam to refresh."
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
  RefreshOther(others_.back());
}
void LibraryController::RefreshOther(Other& other) {
  const auto store=other.source->store();
  model_.UpdateLibrary({{},true,{},false,model_.state().account(store).cache_available,{},store});
  other.request=other.source->Refresh(0,other.generation);
  if(!other.request) model_.UpdateLibrary({{},false,"Library is busy. Retry to refresh.",false,
                                           model_.state().account(store).cache_available,{},store});
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
void LibraryController::Refresh() {
  for(auto& other:others_) RefreshOther(other);
  if(!account_ || clearing_) return;
  Loading();request_=steam_.Refresh(account_,generation_);offline_=false;
  if(!request_) model_.UpdateLibrary({{},false,"Library is busy. Retry to refresh.",model_.state().library_offline,model_.state().account().cache_available,{}});
}
void LibraryController::Refresh(Store store) {
  for(auto& other:others_) if(other.source->store()==store) RefreshOther(other);
}
void LibraryController::EnsureLicenses() {
  if(!account_ || offline_) return;
  if(steam_.EnsureLicenses(account_,generation_)) license_busy_=true;
}
void LibraryController::NoteGrantFresh() {
  license_busy_=false;
  license_failed_=false;
}
void LibraryController::Reset() {
  steam_.Invalidate(++generation_);request_=0;completed_request_=0;account_=0;clearing_=false;clear_result_.reset();offline_=false;needs_renewal_=false;
  license_busy_=false;license_failed_=false;
}
void LibraryController::Clear() {
  clear_result_.reset();clearing_=true;
  model_.UpdateLibrary({{},false,"Clearing saved library…",model_.state().library_offline,model_.state().account().cache_available,{}});
  if(!account_) { clear_result_=steam::LibraryError::kNone;clearing_=false;return; }
  request_=steam_.Clear(account_,++generation_);
  if(!request_) { clear_result_=steam::LibraryError::kPersistence;clearing_=false; }
}
std::optional<steam::LibraryError> LibraryController::TakeClearResult() {
  auto value=clear_result_;clear_result_.reset();return value;
}
void LibraryController::Tick() {
  for(auto& event:steam_.Pump(16)) TickSteam(event);
  for(auto& other:others_)
    for(auto& event:other.source->Pump(16)) TickOther(other,event);
}
void LibraryController::TickOther(Other& other,LibraryEvent& event) {
  if(!other.request || event.request_id!=other.request || event.account_generation!=other.generation) return;
  const auto store=other.source->store();
  if(event.kind==LibraryEventKind::kLoading) {
    model_.UpdateLibrary({{},true,{},false,model_.state().account(store).cache_available,{},store});
    return;
  }
  if(event.kind==LibraryEventKind::kFailure) {
    // The store's games stay; only its status changes.
    model_.UpdateLibrary({{},false,std::string(Traits(store).name)+" games could not be loaded. Retry to refresh.",false,
                          event.cache_available,{},store});
    other.request=0;return;
  }
  if(event.kind!=LibraryEventKind::kCached && event.kind!=LibraryEventKind::kComplete) return;
  const bool cached=event.kind==LibraryEventKind::kCached;
  if(!model_.UpdateLibrary({std::move(event.games),false,{},false,event.cache_available,Time(event),store})) {
    model_.UpdateLibrary({{},false,"Library data could not be displayed.",false,event.cache_available,{},store});
    if(!cached) other.request=0;
    return;
  }
  if(cached) model_.UpdateLibrary({{},true,{},false,event.cache_available,{},store});
  else other.request=0;
}
void LibraryController::TickSteam(LibraryEvent& event) {
  if(event.kind==LibraryEventKind::kUpdated) {
    if(event.license_attempt) {
      license_busy_=false;
      license_failed_=!event.licenses_read;
    }
    // Games named in the background after the last load completed: the whole library again, status untouched.
    if(request_ || clearing_ || !completed_request_ || event.request_id!=completed_request_ ||
       event.account_id!=account_ || event.account_generation!=generation_) return;
    // Keeps the status line, unless saving the bigger library failed.
    const std::string status=event.cache_error!=StorageError::kNone ? "Library loaded, but saving failed. Retry to save."
        : model_.state().account().error;
    model_.UpdateLibrary({std::move(event.games),false,status,false,event.cache_available,Time(event)});
    // The owned list is published before licenses are read. This follow-up is what read them.
    if(event.licenses_read) { licenses_generation_=model_.state().account_generation;++licenses_read_; }
    return;
  }
  if(!request_ || event.request_id!=request_ || event.account_id!=account_ || event.account_generation!=generation_) return;
  if(clearing_) {
    if(event.kind==LibraryEventKind::kCleared || event.kind==LibraryEventKind::kFailure) {
      clear_result_=event.kind==LibraryEventKind::kCleared ? steam::LibraryError::kNone : event.error;
      clearing_=false;request_=0;
      if(*clear_result_==steam::LibraryError::kNone)
        model_.UpdateLibrary({{},false,"Clearing saved session…",false,false,{}});
    }
    return;
  }
  if(event.kind==LibraryEventKind::kLoading) { Loading();return; }
  if(event.kind==LibraryEventKind::kFailure) {
    needs_renewal_=event.error==steam::LibraryError::kCredentialsRevoked || event.error==steam::LibraryError::kNoSession;
    model_.UpdateLibrary({{},false,Message(event),model_.state().account().cache_available || SteamGamesShown(),
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
