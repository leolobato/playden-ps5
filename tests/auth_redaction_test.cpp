#include "diagnostics/redaction.h"
#include "platform/secret.h"
#include "steam/secret.h"
#include <cassert>
#include <cstddef>
#include <string>
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
}
