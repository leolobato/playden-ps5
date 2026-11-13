#include "diagnostics/redaction.h"
#include "platform/secret.h"
#include "steam/secret.h"
#include <cassert>
#include <cstddef>
#include <string>
#include <utility>
int main() {
  // ZeroizeSecret overwrites the bytes, then clears; Steam's name is the same function.
  {
    std::string secret(64, 'k');  // longer than the small-string buffer: the bytes stay in the same allocation
    const char* bytes=secret.data();
    const std::size_t size=secret.size();
    playden::platform::ZeroizeSecret(secret);
    assert(secret.empty());
    assert(secret.data()==bytes);
    for(std::size_t i=0;i<size;++i) assert(bytes[i]==0);
    std::string other="synthetic-refresh-token";
    playden::steam::ZeroizeSecret(other);
    assert(other.empty());
    void (*platform_zeroize)(std::string&)=&playden::platform::ZeroizeSecret;
    void (*steam_zeroize)(std::string&)=&playden::steam::ZeroizeSecret;
    assert(platform_zeroize==steam_zeroize);
  }
  playden::steam::SteamEvent event;
  event.challenge_url="https://s.team/q/private-synthetic-secret";
  event.account_name="private-synthetic-account";
  event.steam_id=123456789;
  event.request_id=7; event.generation=3;
  event.kind=playden::steam::SteamEventKind::kFailure;
  event.error=playden::steam::AuthError::kProtocol;
  const auto record=playden::diagnostics::AuthRecord(event);
  assert(record.find("private-synthetic")==std::string::npos);
  assert(record.find("s.team")==std::string::npos);
  assert(record.find("123456789")==std::string::npos);
  assert(record.find("\"request\":7")!=std::string::npos);
  assert(record.find("\"generation\":3")!=std::string::npos);

  event.kind=playden::steam::SteamEventKind::kOfflineSession;
  event.error=playden::steam::AuthError::kNetwork;
  const auto offline=playden::diagnostics::AuthRecord(event);
  assert(offline.find("\"kind\":\"offline-session\"")!=std::string::npos);
  assert(offline.find("private-synthetic")==std::string::npos);
  assert(offline.find("s.team")==std::string::npos);
  assert(offline.find("123456789")==std::string::npos);

  // Another store's account event: the store, the kind, the request and the generation, nothing that names the account.
  {
    playden::stores::AccountEvent other;
    other.store=playden::Store::kGog;
    other.kind=playden::stores::AccountEventKind::kSignedIn;
    other.request_id=11; other.generation=4;
    other.account_key=0xabcdef12;
    other.display_name="private-synthetic-name";
    other.user_id="48151623421";
    other.error="private-synthetic-error";
    const auto signed_in=playden::diagnostics::AccountRecord(other);
    assert(signed_in=="{\"store\":\"gog\",\"kind\":\"signed-in\",\"request\":11,\"generation\":4}");
    for (const char* hidden : {"private-synthetic", "48151623421", "abcdef12", "2882400018"})
      assert(signed_in.find(hidden)==std::string::npos);
    other.kind=playden::stores::AccountEventKind::kFailure;
    other.restoring=true;
    const auto failure=playden::diagnostics::AccountRecord(other);
    assert(failure.find("\"kind\":\"failure\"")!=std::string::npos);
    assert(failure.find("\"restoring\":true")!=std::string::npos);
    for (const char* hidden : {"private-synthetic", "48151623421", "abcdef12", "2882400018"})
      assert(failure.find(hidden)==std::string::npos);
  }

  {
    // GOG credentials by their shape (the same table as web_logs_source_test.cpp and test_collect_logs.py): each value
    // goes, its key stays; short values and Playden's own error codes stay.
    const std::pair<const char*, const char*> kCredentialCases[] = {
        {"GET https://embed.gog.com/on_login_success?origin=client&code=SyntheticCode0123456789abc",
         "GET https://embed.gog.com/on_login_success?origin=client&code=<redacted>"},
        {"code=SyntheticCode0123456789abc&origin=client",
         "code=<redacted>&origin=client"},
        {"Code=ABCDEFGHIJKLMNOPqrst end",
         "Code=<redacted> end"},
        {"chunk https://gog-cdn-fastly.gog.com/token=nva=1790000000~dirs=4~token=0a1b2c3d4e5f60718293a4b5c6d7e8f9/content-system/v2/store/1/ab/cd/x status=200",
         "chunk https://gog-cdn-fastly.gog.com/token=<redacted> status=200"},
        {"path /~token=0a1b2c3d4e5f60718293a4b5c6d7e8f9",
         "path /~token=<redacted>"},
        {"https://gog-cdn.gcdn.co/content-system/v2/store/1/ab/cd/x?wsSecret=0a1b2c3d4e5f60718293a4b5c6d7e8f9&wsTime=1790000000&prefix=/x",
         "https://gog-cdn.gcdn.co/content-system/v2/store/1/ab/cd/x?wsSecret=<redacted>&wsTime=1790000000&prefix=/x"},
        {"{\"access_token\":\"synthetic-access-1\",\"expires_in\":3600,\"refresh_token\" : \"r\"}",
         "{\"access_token\":\"<redacted>\",\"expires_in\":3600,\"refresh_token\" : \"<redacted>\"}"},
        {"?access_token=a1&refresh_token=r2 next",
         "?access_token=<redacted>&refresh_token=<redacted> next"},
        {"ACCESS_TOKEN=x;",
         "ACCESS_TOKEN=<redacted>;"},
    };
    const char* const kCredentialKept[] = {
        "exit code=1",
        "code=0",
        "step=publish code=publication-control-collision",
        "sharing: account link start ok=0 code=authorization_pending",
        "code=WDJB-MJHT",
        "token=y",
        "missing_token=12345678901234567890",
        "code=abcdef012345678",
        "access_token=",
        "\"access_token\":\"\"",
        "transcode=AAAAAAAAAAAAAAAAAAAAAA",
        "step=app_info at=access_token error=protocol decode=0",
    };
    using playden::diagnostics::RedactSecrets;
    for (const auto& [text, expected] : kCredentialCases) {
      assert(RedactSecrets(text) == expected);
      assert(RedactSecrets(expected) == expected);  // twice is once
    }
    for (const char* kept : kCredentialKept) assert(RedactSecrets(kept) == kept);
    // After the Bearer and known-secret rules, which stay as they were.
    assert(RedactSecrets("Authorization: Bearer synthetic-access-0000000000000001 code=SyntheticCode0123456789abc") ==
           "Authorization: Bearer <redacted> code=<redacted>");
    assert(RedactSecrets("id=known-secret-1 refresh_token=r", "known-secret-1") == "id=<redacted> refresh_token=<redacted>");
    assert(RedactSecrets("Bearer") == "Bearer" && RedactSecrets("forbearer x") == "forbearer x");
  }
}
