#include "app/persistence_service.h"

#include <algorithm>

namespace playden::app {
namespace {

constexpr std::size_t kMaximumPending = 32;
constexpr std::size_t kMaximumEvents = 64;

StorageError PreferenceLoadError(PreferencesLoadStatus status) {
  if (status == PreferencesLoadStatus::kDefaultCorrupt)
    return StorageError::kInvalidData;
  if (status == PreferencesLoadStatus::kDefaultReadError)
    return StorageError::kReadFailed;
  return StorageError::kNone;
}

}  // namespace

PersistenceService::PersistenceService(PreferencesStore& preferences,
                                       OrganizationRepository& organizations,
                                       UpdateCheckRepository* updates)
    : preferences_(preferences), organizations_(organizations), worker_([this] { Run(); }) {
  if (updates) updates_[Store::kSteam] = updates;
}

void PersistenceService::SetUpdateChecks(Store store, UpdateCheckRepository* updates) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (updates) updates_[store] = updates;
  else updates_.erase(store);
}

PersistenceService::~PersistenceService() { Shutdown(); }

bool PersistenceService::IsOrganization(Command command) {
  return command == Command::kLoadOrganization ||
         command == Command::kSaveOrganization ||
         command == Command::kClearOrganization;
}

bool PersistenceService::IsLoad(Command command) {
  return command == Command::kLoadPreferences ||
         command == Command::kLoadOrganization ||
         command == Command::kLoadUpdateCheck;
}

PersistenceRequestId PersistenceService::LoadPreferences() {
  Job job;
  job.command = Command::kLoadPreferences;
  return Submit(std::move(job));
}

PersistenceRequestId PersistenceService::SavePreferences(Preferences preferences,
                                                         std::uint64_t revision) {
  Job job;
  job.command = Command::kSavePreferences;
  job.revision = revision;
  job.preferences = preferences;
  return Submit(std::move(job));
}

PersistenceRequestId PersistenceService::LoadOrganization(
    std::uint64_t account, std::uint64_t generation) {
  Job job;
  job.command = Command::kLoadOrganization;
  job.account = account;
  job.generation = generation;
  return Submit(std::move(job));
}

PersistenceRequestId PersistenceService::SaveOrganization(
    std::uint64_t account, std::uint64_t generation, std::uint64_t revision,
    OrganizationSnapshot snapshot) {
  Job job;
  job.command = Command::kSaveOrganization;
  job.account = account;
  job.generation = generation;
  job.revision = revision;
  job.organization = std::move(snapshot);
  return Submit(std::move(job));
}

PersistenceRequestId PersistenceService::ClearOrganization(
    std::uint64_t account, std::uint64_t generation) {
  Job job;
  job.command = Command::kClearOrganization;
  job.account = account;
  job.generation = generation;
  return Submit(std::move(job));
}

PersistenceRequestId PersistenceService::LoadUpdateCheck(Store store, std::uint64_t account) {
  if (!account) return 0;
  Job job;
  job.command = Command::kLoadUpdateCheck;
  job.store = store;
  job.account = account;
  return Submit(std::move(job));
}

PersistenceRequestId PersistenceService::SaveUpdateCheck(Store store, std::uint64_t account, std::uint64_t revision,
                                                         SavedUpdateCheck check) {
  if (!account) return 0;
  Job job;
  job.command = Command::kSaveUpdateCheck;
  job.store = store;
  job.account = account;
  job.revision = revision;
  job.update_check = std::move(check);
  return Submit(std::move(job));
}

PersistenceRequestId PersistenceService::Submit(Job job) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return 0;
  if (job.command == Command::kLoadUpdateCheck || job.command == Command::kSaveUpdateCheck) {
    // The worker uses the repository the job names, never the map.
    const auto found = updates_.find(job.store);
    if (found == updates_.end()) return 0;
    job.updates = found->second;
  }

  const bool organization = IsOrganization(job.command);
  bool scope_change = false;
  if (organization) {
    if (!job.account || job.generation < organization_generation_ ||
        clear_barrier_) return 0;
    const bool account_change = organization_account_ && organization_account_ != job.account;
    if (job.command == Command::kClearOrganization && account_change) return 0;
    if (job.generation == organization_generation_ && account_change) return 0;
    scope_change = job.command != Command::kClearOrganization &&
                   (job.generation > organization_generation_ || account_change);
  }

  if (job.command == Command::kSavePreferences) {
    if (job.revision < preferences_revision_) return 0;
    const auto found = std::find_if(pending_.rbegin(), pending_.rend(),
                                    [](const Job& pending) {
                                      return pending.command == Command::kSavePreferences;
                                    });
    if (found != pending_.rend()) {
      job.request = ++next_request_;
      *found = std::move(job);
      preferences_revision_ = found->revision;
      wake_.notify_one();
      return next_request_;
    }
  }
  if (job.command == Command::kSaveOrganization) {
    if (!scope_change && job.revision < organization_revision_) return 0;
    const auto found = std::find_if(pending_.rbegin(), pending_.rend(),
                                    [&job](const Job& pending) {
                                      return pending.command == Command::kSaveOrganization &&
                                             pending.account == job.account &&
                                             pending.generation == job.generation;
                                    });
    if (found != pending_.rend()) {
      job.request = ++next_request_;
      *found = std::move(job);
      organization_revision_ = found->revision;
      wake_.notify_one();
      return next_request_;
    }
  }

  if (job.command == Command::kSaveUpdateCheck) {
    const auto found = std::find_if(pending_.rbegin(), pending_.rend(), [&job](const Job& pending) {
      return pending.command == Command::kSaveUpdateCheck && pending.store == job.store && pending.account == job.account;
    });
    if (found != pending_.rend()) {
      job.request = ++next_request_;
      *found = std::move(job);
      wake_.notify_one();
      return next_request_;
    }
  }

  const auto remove_for_submission = [&job, scope_change](const Job& pending) {
    if (job.command == Command::kClearOrganization)
      return pending.command == Command::kLoadOrganization;
    return scope_change && pending.command == Command::kLoadOrganization;
  };
  const auto removed = static_cast<std::size_t>(std::count_if(
      pending_.begin(), pending_.end(), remove_for_submission));
  const auto prospective_pending = pending_.size() - removed + 1;
  const auto outstanding = prospective_pending + events_.size() +
                           (active_.has_value() ? 1u : 0u);
  if (prospective_pending > kMaximumPending || outstanding > kMaximumEvents)
    return 0;

  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                remove_for_submission), pending_.end());
  job.request = ++next_request_;
  if (organization) {
    if (scope_change) organization_revision_ = 0;
    organization_generation_ = job.generation;
    organization_account_ = job.account;
    if (job.command == Command::kSaveOrganization)
      organization_revision_ = job.revision;
  } else if (job.command == Command::kSavePreferences) {
    preferences_revision_ = job.revision;
  }
  if (job.command == Command::kClearOrganization) {
    clear_barrier_ = true;
    clear_requests_.push_back(job.request);
  }
  pending_.push_back(std::move(job));
  wake_.notify_one();
  return next_request_;
}

void PersistenceService::InvalidateOrganization(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation < organization_generation_) return;
  organization_generation_ = generation;
  organization_account_ = 0;
  organization_revision_ = 0;
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [](const Job& job) {
                                  return job.command == Command::kLoadOrganization;
                                }), pending_.end());
  wake_.notify_one();
}

bool PersistenceService::ClearRequest(PersistenceRequestId request) const {
  return std::find(clear_requests_.begin(), clear_requests_.end(), request) !=
         clear_requests_.end();
}

void PersistenceService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (joined_) return;
    stopping_ = true;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [](const Job& job) { return IsLoad(job.command); }),
                   pending_.end());
  }
  wake_.notify_one();
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  joined_ = true;
}

void PersistenceService::WaitIdle() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return pending_.empty() && !active_.has_value(); });
}

std::vector<PersistenceEvent> PersistenceService::Pump(std::size_t maximum) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PersistenceEvent> result;
  const auto count = std::min(maximum, events_.size());
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto request = events_.front().request_id;
    const auto kind = events_.front().kind;
    result.push_back(std::move(events_.front()));
    events_.pop_front();
    if (kind == PersistenceEventKind::kOrganizationCleared) {
      const auto found = std::find(clear_requests_.begin(), clear_requests_.end(), request);
      if (found != clear_requests_.end()) clear_requests_.erase(found);
    }
  }
  return result;
}

void PersistenceService::PushEvent(PersistenceEvent event, bool clear_terminal) {
  if (events_.size() >= kMaximumEvents) {
    const auto disposable = std::find_if(
        events_.begin(), events_.end(), [this](const PersistenceEvent& existing) {
          return !ClearRequest(existing.request_id);
        });
    if (disposable == events_.end()) return;
    events_.erase(disposable);
  }
  if (clear_terminal || events_.size() < kMaximumEvents)
    events_.push_back(std::move(event));
}

void PersistenceService::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (pending_.empty()) {
        if (stopping_) return;
        continue;
      }
      job = std::move(pending_.front());
      pending_.pop_front();
      active_ = job;
    }
    try {
      Execute(std::move(job));
    } catch (...) {
      const auto failed = *active_;
      PersistenceEvent event;
      event.request_id = failed.request;
      event.account_id = failed.account;
      event.store = failed.store;
      event.account_generation = failed.generation;
      event.revision = failed.revision;
      event.error = StorageError::kInvalidData;
      event.message = "unexpected exception";
      if (failed.command == Command::kLoadPreferences)
        event.kind = PersistenceEventKind::kPreferencesLoaded;
      else if (failed.command == Command::kSavePreferences)
        event.kind = PersistenceEventKind::kPreferencesSaved;
      else if (failed.command == Command::kLoadOrganization)
        event.kind = PersistenceEventKind::kOrganizationLoaded;
      else if (failed.command == Command::kSaveOrganization)
        event.kind = PersistenceEventKind::kOrganizationSaved;
      else if (failed.command == Command::kLoadUpdateCheck)
        event.kind = PersistenceEventKind::kUpdateCheckLoaded;
      else if (failed.command == Command::kSaveUpdateCheck)
        event.kind = PersistenceEventKind::kUpdateCheckSaved;
      else
        event.kind = PersistenceEventKind::kOrganizationCleared;
      std::lock_guard<std::mutex> lock(mutex_);
      bool current = true;
      if (failed.command == Command::kLoadOrganization ||
          failed.command == Command::kSaveOrganization)
        current = failed.account == organization_account_ &&
                  failed.generation == organization_generation_;
      if ((failed.command == Command::kLoadPreferences || failed.command == Command::kLoadUpdateCheck) && stopping_)
        current = false;
      if (failed.command == Command::kClearOrganization) clear_barrier_ = false;
      if (current)
        PushEvent(std::move(event), failed.command == Command::kClearOrganization);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_.reset();
    }
    idle_.notify_all();
  }
}

void PersistenceService::Execute(Job job) {
  PersistenceEvent event;
  event.request_id = job.request;
  event.account_id = job.account;
  event.store = job.store;
  event.account_generation = job.generation;
  event.revision = job.revision;

  if (job.command == Command::kLoadPreferences) {
    const auto loaded = preferences_.Load();
    event.kind = PersistenceEventKind::kPreferencesLoaded;
    event.preferences_status = loaded.status;
    event.error = PreferenceLoadError(loaded.status);
    event.preferences = loaded.preferences;
    event.message = loaded.diagnostic;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_) PushEvent(std::move(event));
    return;
  }
  if (job.command == Command::kSavePreferences) {
    const auto saved = preferences_.Save(job.preferences);
    event.kind = PersistenceEventKind::kPreferencesSaved;
    event.error = saved.ok() ? StorageError::kNone : saved.error();
    if (!saved.ok()) event.message = saved.message();
    std::lock_guard<std::mutex> lock(mutex_);
    PushEvent(std::move(event));
    return;
  }
  if (job.command == Command::kLoadOrganization) {
    const auto loaded = organizations_.Load(job.account);
    event.kind = PersistenceEventKind::kOrganizationLoaded;
    event.error = loaded.ok() ? StorageError::kNone : loaded.error();
    if (loaded.ok()) {
      event.organization = loaded.value();
      event.message = organizations_.TakeLoadNote();
    } else {
      event.message = loaded.message();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_ && job.account == organization_account_ &&
        job.generation == organization_generation_) PushEvent(std::move(event));
    return;
  }
  if (job.command == Command::kSaveOrganization) {
    const auto saved = organizations_.Save(job.account, job.organization);
    event.kind = PersistenceEventKind::kOrganizationSaved;
    event.error = saved.ok() ? StorageError::kNone : saved.error();
    if (!saved.ok()) event.message = saved.message();
    std::lock_guard<std::mutex> lock(mutex_);
    if (job.account == organization_account_ &&
        job.generation == organization_generation_) PushEvent(std::move(event));
    return;
  }

  if (job.command == Command::kLoadUpdateCheck) {
    const auto loaded = job.updates->Load(job.account);
    event.kind = PersistenceEventKind::kUpdateCheckLoaded;
    event.error = loaded.ok() ? StorageError::kNone : loaded.error();
    if (loaded.ok()) event.update_check = loaded.value();
    else event.message = loaded.message();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_) PushEvent(std::move(event));
    return;
  }
  if (job.command == Command::kSaveUpdateCheck) {
    const auto saved = job.updates->Save(job.account, job.update_check);
    event.kind = PersistenceEventKind::kUpdateCheckSaved;
    event.error = saved.ok() ? StorageError::kNone : saved.error();
    if (!saved.ok()) event.message = saved.message();
    std::lock_guard<std::mutex> lock(mutex_);
    PushEvent(std::move(event));
    return;
  }

  const auto cleared = organizations_.Clear(job.account);
  event.kind = PersistenceEventKind::kOrganizationCleared;
  event.error = cleared.ok() ? StorageError::kNone : cleared.error();
  std::lock_guard<std::mutex> lock(mutex_);
  clear_barrier_ = false;
  PushEvent(std::move(event), true);
}

}  // namespace playden::app
