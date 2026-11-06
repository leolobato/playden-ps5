#pragma once
// sharing_service.hpp - Shares recipes and sends votes to the Playden API on its own worker (plan
// Phases 5 and 6). It alone reads the console's sharing ID (community/install_secret.h) and keeps
// the vote memory (community/vote_state.h). The model hands it SharingRequests and gets
// SharingResults and the vote memory back; neither ever holds the sharing ID, and nothing it logs
// does either (lines also go through diagnostics::RedactSecrets in the host's logger).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "community/api_client.h"
#include "community/endpoints.h"
#include "community/vote_state.h"
#include "homebrew/simulation.hpp"
#include "update/asset_download.h"
#include "web/logs_source.h"

namespace playden::homebrew {

struct SharingServiceConfig {
  // <user data>/community-recipes, the recipe store's root; its parent must exist. The sharing ID
  // (sharing-id) and the vote memory (votes-cast.json) live there.
  std::string store_root;
  community::Endpoints endpoints = community::ReleaseEndpoints();
  update::Fetch fetch;  // one HTTP exchange (HttpClient::Perform); only ever called on the worker
  std::function<void(const std::string& line)> log;
  std::string user_agent = "Playden";
  std::size_t worker_stack_bytes = 1 << 20;  // as the recipe service: JSON parsing of 16 KB recipes
  // A share or vote whose answer was lost (network, 503 try_again) is sent this many more times,
  // with the same Idempotency-Key, waiting retry_delay_ms × the attempt number between them.
  int retries = 2;
  std::uint32_t retry_delay_ms = 2000;
  // Waits between retries; tests replace it. Returns early when cancel is set.
  std::function<void(std::uint32_t milliseconds, const std::atomic<bool>& cancel)> sleep;
  // Where a test report's logs come from: the same files web access serves, redacted the same way.
  // Unset: reports go without logs or versions.
  std::optional<web::LogsPaths> logs;
  std::function<std::int64_t()> now;  // Unix seconds for the report's summary; default: the clock
};

class SharingService {
 public:
  explicit SharingService(SharingServiceConfig config);
  ~SharingService();
  SharingService(const SharingService&) = delete;
  SharingService& operator=(const SharingService&) = delete;

  // Starts the worker, which loads the vote memory first. Call once.
  void Start();
  // Queues a request; the worker handles them in order. Dropped after Shutdown.
  void Submit(SharingRequest request);
  // The signed-in Steam account, removed from a report's logs (web::Redact). Not a credential.
  void SetAccount(web::Redaction account);
  // The answers since the last call, in order.
  std::vector<SharingResult> TakeResults();
  // Changes whenever the vote memory does (loaded, a vote recorded, a revision asked about).
  std::uint64_t MemoryRevision() const;
  // The vote memory; nothing until it was loaded (or found unusable: then an empty one).
  std::optional<community::VoteState> Memory() const;
  // Whether requests wait or one runs.
  bool Busy() const;
  // Waits until every queued request is handled (tests).
  void Wait();
  // Cancels what runs, drops what waits and joins the worker.
  void Shutdown();

 private:
  struct Shared;
  void Run();
  void Handle(SharingRequest request);
  // The sharing ID, read (or made) on first use; nothing when it can't be.
  std::optional<std::string> Secret(std::string* error);
  void SaveMemory();
  void Log(const std::string& line) const;
  community::ApiContext Context() const;
  bool Retryable(const community::ApiError& error) const;
  void Sleep(std::uint32_t milliseconds);
  // The redacted report zip for the game (its log found by app ID, else by name; Playden's logs
  // only when it has none), and the summary's facts. False when it can't be made or is too large.
  bool ReportLogs(std::uint32_t app, const std::string& name, std::string* zip,
                  std::vector<std::pair<std::string, std::string>>* facts);

  SharingServiceConfig config_;
  std::shared_ptr<Shared> shared_;
  // Worker-only state.
  std::optional<std::string> secret_;
  community::VoteState memory_;
};

}  // namespace playden::homebrew
