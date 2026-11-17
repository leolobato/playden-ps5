// SPDX-License-Identifier: GPL-3.0-or-later
// playden-gog-dev: GOG's calls from Playden's own client code, on the host, for a maintainer probing a real account
// (plan Phase 0). Not shipped: the host preset builds it, the PS5 build doesn't.
//
//   playden-gog-dev url                               the address of GOG's sign-in page, to open in a browser
//   playden-gog-dev --data <folder> sign-in           reads the address GOG opened after sign-in from stdin (not
//                                                     from the command line, so it stays out of shell history), and
//                                                     saves the session in <folder>/gog/session
//   playden-gog-dev --data <folder> restore           loads and refreshes the saved session
//   playden-gog-dev --data <folder> owned             the owned product IDs (DLC included)
//   playden-gog-dev product <id>                      gamesdb's entry as the library reads it (no sign-in)
//   playden-gog-dev builds <id>                       the default branch's Windows build (no sign-in)
//   playden-gog-dev --data <folder> install <id> <game folder> [language]
//                                                     installs the default branch's build as Playden does, record
//                                                     and play tasks included; run again to resume
//   playden-gog-dev --data <folder> update <id> <game folder>
//                                                     brings that install to the default branch's newest build
//   playden-gog-dev --data <folder> get <path> <file>  one signed-in GET of https://embed.gog.com<path>, the answer
//                                                     saved to <file> (0600); prints the status and size
//   playden-gog-dev --data <folder> sign-out          deletes the saved session
//
// --ca <file> picks the CA bundle (default: the one the host app uses). <folder> and <game folder> are absolute,
// with no link in any component (macOS /tmp is one). Output never holds a token, the sign-in code, the username or
// the user ID: the client's own log lines (stderr) name steps, statuses and counts only.
#include "gog/api.h"
#include "gog/auth.h"
#include "gog/content.h"
#include "gog/http.h"
#include "gog/session.h"
#include "gog/session_store.h"
#include "installs/install_service.h"
#include "library/store.h"
#include "platform/secret.h"
#include "stores/gog_install.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef PLAYDEN_GOG_DEV_CA_BUNDLE
#define PLAYDEN_GOG_DEV_CA_BUNDLE ""
#endif

namespace {
using namespace playden;

int Usage() {
  std::fputs(
      "usage: playden-gog-dev [--ca <file>] [--data <folder>] <command> [arguments]\n"
      "  url | sign-in | restore | owned | product <id> | builds <id> | get <path> <file>\n"
      "  install <id> <game folder> [language] | update <id> <game folder> | sign-out\n"
      "sign-in reads the address GOG opened after sign-in from stdin.\n",
      stderr);
  return 2;
}

std::optional<std::uint64_t> ProductId(const char* text) {
  if (!text || !*text || *text == '0' || std::strlen(text) > 20) return std::nullopt;
  std::uint64_t value = 0;
  for (const char* c = text; *c; ++c) {
    if (*c < '0' || *c > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(*c - '0');
    if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

bool Absolute(const std::string& path) { return !path.empty() && path[0] == '/'; }

std::int64_t MonotonicSeconds() {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Log(const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); }

const char* RestoreName(gog::RestoreOutcome::Kind kind) {
  using Kind = gog::RestoreOutcome::Kind;
  switch (kind) {
    case Kind::kNone: return "none";
    case Kind::kStorage: return "storage";
    case Kind::kSignedIn: return "signed-in";
    case Kind::kExpired: return "expired";
    case Kind::kOffline: return "offline";
    case Kind::kCanceled: return "canceled";
  }
  return "unknown";
}

const char* TokenName(gog::AccessToken::Status status) {
  using Status = gog::AccessToken::Status;
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kNetwork: return "network";
    case Status::kRevoked: return "revoked";
    case Status::kSignedOut: return "signed-out";
    case Status::kCanceled: return "canceled";
  }
  return "unknown";
}

const char* BuildName(gog::BuildChoice::Kind kind) {
  using Kind = gog::BuildChoice::Kind;
  switch (kind) {
    case Kind::kGen2: return "gen2";
    case Kind::kGen1: return "gen1";
    case Kind::kNone: return "none";
    case Kind::kProtocol: return "protocol";
  }
  return "unknown";
}

// One line from stdin, at most 4 KiB, without its line end. Wiped by the caller.
std::optional<std::string> ReadPaste() {
  std::string paste;
  int c = 0;
  while ((c = std::fgetc(stdin)) != EOF && c != '\n') {
    if (paste.size() >= 4096) {
      platform::ZeroizeSecret(paste);
      return std::nullopt;
    }
    paste.push_back(static_cast<char>(c));
  }
  if (!paste.empty() && paste.back() == '\r') paste.pop_back();
  return paste;
}

struct Account {
  std::unique_ptr<gog::SessionStore> store;
  std::unique_ptr<gog::Session> session;
};

Account Open(const std::string& data, const std::string& ca) {
  gog::SessionStore::Options options;
  options.user_root = data;
  options.persist = true;
  Account account;
  account.store = std::make_unique<gog::SessionStore>(std::move(options));
  account.session = std::make_unique<gog::Session>(*account.store, gog::ClientTransport(ca), MonotonicSeconds, Log);
  return account;
}

int SignIn(Account& account) {
  std::fputs("Paste the address GOG opened after sign-in, then press Return:\n", stderr);
  auto paste = ReadPaste();
  if (!paste) {
    std::fputs("sign-in: the paste is too long\n", stderr);
    return 1;
  }
  auto code = gog::CodeFromPaste(*paste);
  platform::ZeroizeSecret(*paste);
  if (!code) {
    std::fputs("sign-in: that isn't the address GOG opened after sign-in\n", stderr);
    return 1;
  }
  const std::atomic<bool> cancel{false};
  const auto epoch = account.session->BeginSignIn();
  auto outcome = account.session->Exchange(std::move(*code), epoch, cancel);
  platform::ZeroizeSecret(*code);
  platform::ZeroizeSecret(outcome.username);
  switch (outcome.kind) {
    case gog::ExchangeOutcome::Kind::kSignedIn:
      std::printf("sign-in: signed in%s\n", outcome.storage_full ? " (not saved: storage full)" : "");
      return 0;
    case gog::ExchangeOutcome::Kind::kFailed:
      std::printf("sign-in: failed%s\n", outcome.rejected ? " (GOG refused the code: sign in again)" : "");
      return 1;
    case gog::ExchangeOutcome::Kind::kStale:
      std::puts("sign-in: dropped");
      return 1;
  }
  return 1;
}

int Restore(Account& account) {
  const std::atomic<bool> cancel{false};
  const auto outcome = account.session->Restore(cancel);
  std::printf("restore: %s\n", RestoreName(outcome.kind));
  return outcome.kind == gog::RestoreOutcome::Kind::kSignedIn ? 0 : 1;
}

int Owned(Account& account, const std::string& ca) {
  const std::atomic<bool> cancel{false};
  const auto restored = account.session->Restore(cancel);
  if (restored.kind != gog::RestoreOutcome::Kind::kSignedIn && restored.kind != gog::RestoreOutcome::Kind::kOffline) {
    std::printf("owned: not signed in (%s)\n", RestoreName(restored.kind));
    return 1;
  }
  auto token = account.session->Token(cancel);
  if (token.status != gog::AccessToken::Status::kOk) {
    std::printf("owned: no access token (%s)\n", TokenName(token.status));
    return 1;
  }
  auto transport = gog::ClientTransport(ca);
  const auto owned = gog::FetchOwned(transport, token.token, cancel);
  token.Wipe();
  if (owned.kind != gog::OwnedResult::Kind::kOk) {
    std::printf("owned: failed status=%ld\n", owned.status);
    return 1;
  }
  std::printf("owned: %zu products\n", owned.ids.size());
  for (const auto id : owned.ids) std::printf("%" PRIu64 "\n", id);
  return 0;
}

// One signed-in GET of a path on embed.gog.com (#536 Phase 0: the library tags), its answer written to `out` (0600).
// Prints the status and size only.
int Get(Account& account, const std::string& ca, const std::string& path, const std::string& out) {
  if (path.empty() || path[0] != '/' || path.find("//") != std::string::npos || path.find('@') != std::string::npos) {
    std::puts("get: the path must start with one '/'");
    return 2;
  }
  const std::atomic<bool> cancel{false};
  const auto restored = account.session->Restore(cancel);
  if (restored.kind != gog::RestoreOutcome::Kind::kSignedIn) {
    std::printf("get: not signed in (%s)\n", RestoreName(restored.kind));
    return 1;
  }
  auto token = account.session->Token(cancel);
  if (token.status != gog::AccessToken::Status::kOk) {
    std::printf("get: no access token (%s)\n", TokenName(token.status));
    return 1;
  }
  steam::HttpRequest request;
  request.method = steam::HttpMethod::kGet;
  request.url = std::string("https://embed.gog.com") + path;
  request.timeout_ms = 30'000;
  request.max_response_bytes = 16u << 20;
  std::string bearer = "Bearer ";
  bearer.reserve(7 + token.token.size());
  bearer += token.token;
  token.Wipe();
  request.headers.emplace_back("User-Agent", gog::kUserAgent);
  request.headers.emplace_back("Authorization", std::move(bearer));
  auto transport = gog::ClientTransport(ca);
  const auto response = transport(request, cancel);
  platform::ZeroizeSecret(request.headers.back().second);
  if (!response.ok()) {
    std::puts("get: no answer");
    return 1;
  }
  FILE* file = std::fopen(out.c_str(), "wb");
  if (!file) return 1;
  chmod(out.c_str(), 0600);
  std::fwrite(response.body.data(), 1, response.body.size(), file);
  std::fclose(file);
  std::printf("get: status=%ld bytes=%zu\n", response.status_code, response.body.size());
  return response.status_code == 200 ? 0 : 1;
}

int Product(std::uint64_t id, const std::string& ca) {
  const std::atomic<bool> cancel{false};
  auto transport = gog::ClientTransport(ca);
  const auto answer = gog::FetchProduct(transport, id, {}, cancel);
  if (answer.kind == gog::ProductResult::Kind::kMissing) {
    std::puts("product: gamesdb has no entry");
    return 1;
  }
  if (answer.kind != gog::ProductResult::Kind::kModified) {
    std::printf("product: failed status=%ld\n", answer.status);
    return 1;
  }
  const auto info = gog::ParseProduct(answer.body, id);
  const char* kind = info.kind == gog::ProductInfo::Kind::kGame     ? "game (listed)"
                     : info.kind == gog::ProductInfo::Kind::kHidden ? "hidden (DLC, spam, not visible or no Windows build)"
                                                                    : "invalid";
  std::printf("kind: %s\netag: %s\n", kind, answer.etag.empty() ? "-" : "yes");
  if (info.kind != gog::ProductInfo::Kind::kGame) return 0;
  std::printf("title: %s\ndeveloper: %s\ncover: %s\nhero: %s\ndlc: %zu\n", info.title.c_str(), info.developer.c_str(),
              info.cover.empty() ? "-" : info.cover.c_str(), info.hero.empty() ? "-" : info.hero.c_str(),
              info.dlc_ids.size());
  for (const auto dlc : info.dlc_ids) std::printf("  dlc %" PRIu64 "\n", dlc);
  return 0;
}

int Builds(std::uint64_t id, const std::string& ca) {
  const std::atomic<bool> cancel{false};
  auto transport = gog::ClientTransport(ca);
  const auto answer = gog::FetchBuilds(transport, id, cancel);
  if (!answer.answered || answer.status != 200) {
    std::printf("builds: failed status=%ld\n", answer.status);
    return 1;
  }
  const auto choice = gog::ChooseBuild(answer.body);
  std::printf("build: %s\n", BuildName(choice.kind));
  if (choice.kind != gog::BuildChoice::Kind::kGen2 && choice.kind != gog::BuildChoice::Kind::kGen1) return 1;
  std::printf("build_id: %s\ngeneration: %d\n", choice.build_id.c_str(), choice.generation);
  if (choice.kind == gog::BuildChoice::Kind::kGen2) std::printf("meta: %s\n", choice.meta_id.c_str());
  else std::printf("timestamp: %" PRIu64 "\n", choice.timestamp);
  // Endpoint names and order only: their URLs are public here, but nothing in Playden prints CDN addresses.
  for (const auto& endpoint : choice.kind == gog::BuildChoice::Kind::kGen2 ? choice.meta : choice.repository)
    std::printf("cdn: %s priority=%d%s\n", endpoint.name.c_str(), endpoint.priority,
                endpoint.fallback_only ? " fallback-only" : "");
  return 0;
}

int Download(Account& account, const std::string& ca, std::uint64_t id, const std::string& root,
             const std::string& language, bool update) {
  const std::atomic<bool> cancel{false};
  const auto restored = account.session->Restore(cancel);
  if (restored.kind != gog::RestoreOutcome::Kind::kSignedIn && restored.kind != gog::RestoreOutcome::Kind::kOffline) {
    std::printf("%s: not signed in (%s)\n", update ? "update" : "install", RestoreName(restored.kind));
    return 1;
  }
  stores::GogInstallRunner runner(
      *account.session, [ca] { return gog::ClientTransport(ca); }, [language] { return language; }, Log);
  installs::InstallOptions options;
  options.language = language;
  std::uint64_t last_percent = 101;
  const auto progress = [&last_percent](std::uint64_t done, std::uint64_t total, std::uint64_t held) {
    (void)held;
    const std::uint64_t percent = total ? done * 100 / total : 0;
    if (percent == last_percent) return;
    last_percent = percent;
    std::fprintf(stderr, "progress %" PRIu64 "%% (%" PRIu64 " of %" PRIu64 " bytes)\n", percent, done, total);
  };
  const auto step = [](installs::InstallStep) {};
  const auto result = update ? runner.Update(GameId::Gog(id), root, options, cancel, progress, step, [] { return true; })
                             : runner.Install(GameId::Gog(id), root, options, cancel, progress, step);
  if (result.ok) {
    std::printf("%s: done, %" PRIu64 " bytes\n", update ? "update" : "install", result.install_bytes);
    return 0;
  }
  std::printf("%s: %s%s%s\n", update ? "update" : "install", result.retry_later ? "waiting: " : "failed: ",
              result.retry_later ? result.waiting.c_str() : result.failure.c_str(), result.canceled ? " (canceled)" : "");
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string ca = PLAYDEN_GOG_DEV_CA_BUNDLE;
  std::string data;
  int at = 1;
  for (; at < argc; ++at) {
    if (std::strcmp(argv[at], "--ca") == 0 && at + 1 < argc) {
      ca = argv[++at];
    } else if (std::strcmp(argv[at], "--data") == 0 && at + 1 < argc) {
      data = argv[++at];
    } else {
      break;
    }
  }
  if (at >= argc) return Usage();
  const std::string command = argv[at++];
  const int rest = argc - at;

  if (command == "url" && rest == 0) {
    std::printf("%s\n", gog::AuthorizeUrl().c_str());
    return 0;
  }
  if (ca.empty()) {
    std::fputs("No CA bundle: pass --ca <file>.\n", stderr);
    return 2;
  }
  if (command == "product" || command == "builds") {
    const auto id = rest == 1 ? ProductId(argv[at]) : std::nullopt;
    if (!id) return Usage();
    return command == "product" ? Product(*id, ca) : Builds(*id, ca);
  }

  if (!Absolute(data)) {
    std::fputs("This command needs --data <absolute folder> for the session.\n", stderr);
    return 2;
  }
  auto account = Open(data, ca);
  if (command == "sign-in" && rest == 0) return SignIn(account);
  if (command == "restore" && rest == 0) return Restore(account);
  if (command == "owned" && rest == 0) return Owned(account, ca);
  if (command == "get" && rest == 2) return Get(account, ca, argv[at], argv[at + 1]);
  if (command == "sign-out" && rest == 0) {
    const auto cleared = account.session->SignOut();
    std::printf("sign-out: %s\n", cleared.ok ? "done" : "failed");
    return cleared.ok ? 0 : 1;
  }
  if ((command == "install" && (rest == 2 || rest == 3)) || (command == "update" && rest == 2)) {
    const auto id = ProductId(argv[at]);
    const std::string root = argv[at + 1];
    if (!id || !Absolute(root)) return Usage();
    const std::string language = rest == 3 ? argv[at + 2] : "english";
    return Download(account, ca, *id, root, language, command == "update");
  }
  return Usage();
}
