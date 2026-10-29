#include "steam/backend/auth_client.h"
#include "steam/backend/auth_messages.h"
#include "steam/backend/password_crypto.h"
#include "steam/backend/token_fingerprint.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <string_view>
#include <thread>

namespace playden::steam {
namespace {
constexpr std::uint64_t kLifetimeMs = 300000;
// A password sign-in may wait for the player to fetch an email or open the Steam app, twice after a
// wrong code: it gets longer than a QR code.
constexpr std::uint64_t kCredentialLifetimeMs = 600000;
// How often a sign-in that waits only for a typed code checks for one (it does not poll Steam).
constexpr std::uint32_t kCodeCheckMs = 250;
constexpr std::size_t kBodyLimit = 65536;
constexpr std::uint32_t kRequestTimeoutMs = 10000;

AuthResult Failure(AuthError e) { return {e, std::nullopt}; }
AuthClientHooks DefaultHooks() {
  AuthClientHooks h;
  h.monotonic_ms = [] {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  };
  h.unix_seconds = [] {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
  };
  h.wait = [](std::uint32_t ms, const std::atomic<bool>& cancel) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!cancel.load()) {
      const auto remaining = until - std::chrono::steady_clock::now();
      if (remaining <= decltype(remaining)::zero()) return true;
      std::this_thread::sleep_for(std::min(remaining,
          std::chrono::duration_cast<decltype(remaining)>(std::chrono::milliseconds(25))));
    }
    return false;
  };
  return h;
}
std::string FormBase64(const std::vector<std::uint8_t>& message) {
  const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  unsigned bits = 0, count = 0;
  for (auto byte : message) {
    bits = (bits << 8) | byte; count += 8;
    while (count >= 6) { count -= 6; b64 += alphabet[(bits >> count) & 63]; }
  }
  if (count) b64 += alphabet[(bits << (6 - count)) & 63];
  while (b64.size() % 4) b64 += '=';
  std::string encoded = "input_protobuf_encoded=";
  for (char c : b64) {
    if (c == '+') encoded += "%2B";
    else if (c == '/') encoded += "%2F";
    else if (c == '=') encoded += "%3D";
    else encoded += c;
  }
  return encoded;
}
bool ResultHeader(std::string name) {
  for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return name == "x-eresult";
}
bool ChallengeValid(const std::string& s) {
  constexpr std::string_view prefix = "https://s.team/q/";
  if (s.size() <= prefix.size() || s.size() > 4096 || s.compare(0, prefix.size(), prefix) != 0) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c > 32 && c < 127 && c != '#' && c != '\\'; });
}
bool AccountNameValid(const std::string& s) {
  if (s.empty() || s.size() > 256) return false;
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 32 || c == 127) return false;
    if (c < 128) continue;
    unsigned count;
    unsigned char low = 0x80, high = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) count = 1;
    else if (c >= 0xe0 && c <= 0xef) { count = 2; if (c == 0xe0) low = 0xa0; if (c == 0xed) high = 0x9f; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 3; if (c == 0xf0) low = 0x90; if (c == 0xf4) high = 0x8f; }
    else return false;
    if (count > s.size() - i) return false;
    for (unsigned part = 0; part < count; ++part) {
      const auto next = static_cast<unsigned char>(s[i++]);
      if (next < low || next > high) return false;
      low = 0x80; high = 0xbf;
    }
  }
  return true;
}
bool ValidToken(const std::string& token, std::uint64_t id, std::uint64_t now) {
  const auto claims = InspectTokenClaims(token);
  return claims && claims->steam_id == id && claims->expires_at_unix_seconds > now;
}
std::uint32_t Remaining(std::uint64_t start, const AuthClientHooks& hooks,
                        std::uint64_t lifetime = kLifetimeMs) {
  const auto elapsed = hooks.monotonic_ms() - start;
  return elapsed >= lifetime ? 0 : static_cast<std::uint32_t>(lifetime - elapsed);
}
std::uint32_t PollInterval(float interval) {
  return static_cast<std::uint32_t>(std::min<double>(kLifetimeMs,
      std::ceil(std::max<double>(interval, 1.0) * 1000.0)));
}
// Clears the password however the sign-in ends.
struct PasswordWipe {
  std::string& password;
  ~PasswordWipe() { ZeroizeSecret(password); }
};
}

AuthClient::AuthClient(std::string ca_bundle)
    : hooks_(DefaultHooks()), ca_bundle_(std::move(ca_bundle)) {}
AuthClient::AuthClient(AuthClientHooks hooks) : hooks_(std::move(hooks)) {}

AuthError AuthClient::Post(const char* method, const std::vector<std::uint8_t>& message,
                          const std::atomic<bool>& cancel, std::uint32_t timeout_ms,
                          std::vector<std::uint8_t>* body, bool get) {
  body->clear();
  if (cancel.load()) return AuthError::kCanceled;
  if (!timeout_ms) return AuthError::kExpired;
  if (message.size() > kBodyLimit) return AuthError::kProtocol;
  HttpRequest req;
  req.method = get ? HttpMethod::kGet : HttpMethod::kPost;
  req.url = std::string("https://api.steampowered.com/IAuthenticationService/") + method + "/v1/";
  req.timeout_ms = std::min(timeout_ms, kRequestTimeoutMs); req.max_response_bytes = kBodyLimit;
  const auto form = FormBase64(message);
  if (get) {
    req.headers = {{"User-Agent", "Playden/1.0"}};
    req.url += "?" + form;
  } else {
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"User-Agent", "Playden/1.0"}};
    req.body.assign(form.begin(), form.end());
  }
  HttpResponse response;
  if (hooks_.perform) response = hooks_.perform(req, cancel);
  else {
    if (ca_bundle_.empty()) return AuthError::kProtocol;
    if (!http_) http_ = std::make_unique<HttpClient>(ca_bundle_);
    response = http_->Perform(req, cancel);
  }
  if (cancel.load() || response.error == HttpError::kCanceled) return AuthError::kCanceled;
  last_eresult_ = 1;
  auto report = [&](AuthError error, int eresult) {
    last_eresult_ = eresult;
    if (hooks_.failure) hooks_.failure(method, response.status_code, eresult);
    return error;
  };
  if (!response.ok()) return report(AuthError::kNetwork, 0);
  if (response.status_code == 401 || response.status_code == 403)
    return report(AuthError::kRejected, 0);
  if (response.status_code == 429 || response.status_code >= 500) return report(AuthError::kNetwork, 0);
  if (response.status_code != 200 || response.body.size() > kBodyLimit) return report(AuthError::kProtocol, 0);
  int result = 1; bool seen = false;
  for (const auto& header : response.headers) {
    if (!ResultHeader(header.first)) continue;
    if (seen) return AuthError::kProtocol;
    seen = true;
    std::string_view value = header.second;
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < 1)
      return AuthError::kProtocol;
  }
  // Match the reference's absent-header success policy; malformed/duplicate
  // headers fail closed instead of silently becoming success.
  if (result != 1) {
    if (result == 9 || result == 27) return report(AuthError::kExpired, result);
    return report(AuthError::kRejected, result);
  }
  *body = std::move(response.body);
  return AuthError::kNone;
}

AuthResult AuthClient::AuthenticateQr(const std::atomic<bool>& cancel,
                                       const ChallengeCallback& challenge) {
  if (cancel.load()) return Failure(AuthError::kCanceled);
  if (!hooks_.monotonic_ms || !hooks_.unix_seconds || !hooks_.wait || !challenge)
    return Failure(AuthError::kProtocol);
  const auto started = hooks_.monotonic_ms();
  auth::BeginAuthSessionViaQrRequest begin;
  begin.device_details.device_friendly_name = "Playden (PS5)";
  if (hooks_.machine_id) begin.device_details.machine_id = hooks_.machine_id();
  std::vector<std::uint8_t> request, response;
  if (!auth::Encode(begin, &request).ok()) return Failure(AuthError::kProtocol);
  auto error = Post("BeginAuthSessionViaQR", request, cancel, Remaining(started,hooks_), &response);
  if (error != AuthError::kNone) return Failure(error);
  if (!Remaining(started,hooks_)) return Failure(AuthError::kExpired);
  auth::BeginAuthSessionViaQrResponse session;
  if (!auth::Decode(response,&session).ok() || !session.client_id || session.request_id.empty() ||
      session.request_id.size() > 1024 || !ChallengeValid(session.challenge_url) ||
      !std::isfinite(session.interval) || session.interval <= 0) return Failure(AuthError::kProtocol);
  const auto interval = PollInterval(session.interval);
  challenge(session.challenge_url);
  while (!cancel.load()) {
    const auto remaining = Remaining(started,hooks_);
    if (!remaining) return Failure(AuthError::kExpired);
    if (!hooks_.wait(std::min(interval, remaining),cancel) || cancel.load()) return Failure(AuthError::kCanceled);
    if (!Remaining(started,hooks_)) return Failure(AuthError::kExpired);
    auth::PollAuthSessionStatusResponse approval;
    error = Poll(&session.client_id, session.request_id, cancel, Remaining(started,hooks_), &approval);
    if (error != AuthError::kNone) return Failure(error);
    if (!Remaining(started,hooks_)) return Failure(AuthError::kExpired);
    if (approval.new_challenge_url) {
      if (!ChallengeValid(*approval.new_challenge_url)) return Failure(AuthError::kProtocol);
      challenge(*approval.new_challenge_url);
    }
    if (cancel.load()) return Failure(AuthError::kCanceled);
    if (approval.refresh_token.empty()) continue;
    return Approved(std::move(approval), started, kLifetimeMs, cancel);
  }
  return Failure(AuthError::kCanceled);
}

AuthError AuthClient::Poll(std::uint64_t* client_id, const std::vector<std::uint8_t>& request_id,
                          const std::atomic<bool>& cancel, std::uint32_t timeout_ms,
                          auth::PollAuthSessionStatusResponse* approval) {
  auth::PollAuthSessionStatusRequest poll;
  poll.client_id = *client_id; poll.request_id = request_id;
  std::vector<std::uint8_t> request, response;
  if (!auth::Encode(poll,&request).ok()) return AuthError::kProtocol;
  const auto error = Post("PollAuthSessionStatus",request,cancel,timeout_ms,&response);
  if (error != AuthError::kNone) return error;
  if (!auth::Decode(response,approval).ok()) return AuthError::kProtocol;
  if (approval->new_client_id) {
    if (!*approval->new_client_id) return AuthError::kProtocol;
    *client_id = *approval->new_client_id;
  }
  return AuthError::kNone;
}

AuthResult AuthClient::Approved(auth::PollAuthSessionStatusResponse approval, std::uint64_t started,
                                std::uint64_t lifetime_ms, const std::atomic<bool>& cancel) {
  const auto claims = InspectTokenClaims(approval.refresh_token);
  const auto now = hooks_.unix_seconds();
  if (!claims || claims->expires_at_unix_seconds <= now || !AccountNameValid(approval.account_name))
    return Failure(AuthError::kProtocol);
  // Reported once the sign-in succeeds: the refresh token's own claims, not a promise that Steam keeps it.
  auto signed_in = [&, issued = claims->issued_at_unix_seconds ? claims->issued_at_unix_seconds : now,
                    expires = claims->expires_at_unix_seconds, persistence = claims->persistence](AuthResult result) {
    if (result.error == AuthError::kNone && hooks_.signed_in)
      hooks_.signed_in(persistence ? static_cast<long long>(*persistence) : -1, expires > issued ? expires - issued : 0);
    return result;
  };
  StoredSession stored{claims->steam_id, approval.account_name, std::move(approval.refresh_token),
                       std::move(approval.access_token), std::nullopt};
  if (stored.access_token.empty()) {
    auto renewed = Renew(stored,cancel);
    // A refused new token fails this sign-in only; a saved session it would have replaced stays.
    if (renewed.error == AuthError::kCredentialsRevoked) renewed.error = AuthError::kRejected;
    if (cancel.load()) return Failure(AuthError::kCanceled);
    if (!Remaining(started,hooks_,lifetime_ms)) return Failure(AuthError::kExpired);
    return signed_in(std::move(renewed));
  }
  if (!ValidToken(stored.access_token,stored.steam_id,now)) return Failure(AuthError::kProtocol);
  return signed_in({AuthError::kNone,std::move(stored)});
}

AuthResult AuthClient::AuthenticateCredentials(Credentials& credentials, const std::atomic<bool>& cancel,
                                               const GuardPromptCallback& prompt, const GuardCodeSource& code) {
  PasswordWipe wipe{credentials.password};
  if (cancel.load()) return Failure(AuthError::kCanceled);
  if (!hooks_.monotonic_ms || !hooks_.unix_seconds || !hooks_.wait || !prompt || !code ||
      !AccountNameValid(credentials.account_name) || credentials.password.empty())
    return Failure(AuthError::kProtocol);
  const auto started = hooks_.monotonic_ms();
  auto remaining = [&] { return Remaining(started, hooks_, kCredentialLifetimeMs); };
  // Steam's refusals of the password, the account or the device; the QR code may still work.
  auto refused = [&](AuthError error) {
    if (error != AuthError::kRejected) return error == AuthError::kExpired ? AuthError::kSignInTimedOut : error;
    if (last_eresult_ == 5) return AuthError::kInvalidPassword;
    if (last_eresult_ == 84) return AuthError::kRateLimited;
    return AuthError::kSignInRefused;
  };
  auth::GetPasswordRsaPublicKeyRequest key_request;
  key_request.account_name = credentials.account_name;
  std::vector<std::uint8_t> request, response;
  if (!auth::Encode(key_request, &request).ok()) return Failure(AuthError::kProtocol);
  auto error = Post("GetPasswordRSAPublicKey", request, cancel, remaining(), &response, true);
  if (error != AuthError::kNone) return Failure(refused(error));
  auth::GetPasswordRsaPublicKeyResponse key;
  if (!auth::Decode(response, &key).ok() || !key.timestamp) return Failure(AuthError::kProtocol);
  auth::BeginAuthSessionViaCredentialsRequest begin;
  begin.device_details.device_friendly_name = "Playden (PS5)";
  if (hooks_.machine_id) begin.device_details.machine_id = hooks_.machine_id();
  begin.account_name = credentials.account_name;
  begin.encryption_timestamp = key.timestamp;
  auto encrypted = EncryptPassword(key.publickey_mod, key.publickey_exp, credentials.password);
  ZeroizeSecret(credentials.password);
  if (!encrypted) return Failure(AuthError::kProtocol);
  begin.encrypted_password = std::move(*encrypted);
  if (!auth::Encode(begin, &request).ok()) return Failure(AuthError::kProtocol);
  error = Post("BeginAuthSessionViaCredentials", request, cancel, remaining(), &response);
  if (error != AuthError::kNone) return Failure(refused(error));
  if (!remaining()) return Failure(AuthError::kSignInTimedOut);
  auth::BeginAuthSessionViaCredentialsResponse session;
  if (!auth::Decode(response, &session).ok() || !session.client_id || !session.steam_id ||
      session.request_id.empty() || session.request_id.size() > 1024 ||
      !std::isfinite(session.interval) || session.interval <= 0) return Failure(AuthError::kProtocol);

  // As the Steam client: approval in the app when offered (a typed code works meanwhile), else a code.
  bool approval = false, none = session.allowed_confirmations.empty();
  std::optional<GuardPromptKind> code_kind;
  std::string email_domain;
  for (const auto& confirmation : session.allowed_confirmations) {
    switch (confirmation.confirmation_type) {
      case auth::GuardType::kNone: none = true; break;
      case auth::GuardType::kDeviceConfirmation: approval = true; break;
      case auth::GuardType::kDeviceCode: code_kind = GuardPromptKind::kAuthenticatorCode; break;
      case auth::GuardType::kEmailCode:
        if (!code_kind) code_kind = GuardPromptKind::kEmailCode;
        email_domain = confirmation.associated_message.substr(0, 253);
        break;
      case auth::GuardType::kEmailConfirmation:
        approval = true;
        if (email_domain.empty()) email_domain = confirmation.associated_message.substr(0, 253);
        break;
      default: break;
    }
  }
  if (!none && !approval && !code_kind) return Failure(AuthError::kSignInRefused);
  GuardPrompt shown;
  if (!none) {
    if (approval) {
      shown.kind = GuardPromptKind::kApproval;
      shown.code_allowed = code_kind.has_value();
      if (code_kind) shown.code_kind = *code_kind;
    } else {
      shown.kind = *code_kind;
    }
    if (shown.kind == GuardPromptKind::kEmailCode || (shown.kind == GuardPromptKind::kApproval &&
        (!shown.code_allowed || shown.code_kind == GuardPromptKind::kEmailCode)))
      shown.email_domain = email_domain;
    prompt(shown);
  }
  // A sign-in that waits only for a typed code polls Steam once the code is accepted.
  bool polling = none || approval;
  const auto interval = PollInterval(session.interval);
  while (!cancel.load()) {
    if (!remaining()) return Failure(AuthError::kSignInTimedOut);
    if (code_kind) {
      if (auto typed = code()) {
        auth::UpdateAuthSessionWithSteamGuardCodeRequest update;
        update.client_id = session.client_id;
        update.steam_id = session.steam_id;
        update.code = std::move(*typed);
        update.code_type = *code_kind == GuardPromptKind::kEmailCode ? auth::GuardType::kEmailCode
                                                                      : auth::GuardType::kDeviceCode;
        ZeroizeSecret(*typed);
        const bool usable = !update.code.empty() && update.code.size() <= 16;
        std::vector<std::uint8_t> code_request;
        if (usable && !auth::Encode(update, &code_request).ok()) return Failure(AuthError::kProtocol);
        ZeroizeSecret(update.code);
        error = usable ? Post("UpdateAuthSessionWithSteamGuardCode", code_request, cancel, remaining(), &response)
                       : AuthError::kRejected;
        if (usable) std::fill(code_request.begin(), code_request.end(), std::uint8_t{0});
        // DuplicateRequest (29): Steam already took this code.
        if (error == AuthError::kNone || (error == AuthError::kRejected && last_eresult_ == 29)) {
          polling = true;
        } else if (error == AuthError::kRejected && (!usable || last_eresult_ == 65 || last_eresult_ == 88)) {
          // A wrong code: the same sign-in asks again.
          GuardPrompt again;
          again.kind = *code_kind;
          again.email_domain = *code_kind == GuardPromptKind::kEmailCode ? email_domain : std::string{};
          again.code_rejected = true;
          prompt(again);
          continue;
        } else {
          return Failure(refused(error));
        }
      }
    }
    const auto left = remaining();
    if (!left) return Failure(AuthError::kSignInTimedOut);
    if (!hooks_.wait(std::min(polling ? interval : kCodeCheckMs, left), cancel) || cancel.load())
      return Failure(AuthError::kCanceled);
    if (!polling) continue;
    if (!remaining()) return Failure(AuthError::kSignInTimedOut);
    auth::PollAuthSessionStatusResponse approved;
    error = Poll(&session.client_id, session.request_id, cancel, remaining(), &approved);
    if (error != AuthError::kNone) return Failure(refused(error));
    if (cancel.load()) return Failure(AuthError::kCanceled);
    if (approved.refresh_token.empty()) continue;
    auto result = Approved(std::move(approved), started, kCredentialLifetimeMs, cancel);
    if (result.error == AuthError::kExpired) result.error = AuthError::kSignInTimedOut;
    if (result.error == AuthError::kNone && result.session && result.session->steam_id != session.steam_id)
      return Failure(AuthError::kProtocol);
    return result;
  }
  return Failure(AuthError::kCanceled);
}

AuthResult AuthClient::Renew(const StoredSession& session, const std::atomic<bool>& cancel) {
  // As the Steam client: a SteamClient refresh token goes only to an authenticated CM session. Sent to the
  // Web API it was refused, and every saved sign-in was gone from the first renewal, about a day after it.
  if (!hooks_.cm_renew) return Failure(AuthError::kProtocol);
  return hooks_.cm_renew(session, cancel);
}

AuthResult AuthClient::Restored(const char* path, const StoredSession& before, AuthResult result) {
  if (result.error == AuthError::kNone && result.session && hooks_.restored)
    hooks_.restored(path, TokenFingerprint(before.refresh_token), TokenFingerprint(result.session->refresh_token));
  return result;
}

AuthResult AuthClient::Restore(const StoredSession& session,const std::atomic<bool>& cancel) {
  if (cancel.load()) return Failure(AuthError::kCanceled);
  if (!hooks_.unix_seconds || !AccountNameValid(session.account_name)) return Failure(AuthError::kProtocol);
  const auto now = hooks_.unix_seconds();
  if (!session.steam_id || !ValidToken(session.refresh_token,session.steam_id,now))
    return Failure(AuthError::kCredentialsRevoked);
  // As the reference client: a stored access token with more than a minute left is used as is.
  if (const auto claims = InspectTokenClaims(session.access_token);
      claims && claims->steam_id == session.steam_id && claims->expires_at_unix_seconds > now + 60)
    return Restored("stored", session, {AuthError::kNone, session});
  // Its outcome, with Steam's result code, is in steam-cm.log (owner=auth).
  return Restored("cm", session, Renew(session, cancel));
}
}  // namespace playden::steam
