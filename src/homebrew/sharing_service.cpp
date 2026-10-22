// sharing_service.cpp - Shares recipes and sends votes on a worker; keeps the sharing ID and the
// vote memory there.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/sharing_service.hpp"

#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <utility>

#include "community/install_secret.h"
#include "diagnostics/redaction.h"

namespace playden::homebrew {

struct SharingService::Shared {
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::deque<SharingRequest> queue;
  std::vector<SharingResult> results;
  bool handling = false;
  bool stop = false;
  std::atomic<bool> cancel{false};
  bool thread_started = false;
  pthread_t thread{};
  std::optional<community::VoteState> memory;
  std::uint64_t memory_revision = 0;
};

namespace {

void* RunWorker(void* opaque) {
  std::unique_ptr<std::function<void()>> work(static_cast<std::function<void()>*>(opaque));
  (*work)();
  return nullptr;
}

const char* OutcomeName(community::SubmissionOutcome outcome) {
  switch (outcome) {
    case community::SubmissionOutcome::kCreated: return "created";
    case community::SubmissionOutcome::kDuplicatePublished: return "duplicate-published";
    case community::SubmissionOutcome::kDuplicateInReview: return "duplicate-in-review";
    case community::SubmissionOutcome::kError: return "error";
  }
  return "?";
}

community::ApiError LocalError(std::string code, std::string message) {
  community::ApiError error;
  error.code = std::move(code);
  error.message = std::move(message);
  return error;
}

}  // namespace

SharingService::SharingService(SharingServiceConfig config)
    : config_(std::move(config)), shared_(std::make_shared<Shared>()) {}

SharingService::~SharingService() { Shutdown(); }

void SharingService::Start() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  if (shared_->thread_started || shared_->stop) return;
  auto* job = new std::function<void()>([this] { Run(); });
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, config_.worker_stack_bytes);
  const int rc = pthread_create(&shared_->thread, &attributes, RunWorker, job);
  pthread_attr_destroy(&attributes);
  if (rc != 0) {
    delete job;
    Log("sharing: cannot start the worker (pthread_create " + std::to_string(rc) + ")");
    return;
  }
  shared_->thread_started = true;
}

void SharingService::Submit(SharingRequest request) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->stop) return;
    shared_->queue.push_back(std::move(request));
  }
  shared_->changed.notify_all();
}

std::vector<SharingResult> SharingService::TakeResults() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return std::exchange(shared_->results, {});
}

std::uint64_t SharingService::MemoryRevision() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->memory_revision;
}

std::optional<community::VoteState> SharingService::Memory() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->memory;
}

bool SharingService::Busy() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->handling || !shared_->queue.empty();
}

void SharingService::Wait() {
  std::unique_lock<std::mutex> lock(shared_->mutex);
  shared_->changed.wait(lock, [this] {
    return shared_->stop || !shared_->thread_started ||
           (!shared_->handling && shared_->queue.empty() && shared_->memory.has_value());
  });
}

void SharingService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->stop = true;
    shared_->queue.clear();
  }
  shared_->cancel = true;
  shared_->changed.notify_all();
  bool join = false;
  pthread_t thread{};
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    join = shared_->thread_started;
    thread = shared_->thread;
    shared_->thread_started = false;
  }
  if (join) pthread_join(thread, nullptr);
}

void SharingService::Run() {
  // The vote memory first: the model asks for votes only once it knows what was asked before.
  std::string error, note;
  auto loaded = community::LoadVoteState(config_.store_root, &error, &note);
  if (!note.empty()) Log("sharing: " + note);
  if (!loaded) Log("sharing: vote memory unusable, starting empty: " + error);
  memory_ = loaded ? std::move(*loaded) : community::VoteState{};
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->memory = memory_;
    ++shared_->memory_revision;
  }
  shared_->changed.notify_all();
  for (;;) {
    SharingRequest request;
    {
      std::unique_lock<std::mutex> lock(shared_->mutex);
      shared_->changed.wait(lock, [this] { return shared_->stop || !shared_->queue.empty(); });
      if (shared_->stop) break;
      request = std::move(shared_->queue.front());
      shared_->queue.pop_front();
      shared_->handling = true;
    }
    Handle(std::move(request));
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->handling = false;
    }
    shared_->changed.notify_all();
  }
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->handling = false;
}

community::ApiContext SharingService::Context() const {
  community::ApiContext context;
  context.endpoints = config_.endpoints;
  context.fetch = config_.fetch;
  context.user_agent = config_.user_agent;
  return context;
}

bool SharingService::Retryable(const community::ApiError& error) const {
  // The answer was lost or the API asked to try again: the same request (and key) is safe to repeat.
  return error.code == "network" || error.code == "try_again" || error.http_status == 502 || error.http_status == 503 ||
         error.http_status == 504;
}

void SharingService::Sleep(std::uint32_t milliseconds) {
  if (config_.sleep) {
    config_.sleep(milliseconds, shared_->cancel);
    return;
  }
  // In short steps, so Shutdown doesn't wait for the whole delay.
  for (std::uint32_t waited = 0; waited < milliseconds && !shared_->cancel; waited += 100) {
    timespec step{0, 100 * 1000 * 1000};
    nanosleep(&step, nullptr);
  }
}

std::optional<std::string> SharingService::Secret(std::string* error) {
  if (secret_) return secret_;
  std::string note;
  secret_ = community::LoadOrCreateInstallSecret(config_.store_root, error, &note);
  if (!note.empty()) Log("sharing: " + note);
  return secret_;
}

void SharingService::SaveMemory() {
  std::string error;
  if (!community::SaveVoteState(config_.store_root, memory_, &error)) Log("sharing: vote memory not saved: " + error);
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->memory = memory_;
    ++shared_->memory_revision;
  }
}

void SharingService::Handle(SharingRequest request) {
  SharingResult result;
  result.kind = request.kind;
  result.ticket = request.ticket;
  result.game_id = request.game_id;
  const auto context = Context();
  const auto attempts = static_cast<std::uint32_t>(std::max(0, config_.retries)) + 1;
  switch (request.kind) {
    case SharingRequestKind::Submit: {
      auto& submission = request.submission;
      // One key per share attempt: made here the first time, sent back by the model on Try again.
      if (submission.idempotency_key.empty())
        if (const auto key = community::NewIdempotencyKey()) submission.idempotency_key = *key;
      result.idempotency_key = submission.idempotency_key;
      std::string error;
      const auto secret = Secret(&error);
      if (submission.idempotency_key.empty()) {
        result.submission.error = LocalError("invalid_request", "no random bytes for the request key");
      } else if (!secret) {
        Log("sharing: the sharing ID can't be read: " + error);
        result.submission.error = LocalError("sharing_id", "The sharing ID can't be read on this console.");
      } else {
        for (std::uint32_t attempt = 1; attempt <= attempts; ++attempt) {
          result.submission = community::SubmitRecipe(context, *secret, submission, shared_->cancel);
          const auto& answer = result.submission;
          Log("sharing: share app=" + std::to_string(submission.app_id) + " attempt=" + std::to_string(attempt) +
              " outcome=" + OutcomeName(answer.outcome) + " status=" + std::to_string(answer.error.http_status) +
              " code=" + answer.error.code + " id=" + answer.id + " recipe=" + answer.recipe_id + " state=" + answer.state);
          if (answer.outcome != community::SubmissionOutcome::kError || !Retryable(answer.error) || attempt == attempts ||
              shared_->cancel)
            break;
          const std::uint32_t asked = answer.error.retry_after > 0 && answer.error.retry_after <= 10
                                          ? static_cast<std::uint32_t>(answer.error.retry_after) * 1000
                                          : 0;
          Sleep(std::max(asked, config_.retry_delay_ms * attempt));
        }
      }
      break;
    }
    case SharingRequestKind::Status: {
      // The sharing ID isn't sent here, but an answer echoing it is still refused.
      std::string ignored;
      const auto secret = Secret(&ignored);
      result.status = community::GetSubmission(context, request.submission_id, shared_->cancel, secret ? *secret : std::string());
      result.status.id = request.submission_id;
      Log("sharing: status id=" + request.submission_id + " ok=" + std::to_string(result.status.ok) + " state=" +
          result.status.state + " code=" + result.status.error.code);
      break;
    }
    case SharingRequestKind::Vote: {
      const auto& vote = request.vote;
      std::string error;
      const auto secret = Secret(&error);
      if (!secret) {
        Log("sharing: the sharing ID can't be read: " + error);
        result.vote.error = LocalError("sharing_id", "The sharing ID can't be read on this console.");
      } else {
        for (std::uint32_t attempt = 1; attempt <= attempts; ++attempt) {
          result.vote = community::SendVote(context, *secret, vote, shared_->cancel);
          Log("sharing: vote recipe=" + vote.recipe_id + " vote=" + vote.vote + " attempt=" + std::to_string(attempt) +
              " ok=" + std::to_string(result.vote.ok) + " status=" + std::to_string(result.vote.error.http_status) +
              " code=" + result.vote.error.code);
          if (result.vote.ok || !Retryable(result.vote.error) || attempt == attempts || shared_->cancel) break;
          Sleep(config_.retry_delay_ms * attempt);
        }
      }
      // Answered either way: the revision was asked about. A vote is remembered once the API took it.
      memory_.MarkAsked(vote.recipe_id, vote.revision);
      if (result.vote.ok) memory_.Record({vote.recipe_id, vote.revision, vote.vote, vote.reason});
      SaveMemory();
      break;
    }
    case SharingRequestKind::MarkAsked:
      if (!memory_.WasAsked(request.vote.recipe_id, request.vote.revision)) {
        memory_.MarkAsked(request.vote.recipe_id, request.vote.revision);
        SaveMemory();
      }
      break;
    case SharingRequestKind::ResetSharingId: {
      std::string error;
      auto fresh = community::ResetInstallSecret(config_.store_root, &error);
      result.ok = fresh.has_value();
      if (fresh) secret_ = std::move(fresh);
      else result.error = "It could not be saved: " + error;
      Log(std::string("sharing: reset sharing ID ok=") + (result.ok ? "1" : "0") + (result.ok ? "" : " " + error));
      break;
    }
  }
  if (request.kind == SharingRequestKind::MarkAsked) return;
  std::lock_guard<std::mutex> lock(shared_->mutex);
  shared_->results.push_back(std::move(result));
}

void SharingService::Log(const std::string& line) const {
  // The host's sink redacts too; only this worker knows the sharing ID itself.
  if (config_.log) config_.log(diagnostics::RedactSecrets(line, secret_ ? std::string_view(*secret_) : std::string_view()));
}

}  // namespace playden::homebrew
