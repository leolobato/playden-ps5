#pragma once
#include "homebrew/simulation.hpp"
#include "library/service.h"
#include "stores/library_source.h"
#include <memory>
#include <optional>
#include <vector>

namespace playden::app {
// Presentation-thread projection. The sources own IO and outlive this bridge. Each store's library
// has its own request and generation; the model merges them, so one store's refresh, failure or
// sign-out leaves the others' games alone.
class LibraryController {
 public:
  LibraryController(homebrew::Simulation& model,stores::LibrarySource& steam);
  // Steam's library service, wrapped as its source.
  LibraryController(homebrew::Simulation& model,LibraryService& service);
  // Another store's library. A store without accounts (Imported) is loaded now and on every Refresh; a store
  // with accounts waits for Activate with its signed-in account.
  void AddSource(stores::LibrarySource& source);
  // Steam: the signed-in account's library.
  void Activate(std::uint64_t account_id,bool offline);
  // One store's signed-in account (0: none): its library is loaded, from the saved copy only when offline.
  // Steam forwards to Activate above; a store without accounts ignores it.
  void Activate(Store store,std::uint64_t account_id,bool offline);
  // Steam's library, and every other store's that has no account or a signed-in one.
  void Refresh();
  // One store's library only (not Steam's): its games changed (an imported game was added or removed).
  void Refresh(Store store);
  // Reset drops a store's pending work and its account (the games shown stay), Clear deletes its saved library.
  // The forms without a store are Steam's.
  void Reset();
  void Reset(Store store);
  void Clear();
  void Clear(Store store);
  void Tick();
  void Shutdown();
  std::optional<LibraryError> TakeClearResult();
  std::optional<LibraryError> TakeClearResult(Store store);
  bool NeedsRenewal() const { return offline_ || needs_renewal_; }
  // A Steam load, refresh or clear is still running.
  bool Busy() const { return request_ != 0 || clearing_; }
  // That store's load, refresh or clear is still running.
  bool Busy(Store store) const;
  // That store's library is loaded: it has no accounts, or one is signed in.
  bool Active(Store store) const;
  // The model's account generation when Steam's library last came back from Steam itself (not the saved
  // copy) with the account's licenses read; 0 before that. A load whose license read failed doesn't count.
  std::uint64_t LicensesGeneration() const { return licenses_generation_; }
  // Counts those refreshes.
  std::uint64_t LicensesRead() const { return licenses_read_; }
 private:
  struct Other {
    stores::LibrarySource* source = nullptr;
    std::uint64_t generation = 1,request = 0;
    std::uint64_t account = 0;  // 0: none (always for a store without accounts)
    bool offline = false,clearing = false;
    std::optional<LibraryError> clear_result;
  };
  void Loading();
  void TickSteam(LibraryEvent& event);
  void TickOther(Other& other,LibraryEvent& event);
  void RefreshOther(Other& other);
  void LoadingOther(const Other& other);
  void BusyOther(const Other& other);
  static bool Active(const Other& other);
  Other* Find(Store store);
  const Other* Find(Store store) const;
  bool SteamGamesShown() const;
  homebrew::Simulation& model_;
  std::unique_ptr<stores::LibrarySource> owned_steam_;
  stores::LibrarySource& steam_;
  std::vector<Other> others_;
  std::uint64_t account_=0,generation_=0,request_=0;
  // The last load that completed, for the games its backend names later (kUpdated).
  std::uint64_t completed_request_=0;
  std::uint64_t licenses_generation_=0,licenses_read_=0;
  bool offline_=false,clearing_=false;
  bool needs_renewal_=false;
  std::optional<LibraryError> clear_result_;
};
}
