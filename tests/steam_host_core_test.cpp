// The Steam host's portable core (tools/steam-host/core): the session file it reads at start and the SHA-256 it checks
// Valve's client DLL with.
#include "../tools/steam-host/core/shc.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

std::string Hash(const std::string& bytes) {
  ShcSha256 hash;
  std::uint8_t digest[32];
  char hex[65];
  ShcSha256Init(&hash);
  // In uneven pieces, so the block buffering is exercised.
  for (std::size_t at = 0; at < bytes.size();) {
    const std::size_t piece = std::min<std::size_t>(bytes.size() - at, 1 + at % 97);
    ShcSha256Update(&hash, bytes.data() + at, piece);
    at += piece;
  }
  ShcSha256Final(&hash, digest);
  ShcHex(digest, hex);
  return hex;
}

void TestSha256() {
  // FIPS 180-4 examples.
  assert(Hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(Hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  assert(Hash(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // Lengths around the padding boundary.
  assert(Hash(std::string(55, 'x')) != Hash(std::string(56, 'x')) && Hash(std::string(63, 'x')) != Hash(std::string(64, 'x')));
  assert(ShcTextEqual("abc", "abc") && !ShcTextEqual("abc", "abd") && !ShcTextEqual("abc", "ab") && !ShcTextEqual("", "a"));
  assert(ShcTextEqualPrefix("connect_lobby=1", "connect_lobby=") == 0 && ShcTextEqualPrefix("connect", "connect_lobby=") == 1 &&
         ShcTextEqualPrefix("invite=7", "connect_lobby=") == 1 && ShcTextEqualPrefix("anything", "") == 0);
}

bool Parse(const std::string& text, ShcSession* session) { return ShcParseSession(text.data(), text.size(), session) != 0; }

void TestSession() {
  ShcSession session;
  // What Playden writes (runtime/online_session.cpp), with the keys the host does not use.
  const std::string full =
      "app_id=548430\nsteam_id=76561198000000001\naccount_name=player\nrefresh_token=eyJ.secret.token\n"
      "machine_name=Playden-PS5\nmachine_id=0001\nlogin_id=305419896\ncell_id=7\ninvites=ask\n";
  assert(Parse(full, &session));
  assert(session.app_id == 548430 && session.steam_id == 76561198000000001ULL && !session.keep);
  // Game invites: each choice Playden writes, and Off for a missing or unknown one.
  assert(session.invites == SHC_INVITES_ASK);
  for (const auto& [text, value] : {std::pair<std::string, int>{"join", SHC_INVITES_JOIN}, {"show", SHC_INVITES_SHOW},
                                    {"off", SHC_INVITES_OFF}, {"Ask", SHC_INVITES_OFF}, {"asked", SHC_INVITES_OFF},
                                    {"", SHC_INVITES_OFF}}) {
    std::string text_full = full;
    text_full.replace(text_full.find("invites=ask"), 11, "invites=" + text);
    assert(Parse(text_full, &session) && session.invites == value);
  }
  {
    std::string without = full;
    without.erase(without.find("invites=ask\n"), 12);
    assert(Parse(without, &session) && session.invites == SHC_INVITES_OFF);
  }
  // How the account shows to friends: Invisible only for persona=invisible; Online when the line is missing or names
  // anything else.
  assert(Parse(full, &session) && session.persona == SHC_PERSONA_ONLINE);
  assert(Parse(full + "persona=invisible\n", &session) && session.persona == SHC_PERSONA_INVISIBLE);
  for (const char* other : {"online", "Invisible", "invisible2", "offline", ""})
    assert(Parse(full + "persona=" + other + "\n", &session) && session.persona == SHC_PERSONA_ONLINE);
  // The console's network adapters: left unlisted only for adapters=off; listed when the line is missing or names
  // anything else.
  assert(Parse(full, &session) && !session.adapters_off);
  assert(Parse(full + "adapters=off\n", &session) && session.adapters_off);
  for (const char* other : {"on", "Off", "off2", "kit", ""})
    assert(Parse(full + "adapters=" + other + "\n", &session) && !session.adapters_off);
  // The notice that the account is online: only for notice=1; nothing when the line is missing or says anything else.
  assert(Parse(full, &session) && !session.notice);
  assert(Parse(full + "notice=1\n", &session) && session.notice);
  for (const char* other : {"0", "on", "11", "true", ""})
    assert(Parse(full + "notice=" + other + "\n", &session) && !session.notice);
  assert(Parse(full, &session));
  assert(std::string(session.account) == "player" && std::string(session.token) == "eyJ.secret.token");
  assert(Parse(full + "keep=1\n", &session) && session.keep);
  // The hash of the client Playden installed: 64 lower-case hex digits, or nothing.
  assert(std::string(session.client_sha256).empty());
  const std::string hash(64, 'a');
  assert(Parse(full + "client_sha256=" + hash + "\n", &session) && std::string(session.client_sha256) == hash);
  for (const std::string bad : {std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'A'), std::string(63, 'a') + "g"})
    assert(Parse(full + "client_sha256=" + bad + "\n", &session) && std::string(session.client_sha256).empty());
  // Windows line ends, and no line end after the last value.
  assert(Parse("app_id=1\r\nsteam_id=2\r\naccount_name=a\r\nrefresh_token=t", &session) && session.steam_id == 2);

  // Every needed field is needed.
  for (const char* missing : {"app_id=", "steam_id=", "account_name=", "refresh_token="}) {
    std::string text = full;
    const auto at = text.find(missing);
    text.erase(at, text.find('\n', at) - at + 1);
    assert(!Parse(text, &session));
  }
  // A number that is not one, and values too long for their place, leave the session incomplete.
  assert(!Parse("app_id=12x\nsteam_id=2\naccount_name=a\nrefresh_token=t\n", &session));
  assert(!Parse("app_id=1\nsteam_id=-2\naccount_name=a\nrefresh_token=t\n", &session));
  assert(!Parse("app_id=1\nsteam_id=2\naccount_name=" + std::string(128, 'a') + "\nrefresh_token=t\n", &session));
  assert(!Parse("app_id=1\nsteam_id=2\naccount_name=a\nrefresh_token=" + std::string(2048, 't') + "\n", &session));
  assert(Parse("app_id=1\nsteam_id=2\naccount_name=" + std::string(127, 'a') + "\nrefresh_token=" + std::string(2047, 't') + "\n",
               &session));
  // Lines without '=' and empty input.
  assert(!Parse("", &session) && !Parse("\n\n", &session) && !Parse("app_id\nsteam_id\n", &session));

  // Wiping leaves nothing of the token.
  assert(Parse(full, &session));
  ShcWipeSession(&session);
  const ShcSession empty{};
  assert(std::memcmp(&session, &empty, sizeof session) == 0);
}

bool Launch(const std::string& text, std::int64_t now, ShcLaunch* launch) {
  return ShcParseLaunch(text.data(), text.size(), now, launch) != 0;
}

void TestLaunch() {
  ShcLaunch launch;
  const std::int64_t now = 1794000000;
  // What Playden writes (runtime/online_session.cpp RenderOnlineLaunch).
  const std::string full = "written=1794000000\nconnect_lobby=109775241000000001\ninvite=76561198000000002\n"
                           "invite=76561198000000003\n";
  assert(Launch(full, now, &launch));
  assert(launch.connect_lobby == 109775241000000001ULL && launch.invite_count == 2 &&
         launch.invites[0] == 76561198000000002ULL && launch.invites[1] == 76561198000000003ULL);
  // Only a lobby, only invites, Windows line ends and no line end after the last value.
  assert(Launch("written=1794000000\r\nconnect_lobby=5", now, &launch) && launch.connect_lobby == 5 && !launch.invite_count);
  assert(Launch("written=1794000000\ninvite=7\n", now, &launch) && !launch.connect_lobby && launch.invite_count == 1);

  // Written up to two minutes away from now, either way; older or later, or without written=, it is ignored and empty.
  assert(Launch(full, now + 120, &launch) && Launch(full, now - 120, &launch));
  for (const std::int64_t at : {now + 121, now - 121, std::int64_t{0}}) {
    assert(!Launch(full, at, &launch));
    assert(!launch.connect_lobby && !launch.invite_count);
  }
  assert(!Launch("connect_lobby=5\ninvite=7\n", now, &launch) && !launch.connect_lobby && !launch.invite_count);
  for (const std::string bad : {"written=\n", "written=-1794000000\n", "written=1794000000x\n", "written=0\n"})
    assert(!Launch(bad + "connect_lobby=5\n", now, &launch));
  // Nothing asked for.
  assert(!Launch("written=1794000000\n", now, &launch) && !Launch("", now, &launch));
  assert(!Launch("written=1794000000\nconnect_lobby=0\ninvite=0\ninvite=x\n", now, &launch));

  // Repeats once, at most 16 friends, in order.
  std::string many = "written=1794000000\ninvite=1\ninvite=1\n";
  for (int i = 2; i <= 20; ++i) many += "invite=" + std::to_string(i) + "\n";
  assert(Launch(many, now, &launch) && launch.invite_count == SHC_LAUNCH_MAX_INVITES);
  for (int i = 0; i < SHC_LAUNCH_MAX_INVITES; ++i) assert(launch.invites[i] == static_cast<std::uint64_t>(i + 1));
}

void TestInviteAction() {
  // The title can ask and show: each choice as chosen.
  assert(ShcInviteAction(SHC_INVITES_JOIN, 1, 1) == SHC_INVITE_JOIN);
  assert(ShcInviteAction(SHC_INVITES_ASK, 1, 1) == SHC_INVITE_ASK);
  assert(ShcInviteAction(SHC_INVITES_SHOW, 1, 1) == SHC_INVITE_SHOW);
  assert(ShcInviteAction(SHC_INVITES_OFF, 1, 1) == SHC_INVITE_IGNORE);
  // Without it, Ask and Only show leave the invite (never a join the player wasn't asked about), as the Game invites row
  // says.
  assert(ShcInviteAction(SHC_INVITES_ASK, 0, 0) == SHC_INVITE_IGNORE);
  assert(ShcInviteAction(SHC_INVITES_ASK, 0, 1) == SHC_INVITE_IGNORE);
  assert(ShcInviteAction(SHC_INVITES_SHOW, 1, 0) == SHC_INVITE_IGNORE);
  assert(ShcInviteAction(SHC_INVITES_SHOW, 0, 0) == SHC_INVITE_IGNORE);
  assert(ShcInviteAction(SHC_INVITES_JOIN, 0, 0) == SHC_INVITE_JOIN);
  assert(ShcInviteAction(SHC_INVITES_OFF, 0, 0) == SHC_INVITE_IGNORE);
  assert(ShcInviteAction(42, 1, 1) == SHC_INVITE_IGNORE);
}

void TestTitleTexts() {
  char out[256];
  // What the PS5 shows: the friend's name, or "A friend" when there is none to show.
  ShcInviteText(SHC_TEXT_ASK, "Mara", out, sizeof out);
  assert(std::string(out) == "Mara invited you to their game. Press L3 + R3 within 15 seconds to join.");
  ShcInviteText(SHC_TEXT_SHOW, "Mara", out, sizeof out);
  assert(std::string(out) == "Mara invited you to their game.");
  ShcInviteText(SHC_TEXT_JOINING, "Mara", out, sizeof out);
  assert(std::string(out) == "Joining Mara's game.");
  for (const char* none : {static_cast<const char*>(nullptr), "", "  ", "\t\n"}) {
    ShcInviteText(SHC_TEXT_SHOW, none, out, sizeof out);
    assert(std::string(out) == "A friend invited you to their game.");
    ShcInviteText(SHC_TEXT_JOINING, none, out, sizeof out);
    assert(std::string(out) == "Joining a friend's game.");
  }
  // A name can't break the line the title reads, and a small buffer cuts the text.
  ShcInviteText(SHC_TEXT_SHOW, "Ma\nra\r", out, sizeof out);
  assert(std::string(out) == "Ma ra  invited you to their game.");
  ShcInviteText(SHC_TEXT_SHOW, "Mara", out, 5);
  assert(std::string(out) == "Mara");

  ShcInvitedText(1, out, sizeof out);
  assert(std::string(out) == "Invited 1 friend to your game.");
  ShcInvitedText(3, out, sizeof out);
  assert(std::string(out) == "Invited 3 friends to your game.");

  // The notice as the game starts: the account's name, and whether friends see it.
  ShcOnlineText(SHC_PERSONA_ONLINE, "Mara", out, sizeof out);
  assert(std::string(out) == "Online on Steam as Mara.");
  ShcOnlineText(SHC_PERSONA_INVISIBLE, "Mara", out, sizeof out);
  assert(std::string(out) == "Signed in to Steam as Mara, invisible to friends.");
  for (const char* none : {static_cast<const char*>(nullptr), "", " \t"}) {
    ShcOnlineText(SHC_PERSONA_ONLINE, none, out, sizeof out);
    assert(std::string(out) == "Online on Steam.");
    ShcOnlineText(SHC_PERSONA_INVISIBLE, none, out, sizeof out);
    assert(std::string(out) == "Signed in to Steam, invisible to friends.");
  }
  ShcOnlineText(SHC_PERSONA_ONLINE, "Ma\nra", out, sizeof out);
  assert(std::string(out) == "Online on Steam as Ma ra.");

  // The question line: id, seconds, text, one line end, cut to fit.
  ShcQuestionLine(7, "Mara invited you.", out, sizeof out);
  assert(std::string(out) == "7 15 Mara invited you.\n");
  ShcQuestionLine(18446744073709551615ULL, "x\ny", out, sizeof out);
  assert(std::string(out) == "18446744073709551615 15 x y\n");
  ShcQuestionLine(7, "abcdef", out, 10);
  assert(std::string(out) == "7 15 abc\n");

  // The title's answer.
  std::uint64_t id = 0;
  int accepted = -1;
  assert(ShcParseAnswer("7 accepted\n", 11, &id, &accepted) && id == 7 && accepted == 1);
  assert(ShcParseAnswer("8 expired", 9, &id, &accepted) && id == 8 && accepted == 0);
  for (const std::string bad : {"", "7", "7 ", "7 accept", "7 accepted yes", "0 accepted", "x accepted", " 7 accepted",
                                "7  accepted", "7 Accepted"}) {
    id = 99, accepted = -1;
    assert(!ShcParseAnswer(bad.data(), bad.size(), &id, &accepted) && id == 99 && accepted == -1);
  }
}

void TestChangeStatusMessage() {
  std::uint8_t out[16];
  // 716 with the protobuf flag, an empty header the client fills in, then persona_state.
  assert(ShcChangeStatusMessage(SHC_PERSONA_ONLINE, out, sizeof out) == 10);
  const std::uint8_t online[] = {0xcc, 0x02, 0x00, 0x80, 0, 0, 0, 0, 0x08, 0x01};
  assert(std::memcmp(out, online, sizeof online) == 0);
  assert(ShcChangeStatusMessage(SHC_PERSONA_INVISIBLE, out, sizeof out) == 10 && out[9] == 0x07);
  // A state past 127 takes two varint bytes; a buffer too small gets nothing.
  assert(ShcChangeStatusMessage(300, out, sizeof out) == 11 && out[9] == 0xac && out[10] == 0x02);
  assert(ShcChangeStatusMessage(SHC_PERSONA_ONLINE, out, 9) == 0);
}

void TestLobbyWatch() {
  ShcLobbyWatch watch{};
  // No lobby is not one; the first lobby is the moment to invite, once, whatever comes after.
  assert(!ShcLobbySeen(&watch, 0) && !watch.first);
  assert(ShcLobbySeen(&watch, 11) && watch.first == 11 && watch.last == 11);
  assert(!ShcLobbySeen(&watch, 11));
  assert(!ShcLobbySeen(&watch, 12) && watch.first == 11 && watch.last == 12);
  assert(!ShcLobbySeen(&watch, 0) && watch.last == 12);
}

}  // namespace

int main() {
  TestSha256();
  TestSession();
  TestLaunch();
  TestInviteAction();
  TestTitleTexts();
  TestChangeStatusMessage();
  TestLobbyWatch();
  return 0;
}
