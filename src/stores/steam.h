#pragma once
#include "steam/app_details_service.h"
#include "steam/secret.h"
#include "steam/service.h"
#include "stores/account.h"
#include "stores/details_source.h"
#include "stores/library_source.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace playden::stores {

// Steam's account over steam::SteamService: the saved session, and Steam's own sign-in (QR code,
// account name and password, Steam Guard codes). Its events are Steam's.
class SteamAccount final : public StoreAccount {
 public:
  SteamAccount(steam::AuthBackend& backend, SessionRepository& repository,
               std::function<void(const std::string&)> log = {})
      : service_(std::make_unique<steam::SteamService>(backend, repository, std::move(log))) {}
  Store store() const override { return Store::kSteam; }
  std::uint64_t RestoreSession() override { return service_ ? service_->RestoreSession() : 0; }
  void Cancel(std::uint64_t request) override { if (service_) service_->Cancel(request); }
  void SignOut() override { if (service_) service_->SignOut(); }

  steam::RequestId BeginQrSignIn() { return service_ ? service_->BeginQrSignIn() : 0; }
  steam::RequestId BeginCredentialSignIn(std::string account_name, std::string password) {
    if (!service_) {
      steam::ZeroizeSecret(password);
      return 0;
    }
    return service_->BeginCredentialSignIn(std::move(account_name), std::move(password));
  }
  void SubmitGuardCode(std::string code) {
    if (service_) service_->SubmitGuardCode(std::move(code));
    else steam::ZeroizeSecret(code);
  }
  std::vector<steam::SteamEvent> Pump(std::size_t max_events) {
    return service_ ? service_->Pump(max_events) : std::vector<steam::SteamEvent>{};
  }
  // Stops the worker; a sign-out already asked for still clears the saved session first.
  void Shutdown() { service_.reset(); }

 private:
  std::unique_ptr<steam::SteamService> service_;
};

// Steam's library over the LibraryService, which keeps Steam's cache and its license-based additions.
class SteamLibrarySource final : public LibrarySource {
 public:
  explicit SteamLibrarySource(LibraryService& service) : service_(service) {}
  Store store() const override { return Store::kSteam; }
  std::uint64_t Refresh(std::uint64_t account, std::uint64_t generation) override {
    return service_.Refresh(account, generation);
  }
  std::uint64_t LoadCached(std::uint64_t account, std::uint64_t generation) override {
    return service_.LoadCached(account, generation);
  }
  std::uint64_t Clear(std::uint64_t account, std::uint64_t generation) override {
    return service_.Clear(account, generation);
  }
  void Invalidate(std::uint64_t generation) override { service_.Invalidate(generation); }
  void Shutdown() override { service_.Shutdown(); }
  std::vector<LibraryEvent> Pump(std::size_t max_events) override { return service_.Pump(max_events); }

 private:
  LibraryService& service_;
};

// Steam's app details over the AppDetailsService. Only Steam games are asked for.
class SteamDetailsSource final : public DetailsSource {
 public:
  explicit SteamDetailsSource(steam::AppDetailsService& service) : service_(service) {}
  Store store() const override { return Store::kSteam; }
  void Request(const GameId& id, std::uint64_t generation) override {
    if (id.steam_app()) service_.Request(id, generation);
  }
  void Refresh(const GameId& id, std::uint64_t generation, bool first = false) override {
    if (id.steam_app()) service_.Refresh(id, generation, first);
  }
  void RequestFirst(const GameId& id, std::uint64_t generation) override {
    if (id.steam_app()) service_.Request(id, generation, true);
  }
  void Invalidate(std::uint64_t generation) override { service_.Invalidate(generation); }
  std::vector<GameDetails> Pump(std::size_t maximum) override { return service_.Pump(maximum); }
  void Measure(const steam::SizeRequest& request, std::uint64_t generation) override {
    if (request.id.steam_app()) service_.Measure(request, generation);
  }
  std::vector<steam::MeasuredSize> PumpMeasured() override { return service_.PumpMeasured(); }
  void Shutdown() override { service_.Shutdown(); }

 private:
  steam::AppDetailsService& service_;
};

}  // namespace playden::stores
