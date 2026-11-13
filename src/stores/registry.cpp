#include "stores/registry.h"

#include "stores/steam.h"

namespace playden::stores {

void Registry::Add(std::unique_ptr<StoreAccount> account) {
  if (!account) return;
  const auto store = account->store();
  if (store == Store::kSteam) steam_ = nullptr;
  accounts_[store] = std::move(account);
}

void Registry::Add(std::unique_ptr<SteamAccount> account) {
  if (!account) return;
  auto* steam = account.get();
  accounts_[Store::kSteam] = std::move(account);
  steam_ = steam;
}

void Registry::Add(std::unique_ptr<LibrarySource> source) {
  if (!source) return;
  const auto store = source->store();
  libraries_[store] = std::move(source);
}

void Registry::Add(std::unique_ptr<DetailsSource> source) {
  if (!source) return;
  const auto store = source->store();
  details_[store] = std::move(source);
}

StoreAccount* Registry::account(Store store) const {
  const auto found = accounts_.find(store);
  return found == accounts_.end() ? nullptr : found->second.get();
}

std::vector<StoreAccount*> Registry::accounts() const {
  std::vector<StoreAccount*> result;
  result.reserve(accounts_.size());
  for (const auto& [store, account] : accounts_) result.push_back(account.get());
  return result;
}

LibrarySource* Registry::library(Store store) const {
  const auto found = libraries_.find(store);
  return found == libraries_.end() ? nullptr : found->second.get();
}

DetailsSource* Registry::details(Store store) const {
  const auto found = details_.find(store);
  return found == details_.end() ? nullptr : found->second.get();
}

std::vector<LibrarySource*> Registry::libraries() const {
  std::vector<LibrarySource*> result;
  result.reserve(libraries_.size());
  for (const auto& [store, source] : libraries_) result.push_back(source.get());
  return result;
}

std::vector<DetailsSource*> Registry::details_sources() const {
  std::vector<DetailsSource*> result;
  result.reserve(details_.size());
  for (const auto& [store, source] : details_) result.push_back(source.get());
  return result;
}

}  // namespace playden::stores
