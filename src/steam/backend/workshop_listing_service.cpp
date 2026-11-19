// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/workshop_listing_service.h"

#include <algorithm>
#include <cerrno>
#include <set>
#include <sys/stat.h>

namespace playden::steam {
namespace {

// The bottle exists once the game was prepared (RunWorkshopJob asks the same).
bool HasBottle(const std::string& prefix) {
  struct stat info{};
  return !prefix.empty() && ::lstat((prefix + "/drive_c").c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Anything at all under the name, as the sync counts it.
bool Present(const std::string& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0 || errno != ENOENT;
}

const char* ErrorName(WorkshopListingError error) {
  switch (error) {
    case WorkshopListingError::kNone: return "none";
    case WorkshopListingError::kNoSession: return "no-session";
    case WorkshopListingError::kNetwork: return "network";
    case WorkshopListingError::kSteam: return "steam";
    case WorkshopListingError::kNoBottle: return "no-bottle";
    case WorkshopListingError::kStorage: return "storage";
  }
  return "?";
}

}  // namespace

WorkshopListing ListWorkshopItems(std::uint32_t app_id, const std::string& prefix,
                                  const WorkshopSubscriptions& subscriptions) {
  WorkshopListing listing;
  listing.app_id = app_id;
  if (!HasBottle(prefix)) {
    listing.error = WorkshopListingError::kNoBottle;
    return listing;
  }
  listing.truncated = subscriptions.truncated;
  const auto folder = WorkshopFolder(prefix);
  WorkshopState state;
  std::set<std::uint64_t> off;
  struct stat info{};
  if (::lstat(folder.c_str(), &info) == 0) {
    if (!S_ISDIR(info.st_mode)) {
      listing.error = WorkshopListingError::kStorage;
      return listing;
    }
    if (auto read = ReadWorkshopState(folder, app_id)) state = std::move(*read);
    if (auto read = ReadWorkshopOff(folder, app_id)) off = std::move(*read);
  } else if (errno != ENOENT) {
    listing.error = WorkshopListingError::kStorage;
    return listing;
  }
  listing.saved_off.assign(off.begin(), off.end());
  // Items Valve's client downloaded in Online show as Playden's, as the next sync adopts them.
  if (::lstat(folder.c_str(), &info) == 0) AdoptSteamClientItems(folder, app_id, subscriptions.items, &state);
  const auto content = folder + "/content/" + std::to_string(app_id) + "/";
  std::set<std::uint64_t> present, everything;
  for (const auto& item : subscriptions.items) {
    if (item.id == 0) continue;
    everything.insert(item.id);
    if (Present(content + std::to_string(item.id))) present.insert(item.id);
  }
  for (const auto& local : state.items) {
    everything.insert(local.id);
    if (Present(content + std::to_string(local.id))) present.insert(local.id);
  }
  // The same plan the job makes, once with every item on and once with every item off: the sheet's toggles pick
  // between the two for each item. Both list the same items in the same order.
  const bool complete = !subscriptions.truncated;
  const auto all_on = PlanWorkshopSync(subscriptions.items, state, {}, present, complete);
  const auto all_off = PlanWorkshopSync(subscriptions.items, state, everything, present, complete);
  if (all_on.entries.size() != all_off.entries.size()) {
    listing.error = WorkshopListingError::kStorage;
    return listing;
  }
  std::set<std::uint64_t> subscribed;
  for (const auto& item : subscriptions.items) subscribed.insert(item.id);
  for (std::size_t index = 0; index < all_on.entries.size(); ++index) {
    const auto& on = all_on.entries[index];
    const auto& off_entry = all_off.entries[index];
    if (on.item.id != off_entry.item.id) {
      listing.error = WorkshopListingError::kStorage;
      listing.items.clear();
      return listing;
    }
    WorkshopListingItem item;
    item.id = on.item.id;
    item.title = on.item.title;
    item.file_size = on.item.file_size;
    item.subscribed = subscribed.count(item.id) > 0;
    item.on = off.count(item.id) == 0;
    item.on_action = on.action;
    item.off_action = off_entry.action;
    item.on_bytes = on.bytes;
    item.off_bytes = off_entry.bytes;
    listing.items.push_back(std::move(item));
  }
  return listing;
}

WorkshopListingService::WorkshopListingService(SessionRepository& sessions, WorkshopListingConfig config)
    : sessions_(sessions), config_(std::move(config)), worker_([this] { Run(); }) {}

WorkshopListingService::~WorkshopListingService() { Shutdown(); }

void WorkshopListingService::Request(std::uint32_t app_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || app_id == 0) return;
  // A game the job asked for in the background doesn't push out the one the open sheet waits for.
  if (std::find(pending_.begin(), pending_.end(), app_id) == pending_.end()) {
    if (pending_.size() >= kWorkshopListingMaxPending) pending_.pop_front();
    pending_.push_back(app_id);
  }
  wake_.notify_all();
}

void WorkshopListingService::Invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++generation_;
  pending_.clear();
  done_.clear();
  cancel_.store(true);
}

std::vector<WorkshopListing> WorkshopListingService::Pump() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WorkshopListing> done;
  done.swap(done_);
  return done;
}

void WorkshopListingService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    cancel_.store(true);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void WorkshopListingService::Run() {
  for (;;) {
    std::uint32_t app = 0;
    std::uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      app = pending_.front();
      pending_.pop_front();
      generation = generation_;
      cancel_.store(false);
    }
    WorkshopListing listing;
    listing.app_id = app;
    bool canceled = false;
    // Before Steam is asked anything: without a bottle there is nothing to show beside the subscriptions.
    const auto prefix = config_.bottle ? config_.bottle(app) : std::nullopt;
    if (!prefix || !HasBottle(*prefix)) {
      listing.error = WorkshopListingError::kNoBottle;
    } else {
      const auto session = sessions_.Load();
      if (!session.ok() || !session.value()) {
        listing.error = WorkshopListingError::kNoSession;
      } else if (!config_.subscriptions) {
        listing.error = WorkshopListingError::kSteam;
      } else {
        const auto subscriptions = config_.subscriptions(*session.value(), app, cancel_);
        if (cancel_.load() || subscriptions.error == CmError::kCanceled) {
          canceled = true;
        } else if (!subscriptions.ok()) {
          switch (subscriptions.error) {
            case CmError::kNoSession: listing.error = WorkshopListingError::kNoSession; break;
            case CmError::kNetwork:
            case CmError::kTimeout: listing.error = WorkshopListingError::kNetwork; break;
            default: listing.error = WorkshopListingError::kSteam; break;
          }
        } else {
          listing = ListWorkshopItems(app, *prefix, subscriptions);
        }
      }
    }
    if (config_.log && !canceled) {
      std::size_t held = 0;
      for (const auto& item : listing.items)
        if (item.on_action == WorkshopAction::kKeep || item.on_action == WorkshopAction::kUpdate ||
            item.on_action == WorkshopAction::kRemove)
          ++held;
      config_.log(std::string("workshop listing app=") + std::to_string(app) + " result=" + ErrorName(listing.error) +
                  " items=" + std::to_string(listing.items.size()) + " held=" + std::to_string(held) +
                  " off=" + std::to_string(listing.saved_off.size()) + (listing.truncated ? " truncated=1" : ""));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    if (!canceled && generation == generation_) done_.push_back(std::move(listing));
  }
}

}  // namespace playden::steam
