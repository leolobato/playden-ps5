// recipe_service.cpp - Fetches, checks and keeps community recipes; offers a snapshot of them.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/recipe_service.h"

#include <pthread.h>
#include <strings.h>

#include <ctime>
#include <utility>

#include "runtime/community_rules.h"
#include "update/release.h"

namespace playden::community {

struct CommunityRecipeService::Shared {
  mutable std::mutex snapshot_mutex;
  std::shared_ptr<const RecipeSnapshot> snapshot = std::make_shared<RecipeSnapshot>();
  std::mutex launch_mutex;     // busy, thread, thread_started
  std::mutex write_mutex;      // a refresh's writes and garbage collection, one at a time
  std::atomic<bool> cancel{false};
  std::atomic<bool> busy{false};
  std::atomic<std::int64_t> last_failure_at{0};
  bool thread_started = false;
  pthread_t thread{};
};

namespace {

constexpr int kMaxRedirects = 5;

void* RunJob(void* opaque) {
  std::unique_ptr<std::function<void()>> work(static_cast<std::function<void()>*>(opaque));
  (*work)();
  return nullptr;
}

bool Redirect(long status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

std::string Header(const steam::HttpResponse& response, const char* name) {
  for (const auto& [key, value] : response.headers)
    if (strcasecmp(key.c_str(), name) == 0) return value;
  return {};
}

}  // namespace

const std::vector<RecipeSummary>* RecipeSnapshot::For(std::uint32_t app_id) const {
  const auto found = apps.find(app_id);
  return found == apps.end() ? nullptr : &found->second;
}

std::optional<runtime::CommunityRecipe> CheckRecipeFile(std::string_view bytes, std::string* error) {
  runtime::CommunityRecipe community;
  std::string reason;
  if (bytes.size() > kMaxRecipeBytes) {
    if (error) *error = "larger than 16 KB";
    return std::nullopt;
  }
  if (!runtime::ParseCommunityRecipe(bytes, &community, &reason)) {
    if (error) *error = "not a recipe: " + reason;
    return std::nullopt;
  }
  const auto refusals = runtime::CommunityRecipeRefusals(community, runtime::CommunityCheckOptions{});
  if (!refusals.empty()) {
    if (error) *error = "refused: " + refusals.front();
    return std::nullopt;
  }
  std::uint32_t app_id = 0;
  if (!runtime::SplitCommunityRecipeId(community.recipe.id, &app_id, nullptr) || app_id != community.recipe.app_id) {
    if (error) *error = "its id and app ID differ";
    return std::nullopt;
  }
  return community;
}

std::map<std::uint32_t, std::vector<RecipeSummary>> BuildSummaries(
    const RecipeIndex& index, const RecipeStore& store, const VoteTotals* totals,
    const std::function<void(const std::string&)>& log) {
  std::map<std::uint32_t, std::vector<RecipeSummary>> apps;
  for (const auto& [app_id, entries] : index.apps) {
    for (const auto& entry : entries) {
      const auto bytes = store.ReadObject(entry.revision);
      if (!bytes) continue;  // dropped when it was fetched (logged then)
      std::string error;
      const auto community = CheckRecipeFile(*bytes, &error);
      if (!community || community->recipe.id != entry.id || community->recipe.app_id != app_id) {
        if (log) log("community recipes: " + entry.id + " skipped: " + (community ? "not the recipe the index lists" : error));
        continue;
      }
      RecipeSummary summary;
      summary.id = entry.id;
      summary.app_id = app_id;
      summary.title = community->title;
      summary.status = community->status;
      summary.author = community->author;
      summary.contributors = community->contributors;
      summary.tested_kit = community->tested_kit;
      summary.min_kit = community->min_kit;
      summary.tested_firmware = community->tested_firmware;
      summary.tested_date = community->tested_date;
      summary.notes = community->notes;
      summary.revision = entry.revision;
      summary.flags = runtime::CommunityRecipeFlags(*community);
      if (totals) {
        if (const auto recipe = totals->find(entry.id); recipe != totals->end())
          if (const auto votes = recipe->second.find(entry.revision); votes != recipe->second.end())
            summary.votes = votes->second;
      }
      apps[app_id].push_back(std::move(summary));
    }
  }
  return apps;
}

CommunityRecipeService::CommunityRecipeService(RecipeServiceConfig config)
    : config_(std::move(config)), shared_(std::make_shared<Shared>()) {
#if !PLAYDEN_DEV_ENDPOINTS
  // Release builds can't be steered: the repository and the API are fixed here.
  config_.endpoints = ReleaseEndpoints();
#endif
  store_ = std::make_unique<RecipeStore>(std::filesystem::path(config_.store_root));
  store_->before_write = config_.before_write;
}

CommunityRecipeService::~CommunityRecipeService() { Shutdown(); }

void CommunityRecipeService::Log(const std::string& line) const {
  if (config_.log) config_.log(line);
}

std::int64_t CommunityRecipeService::Now() const {
  return config_.now ? config_.now() : static_cast<std::int64_t>(std::time(nullptr));
}

std::shared_ptr<const RecipeSnapshot> CommunityRecipeService::Snapshot() const {
  std::lock_guard<std::mutex> lock(shared_->snapshot_mutex);
  return shared_->snapshot;
}

bool CommunityRecipeService::Busy() const { return shared_->busy; }

void CommunityRecipeService::Publish(const std::function<void(RecipeSnapshot&)>& change) {
  std::lock_guard<std::mutex> lock(shared_->snapshot_mutex);
  auto next = std::make_shared<RecipeSnapshot>(*shared_->snapshot);
  change(*next);
  ++next->revision;
  shared_->snapshot = std::move(next);
}

bool CommunityRecipeService::Launch(std::function<void()> work) {
  std::lock_guard<std::mutex> lock(shared_->launch_mutex);
  if (shared_->busy) return false;
  if (shared_->thread_started) {
    pthread_join(shared_->thread, nullptr);
    shared_->thread_started = false;
  }
  shared_->busy = true;
  shared_->cancel = false;
  auto shared = shared_;
  auto* job = new std::function<void()>([shared, work = std::move(work)] {
    work();
    shared->busy = false;
  });
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, config_.worker_stack_bytes);
  const int rc = pthread_create(&shared_->thread, &attributes, RunJob, job);
  pthread_attr_destroy(&attributes);
  if (rc != 0) {
    delete job;
    shared_->busy = false;
    Log("community recipes: cannot start the worker (pthread_create " + std::to_string(rc) + ")");
    return false;
  }
  shared_->thread_started = true;
  return true;
}

void CommunityRecipeService::Wait() {
  std::lock_guard<std::mutex> lock(shared_->launch_mutex);
  if (shared_->thread_started) {
    pthread_join(shared_->thread, nullptr);
    shared_->thread_started = false;
  }
}

void CommunityRecipeService::Shutdown() {
  shared_->cancel = true;
  Wait();
}

void CommunityRecipeService::Start() {
  Launch([this] { DoStart(); });
}

void CommunityRecipeService::RequestRefresh(bool force) {
  if (!force) {
    const std::int64_t now = Now();
    const auto snapshot = Snapshot();
    if (snapshot->fetched_at && now - snapshot->fetched_at < kRefreshInterval) return;
    const std::int64_t failed = shared_->last_failure_at;
    if (failed && now - failed < kRetryInterval) return;
  }
  if (!Launch([this] { DoRefresh(); })) Log("community recipes: refresh already running");
}

void CommunityRecipeService::DoStart() {
  std::string error;
  if (!store_->Prepare(&error)) {
    Log("community recipes: " + error);
    Publish([&](RecipeSnapshot& s) { s.last_error = "Community recipes can't be stored."; });
    return;
  }
  LoadFromDisk();
  DoRefresh();
}

void CommunityRecipeService::LoadFromDisk() {
  const auto current = store_->ReadCurrent();
  if (!current) return;
  const auto index_json = store_->ReadRelease(current->tag);
  std::string error;
  const auto index = index_json ? ParseRecipeIndex(*index_json, &error) : std::nullopt;
  if (!index) {
    Log("community recipes: the stored release " + current->tag + " is unusable" + (error.empty() ? "" : ": " + error));
    return;
  }
  std::optional<VoteTotals> totals;
  if (const auto votes = store_->ReadVotes()) totals = ParseVoteSummary(*votes, nullptr);
  auto apps = BuildSummaries(*index, *store_, totals ? &*totals : nullptr, config_.log);
  std::size_t count = 0;
  for (const auto& [app, list] : apps) count += list.size();
  Log("community recipes: loaded " + current->tag + " from disk (" + std::to_string(count) + " recipes)");
  Publish([&](RecipeSnapshot& s) {
    s.apps = std::move(apps);
    s.release_tag = current->tag;
    s.fetched_at = current->fetched_at;
  });
}

void CommunityRecipeService::DoRefresh() {
  if (!store_->prepared()) {
    std::string error;
    if (!store_->Prepare(&error)) {
      Log("community recipes: " + error);
      shared_->last_failure_at = Now();
      Publish([&](RecipeSnapshot& s) { s.last_error = "Community recipes can't be stored."; });
      return;
    }
  }
  Publish([](RecipeSnapshot& s) { s.refreshing = true; });
  std::string error;
  const bool ok = Refresh(&error);
  if (!ok) {
    shared_->last_failure_at = Now();
    Log("community recipes: refresh failed: " + error);
  } else {
    shared_->last_failure_at = 0;
  }
  Publish([&](RecipeSnapshot& s) {
    s.refreshing = false;
    s.last_error = ok ? std::string() : error;
  });
}

std::optional<std::string> CommunityRecipeService::Get(const std::string& url, std::size_t limit,
                                                      const std::string& accept, std::string* error) {
  return GetFrom(url, limit, accept, true, error);
}

std::optional<std::string> CommunityRecipeService::GetFrom(const std::string& url, std::size_t limit,
                                                          const std::string& accept, bool recipes_request,
                                                          std::string* error) {
  if (!config_.fetch) {
    *error = "no network";
    return std::nullopt;
  }
  std::string target = url;
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    if (!EndpointUrlAllowed(config_.endpoints, target)) {
      *error = "host not allowed";
      return std::nullopt;
    }
    steam::HttpRequest request;
    request.url = target;
    request.headers = {{"User-Agent", config_.user_agent}, {"Accept", accept}};
    // The recipes repository's credential (dev endpoints only) goes to its own origin, never to
    // the API or a host a redirect names.
    if (recipes_request && EndpointSendsRecipesAuth(config_.endpoints, target))
      request.headers.emplace_back("Authorization", config_.endpoints.recipes_auth_header);
    request.allow_plain_http = EndpointPlainHttpAllowed(config_.endpoints, target);
    request.timeout_ms = 60000;
    request.max_response_bytes = limit;
    const auto response = config_.fetch(request, shared_->cancel);
    if (shared_->cancel) {
      *error = "cancelled";
      return std::nullopt;
    }
    if (Redirect(response.status_code)) {
      std::string location = Header(response, "location");
      if (location.size() > 1 && location[0] == '/' && location[1] != '/') {
        // A path on the same server.
        const std::string origin = UrlOrigin(target);
        const auto scheme_end = origin.find("://");
        location = origin.substr(0, scheme_end) + "://" + origin.substr(scheme_end + 3) + location;
      }
      target = location;
      continue;
    }
    if (!response.ok() || response.status_code != 200) {
      *error = response.error == steam::HttpError::kResponseTooLarge
                   ? std::string("download larger than allowed")
                   : "status " + std::to_string(response.status_code) + " (" + steam::HttpErrorName(response.error) + ")";
      return std::nullopt;
    }
    if (response.body.size() > limit) {
      *error = "download larger than allowed";
      return std::nullopt;
    }
    return std::string(response.body.begin(), response.body.end());
  }
  *error = "too many redirects";
  return std::nullopt;
}

bool CommunityRecipeService::Refresh(std::string* error) {
  const auto& endpoints = config_.endpoints;
  std::string reason;
  const auto release_json = Get(endpoints.releases_latest_url, kMaxReleaseJsonBytes, "application/vnd.github+json", &reason);
  if (!release_json) {
    *error = "Can't reach the community recipes (" + reason + ").";
    return false;
  }
  const auto release = update::ParseReleaseAnyTag(*release_json, &reason);
  if (!release || release->draft || release->prerelease) {
    *error = "The community recipes release is unusable (" + (release ? std::string("draft") : reason) + ").";
    return false;
  }
  const auto* index_asset = release->Find(kIndexAsset);
  const auto* zip_asset = release->Find(kZipAsset);
  if (!index_asset || !zip_asset) {
    *error = "The community recipes release has no index.json or recipes.zip.";
    return false;
  }
  if (index_asset->size > kMaxIndexBytes || zip_asset->size > kMaxZipBytes) {
    *error = "The community recipes release is larger than allowed.";
    return false;
  }

  const auto current = store_->ReadCurrent();
  std::optional<std::string> stored_index;
  if (current && current->tag == release->tag) stored_index = store_->ReadRelease(release->tag);
  std::string index_json;
  std::optional<RecipeIndex> index;
  std::vector<std::string> good;
  if (stored_index && (index = ParseRecipeIndex(*stored_index, nullptr))) {
    index_json = *stored_index;  // the same release: nothing to download
  } else {
    auto downloaded = Get(index_asset->url, kMaxIndexBytes, "application/octet-stream", &reason);
    if (!downloaded || downloaded->size() != index_asset->size) {
      *error = "Can't download the community recipes index (" + (downloaded ? std::string("size differs") : reason) + ").";
      return false;
    }
    if (!index_asset->sha256.empty() && Sha256Hex(*downloaded) != index_asset->sha256) {
      *error = "The community recipes index does not match its release.";
      return false;
    }
    index_json = std::move(*downloaded);
    index = ParseRecipeIndex(index_json, &reason, config_.log);
    if (!index) {
      *error = "The community recipes index is unusable (" + reason + ").";
      return false;
    }
    const auto zip = Get(zip_asset->url, kMaxZipBytes, "application/octet-stream", &reason);
    if (!zip || zip->size() != zip_asset->size) {
      *error = "Can't download the community recipes (" + (zip ? std::string("size differs") : reason) + ").";
      return false;
    }
    const std::string zip_sha = Sha256Hex(*zip);
    if (zip_sha != index->zip_sha256 || (!zip_asset->sha256.empty() && zip_sha != zip_asset->sha256)) {
      *error = "The community recipes don't match their index.";
      return false;
    }
    const auto files = ReadRecipesZip(*zip, *index, &reason, config_.log);
    if (!files) {
      *error = "The community recipes are unusable (" + reason + ").";
      return false;
    }
    for (const auto& file : *files) {
      std::string why;
      const auto community = CheckRecipeFile(file.bytes, &why);
      if (!community || community->recipe.id != file.entry->id || community->recipe.app_id != file.entry->app_id) {
        Log("community recipes: " + file.entry->id + " dropped: " + (community ? "its id differs from its path" : why));
        continue;
      }
      good.push_back(file.bytes);
    }
    Log("community recipes: release " + release->tag + ": " + std::to_string(good.size()) + " of " +
        std::to_string(index->count) + " recipes usable");
  }

  // Vote totals are optional: without them the recipes still work, with the last ones known.
  std::optional<std::string> votes_json;
  std::optional<VoteTotals> totals;
  {
    std::string why;
    auto summary = GetFrom(endpoints.api_base_url + "/v1/summary.json", kMaxSummaryBytes, "application/json", false, &why);
    if (summary) totals = ParseVoteSummary(*summary, &why);
    if (totals) votes_json = std::move(summary);
    else Log("community recipes: vote totals unavailable (" + why + ")");
  }

  const std::int64_t fetched_at = Now();
  {
    std::lock_guard<std::mutex> lock(shared_->write_mutex);
    // Objects, then the release's index (and votes), and the pointer last: a refresh cut short
    // leaves the previous release in use.
    // Playden closing stops the writes between files; the pointer then stays on the previous release.
    if (!good.empty() && !store_->WriteObjects(good, &reason, &shared_->cancel)) {
      *error = "Can't store the community recipes (" + reason + ").";
      return false;
    }
    if (shared_->cancel) {
      *error = "cancelled";
      return false;
    }
    if (!stored_index && !store_->WriteRelease(release->tag, index_json, &reason)) {
      *error = "Can't store the community recipes (" + reason + ").";
      return false;
    }
    if (votes_json && !store_->WriteVotes(*votes_json, &reason)) Log("community recipes: " + reason);
    if (shared_->cancel) {
      *error = "cancelled";
      return false;
    }
    if (!store_->WriteCurrent(CurrentRelease{release->tag, fetched_at}, &reason)) {
      *error = "Can't store the community recipes (" + reason + ").";
      return false;
    }
    if (current && current->tag != release->tag) store_->RemoveRelease(current->tag);
  }

  if (!totals)
    if (const auto stored = store_->ReadVotes()) totals = ParseVoteSummary(*stored, nullptr);
  auto apps = BuildSummaries(*index, *store_, totals ? &*totals : nullptr, config_.log);
  Publish([&](RecipeSnapshot& s) {
    s.apps = std::move(apps);
    s.release_tag = release->tag;
    s.fetched_at = fetched_at;
  });

  if (config_.referenced) {
    const auto keep = config_.referenced();
    std::lock_guard<std::mutex> lock(shared_->write_mutex);
    const auto removed = keep && !shared_->cancel ? store_->CollectGarbage(*keep, &shared_->cancel) : 0;
    if (!keep) Log("community recipes: kept every recipe file (what is in use is not known yet)");
    if (removed) Log("community recipes: removed " + std::to_string(removed) + " unused recipe files");
  }
  return true;
}

std::optional<runtime::CommunityRecipe> CommunityRecipeService::LoadRecipe(std::string_view revision_or_prefix) const {
  if (!store_->prepared()) {
    std::string error;
    if (!store_->Prepare(&error)) return std::nullopt;
  }
  std::optional<std::string> revision;
  if (IsRevision(revision_or_prefix)) revision = std::string(revision_or_prefix);
  else if (IsRevisionPrefix(revision_or_prefix)) revision = store_->FindObjectByPrefix(revision_or_prefix);
  if (!revision) return std::nullopt;
  const auto bytes = store_->ReadObject(*revision);
  if (!bytes) return std::nullopt;
  std::string error;
  auto community = CheckRecipeFile(*bytes, &error);
  if (!community) Log("community recipes: stored " + revision->substr(0, 32) + " refused: " + error);
  return community;
}

std::size_t CommunityRecipeService::CollectGarbage(const std::set<std::string>& keep) {
  std::lock_guard<std::mutex> lock(shared_->write_mutex);
  if (!store_->prepared()) return 0;
  return store_->CollectGarbage(keep, &shared_->cancel);
}

}  // namespace playden::community
