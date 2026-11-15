/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The portable part of Playden's Steam host (tools/steam-host): the session file Playden hands over at Play, and
 * SHA-256 for checking Valve's client before it is used. No C runtime and no Windows headers, so the same file
 * builds into steam.exe and into the host tests (tests/steam_host_core_test.cpp). */
#ifndef PLAYDEN_STEAM_HOST_SHC_H
#define PLAYDEN_STEAM_HOST_SHC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int value, size_t n);

/* Game invites (the session's invites=): what a friend's invite does while the game runs. Off when the line is missing
 * or names something else. */
enum { SHC_INVITES_OFF = 0, SHC_INVITES_JOIN, SHC_INVITES_ASK, SHC_INVITES_SHOW };

/* EPersonaState: what friends see. Invisible shows the account offline to them. */
enum { SHC_PERSONA_ONLINE = 1, SHC_PERSONA_INVISIBLE = 7 };

/* The handoff file (runtime/online_session.h): key=value lines. The host needs the account, its SteamID, the refresh
 * token, the game's app ID, whether the file is kept for the next start, Game invites, the SHA-256 of the
 * steamclient64.dll Playden installed (64 lower-case hex digits; empty when the line is missing or malformed), how
 * the account shows to friends (persona=invisible, else online), whether the game goes without the console's
 * network adapters (adapters=off; anything else lists them) and whether the player is told the account is online as
 * the game starts (notice=1; anything else, or no line, tells nothing). Other keys are skipped. */
typedef struct {
  uint32_t app_id;
  uint64_t steam_id;
  char account[128];
  char token[2048];
  int keep;
  int invites; /* SHC_INVITES_* */
  char client_sha256[65];
  uint32_t persona; /* SHC_PERSONA_ONLINE or SHC_PERSONA_INVISIBLE */
  int adapters_off;
  int notice;
} ShcSession;

/* 1 when everything a sign-in needs is there; the token is never copied anywhere else. */
int ShcParseSession(const char* text, size_t n, ShcSession* session);
/* Overwrites the token and the rest. */
void ShcWipeSession(ShcSession* session);

/* The one-launch file (runtime/online_session.h RenderOnlineLaunch): written=<Unix seconds>, then connect_lobby=<id> and
 * invite=<steamid> lines. */
enum { SHC_LAUNCH_MAX_INVITES = 16, SHC_LAUNCH_MAX_AGE_SECONDS = 120 };
typedef struct {
  uint64_t connect_lobby;                    /* a friend's lobby to join; 0 for none */
  uint64_t invites[SHC_LAUNCH_MAX_INVITES]; /* friends to invite to the first lobby the game is in */
  int invite_count;
} ShcLaunch;

/* 1 when the file asks for something and was written at most SHC_LAUNCH_MAX_AGE_SECONDS from now (Unix seconds, the
 * same console clock), either way: a file without written=, or one left from a start that failed earlier, is ignored,
 * so a later start from the Home screen never acts on it. 0 IDs and repeats are left out, and invites past the 16th. */
int ShcParseLaunch(const char* text, size_t n, int64_t now, ShcLaunch* launch);

/* What the host does with a friend's invite to this game while it runs, from Game invites and what the game's title can
 * do for the host while the game runs: ask the player (L3 + R3) and show a notification. Without them, Ask and Only show
 * leave the invite, never joining one the player wasn't asked about, as Playden's Game invites row says
 * (runtime/game_settings.cpp). */
enum { SHC_INVITE_IGNORE = 0, SHC_INVITE_JOIN, SHC_INVITE_ASK, SHC_INVITE_SHOW };
int ShcInviteAction(int invites, int title_can_ask, int title_can_show);

/* What the title shows for the host while the game runs (prospero-win's src/pw_helper_channel.h): one line each, with
 * the friend's name (control characters as spaces; "A friend" when there is none), at most cap - 1 bytes:
 *   SHC_TEXT_ASK     the question for Game invites = Ask, answered with L3 + R3 within SHC_QUESTION_SECONDS
 *   SHC_TEXT_SHOW    the invite, for Game invites = Only show
 *   SHC_TEXT_JOINING a join the host passed to the game */
enum { SHC_TEXT_ASK = 1, SHC_TEXT_SHOW, SHC_TEXT_JOINING, SHC_QUESTION_SECONDS = 15 };
void ShcInviteText(int kind, const char* name, char* out, size_t cap);
/* "Invited 1 friend to your game." or "Invited <count> friends to your game.", for the friends picked before Play. */
void ShcInvitedText(int count, char* out, size_t cap);
/* The notice once the account is signed in (notice=1): "Online on Steam as <name>.", or for an invisible account
 * "Signed in to Steam as <name>, invisible to friends." The name as for invites; without one, the sentence ends after
 * "Steam". */
void ShcOnlineText(uint32_t persona, const char* name, char* out, size_t cap);

/* The question file's line: "<id> <seconds> <text>\n", at most cap - 1 bytes (the text is cut to fit). */
void ShcQuestionLine(uint64_t id, const char* text, char* out, size_t cap);

/* The title's answer file: 1 with *id and *accepted when it is "<id> accepted" or "<id> expired" (one line). */
int ShcParseAnswer(const char* text, size_t n, uint64_t* id, int* accepted);

/* The status message (ClientChangeStatus, 716) the host sends on Valve's client's shared connection: the message
 * number with the protobuf flag, a header length of 0 (the client fills the header in), then persona_state = state.
 * Its length, or 0 when cap is too small. */
size_t ShcChangeStatusMessage(uint32_t state, uint8_t* out, size_t cap);

/* The lobbies the account is in while the game runs, as the host learns of them. ShcLobbySeen returns 1 for the first
 * lobby only: the moment the launch's friends are invited, once. */
typedef struct {
  uint64_t first;
  uint64_t last;
} ShcLobbyWatch;
int ShcLobbySeen(ShcLobbyWatch* watch, uint64_t lobby);

/* UserAchievementStored_t (k_iSteamUserStatsCallbacks + 3 = 1103), as Valve's client posts it to a pipe of the game's
 * app once the game's StoreStats has saved an achievement on Steam, or for IndicateAchievementProgress. The SDK packs
 * callbacks to 8 bytes on Windows: uint64 game ID at 0, bool group achievement at 8, char name[128] at 9, uint32 current
 * progress at 140, uint32 maximum at 144, 152 bytes in all. A progress of 0 out of 0 is an unlock. */
enum { SHC_ACHIEVEMENT_STORED = 1103, SHC_ACHIEVEMENT_NAME_MAX = 128 };
typedef struct {
  uint32_t app_id;                        /* the game ID's app (its low 24 bits) */
  char name[SHC_ACHIEVEMENT_NAME_MAX + 1]; /* the achievement's API name, always ended */
  uint32_t progress, max_progress;
  int unlocked; /* progress and max_progress both 0 */
} ShcAchievement;
/* 1 when the message is long enough and names an achievement. */
int ShcParseAchievementStored(const void* data, size_t size, ShcAchievement* achievement);

/* UserStatsReceived_t (k_iSteamUserStatsCallbacks + 1 = 1101): uint64 game ID, EResult at 8, then the user's CSteamID.
 * The SDK's 8-byte packing puts the user at 16 (24 bytes); Valve's client sends it packed to 4 bytes, the user at 12
 * (20 bytes), as for ValidateAuthTicketResponse_t. The user, by the message's size; 0 when it is shorter than 20. */
uint64_t ShcStatsReceivedUser(const void* data, size_t size);

/* The achievements already shown in this session, by name: Valve's client may post the same unlock more than once (a
 * game that stores its stats again). ShcAchievementFirst returns 1 the first time it sees a name, and 1 for every name
 * once it has seen SHC_ACHIEVEMENTS_TRACKED of them (a duplicate notification is better than a missing one). */
enum { SHC_ACHIEVEMENTS_TRACKED = 256 };
typedef struct {
  uint64_t hashes[SHC_ACHIEVEMENTS_TRACKED];
  int count;
} ShcAchievementsShown;
int ShcAchievementFirst(ShcAchievementsShown* shown, const char* name);

/* "Achievement unlocked: <name>" (the achievement's display name, UTF-8, control characters as spaces), or
 * "Achievement unlocked." without one, at most cap - 1 bytes, never ending inside a UTF-8 character. */
void ShcAchievementText(const char* name, char* out, size_t cap);

typedef struct {
  uint32_t state[8];
  uint64_t bytes;
  uint8_t block[64];
  size_t used;
} ShcSha256;

void ShcSha256Init(ShcSha256* hash);
void ShcSha256Update(ShcSha256* hash, const void* data, size_t n);
void ShcSha256Final(ShcSha256* hash, uint8_t digest[32]);
/* 64 lower-case hex digits and a NUL. */
void ShcHex(const uint8_t digest[32], char out[65]);
int ShcTextEqual(const char* a, const char* b);
/* 0 when text starts with prefix (like strncmp), 1 otherwise. */
int ShcTextEqualPrefix(const char* text, const char* prefix);

#ifdef __cplusplus
}
#endif

#endif
