// SPDX-License-Identifier: GPL-3.0-or-later
// GOG sign-in: the login address, the pasted code, the token requests and their answers through a stub transport.
#include "gog/auth.h"
#include "gog/client_config.h"
#include "gog/http.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace playden;
using playden::gog::TokenResult;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

std::string g_fixtures;

std::string Fixture(const std::string& name) {
  const auto path = g_fixtures + "/" + name;
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file != nullptr);
  std::string text;
  char chunk[4096];
  for (std::size_t count; (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) text.append(chunk, count);
  std::fclose(file);
  return text;
}

const std::string kCode = "SyntheticCode0123456789-abc_DEF.ghi~jkl";
const std::string kSuccess = "https://embed.gog.com/on_login_success";

bool Refused(const std::string& paste) { return !gog::CodeFromPaste(paste); }
bool Gives(const std::string& paste, const std::string& code) {
  const auto got = gog::CodeFromPaste(paste);
  return got && *got == code;
}

// One answer for every request; what each request looked like, copied during the call (the URL is wiped after).
struct Stub {
  steam::HttpResponse answer;
  std::vector<steam::HttpRequest> seen;
  gog::Transport Transport() {
    return [this](const steam::HttpRequest& request, const std::atomic<bool>&) {
      seen.push_back(request);
      return answer;
    };
  }
};

steam::HttpResponse Answer(long status, const std::string& body) {
  steam::HttpResponse response;
  response.status_code = status;
  response.body.assign(body.begin(), body.end());
  return response;
}

steam::HttpResponse Failure(steam::HttpError error) {
  steam::HttpResponse response;
  response.error = error;
  return response;
}

TokenResult Exchange(Stub& stub) {
  std::atomic<bool> cancel{false};
  auto transport = stub.Transport();
  return gog::Exchange(transport, kCode, cancel);
}

TokenResult Refresh(Stub& stub) {
  std::atomic<bool> cancel{false};
  auto transport = stub.Transport();
  return gog::Refresh(transport, "synthetic-refresh-0000000000000001", cancel);
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void CheckOneTokenGet(const Stub& stub) {
  CHECK(stub.seen.size() == 1);
  const auto& request = stub.seen[0];
  CHECK(request.method == steam::HttpMethod::kGet);
  CHECK(request.url.rfind("https://auth.gog.com/token?", 0) == 0);
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_fixtures = argv[1];

  // The login address, exactly.
  CHECK(gog::AuthorizeUrl() ==
        "https://auth.gog.com/auth?client_id=46899977096215655"
        "&redirect_uri=https%3A%2F%2Fembed.gog.com%2Fon_login_success%3Forigin%3Dclient"
        "&response_type=code&layout=galaxy");

  // The address GOG opens after sign-in: with and without origin=client, the code first or last.
  CHECK(Gives(kSuccess + "?origin=client&code=" + kCode, kCode));
  CHECK(Gives(kSuccess + "?code=" + kCode, kCode));
  CHECK(Gives(kSuccess + "?code=" + kCode + "&origin=client", kCode));
  CHECK(Gives(kSuccess + "?origin=client&code=" + kCode + "&x=1#done", kCode));
  // A bare code, 16 to 1024 characters.
  CHECK(Gives(kCode, kCode));
  CHECK(Gives(std::string(16, 'a'), std::string(16, 'a')));
  CHECK(Gives(std::string(1024, 'Z'), std::string(1024, 'Z')));
  CHECK(Refused(std::string(15, 'a')) && Refused(std::string(1025, 'a')));
  // Blanks and a newline around it are trimmed.
  CHECK(Gives("  \t" + kSuccess + "?origin=client&code=" + kCode + "\r\n", kCode));
  CHECK(Gives("\n" + kCode + " \n", kCode));
  // Refused: another scheme, host, a host with a suffix, userinfo, a port, another path.
  CHECK(Refused("http://embed.gog.com/on_login_success?origin=client&code=" + kCode));
  CHECK(Refused("https://login.gog.com/on_login_success?origin=client&code=" + kCode));
  CHECK(Refused("https://embed.gog.com.example/on_login_success?code=" + kCode));
  CHECK(Refused("https://user@embed.gog.com/on_login_success?code=" + kCode));
  CHECK(Refused("https://evil.example@embed.gog.com/on_login_success?code=" + kCode));
  CHECK(Refused("https://embed.gog.com:443/on_login_success?code=" + kCode));
  CHECK(Refused("https://EMBED.gog.com/on_login_success?code=" + kCode));
  CHECK(Refused("https://embed.gog.com/on_login_successful?code=" + kCode));
  CHECK(Refused("https://embed.gog.com/on_login_success/x?code=" + kCode));
  CHECK(Refused("https://embed.gog.com/other?code=" + kCode));
  // A code only in the fragment, an empty code, no code.
  CHECK(Refused(kSuccess + "?origin=client#code=" + kCode));
  CHECK(Refused(kSuccess + "#?code=" + kCode));
  CHECK(Refused(kSuccess + "?origin=client&code="));
  CHECK(Refused(kSuccess + "?origin=client&code=&x=1"));
  CHECK(Refused(kSuccess + "?origin=client"));
  CHECK(Refused(kSuccess));
  CHECK(Refused(kSuccess + "?xcode=" + kCode));
  CHECK(Refused("") && Refused("   \n"));
  // Too long, a control character, a NUL.
  CHECK(Refused(kSuccess + "?origin=client&x=" + std::string(4096, 'a') + "&code=" + kCode));
  CHECK(Refused(std::string(4097, 'a')));
  CHECK(Gives("  " + std::string(1024, 'a') + std::string(3000, ' '), std::string(1024, 'a')));
  CHECK(Refused(kSuccess + "?code=" + kCode + "\x01"));
  CHECK(Refused(kSuccess + "?code=abc\ndef" + kCode));
  CHECK(Refused(kCode + "\x7f"));
  CHECK(Refused(kSuccess + "?code=" + kCode + std::string(1, '\0') + "x"));
  // Characters outside the code's set, before or after decoding; a bad escape.
  CHECK(Refused(kSuccess + "?code=abc%20def"));
  CHECK(Refused(kSuccess + "?code=abc+def"));
  CHECK(Refused(kSuccess + "?code=abc%2"));
  CHECK(Refused(kSuccess + "?code=abc%zzdef"));
  CHECK(Refused(kSuccess + "?code=" + std::string(1025, 'a')));
  CHECK(Refused("SyntheticCode0123456789/abc"));
  CHECK(Refused("SyntheticCode0123456789%2Dabc"));  // a bare code is never decoded
  // Percent-decoding happens once, with either case of hex.
  CHECK(Gives(kSuccess + "?code=abc%2Ddef", "abc-def"));
  CHECK(Gives(kSuccess + "?code=abc%2ddef", "abc-def"));
  CHECK(Gives(kSuccess + "?code=abc%7Edef%5f", "abc~def_"));
  CHECK(Refused(kSuccess + "?code=abc%252Ddef"));  // once: abc%2Ddef ('%' is not a code character)
  CHECK(Refused(kSuccess + "?code=abc%26def"));    // abc&def: split on '&' first, then decoded
  CHECK(Refused(kSuccess + "?code=abc%2526def"));
  // Two codes: the first non-empty one.
  CHECK(Gives(kSuccess + "?code=first&code=second", "first"));
  CHECK(Gives(kSuccess + "?code=&origin=client&code=second", "second"));

  // The token requests: every field, encoded; the timeouts and the size cap.
  {
    const auto code = gog::TokenRequest(gog::Grant::kAuthorizationCode, "a b/c&d");
    CHECK(code.method == steam::HttpMethod::kGet);
    CHECK(code.url ==
          std::string("https://auth.gog.com/token?client_id=46899977096215655&client_secret=") + gog::kClientSecret +
              "&grant_type=authorization_code"
              "&redirect_uri=https%3A%2F%2Fembed.gog.com%2Fon_login_success%3Forigin%3Dclient"
              "&code=a%20b%2Fc%26d");
    CHECK(code.timeout_ms == 20'000 && code.connect_timeout_ms == 10'000 && code.max_response_bytes == 64 * 1024);
    CHECK(code.body.empty() && !code.allow_plain_http);
    CHECK(code.headers.size() == 1 && code.headers[0].first == "User-Agent" && code.headers[0].second == "Playden");

    const auto refresh = gog::TokenRequest(gog::Grant::kRefreshToken, "synthetic+refresh=1");
    CHECK(refresh.url ==
          std::string("https://auth.gog.com/token?client_id=46899977096215655&client_secret=") + gog::kClientSecret +
              "&grant_type=refresh_token&refresh_token=synthetic%2Brefresh%3D1");
    CHECK(!Has(refresh.url, "redirect_uri") && !Has(refresh.url, "&code="));
    CHECK(refresh.timeout_ms == 20'000 && refresh.connect_timeout_ms == 10'000 &&
          refresh.max_response_bytes == 64 * 1024);
    CHECK(refresh.headers.size() == 1 && refresh.headers[0].second == "Playden");
  }

  // A code exchanged: the tokens, the user ID, expires_in.
  {
    Stub stub;
    stub.answer = Answer(200, Fixture("token-success.json"));
    auto result = Exchange(stub);
    CHECK(result.kind == TokenResult::Kind::kOk && result.status == 200);
    CHECK(result.tokens.access_token == "synthetic-access-0000000000000001");
    CHECK(result.tokens.refresh_token == "synthetic-refresh-0000000000000001");
    CHECK(result.tokens.user_id == "1000000000000000001");
    CHECK(result.tokens.expires_in == 3600);
    CheckOneTokenGet(stub);
    CHECK(Has(stub.seen[0].url, "grant_type=authorization_code") && Has(stub.seen[0].url, "&code=" + kCode));
    // Wipe zeroes every token.
    std::string& access = result.tokens.access_token;
    const char* bytes = access.data();
    const auto size = access.size();
    result.tokens.Wipe();
    CHECK(result.tokens.access_token.empty() && result.tokens.refresh_token.empty() && result.tokens.user_id.empty());
    for (std::size_t i = 0; i < size; ++i) CHECK(bytes[i] == 0);
  }
  // No expires_in: an hour.
  {
    Stub stub;
    stub.answer = Answer(200, Fixture("token-success-no-expiry.json"));
    const auto result = Exchange(stub);
    CHECK(result.kind == TokenResult::Kind::kOk && result.tokens.expires_in == 3600);
  }
  // expires_in clamped to 60..86400.
  for (const auto& [value, expected] : std::vector<std::pair<std::string, std::uint32_t>>{
           {"0", 60}, {"59", 60}, {"60", 60}, {"7200", 7200}, {"86400", 86400}, {"86401", 86400},
           {"99999999999999999999999", 86400}}) {
    Stub stub;
    stub.answer = Answer(200, "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"1\",\"expires_in\":" +
                                  value + "}");
    const auto result = Exchange(stub);
    CHECK(result.kind == TokenResult::Kind::kOk && result.tokens.expires_in == expected);
  }
  // A refresh: the refresh grant, and the new refresh token when GOG rotates it.
  {
    Stub stub;
    stub.answer = Answer(200, Fixture("token-success-rotated.json"));
    const auto result = Refresh(stub);
    CHECK(result.kind == TokenResult::Kind::kOk);
    CHECK(result.tokens.refresh_token == "synthetic-refresh-0000000000000002");
    CHECK(result.tokens.access_token == "synthetic-access-0000000000000002");
    CheckOneTokenGet(stub);
    CHECK(Has(stub.seen[0].url, "grant_type=refresh_token&refresh_token=synthetic-refresh-0000000000000001"));
  }

  // Rejected: 400 invalid_grant for a code and for a refresh token, and any 401.
  {
    Stub stub;
    stub.answer = Answer(400, Fixture("token-code-invalid-grant.json"));
    const auto result = Exchange(stub);
    CHECK(result.kind == TokenResult::Kind::kRejected && result.status == 400);
    CHECK(result.tokens.access_token.empty() && result.tokens.refresh_token.empty());
    CheckOneTokenGet(stub);
  }
  {
    Stub stub;
    stub.answer = Answer(400, Fixture("token-refresh-invalid-grant.json"));
    const auto result = Refresh(stub);
    CHECK(result.kind == TokenResult::Kind::kRejected && result.status == 400);
    CheckOneTokenGet(stub);
  }
  for (const std::string body : {"", "{\"error\":\"invalid_token\"}", "not json"}) {
    Stub stub;
    stub.answer = Answer(401, body);
    CHECK(Refresh(stub).kind == TokenResult::Kind::kRejected);
  }

  // Try again later: no answer, a server error, too many requests, another 4xx.
  for (const auto error : {steam::HttpError::kTimeout, steam::HttpError::kTlsVerificationFailed,
                           steam::HttpError::kResponseTooLarge, steam::HttpError::kNameResolutionFailed,
                           steam::HttpError::kConnectionFailed, steam::HttpError::kCanceled}) {
    Stub stub;
    stub.answer = Failure(error);
    const auto result = Refresh(stub);
    CHECK(result.kind == TokenResult::Kind::kNetwork && result.status == 0);
    CheckOneTokenGet(stub);
  }
  for (const auto& [status, body] : std::vector<std::pair<long, std::string>>{
           {500, ""}, {502, "<html>"}, {503, Fixture("token-success.json")}, {429, ""},
           {403, "{\"error\":\"access_denied\"}"}, {404, ""}, {400, "{\"error\":\"invalid_request\"}"},
           {400, "not json"}, {400, "{\"error\":\"invalid_grant\""}}) {
    Stub stub;
    stub.answer = Answer(status, body);
    const auto result = Exchange(stub);
    CHECK(result.kind == TokenResult::Kind::kNetwork && result.status == status);
    CHECK(result.tokens.access_token.empty());
  }

  // A 200 that isn't a token answer, and statuses that aren't one either.
  for (const std::string body : {
           std::string("{\"access_token\":\"a\",\"user_id\":\"1\"}"),                         // no refresh_token
           std::string("{\"refresh_token\":\"r\",\"user_id\":\"1\"}"),                         // no access_token
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\"}"),                    // no user_id
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"0123\"}"),
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"0\"}"),
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"12a\"}"),
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"123456789012345678901\"}"),
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"-1\"}"),
           std::string("{\"access_token\":\"\",\"refresh_token\":\"r\",\"user_id\":\"1\"}"),
           std::string("{\"access_token\":1,\"refresh_token\":\"r\",\"user_id\":\"1\"}"),
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"1\",\"expires_in\":\"3600\"}"),
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"1\",\"expires_in\":-5}"),
           std::string("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"1\",\"expires_in\":3.5}"),
           std::string("[]"), std::string("not json"), std::string(""),
       }) {
    Stub stub;
    stub.answer = Answer(200, body);
    const auto result = Exchange(stub);
    CHECK(result.kind == TokenResult::Kind::kProtocol && result.status == 200);
    CHECK(result.tokens.access_token.empty() && result.tokens.refresh_token.empty() && result.tokens.user_id.empty());
  }
  for (const long status : {204L, 302L}) {
    Stub stub;
    stub.answer = Answer(status, Fixture("token-success.json"));
    CHECK(Exchange(stub).kind == TokenResult::Kind::kProtocol);
  }
  // A user ID as a number, and the longest one, are read.
  {
    Stub stub;
    stub.answer = Answer(200, "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":48000000000000001}");
    const auto result = Exchange(stub);
    CHECK(result.kind == TokenResult::Kind::kOk && result.tokens.user_id == "48000000000000001");
    stub.seen.clear();
    stub.answer = Answer(200, "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"user_id\":\"12345678901234567890\"}");
    CHECK(Exchange(stub).tokens.user_id == "12345678901234567890");
  }

  std::puts("gog_auth: ok");
  return 0;
}
