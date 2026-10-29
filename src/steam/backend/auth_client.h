#pragma once

#include "steam/auth_backend.h"
#include "steam/backend/auth_messages.h"
#include "steam/backend/http_client.h"
#include <functional>
#include <memory>

namespace playden::steam {

// Deterministic protocol tests inject transport and clocks. Production uses
// verified HttpClient, steady_clock and cancel-aware waits. These callbacks must
// be bounded and run only on the owning service worker.
struct AuthClientHooks {
  std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)> perform;
  std::function<std::uint64_t()> monotonic_ms;
  std::function<std::uint64_t()> unix_seconds;
  std::function<bool(std::uint32_t, const std::atomic<bool>&)> wait;
  // Optional sanitized failure report: method name, HTTP status and X-EResult number only.
  std::function<void(const char* method, long http_status, int eresult)> failure;
  // Renews the access token through an authenticated CM session, the only place the refresh token is sent
  // after sign-in. Returns the updated session or an error; without it no token can be renewed.
  std::function<AuthResult(const StoredSession&, const std::atomic<bool>&)> cm_renew;
  // Optional report of a successful restore: which path (stored, cm), and the refresh token's
  // fingerprint before and after (they differ when Steam rotated it).
  std::function<void(const char* path, const std::string& before, const std::string& after)> restored;
  // Optional: the machine ID the CM logon sends, so a sign-in names the same device (empty: none sent).
  std::function<std::vector<std::uint8_t>()> machine_id;
  // Optional report of a new sign-in's token claims: Steam's persistence flag (-1: none) and the token's
  // lifetime in seconds. Never the token or the account.
  std::function<void(long long persistence, std::uint64_t lifetime_seconds)> signed_in;
};

class AuthClient final : public AuthBackend {
 public:
  // No network initialization occurs until a service-worker call.
  explicit AuthClient(std::string ca_bundle);
  explicit AuthClient(AuthClientHooks hooks);
  // Installs the sanitized failure report on a production client before first use.
  void SetFailureReport(std::function<void(const char*, long, int)> report) {
    hooks_.failure = std::move(report);
  }
  void SetRestoreReport(std::function<void(const char*, const std::string&, const std::string&)> report) {
    hooks_.restored = std::move(report);
  }
  void SetMachineId(std::function<std::vector<std::uint8_t>()> machine_id) {
    hooks_.machine_id = std::move(machine_id);
  }
  void SetSignInReport(std::function<void(long long, std::uint64_t)> report) {
    hooks_.signed_in = std::move(report);
  }
  void SetCmRenewal(std::function<AuthResult(const StoredSession&, const std::atomic<bool>&)> renew) {
    hooks_.cm_renew = std::move(renew);
  }
  AuthResult AuthenticateQr(const std::atomic<bool>& cancel,
                            const ChallengeCallback& challenge) override;
  AuthResult Restore(const StoredSession& session,
                     const std::atomic<bool>& cancel) override;
  AuthResult AuthenticateCredentials(Credentials& credentials, const std::atomic<bool>& cancel,
                                     const GuardPromptCallback& prompt, const GuardCodeSource& code) override;
 private:
  // get: the message goes in the query string (GetPasswordRSAPublicKey); otherwise it is the form body.
  AuthError Post(const char* method, const std::vector<std::uint8_t>& message,
                 const std::atomic<bool>& cancel, std::uint32_t timeout_ms,
                 std::vector<std::uint8_t>* body, bool get = false);
  // One PollAuthSessionStatus round. Updates client_id when Steam replaces it.
  AuthError Poll(std::uint64_t* client_id, const std::vector<std::uint8_t>& request_id,
                 const std::atomic<bool>& cancel, std::uint32_t timeout_ms,
                 auth::PollAuthSessionStatusResponse* approval);
  // Checks the approved tokens and renews a missing access token.
  AuthResult Approved(auth::PollAuthSessionStatusResponse approval, std::uint64_t started,
                      std::uint64_t lifetime_ms, const std::atomic<bool>& cancel);
  AuthResult Renew(const StoredSession& session, const std::atomic<bool>& cancel);
  AuthResult Restored(const char* path, const StoredSession& before, AuthResult result);
  AuthClientHooks hooks_;
  int last_eresult_ = 1;  // X-EResult of the most recent refused Post, for renewal fallback
  std::string ca_bundle_;
  std::unique_ptr<HttpClient> http_;
};

}  // namespace playden::steam
