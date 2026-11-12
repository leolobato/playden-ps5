/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Playden's Steam host: steam.exe for the folder that holds Valve's Steam client files.
 *
 * It runs beside a game set to Steam features = Online, in a second process on the game's Wine server (the title's
 * [helper]). It loads Valve's unmodified steamclient64.dll, signs the account in with the session
 * Playden handed over, tells Windows programs where Steam is (the registry values the Steamworks library reads) and
 * keeps the client's callbacks running. The game's own steam_api then finds a running Steam, as on a PC.
 *
 * It never starts the game, never changes Valve's files and never writes the token anywhere. Valve's client DLL is
 * used only when its SHA-256 is the one Playden recorded when it installed the client (the session file carries it):
 * the folder is open to everyone. Playden installs Valve's current client, so the calls below go through positions
 * that a newer build may have moved; the log says which version ran.
 *
 * Files, all in the game's bottle:
 *   C:\playden\online-session.txt  read once, then deleted unless it says keep=1 (runtime/online_session.h)
 *   C:\playden\online-launch.txt   one Play's friend lobby to join and friends to invite; always deleted
 *   C:\playden\steam-host.ready    written when the game may start: "ok", or "failed <reason>"
 *   C:\playden\steam-host.log      what happened; never a token
 *   C:\playden\steam-host.ready.notify  when it is there (Playden makes it for Logging = Verbose): one line per step,
 *                                   which the title shows as a notification while it waits
 *   C:\playden\steam-host.ready.say  the title's messages file (below): for a session with notice=1, the account's
 *                                   name as it went online, which the title shows once the game runs
 *   C:\playden\steam-host.no-sign-in  a test switch: the client is loaded and the game starts, but nobody signs in
 *
 * No C runtime: kernel32 and advapi32 only (the .def files beside build.sh). 64-bit only; a 32-bit game loads Valve's
 * 32-bit client DLL in its own process and reaches this one the same way. */

#include <stddef.h>
#include <stdint.h>

#include "shc.h"

#define kClientDll L"steamclient64.dll"

/* ---- Win32 ------------------------------------------------------------------------------------- */

typedef int BOOL;
typedef unsigned long DWORD;
typedef void* HANDLE;
typedef uint16_t WCHAR;
#define WINAPI __stdcall
#define IMPORT __declspec(dllimport)

IMPORT BOOL WINAPI CloseHandle(HANDLE);
IMPORT HANDLE WINAPI CreateFileW(const WCHAR*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
IMPORT BOOL WINAPI DeleteFileW(const WCHAR*);
IMPORT void WINAPI ExitProcess(unsigned);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT DWORD WINAPI GetModuleFileNameW(HANDLE, WCHAR*, DWORD);
IMPORT void* WINAPI GetProcAddress(HANDLE, const char*);
IMPORT void WINAPI GetSystemTimeAsFileTime(uint64_t*);
IMPORT uint64_t WINAPI GetTickCount64(void);
IMPORT HANDLE WINAPI LoadLibraryW(const WCHAR*);
IMPORT BOOL WINAPI MoveFileExW(const WCHAR*, const WCHAR*, DWORD);
IMPORT BOOL WINAPI ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
IMPORT BOOL WINAPI SetCurrentDirectoryW(const WCHAR*);
IMPORT BOOL WINAPI SetDllDirectoryW(const WCHAR*);
IMPORT BOOL WINAPI SetEnvironmentVariableW(const WCHAR*, const WCHAR*);
IMPORT DWORD WINAPI SleepEx(DWORD, BOOL);
IMPORT BOOL WINAPI WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);

IMPORT long WINAPI RegCloseKey(HANDLE);
IMPORT long WINAPI RegCreateKeyExW(HANDLE, const WCHAR*, DWORD, WCHAR*, DWORD, DWORD, void*, HANDLE*, DWORD*);
IMPORT long WINAPI RegSetValueExW(HANDLE, const WCHAR*, DWORD, DWORD, const void*, DWORD);

#define GENERIC_READ 0x80000000ul
#define GENERIC_WRITE 0x40000000ul
#define FILE_SHARE_READ 1ul
#define FILE_SHARE_DELETE 4ul
#define FILE_APPEND_DATA 4ul
#define FILE_SHARE_WRITE 2ul
#define CREATE_ALWAYS 2ul
#define OPEN_EXISTING 3ul
#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)
#define MOVEFILE_REPLACE_EXISTING 1ul
#define HKEY_CURRENT_USER ((HANDLE)(uintptr_t)0x80000001u)
#define HKEY_LOCAL_MACHINE ((HANDLE)(uintptr_t)0x80000002u)
#define KEY_SET_VALUE 2ul
#define REG_SZ 1ul
#define REG_DWORD 4ul

void* memcpy(void* dst, const void* src, size_t n) {
  uint8_t* d = dst;
  const uint8_t* s = src;
  while (n--) *d++ = *s++;
  return dst;
}
void* memset(void* dst, int value, size_t n) {
  volatile uint8_t* d = dst; /* volatile: wiping a token must not be optimised away */
  while (n--) *d++ = (uint8_t)value;
  return dst;
}
int _fltused = 0;

/* ---- Text and the log -------------------------------------------------------------------------- */

static HANDLE g_log = INVALID_HANDLE;
static uint64_t g_started; /* GetTickCount64 at the host's start: the log's times count from it */
static WCHAR g_dir[520]; /* this program's folder: Valve's client files, without a trailing backslash */

static size_t Length(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
static size_t WideLength(const WCHAR* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
/* out = a + b; 0 when it does not fit. */
static int Join(WCHAR* out, size_t cap, const WCHAR* a, const WCHAR* b) {
  const size_t an = WideLength(a), bn = WideLength(b);
  if (an + bn + 1 > cap) return 0;
  memcpy(out, a, an * sizeof(WCHAR));
  memcpy(out + an, b, (bn + 1) * sizeof(WCHAR));
  return 1;
}
static void Widen(WCHAR* out, size_t cap, const char* s) {
  size_t i = 0;
  for (; s[i] && i + 1 < cap; ++i) out[i] = (WCHAR)(uint8_t)s[i];
  out[i] = 0;
}

typedef struct {
  char text[512];
  size_t n;
} Line;
static void Put(Line* line, const char* s) {
  while (*s && line->n + 2 < sizeof line->text) line->text[line->n++] = *s++;
}
static void PutNumber(Line* line, uint64_t value) {
  char digits[24];
  int n = 0;
  do digits[n++] = (char)('0' + value % 10), value /= 10;
  while (value);
  while (n && line->n + 2 < sizeof line->text) line->text[line->n++] = digits[--n];
}
static void PutWide(Line* line, const WCHAR* s) {
  while (*s && line->n + 2 < sizeof line->text) line->text[line->n++] = *s < 128 ? (char)*s : '?', ++s;
}
static void Emit(Line* line) {
  DWORD written = 0;
  line->text[line->n++] = '\n';
  if (g_log != INVALID_HANDLE) WriteFile(g_log, line->text, (DWORD)line->n, &written, 0);
  line->n = 0;
}
static void Log(const char* text) {
  Line line = {{0}, 0};
  Put(&line, text);
  Emit(&line);
}
static void LogValue(const char* text, uint64_t value) {
  Line line = {{0}, 0};
  Put(&line, text);
  PutNumber(&line, value);
  Emit(&line);
}

/* A step the title shows while it waits, when the notify file is there (debugging, Logging = Verbose). */
static void Step(const char* text) {
  DWORD written = 0;
  HANDLE file = CreateFileW(L"C:\\playden\\steam-host.ready.notify", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                            OPEN_EXISTING, 0, 0);
  if (file == INVALID_HANDLE) return;
  WriteFile(file, text, (DWORD)Length(text), &written, 0);
  WriteFile(file, "\n", 1, &written, 0);
  CloseHandle(file);
}

/* The file the title waits for: written once, when the game may start. */
static void Ready(const char* status) {
  static int written;
  DWORD count = 0;
  HANDLE file;
  if (written) return;
  written = 1;
  file = CreateFileW(L"C:\\playden\\steam-host.ready.tmp", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
  if (file == INVALID_HANDLE) {
    LogValue("ready: cannot write the file, error=", GetLastError());
    return;
  }
  WriteFile(file, status, (DWORD)Length(status), &count, 0);
  WriteFile(file, "\n", 1, &count, 0);
  CloseHandle(file);
  MoveFileExW(L"C:\\playden\\steam-host.ready.tmp", L"C:\\playden\\steam-host.ready", MOVEFILE_REPLACE_EXISTING);
  {
    Line line = {{0}, 0};
    Put(&line, "ready: ");
    Put(&line, status);
    Put(&line, " at ms=");
    PutNumber(&line, GetTickCount64() - g_started);
    Emit(&line);
  }
}

/* A failure the game should not wait for: say so and leave cleanly. A crash here would end the game with it. */
static void Fail(const char* reason) {
  Line line = {{0}, 0};
  Put(&line, "failed ");
  Put(&line, reason);
  line.text[line.n] = 0;
  Ready(line.text);
  ExitProcess(0);
}

/* ---- The session file -------------------------------------------------------------------------- */

#define kHandoffPath L"C:\\playden\\online-session.txt"

static ShcSession g_session;

static int ReadSession(void) {
  static char text[8192];
  DWORD read = 0;
  int parsed, keep;
  HANDLE file = CreateFileW(kHandoffPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
  if (file == INVALID_HANDLE) {
    Log("session: none handed over");
    return 0;
  }
  if (!ReadFile(file, text, sizeof text - 1, &read, 0)) read = 0;
  CloseHandle(file);
  parsed = ShcParseSession(text, read, &g_session);
  keep = g_session.keep;
  memset(text, 0, sizeof text);
  if (!keep && !DeleteFileW(kHandoffPath)) LogValue("session: the file was NOT deleted, error=", GetLastError());
  Log(parsed ? (keep ? "session: read, file kept for the Home screen" : "session: read, file deleted")
             : "session: the file is incomplete");
  if (!parsed) ShcWipeSession(&g_session);
  return parsed;
}

/* The console's clock in Unix seconds, as Playden wrote the one-launch file with. */
static int64_t UnixNow(void) {
  uint64_t ticks = 0; /* FILETIME: 100 ns since 1601 */
  GetSystemTimeAsFileTime(&ticks);
  return (int64_t)(ticks / 10000000u) - 11644473600LL;
}

/* What a Play from Playden asked for once (runtime/online_session.h OnlineLaunch): a friend's lobby to join and friends
 * to invite to the first lobby the game is in. The file goes whatever it says, so a later start from the Home screen
 * never sees it; one written more than two minutes away from now is ignored. Steam IDs never go into the log. */
static ShcLaunch g_launch;

static void TakeLaunchRequest(void) {
  static char text[4096];
  DWORD read = 0;
  HANDLE file = CreateFileW(L"C:\\playden\\online-launch.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, 0,
                            OPEN_EXISTING, 0, 0);
  if (file == INVALID_HANDLE) return;
  if (!ReadFile(file, text, sizeof text - 1, &read, 0)) read = 0;
  CloseHandle(file);
  DeleteFileW(L"C:\\playden\\online-launch.txt");
  const int taken = ShcParseLaunch(text, read, UnixNow(), &g_launch);
  memset(text, 0, sizeof text);
  if (!taken) {
    Log("launch request: ignored (nothing asked, or not written within two minutes of now)");
    return;
  }
  {
    Line line = {{0}, 0};
    Put(&line, "launch request: join a friend's lobby=");
    PutNumber(&line, g_launch.connect_lobby ? 1 : 0);
    Put(&line, " invite friends=");
    PutNumber(&line, (uint64_t)g_launch.invite_count);
    Emit(&line);
  }
}

/* ---- Valve's client ---------------------------------------------------------------------------- */

/* The version Playden installed, from its marker beside this program ("<version> <host sha256>"). */
static void LogClientVersion(void) {
  static char text[256];
  static WCHAR path[600];
  DWORD read = 0;
  HANDLE file;
  Line line = {{0}, 0};
  if (!Join(path, 600, g_dir, L"\\.playden-steam-client")) return;
  file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  if (file == INVALID_HANDLE) {
    Log("client: version unknown (no marker)");
    return;
  }
  if (!ReadFile(file, text, sizeof text - 1, &read, 0)) read = 0;
  CloseHandle(file);
  Put(&line, "client: version=");
  for (DWORD i = 0; i < read && text[i] > ' ' && text[i] < 127 && line.n + 2 < sizeof line.text; ++i) line.text[line.n++] = text[i];
  Emit(&line);
}

/* steamclient64.dll is the one Playden installed: its SHA-256 is the one in the session file. */
static int ClientIsInstalled(const WCHAR* path) {
  static uint8_t chunk[1 << 16];
  ShcSha256 hash;
  uint8_t digest[32];
  char hex[65];
  DWORD read = 0;
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  if (file == INVALID_HANDLE) {
    LogValue("client: cannot open steamclient64.dll, error=", GetLastError());
    return 0;
  }
  ShcSha256Init(&hash);
  while (ReadFile(file, chunk, sizeof chunk, &read, 0) && read) ShcSha256Update(&hash, chunk, read);
  CloseHandle(file);
  ShcSha256Final(&hash, digest);
  ShcHex(digest, hex);
  if (g_session.client_sha256[0] && ShcTextEqual(hex, g_session.client_sha256)) return 1;
  {
    Line line = {{0}, 0};
    Put(&line, g_session.client_sha256[0] ? "client: steamclient64.dll is not the file Playden installed, sha256="
                                          : "client: the session names no client file, sha256=");
    Put(&line, hex);
    Emit(&line);
  }
  return 0;
}

static void SetValue(HANDLE root, const WCHAR* key_name, const WCHAR* name, DWORD type, const void* data, DWORD size) {
  HANDLE key = 0;
  if (RegCreateKeyExW(root, key_name, 0, 0, 0, KEY_SET_VALUE, 0, &key, 0) != 0) {
    Line line = {{0}, 0};
    Put(&line, "registry: cannot open ");
    PutWide(&line, key_name);
    Emit(&line);
    return;
  }
  RegSetValueExW(key, name, 0, type, data, size);
  RegCloseKey(key);
}
static void SetText(HANDLE root, const WCHAR* key, const WCHAR* name, const WCHAR* value) {
  SetValue(root, key, name, REG_SZ, value, (DWORD)((WideLength(value) + 1) * sizeof(WCHAR)));
}
static void SetNumber(HANDLE root, const WCHAR* key, const WCHAR* name, DWORD value) {
  SetValue(root, key, name, REG_DWORD, &value, sizeof value);
}

/* Where a Windows program looks for a running Steam: the Steamworks library reads the process ID and the client
 * DLLs' paths from ActiveProcess, and loads the DLL for its own architecture. */
static void WriteRegistry(void) {
  static const WCHAR kSteam[] = L"Software\\Valve\\Steam";
  static const WCHAR kActive[] = L"Software\\Valve\\Steam\\ActiveProcess";
  static WCHAR path[600], account[160];
  if (Join(path, 600, g_dir, L"\\steamclient.dll")) SetText(HKEY_CURRENT_USER, kActive, L"SteamClientDll", path);
  if (Join(path, 600, g_dir, L"\\steamclient64.dll")) SetText(HKEY_CURRENT_USER, kActive, L"SteamClientDll64", path);
  SetNumber(HKEY_CURRENT_USER, kActive, L"pid", GetCurrentProcessId());
  SetText(HKEY_CURRENT_USER, kActive, L"Universe", L"Public");
  SetText(HKEY_CURRENT_USER, kSteam, L"SteamPath", g_dir);
  if (Join(path, 600, g_dir, L"\\steam.exe")) SetText(HKEY_CURRENT_USER, kSteam, L"SteamExe", path);
  Widen(account, 160, g_session.account);
  SetText(HKEY_CURRENT_USER, kSteam, L"AutoLoginUser", account);
  SetText(HKEY_LOCAL_MACHINE, kSteam, L"InstallPath", g_dir);
  SetNumber(HKEY_LOCAL_MACHINE, kSteam, L"SteamPID", GetCurrentProcessId());
  SetText(HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Valve\\Steam", L"InstallPath", g_dir);
}

static void SetEnvironment(void) {
  static WCHAR number[24], account[160], slashes[520];
  Line digits = {{0}, 0};
  size_t i = 0;
  PutNumber(&digits, g_session.app_id);
  digits.text[digits.n] = 0;
  Widen(number, 24, digits.text);
  Widen(account, 160, g_session.account);
  for (; g_dir[i]; ++i) slashes[i] = g_dir[i] == '\\' ? '/' : g_dir[i];
  slashes[i] = 0;
  SetEnvironmentVariableW(L"SteamPath", g_dir);
  SetEnvironmentVariableW(L"ValvePlatformMutex", slashes);
  SetEnvironmentVariableW(L"SteamAppId", number);
  SetEnvironmentVariableW(L"SteamGameId", number);
  SetEnvironmentVariableW(L"SteamOverlayGameId", number);
  SetEnvironmentVariableW(L"SteamUser", account);
  SetEnvironmentVariableW(L"SteamAppUser", account);
  SetEnvironmentVariableW(L"SteamClientLaunch", L"1");
  SetEnvironmentVariableW(L"SteamEnv", L"1");
  /* No shader pre-caching: it starts Valve's GPU driver query program, and this process has no GPU and cannot start
   * programs. With it on, the client asks for the query at every start and logs the failure. */
  SetEnvironmentVariableW(L"STEAM_ENABLE_SHADER_CACHE_MANAGEMENT", L"0");
}

/* The client's private interfaces, called by position (pointer-sized slots), as in build 1788652215 and 1792618285, with the
 * arguments that build's IClientUser proxies serialize: SetLoginToken(token, account name),
 * SetAccountNameForCachedCredentialLogin(account name, bool), BIsSubscribedApp(app id) -> bool. A missing argument is
 * whatever its register holds. */
typedef struct {
  int user;
  int id;
  void* data;
  int size;
} CallbackMessage;

enum {
  kEngineCreateGlobalUser = 0x10 / 8,
  kEngineGetUser = 0x40 / 8,
  kEngineGetFriends = 0x68 / 8,
  kEngineGetUtils = 0x70 / 8, /* GetIClientUtils(pipe) */
  kEngineGetSharedConnection = 0x228 / 8, /* GetIClientSharedConnection(user, pipe) */
  kSharedAllocate = 0x00 / 8,             /* AllocateSharedConnection() -> handle */
  kSharedSendMessage = 0x10 / 8,          /* SendMessage(handle, bytes, size) -> bool */
  kUtilsSetAppIDForCurrentPipe = 0x90 / 8, /* (app ID, track the process: bool) */
  kUtilsGetAppID = 0x98 / 8,
  kUserLogOn = 0x08 / 8,
  kUserLogOff = 0x18 / 8,
  kUserLoggedOn = 0x20 / 8,
  kUserSetLanguage = 0x160 / 8,
  kUserHasCachedCredentials = 0x188 / 8,
  kUserSetAccountNameForCachedCredentialLogin = 0x190 / 8,
  kUserSetLoginToken = 0x1c0 / 8,
  kUserIsSubscribedApp = 0x5a8 / 8,
  kFriendsSetPersonaState = 0x28 / 8,
  /* IClientFriends::NotifyLobbyJoinRequested(app ID, lobby, friend) -> bool, the same position in builds 1788652215
   * and 1792618285. By its name and arguments, what Steam's own interface calls when the player accepts an invite or
   * picks Join, so the running game gets GameLobbyJoinRequested_t; not yet seen doing that. */
  kFriendsNotifyLobbyJoinRequested = 0x610 / 8,
};

/* Valve's public interfaces, by the Steamworks SDK's headers: their layout is fixed for each version name. */
enum {
  kClientGetFriends = 8,           /* ISteamClient (SteamClient017 to 023): GetISteamFriends(user, pipe, version) */
  kClientGetMatchmaking = 10,      /* GetISteamMatchmaking(user, pipe, version) */
  kGetPersonaName = 0,             /* SteamFriends017: GetPersonaName() -> the account's own UTF-8 name */
  kGetFriendPersonaName = 7,       /* SteamFriends017: GetFriendPersonaName(CSteamID) -> UTF-8 name */
  kInviteUserToLobby = 16,         /* SteamMatchMaking009: InviteUserToLobby(CSteamID lobby, CSteamID friend) -> bool */
  kGetNumLobbyMembers = 17,        /* GetNumLobbyMembers(CSteamID lobby) -> int: 0 for a lobby the account isn't in */
};

/* Callbacks the host looks at, from the SDK's headers (k_iSteamFriendsCallbacks 300, k_iSteamMatchmakingCallbacks 500). */
enum {
  kSteamServersConnected = 101,  /* the client is connected to Steam again */
  kGameLobbyJoinRequested = 333, /* CSteamID lobby, CSteamID friend */
  kLobbyInvite = 503,            /* uint64 inviter, uint64 lobby, uint64 game ID */
  kLobbyEnter = 504,             /* uint64 lobby, uint32 permissions, bool locked, uint32 response (1: success) at 16 */
  kLobbyDataUpdate = 505,        /* uint64 lobby, uint64 member (the lobby itself for the lobby's data), bool success */
  kLobbyChatUpdate = 506,        /* uint64 lobby, uint64 changed, uint64 making the change, uint32 state at 24 */
  kLobbyCreated = 513,           /* uint32 result (1: OK), uint64 lobby at 8 */
};

static void* Slot(void* object, int slot) { return (*(void***)object)[slot]; }
#define CALL0(object, slot) ((void* (*)(void*))Slot((object), (slot)))((object))
#define CALL1(object, slot, a) ((void* (*)(void*, void*))Slot((object), (slot)))((object), (void*)(a))
#define CALL2(object, slot, a, b) ((void* (*)(void*, void*, void*))Slot((object), (slot)))((object), (void*)(a), (void*)(b))
#define CALL3(object, slot, a, b, c)                                                                                     \
  ((void* (*)(void*, void*, void*, void*))Slot((object), (slot)))((object), (void*)(a), (void*)(b), (void*)(c))

static void* g_user;
static void* g_friends;        /* the client's own IClientFriends */
static void* g_public_friends; /* ISteamFriends, SteamFriends017 */
static void* g_matchmaking;    /* ISteamMatchmaking, SteamMatchMaking009 */
static int g_pipe;
static BOOL (*g_get_callback)(int, CallbackMessage*, int*);
static void (*g_free_callback)(int);

static void HandleCallback(const CallbackMessage* message);

static void Pump(void) {
  CallbackMessage message;
  int call = 0;
  if (!g_get_callback || !g_free_callback) return;
  for (int drained = 0; drained < 256 && g_get_callback(g_pipe, &message, &call); ++drained) {
    HandleCallback(&message);
    g_free_callback(g_pipe);
  }
}

/* Waits for the sign-in until `deadline` (GetTickCount64); the sleeps are alertable so this thread also takes what the
 * server queues for it. */
static int WaitLoggedOn(uint64_t deadline) {
  while (GetTickCount64() < deadline) {
    Pump();
    if ((int)(intptr_t)CALL0(g_user, kUserLoggedOn)) return 1;
    SleepEx(50, 1);
  }
  return 0;
}

/* The game's Steam library refuses to start when the client does not yet know the account owns the game. Right after a
 * first sign-in (no saved one) the client still has to fetch the account's licenses, so the game would start without
 * Steam and connect half-way later (TF2 then crashed). Wait for the client to know the app is owned, until `deadline`
 * (GetTickCount64). It took about 2.5 s on the console; an account that doesn't own the app (or a game it only borrows)
 * may never say so, so the wait is short. Returns the milliseconds it took, or -1. */
enum {
  kOwnedWaitMs = 10000,     /* after the sign-in */
  kOwnedDeadlineMs = 50000, /* from the host's start: the title waits 60 s from starting the second process */
};

/* Both sign-in attempts end within the title's wait, from the host's start: the client's saved sign-in gets the first
 * 20 s, the token what is left of 45 s. A sign-in that ended after the game started would connect it half-way. */
enum {
  kCachedSignInDeadlineMs = 20000,
  kSignInDeadlineMs = 45000,
};

static int64_t WaitOwned(uint32_t app_id, uint64_t deadline) {
  const uint64_t start = GetTickCount64();
  while (GetTickCount64() < deadline) {
    Pump();
    if ((uint8_t)(uintptr_t)CALL1(g_user, kUserIsSubscribedApp, (uintptr_t)app_id)) return (int64_t)(GetTickCount64() - start);
    SleepEx(50, 1);
  }
  return -1;
}

/* ---- Lobbies, joins and invites while the game runs -------------------------------------------- */

typedef void* (*CreateInterface)(const char*, int*);

static ShcLobbyWatch g_lobbies;
static int g_game_started;     /* "ready" written: the title lets the game start */
static void* g_engine;
static int g_user_handle;
static void* g_shared;              /* IClientSharedConnection */
static uint32_t g_shared_connection; /* its handle, allocated once */

/* Friends' online states reach a connection only once it has set a status itself, and Valve's client never does on its
 * own: on a PC the Steam window's Friends list does, through the client's shared connection. The host sends the same
 * message, with the status Playden's Friends tab chose (online, or invisible: friends see the account offline). Without
 * it the game's friends list shows nobody online and friends see the account offline. */
static uint32_t Persona(void) {
  return g_session.persona == SHC_PERSONA_INVISIBLE ? SHC_PERSONA_INVISIBLE : SHC_PERSONA_ONLINE;
}
static void ShowOnline(const char* why) {
  Line line = {{0}, 0};
  uint8_t message[16];
  const size_t size = ShcChangeStatusMessage(Persona(), message, sizeof message);
  int sent = 0;
  if (!g_shared && g_engine) {
    g_shared = CALL2(g_engine, kEngineGetSharedConnection, (intptr_t)g_user_handle, (intptr_t)g_pipe);
    if (g_shared) g_shared_connection = (uint32_t)(uintptr_t)CALL0(g_shared, kSharedAllocate);
  }
  if (g_shared && g_shared_connection && size)
    sent = (uint8_t)(uintptr_t)CALL3(g_shared, kSharedSendMessage, (uintptr_t)g_shared_connection, message, (uintptr_t)size);
  Put(&line, Persona() == SHC_PERSONA_INVISIBLE ? "persona: invisible for friends (" : "persona: online for friends (");
  Put(&line, why);
  Put(&line, "), shared connection=");
  PutNumber(&line, g_shared_connection);
  Put(&line, " sent=");
  PutNumber(&line, (uint64_t)sent);
  Emit(&line);
}
static uint64_t g_join_due;    /* GetTickCount64 at which Playden's Join goes to the game; 0 when none is pending */
static uint32_t g_callbacks_seen[96];
static int g_callbacks_seen_count;

/* Playden's Join goes to the game this long after "ready": the client doesn't say when the game's Steam library is up
 * (it never reports the account's own game to the host), and a join posted before the library listens is lost. */
enum { kJoinAfterReadyMs = 20000 };

static uint64_t Field64(const CallbackMessage* message, int offset) {
  uint64_t value = 0;
  if (message->data && message->size >= offset + 8) memcpy(&value, (const uint8_t*)message->data + offset, 8);
  return value;
}
static uint32_t Field32(const CallbackMessage* message, int offset) {
  uint32_t value = 0;
  if (message->data && message->size >= offset + 4) memcpy(&value, (const uint8_t*)message->data + offset, 4);
  return value;
}
/* A game ID's app (CGameID: the app in its low 24 bits). */
static uint32_t GameApp(uint64_t game_id) { return (uint32_t)(game_id & 0xffffffu); }

static const char* InvitesName(int invites) {
  switch (invites) {
    case SHC_INVITES_JOIN: return "join";
    case SHC_INVITES_ASK: return "ask";
    case SHC_INVITES_SHOW: return "show";
    default: return "off";
  }
}

/* Which callbacks reach the host's own pipe: each one's number, once, with the time (never its contents). The client's
 * own callbacks are numbered from 1000000. */
static void NoteCallback(int id) {
  Line line = {{0}, 0};
  for (int i = 0; i < g_callbacks_seen_count; ++i)
    if (g_callbacks_seen[i] == (uint32_t)id) return;
  if (g_callbacks_seen_count == (int)(sizeof g_callbacks_seen / sizeof g_callbacks_seen[0])) return;
  g_callbacks_seen[g_callbacks_seen_count++] = (uint32_t)id;
  Put(&line, "callback: first id=");
  PutNumber(&line, (uint64_t)(unsigned)id);
  Put(&line, " at ms=");
  PutNumber(&line, GetTickCount64() - g_started);
  Emit(&line);
}

/* ---- The title while the game runs (prospero-win's src/pw_helper_channel.h) ----------------------------------- */

/* The title makes the messages file when it shows them, and the answer file once it can ask the player (L3 + R3). A
 * title without them gets nothing, and Game invites falls back as its row says. */
#define kTitleSay L"C:\\playden\\steam-host.ready.say"
#define kTitleAsk L"C:\\playden\\steam-host.ready.ask"
#define kTitleAskTemporary L"C:\\playden\\steam-host.ready.ask.tmp"
#define kTitleAnswer L"C:\\playden\\steam-host.ready.answer"

static int TitleHas(const WCHAR* path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
  if (file == INVALID_HANDLE) return 0;
  CloseHandle(file);
  return 1;
}

/* A message the title shows as a notification. The text (a friend's name, say) never goes into the log. */
static void Say(const char* text) {
  DWORD written = 0;
  HANDLE file = CreateFileW(kTitleSay, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
  if (file == INVALID_HANDLE) return;
  WriteFile(file, text, (DWORD)Length(text), &written, 0);
  WriteFile(file, "\n", 1, &written, 0);
  CloseHandle(file);
}

/* The open question: a friend's invite for Game invites = Ask, until the title answers or the time is up. */
static struct {
  uint64_t id, lobby, inviter, give_up;
} g_question;
static uint64_t g_question_ids;

/* Asks the player through the title; the answer comes back in its answer file (CheckAnswer). */
static int Ask(const char* text) {
  char line[400];
  DWORD written = 0;
  HANDLE file;
  ShcQuestionLine(g_question_ids + 1, text, line, sizeof line);
  file = CreateFileW(kTitleAskTemporary, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
  if (file == INVALID_HANDLE) return 0;
  WriteFile(file, line, (DWORD)Length(line), &written, 0);
  CloseHandle(file);
  if (!MoveFileExW(kTitleAskTemporary, kTitleAsk, MOVEFILE_REPLACE_EXISTING)) return 0;
  return (int)++g_question_ids;
}

static const char* FriendName(uint64_t steam_id) {
  return g_public_friends && steam_id ? (const char*)CALL1(g_public_friends, kGetFriendPersonaName, steam_id) : 0;
}

/* Asks Valve's client to pass a lobby to join to the running game, as Steam does when the player accepts an invite or
 * picks Join. */
static void JoinLobby(uint64_t lobby, uint64_t friend_id, const char* why) {
  Line line = {{0}, 0};
  char text[256];
  int answered;
  if (!g_friends) {
    Log("join: Valve's client has no friends interface");
    return;
  }
  ShcInviteText(SHC_TEXT_JOINING, FriendName(friend_id), text, sizeof text);
  Say(text);
  answered = (uint8_t)(uintptr_t)CALL3(g_friends, kFriendsNotifyLobbyJoinRequested, (uintptr_t)g_session.app_id, lobby,
                                       friend_id);
  Put(&line, "join: asked Valve's client to pass a lobby to the game (");
  Put(&line, why);
  Put(&line, "), client answered=");
  PutNumber(&line, (uint64_t)answered);
  Put(&line, " at ms=");
  PutNumber(&line, GetTickCount64() - g_started);
  Emit(&line);
}

/* The account is in a lobby: the game created or joined it. The launch's friends are invited to the first one, once. */
static void InLobby(uint64_t lobby, const char* how) {
  Line line = {{0}, 0};
  int sent = 0;
  if (!lobby) return;
  if (lobby == g_launch.connect_lobby && g_join_due) {
    g_join_due = 0;
    Log("join: the game is in the friend's lobby already");
  }
  if (!ShcLobbySeen(&g_lobbies, lobby)) return;
  Put(&line, "lobby: the game is in a lobby (from ");
  Put(&line, how);
  Put(&line, ") at ms=");
  PutNumber(&line, GetTickCount64() - g_started);
  Emit(&line);
  if (!g_launch.invite_count) return;
  if (g_matchmaking) {
    for (int i = 0; i < g_launch.invite_count; ++i)
      sent += (uint8_t)(uintptr_t)CALL2(g_matchmaking, kInviteUserToLobby, lobby, g_launch.invites[i]) != 0;
    line.n = 0;
    Put(&line, "invites: sent=");
    PutNumber(&line, (uint64_t)sent);
    Put(&line, " of ");
    PutNumber(&line, (uint64_t)g_launch.invite_count);
    Put(&line, " to the game's first lobby");
    Emit(&line);
    if (sent) {
      char text[128];
      ShcInvitedText(sent, text, sizeof text);
      Say(text);
    }
  } else {
    Log("invites: Valve's client has no matchmaking interface; nobody was invited");
  }
  memset(g_launch.invites, 0, sizeof g_launch.invites);
  g_launch.invite_count = 0;
}

/* A friend's invite while the game runs, as Game invites says. A title that can't ask the player or show the host's
 * messages makes Ask and Only show leave the invite, as the Game invites row says for such a title. */
static void Invited(uint64_t inviter, uint64_t lobby, uint64_t game_id) {
  Line line = {{0}, 0};
  char text[256];
  const int action = ShcInviteAction(g_session.invites, TitleHas(kTitleAnswer), TitleHas(kTitleSay));
  Put(&line, "invite: from a friend at ms=");
  PutNumber(&line, GetTickCount64() - g_started);
  if (GameApp(game_id) != g_session.app_id) {
    Put(&line, " to another game: left to Steam");
  } else if (!g_game_started) {
    Put(&line, " before the game started: left alone");
  } else if (action == SHC_INVITE_JOIN) {
    Put(&line, ", Game invites=");
    Put(&line, InvitesName(g_session.invites));
    Emit(&line);
    JoinLobby(lobby, inviter, "a friend's invite");
    return;
  } else if (action == SHC_INVITE_ASK) {
    int id;
    ShcInviteText(SHC_TEXT_ASK, FriendName(inviter), text, sizeof text);
    id = Ask(text);
    Put(&line, ", Game invites=ask: ");
    if (id) {
      g_question.id = (uint64_t)id;
      g_question.lobby = lobby;
      g_question.inviter = inviter;
      g_question.give_up = GetTickCount64() + (SHC_QUESTION_SECONDS + 10) * 1000u;
      Put(&line, "asked the player, question=");
      PutNumber(&line, (uint64_t)id);
    } else {
      Put(&line, "the title could not be asked");
    }
  } else if (action == SHC_INVITE_SHOW) {
    ShcInviteText(SHC_TEXT_SHOW, FriendName(inviter), text, sizeof text);
    Say(text);
    Put(&line, ", Game invites=show: shown");
  } else {
    Put(&line, ", Game invites=");
    Put(&line, InvitesName(g_session.invites));
    Put(&line, ": left alone");
  }
  Emit(&line);
}

/* The title's answer to the open question: an accepted invite is joined. */
static void CheckAnswer(uint64_t now) {
  char text[64];
  DWORD read = 0;
  uint64_t id = 0;
  int accepted = 0;
  HANDLE file;
  if (!g_question.id) return;
  file = CreateFileW(kTitleAnswer, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
  if (file != INVALID_HANDLE) {
    if (!ReadFile(file, text, sizeof text - 1, &read, 0)) read = 0;
    CloseHandle(file);
  }
  if (read && ShcParseAnswer(text, read, &id, &accepted) && id == g_question.id) {
    LogValue(accepted ? "invite: the player accepted question=" : "invite: the question ran out, question=", id);
    if (accepted) JoinLobby(g_question.lobby, g_question.inviter, "a friend's invite, accepted");
    memset(&g_question, 0, sizeof g_question);
  } else if (now >= g_question.give_up) {
    LogValue("invite: no answer from the title, question=", g_question.id);
    memset(&g_question, 0, sizeof g_question);
  }
}

static void HandleCallback(const CallbackMessage* message) {
  NoteCallback(message->id);
  switch (message->id) {
    case kSteamServersConnected:
      /* A new connection starts offline for friends again. */
      if (g_game_started) ShowOnline("connected again");
      break;
    case kLobbyInvite:
      Invited(Field64(message, 0), Field64(message, 8), Field64(message, 16));
      break;
    case kLobbyEnter:
      if (Field32(message, 16) == 1) InLobby(Field64(message, 0), "a lobby entered");
      break;
    case kLobbyDataUpdate: {
      /* The one the host gets when the game makes or joins a lobby (the game's own LobbyCreated and LobbyEnter go to
       * the game's pipe). It also comes for lobbies the game only looked at: only one the account is in counts. */
      const uint64_t lobby = Field64(message, 0), member = Field64(message, 8);
      if ((member == lobby || member == g_session.steam_id) && g_matchmaking &&
          (int)(intptr_t)CALL1(g_matchmaking, kGetNumLobbyMembers, lobby) > 0)
        InLobby(lobby, "the lobby's data");
      break;
    }
    case kLobbyCreated:
      if (Field32(message, 0) == 1) InLobby(Field64(message, 8), "a lobby created");
      break;
    case kLobbyChatUpdate:
      if (Field64(message, 8) == g_session.steam_id && (Field32(message, 24) & 1)) InLobby(Field64(message, 0), "a member update");
      break;
    default:
      break;
  }
}

/* Valve's public interfaces on the host's own user and pipe: friends (names) and matchmaking (invites). Only for a Play
 * that asked for a join or invites, or a game whose Game invites isn't Off, and friends alone for the online notice
 * (notice=1): otherwise a start calls nothing more than sign-in and the ownership check. */
static void OpenPublicInterfaces(CreateInterface create, void* engine, int user_handle) {
  static const char* const kVersions[] = {"SteamClient023", "SteamClient021", "SteamClient020", "SteamClient017"};
  void* client = 0;
  void* utils;
  int error = 0;
  const int lobbies = g_launch.connect_lobby || g_launch.invite_count || g_session.invites != SHC_INVITES_OFF;
  if (!lobbies && !g_session.notice) return;
  /* The host's pipe is the game's app too (not tracked as its process): the client then tells it when the account's
   * lobbies change (LobbyDataUpdate), which it doesn't for a pipe without an app. */
  utils = lobbies ? CALL1(engine, kEngineGetUtils, (intptr_t)g_pipe) : 0;
  if (utils && g_session.app_id) {
    CALL2(utils, kUtilsSetAppIDForCurrentPipe, (uintptr_t)g_session.app_id, (uintptr_t)0);
    LogValue("lobbies: the host's pipe is for app=", (uint64_t)(uint32_t)(uintptr_t)CALL0(utils, kUtilsGetAppID));
  }
  for (unsigned i = 0; i < sizeof kVersions / sizeof kVersions[0] && !client; ++i) client = create(kVersions[i], &error);
  if (!client) {
    Log("lobbies: Valve's client has no public client interface");
    return;
  }
  g_public_friends = CALL3(client, kClientGetFriends, (intptr_t)user_handle, (intptr_t)g_pipe, "SteamFriends017");
  if (lobbies)
    g_matchmaking = CALL3(client, kClientGetMatchmaking, (intptr_t)user_handle, (intptr_t)g_pipe, "SteamMatchMaking009");
  {
    Line line = {{0}, 0};
    Put(&line, "lobbies: friends interface=");
    PutNumber(&line, g_public_friends ? 1 : 0);
    Put(&line, " matchmaking interface=");
    PutNumber(&line, g_matchmaking ? 1 : 0);
    Put(&line, " Game invites=");
    Put(&line, InvitesName(g_session.invites));
    Emit(&line);
  }
}

/* Valve's client does a game's peer-to-peer (ISteamNetworking: Portal 2's co-op) itself, here, and it finds no way to
 * reach the other player without the console's network adapters. iphlpapi lists them through nsiproxy's \\.\Nsi
 * device, which only the runtime kit's hosted mode serves on the console: a 64-bit game hosts it (prospero-win patch
 * 1003), a 32-bit game can't, and this second process of the title gets nothing. So the host serves it, before the
 * client loads, the same way: a game that also hosts it finds the device there and uses this one. Not for a game set
 * to go without its network adapters (adapters=off). An older kit without the hosted mode, or a failure, is logged and
 * the start goes on as before. */
/* The online notice (notice=1): once the game may start, the account's name as Valve's client has it (the session's
 * account name when it has none) and how friends see it, through the title's messages file, which the title shows once
 * the game runs. A title without that file shows nothing. The name never goes into the log. */
static void TellOnline(void) {
  char text[256];
  const char* name = g_public_friends ? (const char*)CALL0(g_public_friends, kGetPersonaName) : 0;
  const int from_client = name && name[0];
  if (!g_session.notice) return;
  if (!TitleHas(kTitleSay)) {
    Log("notice: the title shows no messages");
    return;
  }
  ShcOnlineText(Persona(), from_client ? name : g_session.account, text, sizeof text);
  Say(text);
  Log(from_client ? "notice: online, with the client's name for the account" : "notice: online, with the account name");
}

static void HostNetworkAdapters(void) {
  HANDLE kernel;
  long (*host_start)(const WCHAR* driver, DWORD timeout_ms) = 0;
  if (g_session.adapters_off) {
    Log("adapters: not listed, the game is set to go without them");
    return;
  }
  /* ntoskrnl.exe reads the mode once, as it loads. */
  SetEnvironmentVariableW(L"WINE_NTOSKRNL_HOSTED", L"nsiproxy");
  kernel = LoadLibraryW(L"ntoskrnl.exe");
  if (kernel) host_start = (long (*)(const WCHAR*, DWORD))GetProcAddress(kernel, "wine_ntoskrnl_host_start");
  if (!host_start)
    Log("adapters: the runtime kit has no hosted drivers; peer-to-peer may not connect");
  else
    LogValue("adapters: nsiproxy hosted for the client, status=", (uint64_t)(uint32_t)host_start(L"nsiproxy", 0));
  SetEnvironmentVariableW(L"WINE_NTOSKRNL_HOSTED", 0);
}

void WINAPI Entry(void) {
  static WCHAR client_path[600];
  CreateInterface create;
  HANDLE client;
  void* engine;
  int user_handle, error = 0, cached = 0, logged_on;
  DWORD n, cut = 0;
  const uint64_t started = GetTickCount64();
  g_started = started;

  g_log = CreateFileW(L"C:\\playden\\steam-host.log", GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);
  DeleteFileW(L"C:\\playden\\steam-host.ready");
  n = GetModuleFileNameW(0, g_dir, 512);
  for (DWORD i = 0; i < n; ++i)
    if (g_dir[i] == '\\' || g_dir[i] == '/') cut = i;
  g_dir[cut] = 0;
  {
    Line line = {{0}, 0};
    Put(&line, "start: playden-steam-host pid=");
    PutNumber(&line, GetCurrentProcessId());
    Put(&line, " folder=");
    PutWide(&line, g_dir);
    Emit(&line);
  }

  TakeLaunchRequest();
  if (!ReadSession()) Fail("no Steam session from Playden");
  LogValue("session: app=", g_session.app_id);
  LogClientVersion();
  if (!Join(client_path, 600, g_dir, L"\\" kClientDll)) Fail("path too long");
  if (!ClientIsInstalled(client_path)) {
    ShcWipeSession(&g_session);
    Fail("Valve's client files changed since Playden installed them");
  }

  SetEnvironment();
  WriteRegistry();
  HostNetworkAdapters();
  SetCurrentDirectoryW(g_dir);
  SetDllDirectoryW(g_dir);
  if (!(client = LoadLibraryW(client_path))) {
    LogValue("client: cannot load steamclient64.dll, error=", GetLastError());
    ShcWipeSession(&g_session);
    Fail("Valve's client did not load");
  }
  create = (CreateInterface)GetProcAddress(client, "CreateInterface");
  g_get_callback = (BOOL (*)(int, CallbackMessage*, int*))GetProcAddress(client, "Steam_BGetCallback");
  g_free_callback = (void (*)(int))GetProcAddress(client, "Steam_FreeLastCallback");
  engine = create ? create("CLIENTENGINE_INTERFACE_VERSION005", &error) : 0;
  if (!engine || !g_get_callback || !g_free_callback) {
    LogValue("client: no engine interface, error=", (uint64_t)(unsigned)error);
    ShcWipeSession(&g_session);
    Fail("Valve's client has no engine interface");
  }
  user_handle = (int)(intptr_t)CALL1(engine, kEngineCreateGlobalUser, &g_pipe);
  if (!user_handle || !g_pipe) {
    ShcWipeSession(&g_session);
    Fail("Valve's client made no user");
  }
  g_user = CALL2(engine, kEngineGetUser, (intptr_t)user_handle, (intptr_t)g_pipe);
  g_friends = CALL2(engine, kEngineGetFriends, (intptr_t)user_handle, (intptr_t)g_pipe);
  if (!g_user) {
    ShcWipeSession(&g_session);
    Fail("Valve's client has no user interface");
  }
  LogValue("client: global user pipe=", (uint64_t)(unsigned)g_pipe);
  Step("Steam: client loaded");

  /* A test switch for console runs that must not sign the account in: the client is loaded and set up, the game
   * starts, and the token is never used. */
  {
    HANDLE probe = CreateFileW(L"C:\\playden\\steam-host.no-sign-in", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (probe != INVALID_HANDLE) {
      CloseHandle(probe);
      ShcWipeSession(&g_session);
      Log("logon: skipped, steam-host.no-sign-in is there");
      Ready("ok no sign-in");
      for (;;) {
        Pump();
        SleepEx(50, 1);
      }
    }
  }

  /* The client's own saved sign-in first, only for a session kept for the Home screen, then the token. Playden removes
   * the saved sign-in of a game that signs in at each Play; one it could not remove is not used. */
  if (g_session.keep && (int)(intptr_t)CALL1(g_user, kUserHasCachedCredentials, g_session.account))
    cached = (int)(intptr_t)CALL2(g_user, kUserSetAccountNameForCachedCredentialLogin, g_session.account, 0) != 0;
  if (!cached) CALL2(g_user, kUserSetLoginToken, g_session.token, g_session.account);
  Log(cached ? "logon: with the client's saved sign-in" : "logon: with the session's token");
  Step("Steam: signing in");
  CALL1(g_user, kUserLogOn, (uintptr_t)g_session.steam_id);
  logged_on = WaitLoggedOn(started + (cached ? kCachedSignInDeadlineMs : kSignInDeadlineMs));
  if (!logged_on && cached) {
    Log("logon: the saved sign-in did not work, trying the token");
    CALL2(g_user, kUserSetLoginToken, g_session.token, g_session.account);
    CALL1(g_user, kUserLogOn, (uintptr_t)g_session.steam_id);
    logged_on = WaitLoggedOn(started + kSignInDeadlineMs);
  }
  memset(g_session.token, 0, sizeof g_session.token);
  if (!logged_on) {
    CALL0(g_user, kUserLogOff);
    Fail("sign-in timed out");
  }
  Log("logon: signed in");
  Step("Steam: signed in");
  if (g_friends) CALL1(g_friends, kFriendsSetPersonaState, (intptr_t)Persona());
  g_engine = engine;
  g_user_handle = user_handle;
  ShowOnline("signed in");
  CALL1(g_user, kUserSetLanguage, "english");
  WriteRegistry();
  OpenPublicInterfaces(create, engine, user_handle);
  if (g_session.app_id) {
    uint64_t deadline = GetTickCount64() + kOwnedWaitMs;
    if (deadline > started + kOwnedDeadlineMs) deadline = started + kOwnedDeadlineMs;
    LogValue("ownership: waiting up to 10 s for the client to know this account owns app=", g_session.app_id);
    const int64_t waited = WaitOwned(g_session.app_id, deadline);
    if (waited >= 0) {
      LogValue("ownership: the client knows the app is owned after ms=", (uint64_t)waited);
      Step("Steam: game owned");
    } else {
      Log("ownership: the client did not report the app as owned within 10 s (the account may not own it, or only "
          "borrows it through family sharing); the game starts now and may run without Steam");
      Step("Steam: game not reported as owned");
    }
  }
  Ready("ok");
  Step("Steam: ready");
  g_game_started = 1;
  TellOnline();
  /* Playden's Join: Valve's client is asked to pass it to the game once its Steam library is likely up. */
  if (g_launch.connect_lobby) g_join_due = GetTickCount64() + kJoinAfterReadyMs;

  /* Stay for the game: it ends with the title. */
  {
    uint64_t next_answer = 0;
    for (;;) {
      const uint64_t now = GetTickCount64();
      Pump();
      if (g_join_due && now >= g_join_due) {
        g_join_due = 0;
        JoinLobby(g_launch.connect_lobby, 0, "Playden's Join");
      }
      if (now >= next_answer) {
        CheckAnswer(now);
        next_answer = now + 500;
      }
      SleepEx(50, 1);
    }
  }
}
