// gog_wiring.cpp - GOG in the Mac and PS5 apps, behind the gog-store line.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/gog_wiring.hpp"

#include "app/diagnostic_log.h"
#include "gog/auth.h"
#include "homebrew/controller.hpp"
#include "homebrew/simulation.hpp"
#include "stores/gog.h"
#include "stores/gog_details.h"
#include "stores/gog_install.h"
#include "stores/gog_library.h"
#include "stores/registry.h"
#include "web/gog_relay.h"
#include "web/service.h"

#include <chrono>
#include <utility>

namespace playden::homebrew {
namespace {

web::GogExchangeAnswer::Kind RelayKind(stores::SignInAnswer::Kind kind) {
  using In = stores::SignInAnswer::Kind;
  using Out = web::GogExchangeAnswer::Kind;
  switch (kind) {
    case In::kSignedIn: return Out::kSignedIn;
    case In::kFailed: return Out::kFailed;
    case In::kStale: return Out::kStale;
    case In::kBusy: return Out::kBusy;
    case In::kClosing: return Out::kClosing;
    case In::kTimeout: return Out::kTimeout;
  }
  return Out::kFailed;
}

}  // namespace

GogServices::GogServices(GogWiringConfig config) : config_(std::move(config)) {
  const auto transport = [this] {
    return config_.transport ? config_.transport() : gog::ClientTransport(config_.ca_bundle);
  };
  const std::string logs = config_.log_directory;
  const auto gog_log = [logs](const std::string& line) { app::AppendLogLine(logs, "gog.log", line); };
  gog::SessionStore::Options options;
  options.user_root = config_.user_root;
  options.persist = config_.stay_signed_in;
  store_ = std::make_unique<gog::SessionStore>(std::move(options));
  // The token owner's clock only measures how long the access token has left: monotonic seconds.
  session_ = std::make_unique<gog::Session>(*store_, transport(), [] {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }, gog_log);
  cache_ = std::make_unique<LibraryCache>(*config_.library_storage, "library-gog-");
  tags_ = std::make_unique<CollectionsCache>(*config_.library_storage, "collections-gog-");
  // Its transports are made per download thread while an install runs, never here.
  const std::string install_logs = config_.install_log_directory.empty() ? logs : config_.install_log_directory;
  install_runner_ = std::make_unique<stores::GogInstallRunner>(
      *session_, transport, config_.content_language,
      [install_logs](const std::string& line) { app::AppendLogLine(install_logs, "install.log", line); },
      config_.free_bytes);
  owned_account_ = std::make_unique<stores::GogAccount>(*session_, *store_, gog_log);
  account_ = owned_account_.get();
  owned_library_ = std::make_unique<stores::GogLibrarySource>(*session_, transport(), *cache_, config_.user_root, gog_log,
                                                              std::function<std::uint64_t()>{}, tags_.get());
  library_ = owned_library_.get();
  // Its transport is made on its own worker, at its first request.
  owned_details_ = std::make_unique<stores::GogDetailsSource>(*session_, transport, config_.content_language, gog_log);
  details_ = owned_details_.get();
  if (config_.installed_record) details_->SetInstalled(config_.installed_record);
  if (config_.install_root) details_->SetInstallRoot(config_.install_root);

  web::GogRelayConfig relay;
  relay.parse_code = [](std::string_view paste) { return gog::CodeFromPaste(paste); };
  // On a web server worker. The controller's Shutdown stops the account (which answers this with kClosing) before
  // web access's workers are joined, and the registry keeps the account until after both.
  relay.exchange = [account = account_](std::string code, std::uint64_t request) {
    const auto answer = account->SignInWithCode(std::move(code), request);
    web::GogExchangeAnswer result;
    result.kind = RelayKind(answer.kind);
    result.name = answer.display_name;
    return result;
  };
  relay.random_token = config_.random_token;
  relay.log = [logs](const std::string& line) { app::AppendLogLine(logs, "web.log", line); };
  relay_ = std::make_shared<web::GogRelay>(std::move(relay));
}

GogServices::~GogServices() = default;

void GogServices::Register(stores::Registry& registry, Simulation& model) {
  if (owned_account_) registry.Add(std::move(owned_account_));
  if (owned_library_) registry.Add(std::move(owned_library_));
  if (owned_details_) registry.Add(std::unique_ptr<stores::DetailsSource>(std::move(owned_details_)));
  model.SetGogStoreEnabled(true);
  model.SetStoreInstalls(Store::kGog, true);
}

installs::InstallRunner* GogServices::install_runner() const { return install_runner_.get(); }

void GogServices::ConfigureWeb(web::ServiceConfig& config) const {
  config.gog = relay_;
  config.gog_auth_url = gog::AuthorizeUrl();
}

void GogServices::Attach(Controller& controller) const { controller.SetGogRelay(relay_); }

void GogServices::SetStaySignedIn(bool stay) {
  // On the account worker: the main thread never waits for the store's root lock.
  account_->SetPersist(stay, [logs = config_.log_directory](const gog::SaveResult& moved) {
    app::AppendLogLine(logs, "auth.log", std::string("stay signed in gog moved=") + (moved.ok ? "1" : "0"));
  });
}

std::unique_ptr<GogServices> MakeGogServices(GogWiringConfig config) {
  // Before anything reads it: off, a session saved by an earlier run (and what a failed save left beside it) goes.
  // With the line off this touches nothing unless a gog folder is there, and creates nothing.
  if (!config.stay_signed_in && !gog::SessionStore::ForgetIfNotPersisted(config.user_root))
    app::AppendLogLine(config.log_directory, "auth.log", "stay signed in gog forget result=failed");
  if (!config.enabled || !config.library_storage) return nullptr;
  return std::unique_ptr<GogServices>(new GogServices(std::move(config)));
}

}  // namespace playden::homebrew
