#include "steam/backend/auth_client.h"
#include "steam/backend/proto_wire.h"
#include "steam/service.h"
#include <chrono>
#include <thread>

#include <cassert>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>

using namespace playden;
using namespace playden::steam;
namespace {
std::string B64(std::string input) {
  const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  unsigned bits = 0, count = 0;
  for (unsigned char c : input) {
    bits = (bits << 8) | c; count += 8;
    while (count >= 6) { count -= 6; out += alphabet[(bits >> count) & 63]; }
  }
  if (count) out += alphabet[(bits << (6 - count)) & 63];
  return out;
}
std::string Token(std::string id = "76561198000000001", unsigned expiry = 2000000000) {
  return B64("{\"alg\":\"RS256\"}") + "." +
         B64("{\"sub\":\"" + id + "\",\"exp\":" + std::to_string(expiry) + "}") + "." + B64("synthetic-signature");
}
HttpResponse Reply(proto::Writer& w, int result = 1) {
  HttpResponse r; r.status_code = 200;
  r.headers.emplace_back("X-EResult", std::to_string(result)); r.body = w.bytes(); return r;
}
HttpResponse Begin(float interval = 1.5F, std::string url = "https://s.team/q/synthetic-one") {
  proto::Writer w; w.AddVarint(1, 123); w.AddString(2, url);
  w.AddString(3, "synthetic-request");
  std::uint32_t bits; std::memcpy(&bits, &interval, sizeof(bits)); w.AddFixed32(4, bits);
  return Reply(w);
}
HttpResponse Success(std::string refresh = Token(), std::string access = Token(), std::string name = "synthetic-account") {
  proto::Writer w; w.AddString(3, refresh); w.AddString(4, access); w.AddString(6, name);
  return Reply(w);
}
struct Fixture {
  std::uint64_t clock = 0;
  std::atomic<bool> cancel{false};
  std::deque<HttpResponse> replies;
  std::vector<HttpRequest> requests;
  std::vector<std::uint32_t> waits;
  std::vector<std::string> challenges;
  bool cancel_on_wait = false;
  std::uint32_t transport_ms = 0;
  bool cancel_on_response = false;
  AuthClientHooks Hooks() {
    AuthClientHooks h;
    h.perform = [this](const HttpRequest& req, const std::atomic<bool>&) {
      requests.push_back(req); assert(!replies.empty()); auto r = replies.front(); replies.pop_front();
      clock += transport_ms; if (cancel_on_response) cancel = true; return r;
    };
    h.monotonic_ms = [this] { return clock; };
    h.unix_seconds = [] { return std::uint64_t{1900000000}; };
    h.wait = [this](std::uint32_t ms, const std::atomic<bool>&) {
      waits.push_back(ms); clock += ms; if (cancel_on_wait) cancel = true; return !cancel;
    };
    return h;
  }
  ChallengeCallback Callback() { return [this](const std::string& s) { challenges.push_back(s); }; }
};
// The access token expires within the minute, so restoration must renew it.
StoredSession Stored() { return {76561198000000001ULL, "synthetic-account", Token(), Token("76561198000000001", 1900000030), 7}; }
void QrSuccessAndReplacement() {
  Fixture f; f.replies.push_back(Begin());
  proto::Writer w; w.AddVarint(1, 456); w.AddString(2, "https://s.team/q/synthetic-two");
  f.replies.push_back(Reply(w)); f.replies.push_back(Success());
  AuthClient client(f.Hooks()); auto r = client.AuthenticateQr(f.cancel, f.Callback());
  assert(r.error == AuthError::kNone && r.session && r.session->steam_id == Stored().steam_id);
  assert(f.challenges.size() == 2 && f.waits == std::vector<std::uint32_t>({1500, 1500}));
  assert(f.requests.size() == 3);
  const auto& req = f.requests[0];
  assert(req.url == "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/");
  assert(req.method == HttpMethod::kPost && req.timeout_ms == 10000 && req.max_response_bytes == 65536);
  assert(req.headers[0].second == "application/x-www-form-urlencoded");
  const std::string body(req.body.begin(), req.body.end());
  assert(body.rfind("input_protobuf_encoded=", 0) == 0 && body.find('+') == std::string::npos);
  // Byte-exact encoded poll requests pin both request bytes and the replaced client ID.
  assert(std::string(f.requests[1].body.begin(), f.requests[1].body.end()) ==
         "input_protobuf_encoded=CHsSEXN5bnRoZXRpYy1yZXF1ZXN0");
  assert(std::string(f.requests[2].body.begin(), f.requests[2].body.end()) ==
         "input_protobuf_encoded=CMgDEhFzeW50aGV0aWMtcmVxdWVzdA%3D%3D");
}
// The form body Playden posts for an encoded message: standard base64, then URL-escaped.
std::string FormBody(const std::vector<std::uint8_t>& bytes) {
  const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  for (std::size_t at = 0; at < bytes.size(); at += 3) {
    const unsigned n = bytes[at] << 16 | (at + 1 < bytes.size() ? bytes[at + 1] << 8 : 0) |
                       (at + 2 < bytes.size() ? bytes[at + 2] : 0);
    b64 += alphabet[n >> 18 & 63]; b64 += alphabet[n >> 12 & 63];
    b64 += at + 1 < bytes.size() ? alphabet[n >> 6 & 63] : '=';
    b64 += at + 2 < bytes.size() ? alphabet[n & 63] : '=';
  }
  std::string body = "input_protobuf_encoded=";
  for (char c : b64) body += c == '+' ? "%2B" : c == '/' ? "%2F" : c == '=' ? "%3D" : std::string(1, c);
  return body;
}
// A sign-in names the CM logon's machine and reports the new token's persistence and lifetime.
void QrNamesTheMachineAndReportsClaims() {
  const std::string refresh = B64("{\"alg\":\"RS256\"}") + "." +
      B64("{\"sub\":\"76561198000000001\",\"exp\":1918144000,\"iat\":1900000000,\"per\":1}") + "." + B64("sig");
  Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(refresh));
  auto hooks = f.Hooks();
  int machine_calls = 0;
  hooks.machine_id = [&machine_calls] { ++machine_calls; return std::vector<std::uint8_t>{0x00, 0xFE, 0x08}; };
  long long persistence = -2; std::uint64_t lifetime = 0;
  hooks.signed_in = [&](long long per, std::uint64_t seconds) { persistence = per; lifetime = seconds; };
  AuthClient client(hooks);
  assert(client.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kNone);
  auth::BeginAuthSessionViaQrRequest expected;
  expected.device_details.device_friendly_name = "Playden (PS5)";
  expected.device_details.machine_id = {0x00, 0xFE, 0x08};
  std::vector<std::uint8_t> encoded;
  assert(auth::Encode(expected, &encoded).ok());
  assert(machine_calls == 1);
  assert(std::string(f.requests[0].body.begin(), f.requests[0].body.end()) == FormBody(encoded));
  assert(persistence == 1 && lifetime == 18144000);
  // A token without the claims reports none, and the lifetime from the sign-in time.
  Fixture g; g.replies.push_back(Begin()); g.replies.push_back(Success());
  auto plain = g.Hooks();
  plain.signed_in = [&](long long per, std::uint64_t seconds) { persistence = per; lifetime = seconds; };
  AuthClient without(plain);
  assert(without.AuthenticateQr(g.cancel, g.Callback()).error == AuthError::kNone);
  assert(persistence == -1 && lifetime == 2000000000ULL - 1900000000ULL);
  // A sign-in that fails after the refresh token arrives (a bad access token) is not reported.
  persistence = -2;
  Fixture h; h.replies.push_back(Begin()); h.replies.push_back(Success(refresh, Token("76561198000000002")));
  auto failing = h.Hooks();
  failing.signed_in = [&](long long per, std::uint64_t) { persistence = per; };
  AuthClient refused(failing);
  assert(refused.AuthenticateQr(h.cancel, h.Callback()).error == AuthError::kProtocol && persistence == -2);
}
void FailuresAndDeadline() {
  {
    Fixture f; f.cancel = true; AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kCanceled && f.requests.empty());
  }
  {
    Fixture f; f.cancel_on_wait = true; f.replies.push_back(Begin()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kCanceled && f.requests.size() == 1);
  }
  for (float interval : {0.0F, -1.0F, INFINITY, NAN}) {
    Fixture f; f.replies.push_back(Begin(interval)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kProtocol && f.challenges.empty());
  }
  {
    Fixture f; f.replies.push_back(Begin(300.0F)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kExpired && f.requests.size() == 1);
  }
  for (int result : {9, 27, 15}) {
    Fixture f; f.replies.push_back(Begin()); proto::Writer w; f.replies.push_back(Reply(w,result)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == (result == 15 ? AuthError::kRejected : AuthError::kExpired));
  }
  {
    Fixture f; auto r = Begin(); r.error = HttpError::kTimeout; f.replies.push_back(r); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kNetwork && f.challenges.empty());
  }
  for (const std::string url : {"http://s.team/q/bad", "https://s.team.evil.invalid/q/bad", "https://s.team/q/bad\n"}) {
    Fixture f; f.replies.push_back(Begin(1.0F,url)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kProtocol && f.challenges.empty());
  }
}
void HeadersAndTokens() {
  for (int bad = 0; bad != 4; ++bad) {
    Fixture f; auto r = Begin();
    if (bad == 0) r.headers.emplace_back("x-eresult", "1");
    if (bad == 1) r.headers[0].second = "garbage";
    if (bad == 2) r.body = {0xff};
    if (bad == 3) r.body.resize(65537);
    f.replies.push_back(r); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kProtocol && f.challenges.empty());
  }
  for (const std::string& token : {std::string("synthetic.not-a-token.invalid"), Token("76561198000000002"), Token("76561198000000001", 1800000000)}) {
    Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(Token(), token)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kProtocol);
  }
  {
    // An approval without an access token gets one through the CM, not the Web API.
    Fixture f; auto begin = Begin(); begin.headers.clear(); f.replies.push_back(begin); f.replies.push_back(Success(Token(), ""));
    auto hooks = f.Hooks(); int cm_calls = 0;
    hooks.cm_renew = [&](const StoredSession& s, const std::atomic<bool>&) {
      ++cm_calls; auto renewed = s; renewed.access_token = Token(); return AuthResult{AuthError::kNone, renewed};
    };
    AuthClient c(hooks); auto r = c.AuthenticateQr(f.cancel, f.Callback());
    assert(r.error == AuthError::kNone && !r.session->access_token.empty() && cm_calls == 1 && f.requests.size() == 2);
  }
}
void FailedReplacementKeepsSavedSession() {
  class Repository : public SessionRepository {
   public:
    mutable std::mutex mutex;
    mutable std::optional<StoredSession> saved = Stored();
    mutable int clears = 0;
    Result<std::optional<StoredSession>> Load() const override {
      std::lock_guard<std::mutex> lock(mutex); return Result<std::optional<StoredSession>>::Success(saved);
    }
    Result<void> Save(const StoredSession& s) const override {
      std::lock_guard<std::mutex> lock(mutex); saved = s; return Result<void>::Success();
    }
    Result<void> Clear() const override {
      std::lock_guard<std::mutex> lock(mutex); ++clears; saved.reset(); return Result<void>::Success();
    }
  } repository;
  Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(Token("76561198000000002"),""));
  auto hooks = f.Hooks();
  // What RenewThroughCm reports when the CM refuses the logon.
  hooks.cm_renew = [](const StoredSession&, const std::atomic<bool>&) {
    return AuthResult{AuthError::kCredentialsRevoked, std::nullopt};
  };
  AuthClient client(hooks); SteamService service(client,repository); service.BeginQrSignIn();
  bool failed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!failed && std::chrono::steady_clock::now() < deadline) {
    for (const auto& event : service.Pump(64)) if (event.kind == SteamEventKind::kFailure) {
      assert(event.error == AuthError::kRejected); failed = true;
    }
    if (!failed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(failed);
  const auto saved = repository.Load();
  assert(saved.ok() && saved.value() && saved.value()->steam_id == Stored().steam_id);
  std::lock_guard<std::mutex> lock(repository.mutex); assert(repository.clears == 0);
}
void AccountNames() {
  for (const std::string& name : {std::string(), std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
                                 std::string("\xf4\x90\x80\x80"), std::string("\xe2\x82"), std::string(257,'a')}) {
    Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(Token(),Token(),name)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kProtocol);
  }
  Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(Token(),Token(),"Synthetic \xc3\xa9")); AuthClient c(f.Hooks());
  assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kNone);
}
void DeadlineAndCancellationEdges() {
  {
    Fixture f; f.replies.push_back(Begin()); f.cancel_on_response = true; AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kCanceled && f.challenges.empty());
  }
  {
    Fixture f; f.replies.push_back(Begin()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,[&](const std::string&) { f.cancel = true; }).error == AuthError::kCanceled);
    assert(f.requests.size() == 1 && f.waits.empty());
  }
  {
    Fixture f; f.replies.push_back(Begin(299.0F)); f.replies.push_back(Success()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kNone);
    assert(f.requests[1].timeout_ms == 1000); // Never give a request more than the session's remaining lifetime.
  }
  {
    Fixture f; f.transport_ms = 1000; f.replies.push_back(Begin(298.0F)); f.replies.push_back(Success()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kExpired);
  }
  {
    Fixture f; f.replies.push_back(Begin(0.1F)); f.replies.push_back(Success()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kNone && f.waits[0] == 1000);
  }
  {
    Fixture f; f.replies.push_back(Begin()); proto::Writer bad;
    bad.AddVarint(1,0); f.replies.push_back(Reply(bad)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kProtocol);
  }
  {
    Fixture f; f.replies.push_back(Begin()); proto::Writer bad;
    bad.AddString(2,"https://evil.invalid/"); f.replies.push_back(Reply(bad)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kProtocol && f.challenges.size() == 1);
  }
  {
    Fixture f; f.replies.push_back(Begin()); proto::Writer rejected; f.replies.push_back(Reply(rejected,15));
    f.replies.push_back(Begin()); f.replies.push_back(Success()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kRejected);
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kNone);
    assert(f.requests[2].url.find("BeginAuthSessionViaQR") != std::string::npos);
  }
}
void Restoration() {
  {
    // As the reference client, an access token with more than a minute left is reused offline.
    Fixture f; auto saved = Stored(); saved.access_token = Token(); AuthClient c(f.Hooks());
    auto r = c.Restore(saved,f.cancel);
    assert(r.error == AuthError::kNone && r.session->access_token == saved.access_token && f.requests.empty());
  }
  {
    // An expired access token is renewed through the CM only: the refresh token never goes to the Web API.
    Fixture f; int cm_calls = 0; const auto rotation = Token("76561198000000001",2100000000);
    auto hooks = f.Hooks();
    hooks.cm_renew = [&](const StoredSession& s, const std::atomic<bool>&) {
      ++cm_calls; auto renewed = s; renewed.access_token = Token(); renewed.refresh_token = rotation;
      return AuthResult{AuthError::kNone, renewed};
    };
    std::string path;
    hooks.restored = [&](const char* p, const std::string&, const std::string&) { path = p; };
    AuthClient c(hooks); auto r = c.Restore(Stored(),f.cancel);
    assert(r.error == AuthError::kNone && cm_calls == 1 && r.session->refresh_token == rotation);
    assert(r.session->cell_id == 7 && path == "cm" && f.requests.empty());
  }
  for (AuthError error : {AuthError::kCredentialsRevoked, AuthError::kNetwork, AuthError::kCanceled}) {
    Fixture f; auto hooks = f.Hooks();
    hooks.cm_renew = [&](const StoredSession&, const std::atomic<bool>&) { return AuthResult{error, std::nullopt}; };
    AuthClient c(hooks);
    assert(c.Restore(Stored(),f.cancel).error == error && f.requests.empty());
  }
  {
    // Without a CM there is nothing to renew through.
    Fixture f; AuthClient c(f.Hooks());
    assert(c.Restore(Stored(),f.cancel).error == AuthError::kProtocol && f.requests.empty());
  }
  {
    Fixture f; auto saved = Stored(); saved.steam_id++; AuthClient c(f.Hooks());
    assert(c.Restore(saved,f.cancel).error == AuthError::kCredentialsRevoked && f.requests.empty());
  }
}
}
// Password sign-in. The modulus is synthetic: any odd 2048-bit value encrypts.
const std::string kModulus = "c" + std::string(510, '3') + "5";
HttpResponse Key(std::uint64_t timestamp = 1700000000000000ULL) {
  proto::Writer w; w.AddString(1, kModulus); w.AddString(2, "010001"); w.AddVarint(3, timestamp);
  return Reply(w);
}
HttpResponse BeginCredentials(std::vector<std::pair<int, std::string>> confirmations, float interval = 0.1F) {
  proto::Writer w; w.AddVarint(1, 123); w.AddString(2, "synthetic-request");
  std::uint32_t bits; std::memcpy(&bits, &interval, sizeof(bits)); w.AddFixed32(3, bits);
  for (const auto& [type, message] : confirmations) {
    proto::Writer c; c.AddVarint(1, static_cast<std::uint64_t>(type)); if (!message.empty()) c.AddString(2, message);
    w.AddMessage(4, c);
  }
  w.AddVarint(5, 76561198000000001ULL);
  return Reply(w);
}
HttpResponse Empty(int result = 1) { proto::Writer w; return Reply(w, result); }
// The protobuf bytes a request carried, from its query string (GET) or form body (POST).
std::string Sent(const HttpRequest& request) {
  std::string form = request.method == HttpMethod::kGet ? request.url.substr(request.url.find('?') + 1)
                                                        : std::string(request.body.begin(), request.body.end());
  form = form.substr(std::strlen("input_protobuf_encoded="));
  std::string b64;
  for (std::size_t i = 0; i < form.size(); ++i) {
    if (form[i] == '%') { const auto hex = form.substr(i + 1, 2); b64 += hex == "2B" ? '+' : hex == "2F" ? '/' : '='; i += 2; }
    else b64 += form[i];
  }
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out; unsigned bits = 0, count = 0;
  for (char c : b64) {
    if (c == '=') break;
    bits = (bits << 6) | static_cast<unsigned>(alphabet.find(c)); count += 6;
    if (count >= 8) { count -= 8; out += static_cast<char>((bits >> count) & 0xff); }
  }
  return out;
}
struct Guard {
  std::vector<GuardPrompt> prompts;
  std::deque<std::optional<std::string>> codes;
  GuardPromptCallback Prompt() { return [this](const GuardPrompt& p) { prompts.push_back(p); }; }
  GuardCodeSource Code() {
    return [this]() -> std::optional<std::string> {
      if (codes.empty()) return std::nullopt;
      auto c = codes.front(); codes.pop_front(); return c;
    };
  }
};
AuthResult SignIn(Fixture& f, Guard& g, std::string* password_after = nullptr,
                  std::vector<std::uint8_t> machine_id = {}) {
  auto hooks = f.Hooks();
  if (!machine_id.empty()) hooks.machine_id = [machine_id] { return machine_id; };
  AuthClient c(hooks);
  playden::steam::Credentials credentials{"synthetic_account", "hunter2-synthetic"};
  auto r = c.AuthenticateCredentials(credentials, f.cancel, g.Prompt(), g.Code());
  if (password_after) *password_after = credentials.password;
  return r;
}
constexpr int kDeviceConfirmation = 4, kEmailCode = 2, kDeviceCode = 3;
void CredentialsWithoutGuard() {
  Fixture f; Guard g; std::string left = "?";
  f.replies = {Key(), BeginCredentials({}), Success()};
  auto r = SignIn(f, g, &left);
  assert(r.error == AuthError::kNone && r.session && r.session->steam_id == 76561198000000001ULL);
  assert(left.empty() && g.prompts.empty() && f.requests.size() == 3);
  const auto& key = f.requests[0];
  assert(key.method == HttpMethod::kGet && key.body.empty());
  assert(key.url.rfind("https://api.steampowered.com/IAuthenticationService/GetPasswordRSAPublicKey/v1/?input_protobuf_encoded=", 0) == 0);
  assert(Sent(key).find("synthetic_account") != std::string::npos);
  const auto& begin = f.requests[1];
  assert(begin.method == HttpMethod::kPost &&
         begin.url == "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaCredentials/v1/");
  const auto sent = Sent(begin);
  assert(sent.find("synthetic_account") != std::string::npos && sent.find("Playden (PS5)") != std::string::npos);
  // The password sign-in names the CM logon's machine too: device details field 6.
  Fixture named; Guard named_guard;
  named.replies = {Key(), BeginCredentials({}), Success()};
  assert(SignIn(named, named_guard, nullptr, {0x00, 'M', 'I', 'D', 0x08}).error == AuthError::kNone);
  assert(Sent(named.requests[1]).find(std::string("\x32\x05\x00MID\x08", 7)) != std::string::npos);
  // Only the encrypted password goes out.
  for (const auto& request : f.requests) {
    assert(Sent(request).find("hunter2-synthetic") == std::string::npos);
    assert(request.url.find("hunter2") == std::string::npos);
  }
  assert(f.waits == std::vector<std::uint32_t>({1000}));
}
void CredentialsEmailCode() {
  Fixture f; Guard g;
  g.codes = {std::nullopt, std::string("WRONG"), std::nullopt, std::string("AB12C")};
  f.replies = {Key(), BeginCredentials({{kEmailCode, "example.invalid"}}), Empty(65), Empty(), Success()};
  auto r = SignIn(f, g);
  assert(r.error == AuthError::kNone && r.session);
  assert(g.prompts.size() == 2);
  assert(g.prompts[0].kind == GuardPromptKind::kEmailCode && g.prompts[0].email_domain == "example.invalid" &&
         !g.prompts[0].code_rejected);
  assert(g.prompts[1].kind == GuardPromptKind::kEmailCode && g.prompts[1].code_rejected &&
         g.prompts[1].email_domain == "example.invalid");
  // No polling while it waits for a code: one check, then the wrong code, then a check, the right code and a poll.
  assert(f.waits == std::vector<std::uint32_t>({250, 250, 1000}));
  assert(f.requests.size() == 5);
  assert(f.requests[2].url.find("UpdateAuthSessionWithSteamGuardCode") != std::string::npos);
  const auto update = Sent(f.requests[3]);
  assert(update.find("AB12C") != std::string::npos && update.back() == kEmailCode);
}
void CredentialsAuthenticatorCode() {
  Fixture f; Guard g; g.codes = {std::string("XY34Z")};
  f.replies = {Key(), BeginCredentials({{kDeviceCode, ""}}), Empty(), Success()};
  auto r = SignIn(f, g);
  assert(r.error == AuthError::kNone);
  assert(g.prompts.size() == 1 && g.prompts[0].kind == GuardPromptKind::kAuthenticatorCode &&
         g.prompts[0].email_domain.empty());
  const auto update = Sent(f.requests[2]);
  assert(update.find("XY34Z") != std::string::npos && update.back() == kDeviceCode);
  // A code Steam already took (DuplicateRequest) is accepted as well.
  Fixture d; Guard dg; dg.codes = {std::string("XY34Z")};
  d.replies = {Key(), BeginCredentials({{kDeviceCode, ""}}), Empty(29), Success()};
  assert(SignIn(d, dg).error == AuthError::kNone);
}
void CredentialsApproval() {
  {
    Fixture f; Guard g;
    f.replies = {Key(), BeginCredentials({{kDeviceConfirmation, ""}, {kDeviceCode, ""}}), Empty(), Success()};
    auto r = SignIn(f, g);
    assert(r.error == AuthError::kNone);
    assert(g.prompts.size() == 1 && g.prompts[0].kind == GuardPromptKind::kApproval && g.prompts[0].code_allowed &&
           g.prompts[0].code_kind == GuardPromptKind::kAuthenticatorCode);
    assert(f.waits == std::vector<std::uint32_t>({1000, 1000}) && f.requests.size() == 4);
  }
  {
    // Approval offered, a code typed instead after one poll.
    Fixture f; Guard g; g.codes = {std::nullopt, std::string("XY34Z")};
    f.replies = {Key(), BeginCredentials({{kDeviceConfirmation, ""}, {kDeviceCode, ""}}), Empty(), Empty(), Success()};
    auto r = SignIn(f, g);
    assert(r.error == AuthError::kNone && f.requests.size() == 5);
    assert(f.requests[3].url.find("UpdateAuthSessionWithSteamGuardCode") != std::string::npos);
  }
  {
    // Approval only, without a code: email domain shown for an email confirmation.
    Fixture f; Guard g; g.codes = {std::string("IGNORED")};
    f.replies = {Key(), BeginCredentials({{5, "example.invalid"}}), Success()};
    assert(SignIn(f, g).error == AuthError::kNone && f.requests.size() == 3);
    assert(g.prompts.size() == 1 && g.prompts[0].kind == GuardPromptKind::kApproval && !g.prompts[0].code_allowed &&
           g.prompts[0].email_domain == "example.invalid");
  }
}
void CredentialsFailures() {
  for (auto [result, expected] : {std::pair{5, AuthError::kInvalidPassword}, std::pair{84, AuthError::kRateLimited},
                                  std::pair{63, AuthError::kSignInRefused}, std::pair{27, AuthError::kSignInTimedOut}}) {
    Fixture f; Guard g; std::string left = "?";
    f.replies = {Key(), Empty(result)};
    assert(SignIn(f, g, &left).error == expected && left.empty() && g.prompts.empty());
    Fixture k; Guard kg; k.replies = {Empty(result)};
    assert(SignIn(k, kg, &left).error == expected && left.empty());
  }
  {
    Fixture f; Guard g; std::string left = "?";
    auto bad = Key(); bad.body = {0xff}; f.replies = {bad};
    assert(SignIn(f, g, &left).error == AuthError::kProtocol && left.empty());
    proto::Writer w; w.AddString(1, "zz"); w.AddString(2, "010001"); w.AddVarint(3, 1);
    Fixture h; h.replies = {Reply(w)}; assert(SignIn(h, g, &left).error == AuthError::kProtocol && h.requests.size() == 1);
  }
  {
    // A guard type Playden cannot answer (machine token) refuses the sign-in.
    Fixture f; Guard g; f.replies = {Key(), BeginCredentials({{6, ""}})};
    assert(SignIn(f, g).error == AuthError::kSignInRefused && g.prompts.empty());
  }
  {
    // A wrong code that Steam refuses differently ends the sign-in.
    Fixture f; Guard g; g.codes = {std::string("AB12C")};
    f.replies = {Key(), BeginCredentials({{kEmailCode, "example.invalid"}}), Empty(84)};
    assert(SignIn(f, g).error == AuthError::kRateLimited);
  }
  {
    // Polls that Steam stops (session expired) time out.
    Fixture f; Guard g; f.replies = {Key(), BeginCredentials({{kDeviceConfirmation, ""}}), Empty(9)};
    assert(SignIn(f, g).error == AuthError::kSignInTimedOut);
  }
  {
    Fixture f; Guard g; std::string left = "?";
    AuthClient c(f.Hooks());
    playden::steam::Credentials empty{"synthetic_account", ""};
    assert(c.AuthenticateCredentials(empty, f.cancel, g.Prompt(), g.Code()).error == AuthError::kProtocol);
    playden::steam::Credentials nameless{"", "secret"};
    assert(c.AuthenticateCredentials(nameless, f.cancel, g.Prompt(), g.Code()).error == AuthError::kProtocol &&
           nameless.password.empty() && f.requests.empty());
  }
}
void CredentialsCancelAndDeadline() {
  {
    Fixture f; Guard g; std::string left = "?"; f.cancel = true;
    assert(SignIn(f, g, &left).error == AuthError::kCanceled && f.requests.empty() && left.empty());
  }
  {
    Fixture f; Guard g; f.cancel_on_response = true; f.replies = {Key()};
    assert(SignIn(f, g).error == AuthError::kCanceled && f.requests.size() == 1);
  }
  for (int guard : {kEmailCode, kDeviceConfirmation}) {
    // Waiting for a code, or polling for approval.
    Fixture f; Guard g; f.cancel_on_wait = true; f.replies = {Key(), BeginCredentials({{guard, ""}})};
    assert(SignIn(f, g).error == AuthError::kCanceled && f.requests.size() == 2 && g.prompts.size() == 1);
  }
  {
    // A code never arrives: the sign-in times out after ten minutes without polling Steam.
    Fixture f; Guard g; f.replies = {Key(), BeginCredentials({{kEmailCode, "example.invalid"}})};
    assert(SignIn(f, g).error == AuthError::kSignInTimedOut && f.requests.size() == 2 && f.clock >= 600000);
  }
  {
    // Approval never arrives.
    Fixture f; Guard g; f.replies = {Key(), BeginCredentials({{kDeviceConfirmation, ""}})};
    for (int i = 0; i < 700; ++i) f.replies.push_back(Empty());
    assert(SignIn(f, g).error == AuthError::kSignInTimedOut && f.clock >= 600000 && f.requests.size() < 700);
  }
  {
    // The approved account must be the one that signed in.
    Fixture f; Guard g; f.replies = {Key(), BeginCredentials({}), Success(Token("76561198000000002"), Token("76561198000000002"))};
    assert(SignIn(f, g).error == AuthError::kProtocol);
  }
}

int main() {
  CredentialsWithoutGuard(); CredentialsEmailCode(); CredentialsAuthenticatorCode(); CredentialsApproval();
  CredentialsFailures(); CredentialsCancelAndDeadline();
  QrSuccessAndReplacement(); QrNamesTheMachineAndReportsClaims(); FailuresAndDeadline(); HeadersAndTokens(); DeadlineAndCancellationEdges(); Restoration(); AccountNames(); FailedReplacementKeepsSavedSession();
  std::cout << "auth_client_test passed\n";
}
