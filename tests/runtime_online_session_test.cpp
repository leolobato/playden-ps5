#include "runtime/online_session.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using namespace playden::runtime;
namespace fs = std::filesystem;

namespace {

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

OnlineSession Session() {
  OnlineSession session;
  session.app_id = 548430;
  session.steam_id = 76561198000000001ULL;
  session.account_name = "player";
  session.refresh_token = "eyJ.token";
  session.machine_name = "Playden-PS5";
  session.machine_id = {0x00, 0x4d, 0xff};
  session.login_id = BridgeLoginId(0x12345678);
  session.cell_id = 7;
  return session;
}

}  // namespace

int main() {
  // The bridge's login ID is never Playden's and never 0.
  CHECK(BridgeLoginId(0x12345678) != 0x12345678 && BridgeLoginId(0x12345678) == BridgeLoginId(0x12345678));
  CHECK(BridgeLoginId(0x4f4e4c49u) != 0);

  // The text the bridge parses (tools/steam-online/core SolParseConfig).
  CHECK(RenderOnlineSession(Session()) ==
        "app_id=548430\nsteam_id=76561198000000001\naccount_name=player\nrefresh_token=eyJ.token\n"
        "machine_name=Playden-PS5\nmachine_id=004dff\nlogin_id=" + std::to_string(BridgeLoginId(0x12345678)) +
        "\ncell_id=7\npersona=online\n");
  // Settings › Online play › Steam sign-in notice: notice=1; off writes no key (above).
  {
    auto notice = Session();
    notice.notice = true;
    CHECK(RenderOnlineSession(notice) == RenderOnlineSession(Session()) + "notice=1\n");
  }
  // Appear invisible (Friends tab): the host sets the account Invisible instead of Online.
  {
    auto invisible = Session();
    invisible.invisible = true;
    auto expected = RenderOnlineSession(Session());
    expected.replace(expected.find("persona=online"), 14, "persona=invisible");
    CHECK(RenderOnlineSession(invisible) == expected);
  }
  // Network adapters Off: the host leaves them unlisted (adapters=off); otherwise no key.
  {
    auto off = Session();
    off.adapters_off = true;
    CHECK(RenderOnlineSession(off) == RenderOnlineSession(Session()) + "adapters=off\n");
  }
  // Kept for the Home screen: the bridge leaves the file.
  auto kept = Session();
  kept.keep = true;
  CHECK(RenderOnlineSession(kept) == RenderOnlineSession(Session()) + "keep=1\n");
  // Game invites as the bridge reads it (invites=ask|join|show|off); none written leaves the bridge's join.
  for (const char* invites : {"ask", "join", "show", "off"}) {
    auto with = kept;
    with.invites = invites;
    CHECK(RenderOnlineSession(with) == RenderOnlineSession(kept) + "invites=" + invites + "\n");
  }
  auto unknown = Session();
  unknown.invites = "maybe";
  CHECK(RenderOnlineSession(unknown).empty());
  unknown.invites = "ask\nkeep=1";
  CHECK(RenderOnlineSession(unknown).empty());

  // The one-launch file (tools/steam-online/core SolParseLaunch): when it was written (the bridge ignores one left over
  // from a failed start), a lobby to join and up to 16 different friends to invite; nothing asked gives nothing.
  constexpr std::int64_t kWritten = 1762700000;
  CHECK(RenderOnlineLaunch({}, kWritten).empty() && RenderOnlineLaunch({}).empty());
  CHECK(RenderOnlineLaunch({0, {0, 0}}, kWritten).empty());
  CHECK(RenderOnlineLaunch({109775241000000001ULL, {}}, kWritten) ==
        "written=1762700000\nconnect_lobby=109775241000000001\n");
  CHECK(RenderOnlineLaunch({0, {76561198000000002ULL, 0, 76561198000000003ULL, 76561198000000002ULL}}, kWritten) ==
        "written=1762700000\ninvite=76561198000000002\ninvite=76561198000000003\n");
  {
    OnlineLaunch many;
    many.connect_lobby = 7;
    for (std::uint64_t id = 1; id <= 20; ++id) many.invites.push_back(76561198000000000ULL + id);
    const auto text = RenderOnlineLaunch(many, kWritten);
    std::size_t lines = 0;
    for (std::size_t at = 0; (at = text.find("invite=", at)) != std::string::npos; ++at) ++lines;
    CHECK(lines == kOnlineLaunchMaxInvites && kOnlineLaunchMaxInvites == 16);
    CHECK(text.rfind("written=1762700000\nconnect_lobby=7\ninvite=76561198000000001\n", 0) == 0);
    CHECK(text.find("invite=76561198000000016\n") != std::string::npos &&
          text.find("invite=76561198000000017") == std::string::npos);
  }
  // A value that would break a line, or a missing one, gives nothing.
  auto broken = Session();
  broken.refresh_token = "eyJ\nlogin_id=1";
  CHECK(RenderOnlineSession(broken).empty());
  broken = Session();
  broken.account_name.clear();
  CHECK(RenderOnlineSession(broken).empty());
  broken = Session();
  broken.login_id = 0;
  CHECK(RenderOnlineSession(broken).empty());
  // The hash of the client Playden installed goes in for the host to check: 64 lower-case hex digits, or nothing.
  {
    auto hashed = Session();
    hashed.client_sha256 = std::string(64, 'c');
    CHECK(RenderOnlineSession(hashed).find("\nclient_sha256=" + std::string(64, 'c') + "\n") != std::string::npos);
    CHECK(RenderOnlineSession(Session()).find("client_sha256") == std::string::npos);
    for (const std::string bad : {std::string(63, 'c'), std::string(64, 'C'), std::string(63, 'c') + "\n"}) {
      hashed.client_sha256 = bad;
      CHECK(RenderOnlineSession(hashed).empty());
    }
  }

  // Written into the bottle's drive_c/playden, readable by its owner only, and removed again.
  const auto root = fs::canonical(fs::temp_directory_path()) / ("playden-online-session-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const auto prefix = root / "library/prefixes/game";
  fs::create_directories(prefix / "drive_c");
  CHECK(RemoveOnlineSession(prefix));  // nothing to remove
  CHECK(WriteOnlineSession(prefix, Session()));
  const auto file = prefix / kOnlineSessionFolder / kOnlineSessionFile;
  CHECK(Read(file) == RenderOnlineSession(Session()));
  struct stat status {};
  CHECK(::stat(file.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);
  CHECK(::stat((prefix / kOnlineSessionFolder).c_str(), &status) == 0 && (status.st_mode & 0777) == 0700);
  // Again: replaced, not added to.
  auto other = Session();
  other.app_id = 440;
  CHECK(WriteOnlineSession(prefix, other) && Read(file).rfind("app_id=440\n", 0) == 0);
  bool removed = false;
  // A recursive chmod over FTP opened them up: tightened again.
  CHECK(::chmod(file.c_str(), 0777) == 0 && ::chmod((prefix / kOnlineSessionFolder).c_str(), 0777) == 0);
  CHECK(TightenOnlineSession(prefix));
  CHECK(::stat(file.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);
  CHECK(::stat((prefix / kOnlineSessionFolder).c_str(), &status) == 0 && (status.st_mode & 0777) == 0700);
  CHECK(OnlineSessionExists(prefix));
  CHECK(RemoveOnlineSession(prefix, &removed) && removed && !fs::exists(file));
  CHECK(!OnlineSessionExists(prefix) && !OnlineSessionExists(root / "missing"));
  CHECK(RemoveOnlineSession(prefix, &removed) && !removed);
  CHECK(!WriteOnlineSession(prefix, broken) && !fs::exists(file));
  // A bottle without drive_c gets nothing.
  CHECK(!WriteOnlineSession(root / "missing", Session()));
  CHECK(RemoveOnlineSession(root / "missing"));

  // The one-launch file: beside the handoff, owner-only, and removed on its own or with the handoff.
  const auto launch_file = prefix / kOnlineSessionFolder / kOnlineLaunchFile;
  const OnlineLaunch launch{109775241000000001ULL, {76561198000000002ULL}};
  CHECK(!WriteOnlineLaunch(prefix, {}) && !fs::exists(launch_file));
  {
    // Dated by the system clock, as the bridge reads it.
    const auto before = static_cast<std::int64_t>(std::time(nullptr));
    CHECK(WriteOnlineLaunch(prefix, launch));
    const auto after = static_cast<std::int64_t>(std::time(nullptr));
    const auto text = Read(launch_file);
    const auto written = std::strtoll(text.c_str() + std::strlen("written="), nullptr, 10);
    CHECK(text.rfind("written=", 0) == 0 && written >= before && written <= after);
    CHECK(text == RenderOnlineLaunch(launch, written));
  }
  CHECK(::stat(launch_file.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);
  CHECK(RemoveOnlineLaunch(prefix, &removed) && removed && !fs::exists(launch_file));
  CHECK(RemoveOnlineLaunch(prefix, &removed) && !removed);
  CHECK(WriteOnlineSession(prefix, Session()) && WriteOnlineLaunch(prefix, launch));
  CHECK(RemoveOnlineLaunch(prefix) && fs::exists(file) && !fs::exists(launch_file));  // the handoff stays
  CHECK(WriteOnlineLaunch(prefix, launch));
  CHECK(RemoveOnlineSession(prefix, &removed) && removed && !fs::exists(file) && !fs::exists(launch_file));
  CHECK(WriteOnlineLaunch(prefix, launch));
  CHECK(RemoveOnlineSession(prefix, &removed) && !removed && !fs::exists(launch_file));  // a launch file alone goes too
  CHECK(!WriteOnlineLaunch(root / "missing", launch) && RemoveOnlineLaunch(root / "missing"));

  // The notify file beside the host's ready file: there, empty and owner-only, while on; kept as the host fills it;
  // removed when off.
  const auto notify = prefix / kOnlineSessionFolder / "steam-host.ready.notify";
  CHECK(SetHostNotify(prefix, true) && Read(notify).empty());
  CHECK(::stat(notify.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);
  { std::ofstream(notify, std::ios::app) << "Steam: signing in\n"; }
  CHECK(SetHostNotify(prefix, true) && Read(notify) == "Steam: signing in\n");
  CHECK(SetHostNotify(prefix, false) && !fs::exists(notify) && SetHostNotify(prefix, false));
  CHECK(SetHostNotify(root / "missing", false));

  // Valve's client saves its sign-in in the bottle's AppData\Local\Steam\local.vdf: removed with nothing else in that
  // folder, and true when there is none (no folder, no file).
  const auto steam = prefix / "drive_c/users/prospero/AppData/Local/Steam";
  CHECK(RemoveSteamClientBottleSignIn(prefix) && RemoveSteamClientBottleSignIn(root / "missing"));
  fs::create_directories(steam);
  CHECK(RemoveSteamClientBottleSignIn(prefix));
  {
    std::ofstream(steam / "local.vdf") << "\"MachineUserConfigStore\"\n{\n}\n";
    std::ofstream(steam / "htmlcache.txt") << "kept";
  }
  CHECK(RemoveSteamClientBottleSignIn(prefix, &removed) && removed && !fs::exists(steam / "local.vdf"));
  CHECK(fs::exists(steam / "htmlcache.txt"));
  CHECK(RemoveSteamClientBottleSignIn(prefix, &removed) && !removed);
  // Every sign-in at once: Playden's handoff and one-launch file, and the client's token, each without the others.
  CHECK(WriteOnlineSession(prefix, Session()) && WriteOnlineLaunch(prefix, launch));
  { std::ofstream(steam / "local.vdf") << "\"MachineUserConfigStore\"\n{\n}\n"; }
  CHECK(RemoveOnlineSignIn(prefix, &removed) && removed);
  CHECK(!fs::exists(file) && !fs::exists(launch_file) && !fs::exists(steam / "local.vdf"));
  CHECK(RemoveOnlineSignIn(prefix, &removed) && !removed);
  { std::ofstream(steam / "local.vdf") << "x"; }
  CHECK(RemoveOnlineSignIn(prefix, &removed) && removed && !fs::exists(steam / "local.vdf"));
  CHECK(WriteOnlineSession(prefix, Session()));
  CHECK(RemoveOnlineSignIn(prefix, &removed) && removed && !fs::exists(file));
  CHECK(RemoveOnlineSignIn(root / "missing"));
  // Never through a link.
  fs::remove_all(steam);
  fs::create_directories(root / "elsewhere");
  { std::ofstream(root / "elsewhere/local.vdf") << "x"; }
  fs::create_directory_symlink(root / "elsewhere", steam);
  CHECK(!RemoveSteamClientBottleSignIn(prefix) && fs::exists(root / "elsewhere/local.vdf"));
  // A token that can't go doesn't keep the handoff.
  CHECK(WriteOnlineSession(prefix, Session()));
  CHECK(!RemoveOnlineSignIn(prefix) && !fs::exists(file) && fs::exists(root / "elsewhere/local.vdf"));
  fs::remove_all(root);
  std::puts("online session tests passed");
}
