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
  // Another store's library (one without a Steam account): loaded now and on every Refresh.
  void AddSource(stores::LibrarySource& source);
  // Steam: the signed-in account's library.
  void Activate(std::uint64_t account_id,bool offline);
  void Refresh();
  // Steam: read licenses for the library now showing, when the saved grant is missing or old.
  void EnsureLicenses();
  // The saved grant is fresh: a license read is no longer outstanding.
  void NoteGrantFresh();
  // A library refresh is starting. A license read that already failed is not the answer to this one.
  void NoteLibraryRefresh() { license_failed_ = false; }
  bool LicenseFailed() const { return license_failed_; }
  // One store's library only (not Steam's): its games changed (an imported game was added or removed).
  void Refresh(Store store);
  // Steam only: Reset drops Steam's pending work, Clear deletes Steam's saved library.
  void Reset();
  void Clear();
  void Tick();
  void Shutdown();
  std::optional<steam::LibraryError> TakeClearResult();
  bool NeedsRenewal() const { return offline_ || needs_renewal_; }
  // A Steam load, refresh or clear is still running.
  bool Busy() const { return request_ != 0 || clearing_ || license_busy_; }
  // The model's account generation when Steam's library last came back from Steam itself (not the saved
  // copy) with the account's licenses read; 0 before that. A load whose license read failed doesn't count.
  std::uint64_t LicensesGeneration() const { return licenses_generation_; }
  // Counts those refreshes.
  std::uint64_t LicensesRead() const { return licenses_read_; }
 private:
  struct Other {
    stores::LibrarySource* source = nullptr;
    std::uint64_t generation = 1,request = 0;
  };
  void Loading();
  void TickSteam(LibraryEvent& event);
  void TickOther(Other& other,LibraryEvent& event);
  void RefreshOther(Other& other);
  bool SteamGamesShown() const;
  homebrew::Simulation& model_;
  std::unique_ptr<stores::LibrarySource> owned_steam_;
  stores::LibrarySource& steam_;
  std::vector<Other> others_;
  std::uint64_t account_=0,generation_=0,request_=0;
  // The last load that completed, for the games its backend names later (kUpdated).
  std::uint64_t completed_request_=0;
  std::uint64_t licenses_generation_=0,licenses_read_=0;
  bool offline_=false,clearing_=false,license_busy_=false,license_failed_=false;
  bool needs_renewal_=false;
  std::optional<steam::LibraryError> clear_result_;
};
}
