#pragma once
#include "library/store.h"
#include "stores/account.h"
#include "stores/details_source.h"
#include "stores/library_source.h"

#include <map>
#include <memory>
#include <vector>

namespace playden::stores {

class SteamAccount;

// Each store's account, library and details, at most one of each per store. Built where services are
// wired (host.cpp, ps5.cpp); the services an adapter wraps outlive the registry.
class Registry {
 public:
  Registry() = default;
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // A second one for the same store replaces the first.
  void Add(std::unique_ptr<StoreAccount> account);
  void Add(std::unique_ptr<SteamAccount> account);
  void Add(std::unique_ptr<LibrarySource> source);
  void Add(std::unique_ptr<DetailsSource> source);

  StoreAccount* account(Store store) const;
  // Every store's account, Steam's too, in store order.
  std::vector<StoreAccount*> accounts() const;
  // Steam's account with its own sign-in (QR code, password, Steam Guard).
  SteamAccount* steam_account() const { return steam_; }
  LibrarySource* library(Store store) const;
  DetailsSource* details(Store store) const;
  // Every store's library, in store order.
  std::vector<LibrarySource*> libraries() const;
  // Every store's details, in store order.
  std::vector<DetailsSource*> details_sources() const;

 private:
  std::map<Store, std::unique_ptr<StoreAccount>> accounts_;
  std::map<Store, std::unique_ptr<LibrarySource>> libraries_;
  std::map<Store, std::unique_ptr<DetailsSource>> details_;
  SteamAccount* steam_ = nullptr;
};

}  // namespace playden::stores
