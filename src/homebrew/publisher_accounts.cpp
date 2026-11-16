// publisher_accounts.cpp - The phone sign-in of publisher accounts (Rockstar Games).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/publisher_accounts.hpp"

#include "runtime/publisher_account.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <iterator>
#include <unistd.h>

namespace playden::homebrew {
namespace {

constexpr std::size_t kMaxPageBytes = 1024 * 1024;

}  // namespace

// POSIX reads: file streams pull libc++'s locale support, which the PS5 app can't link.
std::optional<std::string> ReadPublisherSignInPage(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string page;
  char buffer[16 * 1024];
  bool ok = true;
  for (;;) {
    const auto got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      ok = got == 0;
      break;
    }
    page.append(buffer, static_cast<std::size_t>(got));
    if (page.size() > kMaxPageBytes) {
      ok = false;
      break;
    }
  }
  ::close(fd);
  if (!ok) return std::nullopt;
  return page;
}

PublisherAccounts::PublisherAccounts(Config config) : config_(std::move(config)) {
  if (!config_.read_page) config_.read_page = ReadPublisherSignInPage;
}

PublisherAccounts::~PublisherAccounts() {
  if (relay_) relay_->Stop();
  for (auto& relay : retired_) relay->Stop();
}

void PublisherAccounts::Retire() {
  if (relay_) {
    relay_->Close();
    retired_.push_back(std::move(relay_));
  }
  // Pages whose thread ended are joined at once.
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [](const std::unique_ptr<runtime::PhoneRelay>& relay) {
                                  if (relay->Running()) return false;
                                  relay->Stop();
                                  return true;
                                }),
                 retired_.end());
}

void PublisherAccounts::Begin(runtime::Publisher publisher, const std::string& name, const std::string& page_path) {
  Retire();
  publisher_ = publisher;
  const std::string slug = runtime::PublisherSlug(publisher);
  view_ = {slug, PublisherSignInState::Starting, {}, 0, {}, {}};
  const auto log = [this](const std::string& line) {
    if (config_.log) config_.log(line);
  };
  const auto host = config_.lan_address ? config_.lan_address() : std::nullopt;
  if (!host) {
    view_.state = PublisherSignInState::NoNetwork;
    log("publisher sign-in " + slug + " not started: no network address");
    return;
  }
  runtime::PhoneRelayPage page{"Sign in to " + name, {}};
  if (!page_path.empty()) {
    if (auto html = config_.read_page(page_path)) page.html = std::move(*html);
    else log("publisher sign-in " + slug + " page unreadable; the paste page answers");
  }
  const bool pack_page = !page.html.empty();
  auto relay = std::make_unique<runtime::PhoneRelay>();
  std::string reason;
  const bool started = relay->Start(
      *host, config_.port, std::move(page),
      [submit = config_.submit, publisher](std::string credential) {
        if (!submit) return runtime::PhoneRelayAnswer{false, {}, "Playden can't keep this sign-in."};
        return submit(publisher, std::move(credential));
      },
      &reason, config_.lifetime_ms);
  if (!started) {
    view_.state = PublisherSignInState::Failed;
    view_.message = "Playden couldn't open the sign-in page (" + reason + ").";
    log("publisher sign-in " + slug + " page failed: " + reason);
    return;
  }
  relay_ = std::move(relay);
  view_.address = relay_->Address();
  view_.expires_at = (config_.now_unix ? config_.now_unix() : 0) + config_.lifetime_ms / 1000;
  log("publisher sign-in " + slug + " page open" + (pack_page ? " (pack page)" : " (paste page)"));
}

void PublisherAccounts::Cancel() {
  if ((relay_ || view_.state != PublisherSignInState::Idle) && config_.log)
    config_.log("publisher sign-in " + view_.slug + " closed");
  Retire();
  view_ = {};
}

LivePublisherSignIn PublisherAccounts::Current() {
  if (!relay_) return view_;
  auto view = view_;
  const auto status = relay_->Status();
  switch (status.phase) {
    case runtime::PhoneRelayPhase::kWaiting: view.state = PublisherSignInState::Waiting; break;
    case runtime::PhoneRelayPhase::kConnected: view.state = PublisherSignInState::Connected; break;
    case runtime::PhoneRelayPhase::kChecking: view.state = PublisherSignInState::Checking; break;
    case runtime::PhoneRelayPhase::kAccepted:
      view.state = PublisherSignInState::SignedIn;
      view.display = status.display;
      break;
    case runtime::PhoneRelayPhase::kRefused:
      view.state = PublisherSignInState::Refused;
      view.message = status.message;
      break;
    case runtime::PhoneRelayPhase::kClosed:
      // Its ten minutes ran out.
      view.state = PublisherSignInState::Expired;
      view.address.clear();
      break;
  }
  return view;
}

}  // namespace playden::homebrew
