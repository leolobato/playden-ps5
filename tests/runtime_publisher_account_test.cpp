#include "runtime/publisher_account.h"
#include "platform/data_roots.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace playden::runtime;

namespace {

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream bytes;
  bytes << in.rdbuf();
  return bytes.str();
}

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

unsigned Mode(const fs::path& path) {
  struct stat info {};
  CHECK(::stat(path.c_str(), &info) == 0);
  return static_cast<unsigned>(info.st_mode & 0777);
}

// A made-up publisher's account spec, as its stage manifest would declare it.
PublisherAccountSpec Spec() {
  PublisherAccountSpec spec;
  spec.name = "Example";
  spec.display_fields = {"name", "Name"};
  spec.stage_fields = {"token", "Token"};
  spec.stage_min = 8;
  spec.stage_max = 32;
  spec.max_bytes = 512;
  return spec;
}

const std::string kToken = "QUJDREVGR0hJSktM+/0=";

}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / ("playden-publisher-account-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);  // the state folder itself is created on the first save
  const auto state = (root / "state").string();
  const auto spec = Spec();
  const auto file = root / "state" / "publisher-account-rockstar.v1";
  CHECK(PublisherAccountFileName(Publisher::kRockstar) == "publisher-account-rockstar.v1");
  CHECK(std::string(PublisherSlug(Publisher::kRockstar)) == "rockstar");

  // Nothing stored: signed out, no stage value, sign-out and expiry are no-ops.
  std::string reason, display, value;
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedOut);
  CHECK(!PublisherStageValue(state, Publisher::kRockstar, spec, &value, &reason) && value.empty());
  CHECK(MarkPublisherAccountExpired(state, Publisher::kRockstar, &reason) && !fs::exists(file));

  // Accepted: stored whole and privately; the second display field and stage field count too.
  const std::string blob = R"({"Name":"Player \"One\"","token":")" + kToken + R"(","extra":{"n":[1,2]}})";
  CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, blob, spec, 1760000000, &display, &reason));
  CHECK(display == "Player \"One\"");
  CHECK(Mode(file) == 0600);
  // Owner-only on the console's open tree too, and kept so by the self-heal.
  for (const auto publisher : kPublishers)
    CHECK(playden::platform::data_roots::IsSecretFile(PublisherAccountFileName(publisher)));
  CHECK(Read(file) == R"({"version":1,"obtained_at":1760000000,"state":"signed_in","display":"Player \"One\"","blob":)" +
                          blob + "}");
  CHECK(!fs::exists(file.string() + ".tmp"));
  auto account = LoadPublisherAccount(state, Publisher::kRockstar);
  CHECK(account.state == PublisherAccountState::kSignedIn && account.display == "Player \"One\"");
  CHECK(account.obtained_at == 1760000000);
  CHECK(PublisherStageValue(state, Publisher::kRockstar, spec, &value, &reason) && value == kToken);

  // A later sign-in replaces it; no display field leaves the name empty.
  const std::string second = R"({"Token":"ZXhhbXBsZTI="})";
  CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, second, spec, 1760000100, &display, &reason));
  CHECK(display.empty() && LoadPublisherAccount(state, Publisher::kRockstar).obtained_at == 1760000100);
  CHECK(PublisherStageValue(state, Publisher::kRockstar, spec, &value, &reason) && value == "ZXhhbXBsZTI=");
  CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, blob, spec, 1760000000, &display, &reason));

  // Refused shapes leave the stored account alone.
  const auto stored = Read(file);
  const auto refused = [&](const std::string& candidate) {
    reason.clear();
    const bool ok = AcceptPublisherCredential(state, Publisher::kRockstar, candidate, spec, 1, &display, &reason);
    CHECK(Read(file) == stored);
    return !ok && !reason.empty();
  };
  CHECK(refused(""));
  CHECK(refused("[\"" + kToken + "\"]"));                                       // not an object
  CHECK(refused("\"" + kToken + "\""));                                         // not an object
  CHECK(refused("{\"token\":\"" + kToken + "\""));                              // not JSON
  CHECK(refused(R"({"token":{"value":")" + kToken + R"("}})"));                 // JSON in the stage field
  CHECK(refused(R"({"token":"{\"value\":\")" + kToken + R"(\"}"})"));           // JSON text in the stage field
  CHECK(refused(R"({"token":"QUJD"})"));                                        // too short
  CHECK(refused(R"({"token":")" + std::string(33, 'A') + "\"}"));               // too long
  CHECK(refused(R"({"token":"QUJDREVG-_x"})"));                                 // base64url, not base64
  CHECK(refused(R"({"token":"QUJDREVGR0hJ SktM"})"));                           // a space
  CHECK(refused(R"({"other":")" + kToken + "\"}"));                             // no stage field
  CHECK(refused(R"({"token":")" + kToken + R"(","token":"AAAAAAAAAA"})"));      // repeated key
  CHECK(refused(R"({"token":")" + kToken + R"(","name":"a\nb"})"));             // display with a line break
  CHECK(refused(R"({"token":")" + kToken + R"(","name":")" + std::string(129, 'n') + "\"}"));
  CHECK(refused(R"({"token":")" + kToken + R"(","pad":")" + std::string(512, 'p') + "\"}"));  // over max_bytes
  // The first present string wins: a non-string first field falls through to the next.
  CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, R"({"token":7,"Token":")" + kToken + "\"}", spec, 5,
                                  &display, &reason));
  CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, blob, spec, 1760000000, &display, &reason));

  // A stage value the current spec no longer accepts is not handed out.
  auto narrower = spec;
  narrower.stage_max = 10;
  value.clear();
  CHECK(!PublisherStageValue(state, Publisher::kRockstar, narrower, &value, &reason) && value.empty());

  // Expired: the name and sign-in time stay, the blob goes.
  CHECK(MarkPublisherAccountExpired(state, Publisher::kRockstar, &reason));
  CHECK(Mode(file) == 0600);
  CHECK(Read(file) == R"({"version":1,"state":"expired","display":"Player \"One\"","obtained_at":1760000000})");
  CHECK(Read(file).find(kToken) == std::string::npos);
  account = LoadPublisherAccount(state, Publisher::kRockstar);
  CHECK(account.state == PublisherAccountState::kExpired && account.display == "Player \"One\"");
  CHECK(account.obtained_at == 1760000000);
  CHECK(!PublisherStageValue(state, Publisher::kRockstar, spec, &value, &reason));
  CHECK(MarkPublisherAccountExpired(state, Publisher::kRockstar, &reason));

  // Sign-out deletes it; twice is fine.
  CHECK(SignOutPublisherAccount(state, Publisher::kRockstar, &reason) && !fs::exists(file));
  CHECK(SignOutPublisherAccount(state, Publisher::kRockstar, &reason));

  // A damaged file reads as signed out.
  Write(file, "{\"version\":1,\"state\":\"signed_in\",\"display\":\"x\",\"obtained_at\":1}");  // no blob
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedOut);
  Write(file, "{\"version\":2,\"state\":\"expired\",\"display\":\"x\",\"obtained_at\":1}");
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedOut);
  Write(file, "not json");
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedOut);

  // The status a session leaves behind.
  const auto title = root / "game" / "Title";
  fs::create_directories(title);
  PublisherStatusSpec status{".examplepub/status", "state=stale"};
  bool expired = true;
  CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, blob, spec, 1760000000, &display, &reason));
  // Missing: no change.
  CHECK(ApplyPublisherSessionStatus(state, Publisher::kRockstar, title.string(), status, &expired, &reason));
  CHECK(!expired && LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedIn);
  // Another status: no change, the file is removed.
  Write(title / ".examplepub/status", "state=ok\n");
  CHECK(ApplyPublisherSessionStatus(state, Publisher::kRockstar, title.string(), status, &expired, &reason));
  CHECK(!expired && !fs::exists(title / ".examplepub/status"));
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedIn);
  // The expired status, with spaces and line ends around it: expired, the file is removed.
  Write(title / ".examplepub/status", "  state=stale\r\n");
  CHECK(ApplyPublisherSessionStatus(state, Publisher::kRockstar, title.string(), status, &expired, &reason));
  CHECK(expired && !fs::exists(title / ".examplepub/status"));
  account = LoadPublisherAccount(state, Publisher::kRockstar);
  CHECK(account.state == PublisherAccountState::kExpired && account.display == "Player \"One\"");
  // Links are not followed, neither for the file nor for its folder.
  CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, blob, spec, 1760000000, &display, &reason));
  Write(root / "elsewhere/status", "state=stale");
  fs::create_symlink(root / "elsewhere/status", title / ".examplepub/status");
  CHECK(!ApplyPublisherSessionStatus(state, Publisher::kRockstar, title.string(), status, &expired, &reason));
  CHECK(!expired && LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedIn);
  fs::remove_all(title / ".examplepub");
  fs::create_directory_symlink(root / "elsewhere", title / ".examplepub");
  CHECK(!ApplyPublisherSessionStatus(state, Publisher::kRockstar, title.string(), status, &expired, &reason));
  CHECK(!expired && fs::exists(root / "elsewhere/status"));
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedIn);
  // The status of a start without any sign-in: said, the account stays, the file is removed.
  fs::remove_all(title / ".examplepub");
  PublisherStatusSpec with_missing{".examplepub/status", "state=stale", "state=none"};
  bool missing = false;
  Write(title / ".examplepub/status", "state=none\n");
  CHECK(ApplyPublisherSessionStatus(state, Publisher::kRockstar, title.string(), with_missing, &expired, &reason, &missing));
  CHECK(missing && !expired && !fs::exists(title / ".examplepub/status"));
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kSignedIn);
  // Without the manifest declaring it, the same content is just another status.
  Write(title / ".examplepub/status", "state=none\n");
  CHECK(ApplyPublisherSessionStatus(state, Publisher::kRockstar, title.string(), status, &expired, &reason, &missing));
  CHECK(!missing && !expired && !fs::exists(title / ".examplepub/status"));
  // An unsafe path is refused.
  PublisherStatusSpec unsafe{"../status", "state=stale"};
  CHECK(!ApplyPublisherSessionStatus(state, Publisher::kRockstar, title.string(), unsafe, &expired, &reason));

  // Refresh: a made-up recipe renewing the token from a cookie pair kept in the sign-in.
  PublisherRefreshSpec recipe;
  recipe.url = "https://accounts.example.test/renew?device=%7B%7D";
  recipe.headers = {{"X-Kind", "renew"}};
  recipe.cookies = {{"Access", "Session.Access"}, {"Renew", "Session.Renew"}};
  recipe.fields = {"token", "Ticket"};
  recipe.ok_field = "ok";
  recipe.expired_statuses = {401, 403};
  recipe.every_hours = 20;
  const std::string kRenewed = "UkVORVdFRFRPS0VO";
  const std::string session_blob = R"({"Name":"Player","token":")" + kToken +
                                   R"(","Session":{"Access":"aaa","Renew":"bbb"},"keep":[1,{"x":true}],"n":1.50})";
  const auto sign_in = [&](std::uint64_t at) {
    CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, session_blob, spec, at, &display, &reason));
  };
  // What the server answers, and what the transport saw.
  PublisherRefreshRequest seen;
  int calls = 0;
  PublisherRefreshAnswer next;
  const auto transport = [&](const PublisherRefreshRequest& request) {
    ++calls;
    seen = request;
    return next;
  };
  const auto good_answer = [&] {
    PublisherRefreshAnswer answer;
    answer.reached = true;
    answer.status = 200;
    answer.headers = {{"Content-Type", "application/json"},
                      {"Set-Cookie", "Access=ccc; expires=Sun, 22-Nov-2026 20:00:00 GMT; secure"},
                      {"set-cookie", "Renew=ddd; path=/; SameSite=Strict"},
                      {"Set-Cookie", "Other=zzz"}};
    answer.body = R"({"ok":true,"token":")" + kRenewed + R"(","Ticket":"t-1","other":"x"})";
    return answer;
  };
  const auto refresh = [&](std::uint64_t now) {
    return RefreshPublisherAccount(state, Publisher::kRockstar, spec, recipe, transport, now);
  };

  // Renewed: the request carries the recipe's headers and the cookies; the answer's recipe fields and the
  // renewed cookies replace the old ones, everything else stays as it was (numbers keep their text).
  sign_in(1760001000);
  next = good_answer();
  auto outcome = refresh(1760002000);
  CHECK(outcome.result == PublisherRefreshResult::kRefreshed && outcome.status == 200 && calls == 1);
  CHECK(std::string(PublisherRefreshResultName(outcome.result)) == "refreshed");
  CHECK(seen.url == recipe.url);
  CHECK(seen.headers.size() == 2 && seen.headers[0] == std::make_pair(std::string("X-Kind"), std::string("renew")));
  CHECK(seen.headers[1].first == "Cookie" && seen.headers[1].second == "Access=aaa; Renew=bbb");
  CHECK(Read(file) == R"({"version":1,"obtained_at":1760001000,"refreshed_at":1760002000,"state":"signed_in",)"
                      R"("display":"Player","blob":{"Name":"Player","token":")" + kRenewed +
                          R"(","Session":{"Access":"ccc","Renew":"ddd"},"keep":[1,{"x":true}],"n":1.50,"Ticket":"t-1"}})");
  CHECK(Mode(file) == 0600);
  account = LoadPublisherAccount(state, Publisher::kRockstar);
  CHECK(account.state == PublisherAccountState::kSignedIn && account.display == "Player");
  CHECK(account.obtained_at == 1760001000 && account.renewed_at == 1760002000);
  CHECK(PublisherStageValue(state, Publisher::kRockstar, spec, &value, &reason) && value == kRenewed);
  // The next refresh sends the renewed pair.
  next = good_answer();
  CHECK(refresh(1760003000).result == PublisherRefreshResult::kRefreshed);
  CHECK(seen.headers[1].second == "Access=ccc; Renew=ddd");
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).renewed_at == 1760003000);

  // A sign-in that was never renewed reports its sign-in time.
  sign_in(1760004000);
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).renewed_at == 1760004000);
  const auto unrenewed = Read(file);
  // Failures change nothing: no answer, another status, a refusal in the answer, a cookie not renewed,
  // a token Playden can't use, an answer that isn't JSON.
  next = PublisherRefreshAnswer{};
  outcome = refresh(1760005000);
  CHECK(outcome.result == PublisherRefreshResult::kFailed && Read(file) == unrenewed);
  next = good_answer();
  next.status = 500;
  CHECK(refresh(1760005000).result == PublisherRefreshResult::kFailed && Read(file) == unrenewed);
  next = good_answer();
  next.body = R"({"ok":false,"token":")" + kRenewed + R"("})";
  CHECK(refresh(1760005000).result == PublisherRefreshResult::kFailed && Read(file) == unrenewed);
  next = good_answer();
  next.headers.erase(next.headers.begin() + 2);  // Renew isn't set again
  outcome = refresh(1760005000);
  CHECK(outcome.result == PublisherRefreshResult::kFailed && outcome.reason.find("Renew") != std::string::npos);
  CHECK(Read(file) == unrenewed);
  next = good_answer();
  next.headers[1].second = "Access=bad value; secure";  // a space: not a value Playden keeps
  CHECK(refresh(1760005000).result == PublisherRefreshResult::kFailed && Read(file) == unrenewed);
  next = good_answer();
  next.body = R"({"ok":true,"token":"short"})";
  CHECK(refresh(1760005000).result == PublisherRefreshResult::kFailed && Read(file) == unrenewed);
  next = good_answer();
  next.body = "<html>busy</html>";
  CHECK(refresh(1760005000).result == PublisherRefreshResult::kFailed && Read(file) == unrenewed);
  // Nothing in the outcome holds a secret.
  CHECK(outcome.reason.find("aaa") == std::string::npos && outcome.reason.find(kRenewed) == std::string::npos);

  // A sign-in in the meantime wins: the refresh doesn't write over it.
  next = good_answer();
  const auto racing = [&](const PublisherRefreshRequest&) {
    sign_in(1760006000);
    return good_answer();
  };
  outcome = RefreshPublisherAccount(state, Publisher::kRockstar, spec, recipe, racing, 1760007000);
  CHECK(outcome.result == PublisherRefreshResult::kFailed);
  account = LoadPublisherAccount(state, Publisher::kRockstar);
  CHECK(account.obtained_at == 1760006000 && account.renewed_at == 1760006000);
  CHECK(PublisherStageValue(state, Publisher::kRockstar, spec, &value, &reason) && value == kToken);

  // An expired status: the account becomes Expired.
  next = good_answer();
  next.status = 403;
  next.body = "{}";
  outcome = refresh(1760008000);
  CHECK(outcome.result == PublisherRefreshResult::kExpired && outcome.status == 403);
  CHECK(LoadPublisherAccount(state, Publisher::kRockstar).state == PublisherAccountState::kExpired);
  // Not signed in: nothing is sent.
  calls = 0;
  CHECK(refresh(1760009000).result == PublisherRefreshResult::kSignedOut && calls == 0);
  // A sign-in without the cookies (a pasted one) is not refreshable: nothing is sent.
  CHECK(AcceptPublisherCredential(state, Publisher::kRockstar, blob, spec, 1760010000, &display, &reason));
  CHECK(refresh(1760011000).result == PublisherRefreshResult::kNotRefreshable && calls == 0);

  // The untested notice the player went past, per game: kept apart from the account, so signing out keeps it.
  {
    const auto seen = root / "state" / "publisher-untested-rockstar.v1";
    CHECK(PublisherUntestedFileName(Publisher::kRockstar) == "publisher-untested-rockstar.v1");
    CHECK(!PublisherUntestedNoticeSeen(state, Publisher::kRockstar, 110800));
    CHECK(NotePublisherUntestedNoticeSeen(state, Publisher::kRockstar, 110800, &reason));
    CHECK(NotePublisherUntestedNoticeSeen(state, Publisher::kRockstar, 12200, &reason));
    CHECK(NotePublisherUntestedNoticeSeen(state, Publisher::kRockstar, 110800, &reason));  // once is enough
    CHECK(Read(seen) == "12200\n110800\n");
    CHECK(PublisherUntestedNoticeSeen(state, Publisher::kRockstar, 110800) &&
          PublisherUntestedNoticeSeen(state, Publisher::kRockstar, 12200));
    CHECK(!PublisherUntestedNoticeSeen(state, Publisher::kRockstar, 204100));
    CHECK(SignOutPublisherAccount(state, Publisher::kRockstar, &reason));
    CHECK(PublisherUntestedNoticeSeen(state, Publisher::kRockstar, 110800));
    // A damaged file holds nothing: the notice shows again, and the next note starts it over.
    for (const char* damaged : {"110800\nabc\n", "0110800\n", "99999999999\n", "110800\n\n", "110800 \n"}) {
      Write(seen, damaged);
      CHECK(!PublisherUntestedNoticeSeen(state, Publisher::kRockstar, 110800));
    }
    CHECK(NotePublisherUntestedNoticeSeen(state, Publisher::kRockstar, 204100, &reason));
    CHECK(Read(seen) == "204100\n");
  }

  fs::remove_all(root);
  return 0;
}
