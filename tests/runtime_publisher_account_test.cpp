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
  CHECK(std::string(PublisherSlug(Publisher::kUbisoft)) == "ubisoft");

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
  for (const auto publisher : {Publisher::kSteamHost, Publisher::kEa, Publisher::kRockstar, Publisher::kUbisoft})
    CHECK(playden::platform::data_roots::IsSecretFile(PublisherAccountFileName(publisher)));
  CHECK(Read(file) == R"({"version":1,"obtained_at":1760000000,"state":"signed_in","display":"Player \"One\"","blob":)" +
                          blob + "}");
  CHECK(!fs::exists(file.string() + ".tmp"));
  auto account = LoadPublisherAccount(state, Publisher::kRockstar);
  CHECK(account.state == PublisherAccountState::kSignedIn && account.display == "Player \"One\"");
  CHECK(account.obtained_at == 1760000000);
  CHECK(PublisherStageValue(state, Publisher::kRockstar, spec, &value, &reason) && value == kToken);
  CHECK(LoadPublisherAccount(state, Publisher::kEa).state == PublisherAccountState::kSignedOut);

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

  fs::remove_all(root);
  return 0;
}
