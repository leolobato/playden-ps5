#pragma once
// publisher_accounts.hpp - The phone sign-in of publisher accounts (Rockstar Games).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/simulation.hpp"
#include "runtime/phone_relay.h"
#include "runtime/publisher_pack.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playden::homebrew {

// Owns the one-time phone page of a publisher sign-in (runtime/phone_relay.h). The page's thread
// hands the credential the phone sends to the account store (the runtime host) and answers the
// phone with the outcome; the application thread only starts, cancels and reads. The account
// itself lives in the runtime host: this only carries one sign-in at a time.
class PublisherAccounts {
 public:
  struct Config {
    std::function<std::optional<std::string>()> lan_address = runtime::LanAddress;
    std::uint16_t port = 8099;
    std::int64_t lifetime_ms = 10 * 60 * 1000;
    std::function<std::int64_t()> now_unix;
    // Stores a credential and says how it went; called on the page's thread, it may wait for the
    // runtime host (RuntimeHost::SubmitPublisherSignInNow). It wipes the credential.
    std::function<runtime::PhoneRelayAnswer(runtime::Publisher, std::string credential)> submit;
    // The publisher pack's sign-in page; nothing when it can't be read (the paste page answers).
    std::function<std::optional<std::string>(const std::string& path)> read_page;
    std::function<void(const std::string&)> log;  // no credentials or addresses are passed
  };

  explicit PublisherAccounts(Config config);
  ~PublisherAccounts();
  PublisherAccounts(const PublisherAccounts&) = delete;
  PublisherAccounts& operator=(const PublisherAccounts&) = delete;

  // Opens a new page for the publisher (name: "Rockstar Games"; page_path: its pack's sign-in
  // page, "" for none), closing any earlier one.
  void Begin(runtime::Publisher publisher, const std::string& name, const std::string& page_path);
  // Closes the page; a credential being checked still finishes, but the screen no longer follows it.
  void Cancel();
  LivePublisherSignIn Current();

 private:
  void Retire();
  Config config_;
  runtime::Publisher publisher_ = runtime::Publisher::kRockstar;
  LivePublisherSignIn view_;  // NoNetwork and Failed; otherwise the page's state is read live
  std::unique_ptr<runtime::PhoneRelay> relay_;
  // Closed pages whose thread may still be answering a phone; joined once they end, or here at
  // the end, so none outlives the runtime host it hands credentials to.
  std::vector<std::unique_ptr<runtime::PhoneRelay>> retired_;
};

// Reads a publisher pack's sign-in page (at most 1 MiB); nothing when it is missing or too large.
std::optional<std::string> ReadPublisherSignInPage(const std::string& path);

}  // namespace playden::homebrew
