/* Playden's online bridge: a steam_api64.dll (or 32-bit steam_api.dll) that hands every call to gbe_fork
 * (steam_api64_gbe.dll / steam_api_gbe.dll beside it) and adds what the emulator cannot do, a logged-on
 * Steam session for the game:
 *
 *   ISteamUser: real session, Web API and encrypted app tickets, activated with Steam through the auth
 *     list; BeginAuthSession/EndAuthSession put other players' tickets on the list for Steam to check.
 *   ISteamMatchmaking: lobbies on Steam's matchmaking servers (create, list with filters, join, leave,
 *     data, member data, chat, game server, owner, invites).
 *   ISteamFriends: the account's friends, their names, states and the game or lobby they are in.
 *   Presence: the account shows online and in the game; an invite to a lobby is accepted at once (there
 *     is no Steam overlay on the console).
 *   ISteamUtils: the bridge's own API calls complete through IsAPICallCompleted/GetAPICallResult, and its
 *     callbacks reach the game through SteamAPI_RunCallbacks or manual dispatch.
 *
 * Steam networking (P2P through Steam's relays) is not bridged: the log says when a game asks for it.
 * The session logs on to a CM over WebSocket (WinHTTP) with the refresh token Playden writes to
 * steam_settings/playden_online.txt, on a thread of its own. Built freestanding (no C runtime) by build.sh;
 * online.c and the generated stubs are the whole DLL. The log (steam_settings/playden_online.log) never
 * holds a token or ticket bytes. */

#include <stddef.h>
#include <stdint.h>

#include "slots.h"

/* Interface methods: thiscall on 32-bit Windows (this in ECX), the usual convention on 64-bit. */
#if defined(_WIN64)
#define METHOD
#define kEmulatorName L"steam_api64_gbe.dll"
#else
#define METHOD __attribute__((thiscall))
#define kEmulatorName L"steam_api_gbe.dll"
#endif

/* ---- Win32 ---------------------------------------------------------------------------------- */

typedef int BOOL;
typedef unsigned long DWORD;
typedef void* HANDLE;
typedef void* HINTERNET;
typedef uint16_t WCHAR;
#define WINAPI __stdcall
#define DECLSPEC_IMPORT __declspec(dllimport)

typedef struct {
  void* DebugInfo;
  long LockCount, RecursionCount;
  HANDLE OwningThread, LockSemaphore;
  uintptr_t SpinCount;
} CRITICAL_SECTION;

DECLSPEC_IMPORT void* WINAPI AddVectoredExceptionHandler(DWORD, long(WINAPI*)(void*));
DECLSPEC_IMPORT BOOL WINAPI CloseHandle(HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI CreateFileW(const WCHAR*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI CreateThread(void*, size_t, DWORD(WINAPI*)(void*), void*, DWORD, DWORD*);
DECLSPEC_IMPORT void WINAPI EnterCriticalSection(CRITICAL_SECTION*);
DECLSPEC_IMPORT DWORD WINAPI GetModuleFileNameW(HANDLE, WCHAR*, DWORD);
DECLSPEC_IMPORT BOOL WINAPI GetModuleHandleExW(DWORD, const void*, HANDLE*);
DECLSPEC_IMPORT void* WINAPI GetProcAddress(HANDLE, const char*);
DECLSPEC_IMPORT HANDLE WINAPI GetProcessHeap(void);
DECLSPEC_IMPORT void WINAPI GetSystemTimeAsFileTime(uint64_t*);
DECLSPEC_IMPORT uint64_t WINAPI GetTickCount64(void);
DECLSPEC_IMPORT void* WINAPI HeapAlloc(HANDLE, DWORD, size_t);
DECLSPEC_IMPORT BOOL WINAPI HeapFree(HANDLE, DWORD, void*);
DECLSPEC_IMPORT void WINAPI InitializeCriticalSection(CRITICAL_SECTION*);
DECLSPEC_IMPORT void WINAPI LeaveCriticalSection(CRITICAL_SECTION*);
DECLSPEC_IMPORT HANDLE WINAPI LoadLibraryW(const WCHAR*);
DECLSPEC_IMPORT BOOL WINAPI ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
DECLSPEC_IMPORT void WINAPI Sleep(DWORD);
DECLSPEC_IMPORT BOOL WINAPI WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);

DECLSPEC_IMPORT BOOL WINAPI WinHttpCloseHandle(HINTERNET);
DECLSPEC_IMPORT HINTERNET WINAPI WinHttpConnect(HINTERNET, const WCHAR*, uint16_t, DWORD);
DECLSPEC_IMPORT HINTERNET WINAPI WinHttpOpen(const WCHAR*, DWORD, const WCHAR*, const WCHAR*, DWORD);
DECLSPEC_IMPORT HINTERNET WINAPI WinHttpOpenRequest(HINTERNET, const WCHAR*, const WCHAR*, const WCHAR*,
                                                    const WCHAR*, const WCHAR**, DWORD);
DECLSPEC_IMPORT BOOL WINAPI WinHttpQueryHeaders(HINTERNET, DWORD, const WCHAR*, void*, DWORD*, DWORD*);
DECLSPEC_IMPORT BOOL WINAPI WinHttpReadData(HINTERNET, void*, DWORD, DWORD*);
DECLSPEC_IMPORT BOOL WINAPI WinHttpReceiveResponse(HINTERNET, void*);
DECLSPEC_IMPORT BOOL WINAPI WinHttpSendRequest(HINTERNET, const WCHAR*, DWORD, void*, DWORD, DWORD, uintptr_t);
DECLSPEC_IMPORT BOOL WINAPI WinHttpSetOption(HINTERNET, DWORD, void*, DWORD);
DECLSPEC_IMPORT BOOL WINAPI WinHttpSetTimeouts(HINTERNET, int, int, int, int);
DECLSPEC_IMPORT DWORD WINAPI WinHttpWebSocketClose(HINTERNET, uint16_t, void*, DWORD);
DECLSPEC_IMPORT HINTERNET WINAPI WinHttpWebSocketCompleteUpgrade(HINTERNET, uintptr_t);
DECLSPEC_IMPORT DWORD WINAPI WinHttpWebSocketReceive(HINTERNET, void*, DWORD, DWORD*, int*);
DECLSPEC_IMPORT DWORD WINAPI WinHttpWebSocketSend(HINTERNET, int, void*, DWORD);

#define GENERIC_READ 0x80000000ul
#define FILE_APPEND_DATA 4ul
#define FILE_SHARE_READ 1ul
#define FILE_SHARE_WRITE 2ul
#define OPEN_EXISTING 3ul
#define OPEN_ALWAYS 4ul
#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)
#define WINHTTP_FLAG_SECURE 0x00800000ul
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114ul
#define WINHTTP_QUERY_STATUS_CODE 19ul
#define WINHTTP_QUERY_FLAG_NUMBER 0x20000000ul
#define WS_BINARY_MESSAGE 0
#define WS_BINARY_FRAGMENT 1
#define WS_CLOSE 4

/* ---- Freestanding basics ---------------------------------------------------------------------- */

void* memcpy(void* dst, const void* src, size_t n) {
  uint8_t* d = dst;
  const uint8_t* s = src;
  while (n--) *d++ = *s++;
  return dst;
}
void* memset(void* dst, int value, size_t n) {
  uint8_t* d = dst;
  while (n--) *d++ = (uint8_t)value;
  return dst;
}
void* memmove(void* dst, const void* src, size_t n) {
  uint8_t* d = dst;
  const uint8_t* s = src;
  if (d < s) while (n--) *d++ = *s++;
  else while (n--) d[n] = s[n];
  return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
  const uint8_t *x = a, *y = b;
  for (; n; --n, ++x, ++y)
    if (*x != *y) return *x < *y ? -1 : 1;
  return 0;
}
int _fltused = 0;

static void* Alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 8 /* HEAP_ZERO_MEMORY */, n ? n : 1); }
static void Free(void* p) {
  if (p) HeapFree(GetProcessHeap(), 0, p);
}
static size_t Strlen(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
static int StartsWith(const char* s, const char* prefix) {
  while (*prefix)
    if (*s++ != *prefix++) return 0;
  return 1;
}

typedef struct {
  uint8_t* p;
  size_t n, cap;
  int failed;
} Buf;

static void BufReserve(Buf* b, size_t extra) {
  if (b->failed || b->n + extra <= b->cap) return;
  size_t cap = b->cap ? b->cap * 2 : 256;
  while (cap < b->n + extra) cap *= 2;
  uint8_t* p = Alloc(cap);
  if (!p) {
    b->failed = 1;
    return;
  }
  if (b->n) memcpy(p, b->p, b->n);
  Free(b->p);
  b->p = p;
  b->cap = cap;
}
static void BufPut(Buf* b, const void* data, size_t n) {
  BufReserve(b, n);
  if (b->failed) return;
  memcpy(b->p + b->n, data, n);
  b->n += n;
}
static void BufByte(Buf* b, uint8_t v) { BufPut(b, &v, 1); }
static void BufLE32(Buf* b, uint32_t v) {
  for (int i = 0; i < 4; ++i) BufByte(b, (uint8_t)(v >> (8 * i)));
}
static void BufLE64(Buf* b, uint64_t v) {
  for (int i = 0; i < 8; ++i) BufByte(b, (uint8_t)(v >> (8 * i)));
}
static void BufFree(Buf* b) {
  Free(b->p);
  b->p = 0;
  b->n = b->cap = 0;
  b->failed = 0;
}
static uint32_t LE32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- Log -------------------------------------------------------------------------------------- */

static CRITICAL_SECTION g_log_lock;
static HANDLE g_log = INVALID_HANDLE;

static void LogText(Buf* line, const char* text) { BufPut(line, text, Strlen(text)); }
/* 64-bit division without the C runtime's helpers, which 32-bit code would call. */
static uint64_t UDiv64(uint64_t n, uint32_t d, uint32_t* remainder) {
  uint64_t q = 0, r = 0;
  for (int i = 63; i >= 0; --i) {
    r = r << 1 | ((n >> i) & 1);
    if (r >= d) {
      r -= d;
      q |= (uint64_t)1 << i;
    }
  }
  if (remainder) *remainder = (uint32_t)r;
  return q;
}
static void LogNumber(Buf* line, uint64_t value) {
  char digits[24];
  int n = 0;
  do {
    uint32_t digit = 0;
    value = UDiv64(value, 10, &digit);
    digits[n++] = (char)('0' + digit);
  } while (value && n < 24);
  while (n) BufByte(line, (uint8_t)digits[--n]);
}
static void LogLine(Buf* line) {
  BufByte(line, '\n');
  if (!line->failed && g_log != INVALID_HANDLE) {
    EnterCriticalSection(&g_log_lock);
    DWORD written = 0;
    WriteFile(g_log, line->p, (DWORD)line->n, &written, 0);
    LeaveCriticalSection(&g_log_lock);
  }
  BufFree(line);
}
static void Log(const char* text) {
  Buf line = {0};
  LogText(&line, text);
  LogLine(&line);
}
static void LogValue(const char* text, uint64_t value) {
  Buf line = {0};
  LogText(&line, text);
  LogNumber(&line, value);
  LogLine(&line);
}

/* ---- Crash trail -------------------------------------------------------------------------------- */

/* A fatal exception in the game process is written to the log with the module and offset it happened in and the
 * modules whose code the stack points into, so a crash during online play can be placed without a debugger. */
static void LogHex(Buf* line, uint64_t value) {
  BufPut(line, "0x", 2);
  int started = 0;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const unsigned digit = (unsigned)(value >> shift) & 15u;
    if (!digit && !started && shift) continue;
    started = 1;
    BufByte(line, (uint8_t)(digit < 10 ? '0' + digit : 'a' + digit - 10));
  }
}
/* Appends "name+0xoffset" for the module containing address, or the bare address when none does. */
static int LogModuleOffset(Buf* line, const void* address) {
  HANDLE module = 0;
  WCHAR path[260];
  if (!GetModuleHandleExW(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, address, &module) || !module) {
    LogHex(line, (uintptr_t)address);
    return 0;
  }
  const DWORD n = GetModuleFileNameW(module, path, 260);
  DWORD start = 0;
  for (DWORD i = 0; i < n; ++i)
    if (path[i] == '\\' || path[i] == '/') start = i + 1;
  for (DWORD i = start; i < n; ++i) BufByte(line, (uint8_t)(path[i] < 128 ? path[i] : '?'));
  BufByte(line, '+');
  LogHex(line, (uintptr_t)address - (uintptr_t)module);
  return 1;
}
static volatile long g_crash_logged;
static long WINAPI CrashTrail(void* pointers) {
  void** info = (void**)pointers;
  const DWORD* record = (const DWORD*)info[0];
  const uint8_t* context = (const uint8_t*)info[1];
  const DWORD code = record[0];
  /* Access violations, illegal and privileged instructions, stack overflow, fast-fail and heap corruption. */
  if (code != 0xC0000005 && code != 0xC000001D && code != 0xC0000096 && code != 0xC00000FD && code != 0xC0000409 &&
      code != 0xC0000374 && code != 0xC0000094)
    return 0;
  if (g_crash_logged >= 6) return 0;
  ++g_crash_logged;
#if defined(_WIN64)
  const uintptr_t sp = *(const uintptr_t*)(context + 0x98);
  const uintptr_t ip = *(const uintptr_t*)(context + 0xF8);
#else
  const uintptr_t sp = *(const uintptr_t*)(context + 0xC4);
  const uintptr_t ip = *(const uintptr_t*)(context + 0xB8);
#endif
  Buf line = {0};
  LogText(&line, "CRASH exception=");
  LogHex(&line, code);
  LogText(&line, " at ");
  LogModuleOffset(&line, (const void*)ip);
  if (code == 0xC0000005 && record[4] >= 2) {
    const uintptr_t* parameters = (const uintptr_t*)((const uint8_t*)record + (sizeof(void*) == 8 ? 32 : 20));
    LogText(&line, parameters[0] == 0 ? " reading " : parameters[0] == 1 ? " writing " : " executing ");
    LogHex(&line, parameters[1]);
  }
  LogLine(&line);
  /* Stack words that point into a module's code are likely return addresses, nearest first. */
  Buf trail = {0};
  LogText(&trail, "CRASH stack:");
  int found = 0;
  for (int i = 0; i < 512 && found < 14; ++i) {
    const uintptr_t word = ((const uintptr_t*)sp)[i];
    if (word < 0x10000) continue;
    Buf one = {0};
    if (LogModuleOffset(&one, (const void*)word)) {
      BufByte(&trail, ' ');
      BufPut(&trail, one.p, one.n);
      ++found;
    }
    BufFree(&one);
  }
  LogLine(&trail);
  return 0;
}


/* ---- Protobuf --------------------------------------------------------------------------------- */

static void PbVarint(Buf* b, uint64_t v) {
  while (v >= 0x80) {
    BufByte(b, (uint8_t)(v | 0x80));
    v >>= 7;
  }
  BufByte(b, (uint8_t)v);
}
static void PbKey(Buf* b, uint32_t field, uint32_t wire) { PbVarint(b, (uint64_t)field << 3 | wire); }
static void PbUint(Buf* b, uint32_t field, uint64_t v) {
  PbKey(b, field, 0);
  PbVarint(b, v);
}
static void PbFixed32(Buf* b, uint32_t field, uint32_t v) {
  PbKey(b, field, 5);
  BufLE32(b, v);
}
static void PbFixed64(Buf* b, uint32_t field, uint64_t v) {
  PbKey(b, field, 1);
  BufLE64(b, v);
}
static void PbBytes(Buf* b, uint32_t field, const void* data, size_t n) {
  PbKey(b, field, 2);
  PbVarint(b, n);
  BufPut(b, data, n);
}
static void PbString(Buf* b, uint32_t field, const char* s) { PbBytes(b, field, s, Strlen(s)); }

typedef struct {
  const uint8_t* p;
  size_t n, at;
  int bad;
} PbReader;
typedef struct {
  uint32_t number, wire;
  uint64_t value;
  const uint8_t* data;
  size_t size;
} PbField;

static int PbReadVarint(PbReader* r, uint64_t* out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (r->at >= r->n) return 0;
    const uint8_t byte = r->p[r->at++];
    v |= (uint64_t)(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      *out = v;
      return 1;
    }
  }
  return 0;
}
/* The next field; 0 at the end or on malformed input (r->bad). */
static int PbNext(PbReader* r, PbField* f) {
  if (r->bad || r->at >= r->n) return 0;
  uint64_t key = 0;
  if (!PbReadVarint(r, &key)) goto bad;
  f->number = (uint32_t)(key >> 3);
  f->wire = (uint32_t)(key & 7);
  f->value = 0;
  f->data = 0;
  f->size = 0;
  switch (f->wire) {
    case 0:
      if (!PbReadVarint(r, &f->value)) goto bad;
      return 1;
    case 1:
      if (r->n - r->at < 8) goto bad;
      f->value = LE32(r->p + r->at) | (uint64_t)LE32(r->p + r->at + 4) << 32;
      r->at += 8;
      return 1;
    case 5:
      if (r->n - r->at < 4) goto bad;
      f->value = LE32(r->p + r->at);
      r->at += 4;
      return 1;
    case 2: {
      uint64_t size = 0;
      if (!PbReadVarint(r, &size) || size > r->n - r->at) goto bad;
      f->data = r->p + r->at;
      f->size = (size_t)size;
      r->at += (size_t)size;
      return 1;
    }
    default:
      goto bad;
  }
bad:
  r->bad = 1;
  return 0;
}

/* ---- gzip (RFC 1951/1952), for compressed Multi messages --------------------------------------- */

typedef struct {
  uint16_t counts[16];
  uint16_t symbols[320];
} Huffman;
typedef struct {
  const uint8_t* src;
  size_t srclen, pos;
  uint32_t bitbuf;
  int bitcnt;
  uint8_t* dst;
  size_t dstlen, dpos;
} Inflate;

static int GetBit(Inflate* s) {
  if (!s->bitcnt) {
    if (s->pos >= s->srclen) return -1;
    s->bitbuf = s->src[s->pos++];
    s->bitcnt = 8;
  }
  const int bit = (int)(s->bitbuf & 1);
  s->bitbuf >>= 1;
  --s->bitcnt;
  return bit;
}
static int GetBits(Inflate* s, int n) {
  int value = 0;
  for (int i = 0; i < n; ++i) {
    const int bit = GetBit(s);
    if (bit < 0) return -1;
    value |= bit << i;
  }
  return value;
}
static void BuildHuffman(Huffman* h, const uint8_t* lengths, int n) {
  uint16_t offsets[17];
  memset(h->counts, 0, sizeof h->counts);
  for (int i = 0; i < n; ++i) h->counts[lengths[i]]++;
  h->counts[0] = 0;
  offsets[1] = 0;
  for (int i = 1; i < 16; ++i) offsets[i + 1] = (uint16_t)(offsets[i] + h->counts[i]);
  for (int i = 0; i < n; ++i)
    if (lengths[i]) h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
}
static int Decode(Inflate* s, const Huffman* h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; ++len) {
    const int bit = GetBit(s);
    if (bit < 0) return -1;
    code |= bit;
    const int count = h->counts[len];
    if (code - count < first) return h->symbols[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}
static int InflateCodes(Inflate* s, const Huffman* lit, const Huffman* dist) {
  static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int symbol = Decode(s, lit);
    if (symbol < 0) return 0;
    if (symbol < 256) {
      if (s->dpos >= s->dstlen) return 0;
      s->dst[s->dpos++] = (uint8_t)symbol;
    } else if (symbol == 256) {
      return 1;
    } else {
      symbol -= 257;
      if (symbol >= 29) return 0;
      const int extra = GetBits(s, kLenExtra[symbol]);
      const int d = Decode(s, dist);
      if (extra < 0 || d < 0 || d >= 30) return 0;
      const int dextra = GetBits(s, kDistExtra[d]);
      if (dextra < 0) return 0;
      const size_t length = kLenBase[symbol] + (size_t)extra;
      const size_t distance = kDistBase[d] + (size_t)dextra;
      if (distance > s->dpos || length > s->dstlen - s->dpos) return 0;
      for (size_t i = 0; i < length; ++i, ++s->dpos) s->dst[s->dpos] = s->dst[s->dpos - distance];
    }
  }
}
static int InflateRaw(Inflate* s) {
  static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  Huffman lit, dist;
  int last;
  do {
    last = GetBit(s);
    const int type = GetBits(s, 2);
    if (last < 0 || type < 0) return 0;
    if (type == 0) {
      s->bitcnt = 0;
      if (s->srclen - s->pos < 4) return 0;
      const size_t len = s->src[s->pos] | s->src[s->pos + 1] << 8;
      const size_t nlen = s->src[s->pos + 2] | s->src[s->pos + 3] << 8;
      s->pos += 4;
      if (len != (~nlen & 0xffff) || len > s->srclen - s->pos || len > s->dstlen - s->dpos) return 0;
      memcpy(s->dst + s->dpos, s->src + s->pos, len);
      s->pos += len;
      s->dpos += len;
      continue;
    }
    uint8_t lengths[320];
    if (type == 1) {
      int i = 0;
      for (; i < 144; ++i) lengths[i] = 8;
      for (; i < 256; ++i) lengths[i] = 9;
      for (; i < 280; ++i) lengths[i] = 7;
      for (; i < 288; ++i) lengths[i] = 8;
      BuildHuffman(&lit, lengths, 288);
      for (i = 0; i < 30; ++i) lengths[i] = 5;
      BuildHuffman(&dist, lengths, 30);
    } else if (type == 2) {
      const int nlen = GetBits(s, 5) + 257, ndist = GetBits(s, 5) + 1, ncode = GetBits(s, 4) + 4;
      if (nlen < 257 || ndist < 1 || ncode < 4 || nlen > 286 || ndist > 30) return 0;
      memset(lengths, 0, sizeof lengths);
      for (int i = 0; i < ncode; ++i) {
        const int v = GetBits(s, 3);
        if (v < 0) return 0;
        lengths[kOrder[i]] = (uint8_t)v;
      }
      Huffman code;
      BuildHuffman(&code, lengths, 19);
      int index = 0;
      while (index < nlen + ndist) {
        int symbol = Decode(s, &code);
        if (symbol < 0) return 0;
        if (symbol < 16) {
          lengths[index++] = (uint8_t)symbol;
          continue;
        }
        uint8_t value = 0;
        int repeat;
        if (symbol == 16) {
          if (!index) return 0;
          value = lengths[index - 1];
          repeat = 3 + GetBits(s, 2);
        } else if (symbol == 17) {
          repeat = 3 + GetBits(s, 3);
        } else {
          repeat = 11 + GetBits(s, 7);
        }
        if (repeat < 3 || index + repeat > nlen + ndist) return 0;
        while (repeat--) lengths[index++] = value;
      }
      BuildHuffman(&lit, lengths, nlen);
      BuildHuffman(&dist, lengths + nlen, ndist);
    } else {
      return 0;
    }
    if (!InflateCodes(s, &lit, &dist)) return 0;
  } while (!last);
  return 1;
}
static int Gunzip(const uint8_t* src, size_t n, size_t expected, Buf* out) {
  if (n < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8 || expected > 8u << 20) return 0;
  const uint8_t flags = src[3];
  size_t at = 10;
  if (flags & 4) {
    if (n - at < 2) return 0;
    at += 2 + (src[at] | src[at + 1] << 8);
  }
  if (flags & 8) while (at < n && src[at++]) {}
  if (flags & 16) while (at < n && src[at++]) {}
  if (flags & 2) at += 2;
  if (at >= n) return 0;
  BufReserve(out, expected);
  if (out->failed) return 0;
  Inflate s = {src + at, n - at, 0, 0, 0, out->p, expected, 0};
  if (!InflateRaw(&s) || s.dpos != expected) return 0;
  out->n = expected;
  return 1;
}

/* ---- Configuration ---------------------------------------------------------------------------- */

static WCHAR g_dir[520];  /* this DLL's folder, with a trailing backslash */
static struct {
  int online;
  uint32_t app_id;
  uint64_t steam_id;
  char account[128];
  char token[2048];
} g_config;

static void PathIn(WCHAR* out, size_t cap, const WCHAR* name) {
  size_t n = 0;
  for (; g_dir[n] && n + 1 < cap; ++n) out[n] = g_dir[n];
  for (; *name && n + 1 < cap; ++n) out[n] = *name++;
  out[n] = 0;
}
static uint64_t ParseNumber(const char* s, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (uint64_t)(s[i] - '0');
  return v;
}
static void CopyValue(char* out, size_t cap, const char* s, size_t n) {
  if (n >= cap) n = 0;
  memcpy(out, s, n);
  out[n] = 0;
}
/* steam_settings/playden_online.txt: app_id=, steam_id=, account_name=, refresh_token= lines. */
static void LoadConfig(void) {
  WCHAR path[600];
  PathIn(path, 600, L"steam_settings\\playden_online.txt");
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  if (file == INVALID_HANDLE) return;
  static char text[8192];
  DWORD read = 0;
  const BOOL ok = ReadFile(file, text, sizeof text - 1, &read, 0);
  CloseHandle(file);
  if (!ok) return;
  size_t at = 0;
  while (at < read) {
    size_t end = at;
    while (end < read && text[end] != '\n' && text[end] != '\r') ++end;
    const char* line = text + at;
    const size_t n = end - at;
    size_t eq = 0;
    while (eq < n && line[eq] != '=') ++eq;
    if (eq < n) {
      const char* value = line + eq + 1;
      const size_t vn = n - eq - 1;
      if (eq == 6 && StartsWith(line, "app_id")) g_config.app_id = (uint32_t)ParseNumber(value, vn);
      else if (eq == 8 && StartsWith(line, "steam_id")) g_config.steam_id = ParseNumber(value, vn);
      else if (eq == 12 && StartsWith(line, "account_name")) CopyValue(g_config.account, sizeof g_config.account, value, vn);
      else if (eq == 13 && StartsWith(line, "refresh_token")) CopyValue(g_config.token, sizeof g_config.token, value, vn);
    }
    at = end + 1;
  }
  memset(text, 0, sizeof text);
  g_config.online = g_config.app_id && g_config.steam_id && g_config.account[0] && g_config.token[0];
}

/* ---- CM session ------------------------------------------------------------------------------- */

enum {
  kEMsgMulti = 1,
  kEMsgClientHeartBeat = 703,
  kEMsgClientChangeStatus = 716,
  kEMsgClientGamesPlayed = 742,
  kEMsgClientLogOnResponse = 751,
  kEMsgClientLoggedOff = 757,
  kEMsgClientPersonaState = 766,
  kEMsgClientFriendsList = 767,
  kEMsgClientGameConnectTokens = 779,
  kEMsgClientRequestFriendData = 815,
  kEMsgClientGetAppOwnershipTicket = 857,
  kEMsgClientGetAppOwnershipTicketResponse = 858,
  kEMsgClientFriendMsgIncoming = 5427,
  kEMsgClientTicketAuthComplete = 5429,
  kEMsgClientAuthList = 5432,
  kEMsgClientLogon = 5514,
  kEMsgClientRequestEncryptedAppTicket = 5526,
  kEMsgClientRequestEncryptedAppTicketResponse = 5527,
  kEMsgClientAuthListAck = 5575,
  kEMsgClientMMSCreateLobby = 6601,
  kEMsgClientMMSCreateLobbyResponse = 6602,
  kEMsgClientMMSJoinLobby = 6603,
  kEMsgClientMMSJoinLobbyResponse = 6604,
  kEMsgClientMMSLeaveLobby = 6605,
  kEMsgClientMMSLeaveLobbyResponse = 6606,
  kEMsgClientMMSGetLobbyList = 6607,
  kEMsgClientMMSGetLobbyListResponse = 6608,
  kEMsgClientMMSSetLobbyData = 6609,
  kEMsgClientMMSSetLobbyDataResponse = 6610,
  kEMsgClientMMSGetLobbyData = 6611,
  kEMsgClientMMSLobbyData = 6612,
  kEMsgClientMMSSendLobbyChatMsg = 6613,
  kEMsgClientMMSLobbyChatMsg = 6614,
  kEMsgClientMMSSetLobbyOwner = 6615,
  kEMsgClientMMSSetLobbyOwnerResponse = 6616,
  kEMsgClientMMSSetLobbyGameServer = 6617,
  kEMsgClientMMSLobbyGameServerSet = 6618,
  kEMsgClientMMSUserJoinedLobby = 6619,
  kEMsgClientMMSUserLeftLobby = 6620,
  kEMsgClientMMSInviteToLobby = 6621,
  kEMsgClientMMSSetLobbyLinked = 6624,
  kEMsgClientHello = 9805,
};
#define kProtoMask 0x80000000u
#define kProtocolVersion 65580u
#define kClientPackageVersion 1771u
#define kAnonymousSteamId 0x0110000100000000ull
#define kMaxTokens 32
#define kMaxTickets 64
#define kMaxRemoteTickets 32
#define kMaxTicketBytes 4096

typedef struct {
  uint32_t handle;
  uint32_t size;
  uint64_t steam_id;  /* another player's ticket (BeginAuthSession); 0 for the game's own */
  uint8_t bytes[kMaxTicketBytes];
} Ticket;

static CRITICAL_SECTION g_lock;  /* everything below, and the lobby and friend state */
static CRITICAL_SECTION g_send_lock;
static HINTERNET g_socket;
static int g_logged_on;
static uint64_t g_session_steam_id = kAnonymousSteamId;
static int32_t g_session_id;
static uint32_t g_heartbeat_seconds = 9;
static uint64_t g_logon_tick;
static uint32_t g_connections;
static uint64_t g_tokens[kMaxTokens];
static int g_token_count;
static uint8_t g_ownership[kMaxTicketBytes];
static uint32_t g_ownership_size;
static Ticket* g_tickets[kMaxTickets];
static int g_ticket_count;
static Ticket* g_remote[kMaxRemoteTickets];
static int g_remote_count;
static uint32_t g_auth_sequence, g_auth_acked;
static uint8_t g_encrypted[kMaxTicketBytes];
static uint32_t g_encrypted_size;
static int g_encrypted_state;  /* 0 none, 1 waiting, 2 have, 3 refused */

static int SendMessage(uint32_t emsg, const Buf* body) {
  Buf header = {0}, frame = {0};
  EnterCriticalSection(&g_lock);
  PbFixed64(&header, 1, g_session_steam_id);
  PbUint(&header, 2, (uint64_t)(int64_t)g_session_id);
  LeaveCriticalSection(&g_lock);
  BufLE32(&frame, emsg | kProtoMask);
  BufLE32(&frame, (uint32_t)header.n);
  BufPut(&frame, header.p, header.n);
  if (body) BufPut(&frame, body->p, body->n);
  int ok = 0;
  EnterCriticalSection(&g_send_lock);
  if (g_socket && !frame.failed && !header.failed)
    ok = WinHttpWebSocketSend(g_socket, WS_BINARY_MESSAGE, frame.p, (DWORD)frame.n) == 0;
  LeaveCriticalSection(&g_send_lock);
  BufFree(&header);
  BufFree(&frame);
  return ok;
}
/* Sends and frees body. */
static int SendAndFree(uint32_t emsg, Buf* body) {
  const int ok = SendMessage(emsg, body);
  BufFree(body);
  return ok;
}

static uint64_t SelfId(void) {
  EnterCriticalSection(&g_lock);
  const uint64_t id = g_logged_on ? g_session_steam_id : g_config.steam_id;
  LeaveCriticalSection(&g_lock);
  return id;
}

/* ---- Callbacks the bridge posts --------------------------------------------------------------- */

/* Steam's callback IDs and their structs (pack 8 on Windows, the same layout on 32 and 64 bits). */
enum {
  kCallbackPersonaStateChange = 304,     /* {u64 id, i32 flags} */
  kCallbackGameLobbyJoinRequested = 333, /* {u64 lobby, u64 friend} */
  kCallbackValidateAuthTicket = 143,     /* {u64 id, i32 response, pad, u64 owner} */
  kCallbackLobbyInvite = 503,            /* {u64 user, u64 lobby, u64 game} */
  kCallbackLobbyEnter = 504,             /* {u64 lobby, u32 permissions, u8 locked, pad, u32 response} */
  kCallbackLobbyDataUpdate = 505,        /* {u64 lobby, u64 member, u8 success} */
  kCallbackLobbyChatUpdate = 506,        /* {u64 lobby, u64 changed, u64 by, u32 state} */
  kCallbackLobbyChatMsg = 507,           /* {u64 lobby, u64 user, u8 type, pad, u32 chat id} */
  kCallbackLobbyGameCreated = 509,       /* {u64 lobby, u64 server, u32 ip, u16 port} */
  kCallbackLobbyMatchList = 510,         /* {u32 count} */
  kCallbackLobbyCreated = 513,           /* {i32 result, pad, u64 lobby} */
  kCallbackSteamAPICallCompleted = 703,  /* {u64 call, i32 id, u32 size} */
  kCallbackWebApiTicket = 168,
};
#define kOurCallTag 0x5044ull  /* the top 16 bits of the bridge's SteamAPICall_t */

typedef struct {
  int id;
  uint64_t call;  /* 0: a callback */
  uint32_t size;
  uint8_t data[64];
} Item;

static CRITICAL_SECTION g_dispatch_lock;
#define kMaxQueued 256
#define kMaxDone 64
static Item* g_queue[kMaxQueued];
static int g_queue_count;
static Item* g_done[kMaxDone];  /* finished call results nobody registered for: polled through ISteamUtils */
static int g_done_count;
static uint64_t g_next_call = 1;
static int g_manual;            /* the game uses manual dispatch */
static Item* g_manual_last;

typedef struct {
  void* object;
  int id;
} Registered;
static Registered g_registered[512];
static int g_registered_count;
typedef struct {
  void* object;
  uint64_t call;
} RegisteredResult;
static RegisteredResult g_registered_results[128];
static int g_registered_result_count;

static uint64_t NewCall(void) {
  EnterCriticalSection(&g_dispatch_lock);
  const uint64_t call = kOurCallTag << 48 | g_next_call++;
  LeaveCriticalSection(&g_dispatch_lock);
  return call;
}
static int IsOurCall(uint64_t call) { return call && (call >> 48) == kOurCallTag; }

static void Post(int id, uint64_t call, const void* data, uint32_t size) {
  if (size > sizeof(((Item*)0)->data)) return;
  Item* item = Alloc(sizeof(Item));
  if (!item) return;
  item->id = id;
  item->call = call;
  item->size = size;
  memcpy(item->data, data, size);
  EnterCriticalSection(&g_dispatch_lock);
  if (g_queue_count == kMaxQueued) {
    Free(g_queue[0]);
    memmove(g_queue, g_queue + 1, sizeof(Item*) * (kMaxQueued - 1));
    --g_queue_count;
  }
  g_queue[g_queue_count++] = item;
  LeaveCriticalSection(&g_dispatch_lock);
}
static void PostCallback(int id, const void* data, uint32_t size) { Post(id, 0, data, size); }
static void PostResult(uint64_t call, int id, const void* data, uint32_t size) {
  if (call) Post(id, call, data, size);
}

/* Takes the first queued item. */
static Item* PopQueued(void) {
  EnterCriticalSection(&g_dispatch_lock);
  Item* item = 0;
  if (g_queue_count) {
    item = g_queue[0];
    memmove(g_queue, g_queue + 1, sizeof(Item*) * (size_t)(--g_queue_count));
  }
  LeaveCriticalSection(&g_dispatch_lock);
  return item;
}
/* Keeps a finished call result for ISteamUtils polling; the caller holds g_dispatch_lock. */
static void KeepDoneLocked(Item* item) {
  if (g_done_count == kMaxDone) {
    Free(g_done[0]);
    memmove(g_done, g_done + 1, sizeof(Item*) * (kMaxDone - 1));
    --g_done_count;
  }
  g_done[g_done_count++] = item;
}
/* A finished (or still queued) call result of the bridge's, removed when take is set. */
static Item* FindResultLocked(uint64_t call, int take) {
  for (int i = 0; i < g_done_count; ++i) {
    if (g_done[i]->call != call) continue;
    Item* item = g_done[i];
    if (take) memmove(g_done + i, g_done + i + 1, sizeof(Item*) * (size_t)(--g_done_count - i));
    return item;
  }
  for (int i = 0; i < g_queue_count; ++i) {
    if (g_queue[i]->call != call) continue;
    Item* item = g_queue[i];
    if (take) memmove(g_queue + i, g_queue + i + 1, sizeof(Item*) * (size_t)(--g_queue_count - i));
    return item;
  }
  return 0;
}

/* CCallbackBase: vtable, u8 flags (2: game server), i32 callback ID. Index 0 of its vtable is
 * Run(void*, bool, SteamAPICall_t) under MSVC, which lists overloads in reverse order; a CCallback's
 * forwards to Run(void*). */
typedef void(METHOD* RunResultFn)(void*, void*, int8_t, uint64_t);
static void RunObject(void* object, void* data, uint64_t call) {
  void** vtable = *(void***)object;
  ((RunResultFn)vtable[0])(object, data, 0, call);
}

static void DispatchOurs(void) {
  for (;;) {
    Item* item = PopQueued();
    if (!item) return;
    void* targets[32];
    int count = 0;
    EnterCriticalSection(&g_dispatch_lock);
    if (item->call) {
      for (int i = 0; i < g_registered_result_count && count < 32; ++i)
        if (g_registered_results[i].call == item->call) targets[count++] = g_registered_results[i].object;
      if (!count) {
        KeepDoneLocked(item);
        item = 0;
      }
    } else {
      for (int i = 0; i < g_registered_count && count < 32; ++i) {
        const uint8_t flags = *((uint8_t*)g_registered[i].object + sizeof(void*));
        if (g_registered[i].id == item->id && !(flags & 2)) targets[count++] = g_registered[i].object;
      }
    }
    LeaveCriticalSection(&g_dispatch_lock);
    if (!item) continue;
    for (int i = 0; i < count; ++i) RunObject(targets[i], item->data, item->call);
    if (item->call) {
      /* A CCallResult unregisters itself before it runs; drop any left. */
      EnterCriticalSection(&g_dispatch_lock);
      for (int i = 0; i < g_registered_result_count; ++i)
        if (g_registered_results[i].call == item->call)
          g_registered_results[i--] = g_registered_results[--g_registered_result_count];
      LeaveCriticalSection(&g_dispatch_lock);
    }
    Free(item);
  }
}

static int8_t CopyResult(uint64_t call, void* out, int size, int expected, int8_t* failed) {
  EnterCriticalSection(&g_dispatch_lock);
  Item* item = FindResultLocked(call, 0);
  int ok = item && item->id == expected && size >= 0;
  if (ok) {
    FindResultLocked(call, 1);
    memcpy(out, item->data, (uint32_t)size < item->size ? (uint32_t)size : item->size);
  }
  LeaveCriticalSection(&g_dispatch_lock);
  if (failed) *failed = item && !ok;
  if (ok) Free(item);
  return (int8_t)ok;
}

/* ---- Lobbies (ISteamMatchmaking) -------------------------------------------------------------- */

typedef struct {
  char* key;
  char* value;
} KeyValue;
typedef struct {
  KeyValue* items;
  int count, cap;
} KeyValues;

static char* Dup(const char* s, size_t n) {
  char* copy = Alloc(n + 1);
  if (copy) memcpy(copy, s, n);
  return copy;
}
static int StrEq(const char* a, const char* b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}
/* Values are never freed: a pointer the game got from GetLobbyData stays valid. */
static void KvSet(KeyValues* kv, const char* key, size_t key_n, const char* value, size_t value_n) {
  for (int i = 0; i < kv->count; ++i)
    if (Strlen(kv->items[i].key) == key_n && !memcmp(kv->items[i].key, key, key_n)) {
      kv->items[i].value = Dup(value, value_n);
      return;
    }
  if (kv->count == kv->cap) {
    const int cap = kv->cap ? kv->cap * 2 : 8;
    KeyValue* items = Alloc(sizeof(KeyValue) * (size_t)cap);
    if (!items) return;
    if (kv->count) memcpy(items, kv->items, sizeof(KeyValue) * (size_t)kv->count);
    Free(kv->items);
    kv->items = items;
    kv->cap = cap;
  }
  kv->items[kv->count].key = Dup(key, key_n);
  kv->items[kv->count].value = Dup(value, value_n);
  ++kv->count;
}
static const char* KvGet(const KeyValues* kv, const char* key) {
  for (int i = 0; i < kv->count; ++i)
    if (StrEq(kv->items[i].key, key)) return kv->items[i].value;
  return "";
}
static int KvDelete(KeyValues* kv, const char* key) {
  for (int i = 0; i < kv->count; ++i)
    if (StrEq(kv->items[i].key, key)) {
      kv->items[i] = kv->items[--kv->count];
      return 1;
    }
  return 0;
}
static void KvClear(KeyValues* kv) { kv->count = 0; }

/* Binary KeyValues as MMS metadata_kv: 00 "" {01 key value}* 08 08. */
static void KvEncodeBinary(const KeyValues* kv, Buf* out) {
  BufByte(out, 0);
  BufByte(out, 0);
  for (int i = 0; i < kv->count; ++i) {
    BufByte(out, 1);
    BufPut(out, kv->items[i].key, Strlen(kv->items[i].key) + 1);
    BufPut(out, kv->items[i].value, Strlen(kv->items[i].value) + 1);
  }
  BufByte(out, 8);
  BufByte(out, 8);
}
static void KvDecodeBinary(const uint8_t* p, size_t n, KeyValues* kv) {
  size_t at = 0;
  if (n < 2 || p[0] != 0) return;
  at = 1;
  while (at < n && p[at]) ++at;  /* the root's name */
  ++at;
  while (at < n && p[at] == 1) {
    const size_t key = ++at;
    while (at < n && p[at]) ++at;
    if (at >= n) return;
    const size_t key_n = at - key, value = ++at;
    while (at < n && p[at]) ++at;
    if (at >= n) return;
    KvSet(kv, (const char*)p + key, key_n, (const char*)p + value, at - value);
    ++at;
  }
}
/* MMSKeyValuePair {name = 1, value = 2}. */
static void KvDecodePair(const uint8_t* p, size_t n, KeyValues* kv) {
  PbReader r = {p, n, 0, 0};
  PbField f;
  const uint8_t *key = 0, *value = (const uint8_t*)"";
  size_t key_n = 0, value_n = 0;
  while (PbNext(&r, &f)) {
    if (f.number == 1 && f.wire == 2) key = f.data, key_n = f.size;
    if (f.number == 2 && f.wire == 2) value = f.data, value_n = f.size;
  }
  if (!r.bad && key) KvSet(kv, (const char*)key, key_n, (const char*)value, value_n);
}
static void KvEncodePairs(const KeyValues* kv, uint32_t field, Buf* out) {
  for (int i = 0; i < kv->count; ++i) {
    Buf pair = {0};
    PbString(&pair, 1, kv->items[i].key);
    PbString(&pair, 2, kv->items[i].value);
    PbBytes(out, field, pair.p, pair.n);
    BufFree(&pair);
  }
}

#define kMaxLobbies 64
#define kMaxMembers 64
typedef struct {
  uint64_t id;
  char* name;
  KeyValues data;
} Member;
typedef struct {
  uint64_t id, owner, server_id;
  uint32_t server_ip;
  uint16_t server_port;
  int32_t type, flags, max_members, num_members;
  int joinable, joined;
  Member members[kMaxMembers];
  int member_count;
  KeyValues data;
} Lobby;
static Lobby* g_lobbies[kMaxLobbies];
static int g_lobby_count;
static uint64_t g_list[kMaxLobbies];
static int g_list_count;
static Buf g_filters;           /* the next RequestLobbyList's encoded filters */
static int32_t g_filter_count = 50;
static uint64_t g_pending_create, g_pending_list;
static int32_t g_pending_create_type, g_pending_create_max;
static struct {
  uint64_t lobby, call;
} g_pending_join[8];

typedef struct {
  uint64_t lobby, sender;
  int id;
  uint32_t size;
  uint8_t bytes[4096];
} ChatEntry;
#define kMaxChat 32
static ChatEntry* g_chat[kMaxChat];
static int g_chat_count, g_next_chat_id;
static char g_my_name[128];

/* The lobby with that ID, created when create is set. Caller holds g_lock. */
static Lobby* FindLobby(uint64_t id, int create) {
  for (int i = 0; i < g_lobby_count; ++i)
    if (g_lobbies[i]->id == id) return g_lobbies[i];
  if (!create || !id) return 0;
  if (g_lobby_count == kMaxLobbies) {
    int drop = -1;
    for (int i = 0; i < g_lobby_count && drop < 0; ++i)
      if (!g_lobbies[i]->joined) drop = i;
    if (drop < 0) return 0;
    g_lobbies[drop] = g_lobbies[--g_lobby_count];  /* left in memory: the game may hold its strings */
  }
  Lobby* lobby = Alloc(sizeof(Lobby));
  if (!lobby) return 0;
  lobby->id = id;
  lobby->joinable = 1;
  g_lobbies[g_lobby_count++] = lobby;
  return lobby;
}
static Member* FindMember(Lobby* lobby, uint64_t id, int create) {
  for (int i = 0; i < lobby->member_count; ++i)
    if (lobby->members[i].id == id) return &lobby->members[i];
  if (!create || lobby->member_count == kMaxMembers) return 0;
  Member* member = &lobby->members[lobby->member_count++];
  memset(member, 0, sizeof *member);
  member->id = id;
  return member;
}
static void RemoveMember(Lobby* lobby, uint64_t id) {
  for (int i = 0; i < lobby->member_count; ++i)
    if (lobby->members[i].id == id) {
      lobby->members[i] = lobby->members[--lobby->member_count];
      return;
    }
}
static const char* PersonaName(void) { return g_my_name[0] ? g_my_name : g_config.account; }

static void LogLobby(const char* what, uint64_t lobby, int64_t value) {
  Buf line = {0};
  LogText(&line, what);
  LogText(&line, " lobby=");
  LogNumber(&line, lobby);
  LogText(&line, " value=");
  if (value < 0) {
    LogText(&line, "-");
    value = -value;
  }
  LogNumber(&line, (uint64_t)value);
  LogLine(&line);
}

/* Sends the lobby's (member_id 0) or a member's metadata to Steam. */
static void SendLobbyData(uint64_t lobby_id, uint64_t member_id) {
  Buf body = {0};
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  if (lobby) {
    PbUint(&body, 1, g_config.app_id);
    PbFixed64(&body, 2, lobby_id);
    PbFixed64(&body, 3, member_id);
    PbUint(&body, 4, (uint64_t)(int64_t)lobby->max_members);
    PbUint(&body, 5, (uint64_t)(int64_t)lobby->type);
    PbUint(&body, 6, (uint64_t)(int64_t)lobby->flags);
    Member* member = member_id ? FindMember(lobby, member_id, 1) : 0;
    const KeyValues* kv = member ? &member->data : &lobby->data;
    Buf binary = {0};
    KvEncodeBinary(kv, &binary);
    PbBytes(&body, 7, binary.p, binary.n);
    BufFree(&binary);
    KvEncodePairs(kv, 9, &body);
  }
  LeaveCriticalSection(&g_lock);
  if (lobby) SendAndFree(kEMsgClientMMSSetLobbyData, &body);
  else BufFree(&body);
}

/* Reads the member list of a JoinLobbyResponse (field 9) or LobbyData (field 9) entry. */
static void ReadMember(Lobby* lobby, const uint8_t* p, size_t n, uint32_t pairs_field) {
  PbReader r = {p, n, 0, 0};
  PbField f;
  uint64_t id = 0;
  const uint8_t *name = 0, *kv = 0;
  size_t name_n = 0, kv_n = 0;
  while (PbNext(&r, &f)) {
    if (f.number == 1 && f.wire == 1) id = f.value;
    if (f.number == 2 && f.wire == 2) name = f.data, name_n = f.size;
    if (f.number == 3 && f.wire == 2) kv = f.data, kv_n = f.size;
  }
  if (r.bad || !id) return;
  Member* member = FindMember(lobby, id, 1);
  if (!member) return;
  if (name) member->name = Dup((const char*)name, name_n);
  KvClear(&member->data);
  if (kv) KvDecodeBinary(kv, kv_n, &member->data);
  PbReader again = {p, n, 0, 0};
  while (PbNext(&again, &f))
    if (f.number == pairs_field && f.wire == 2) KvDecodePair(f.data, f.size, &member->data);
}

typedef struct {
  uint64_t lobby;
  uint32_t permissions;
  uint8_t locked, pad[3];
  uint32_t response;
  uint32_t pad2;
} LobbyEnterData;
typedef struct {
  uint64_t lobby, member;
  uint8_t success, pad[7];
} LobbyDataUpdateData;

static void PostLobbyUpdated(uint64_t lobby, uint64_t member) {
  LobbyDataUpdateData data = {lobby, member, 1, {0}};
  PostCallback(kCallbackLobbyDataUpdate, &data, sizeof data);
}

static void HandleLobbyMessage(uint32_t emsg, const uint8_t* body, size_t n) {
  PbReader r = {body, n, 0, 0};
  PbField f;
  switch (emsg) {
    case kEMsgClientMMSCreateLobbyResponse: {
      int32_t eresult = 2;
      uint64_t lobby_id = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 1) lobby_id = f.value;
        if (f.number == 3 && f.wire == 0) eresult = (int32_t)f.value;
      }
      LogLobby("lobby created", lobby_id, eresult);
      EnterCriticalSection(&g_lock);
      const uint64_t call = g_pending_create;
      g_pending_create = 0;
      if (eresult == 1) {
        Lobby* lobby = FindLobby(lobby_id, 1);
        if (lobby) {
          lobby->owner = g_session_steam_id;
          lobby->type = g_pending_create_type;
          lobby->max_members = g_pending_create_max;
          lobby->joined = 1;
          lobby->num_members = 1;
          Member* me = FindMember(lobby, g_session_steam_id, 1);
          if (me) me->name = Dup(PersonaName(), Strlen(PersonaName()));
        }
      }
      LeaveCriticalSection(&g_lock);
      struct {
        int32_t result, pad;
        uint64_t lobby;
      } created = {eresult == 1 ? 1 : eresult, 0, eresult == 1 ? lobby_id : 0};
      PostResult(call, kCallbackLobbyCreated, &created, sizeof created);
      PostCallback(kCallbackLobbyCreated, &created, sizeof created);
      if (eresult == 1) {
        LobbyEnterData enter = {lobby_id, 0, 0, {0}, 1, 0};
        PostCallback(kCallbackLobbyEnter, &enter, sizeof enter);
        PostLobbyUpdated(lobby_id, lobby_id);
      }
      break;
    }
    case kEMsgClientMMSJoinLobbyResponse: {
      uint64_t lobby_id = 0, owner = 0;
      int32_t response = 2, max = 0, type = 0, flags = 0;
      const uint8_t* kv = 0;
      size_t kv_n = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 1) lobby_id = f.value;
        if (f.number == 3 && f.wire == 0) response = (int32_t)f.value;
        if (f.number == 4 && f.wire == 0) max = (int32_t)f.value;
        if (f.number == 5 && f.wire == 0) type = (int32_t)f.value;
        if (f.number == 6 && f.wire == 0) flags = (int32_t)f.value;
        if (f.number == 7 && f.wire == 1) owner = f.value;
        if (f.number == 8 && f.wire == 2) kv = f.data, kv_n = f.size;
      }
      LogLobby("lobby joined", lobby_id, response);
      uint64_t call = 0;
      EnterCriticalSection(&g_lock);
      for (int i = 0; i < 8; ++i)
        if (g_pending_join[i].lobby == lobby_id && g_pending_join[i].call) {
          call = g_pending_join[i].call;
          g_pending_join[i].call = 0;
        }
      Lobby* lobby = response == 1 ? FindLobby(lobby_id, 1) : 0;
      if (lobby) {
        lobby->owner = owner;
        lobby->max_members = max;
        lobby->type = type;
        lobby->flags = flags;
        lobby->joined = 1;
        lobby->member_count = 0;
        KvClear(&lobby->data);
        if (kv) KvDecodeBinary(kv, kv_n, &lobby->data);
        PbReader again = {body, n, 0, 0};
        while (PbNext(&again, &f)) {
          if (f.number == 9 && f.wire == 2) ReadMember(lobby, f.data, f.size, 4);
          if (f.number == 10 && f.wire == 2) KvDecodePair(f.data, f.size, &lobby->data);
        }
        lobby->num_members = lobby->member_count;
      }
      LeaveCriticalSection(&g_lock);
      LobbyEnterData enter = {lobby_id, 0, 0, {0}, (uint32_t)response, 0};
      PostResult(call, kCallbackLobbyEnter, &enter, sizeof enter);
      PostCallback(kCallbackLobbyEnter, &enter, sizeof enter);
      if (lobby) PostLobbyUpdated(lobby_id, lobby_id);
      break;
    }
    case kEMsgClientMMSGetLobbyListResponse: {
      int32_t eresult = 2;
      int count = 0;
      EnterCriticalSection(&g_lock);
      while (PbNext(&r, &f)) {
        if (f.number == 3 && f.wire == 0) eresult = (int32_t)f.value;
        if (f.number != 4 || f.wire != 2 || count == kMaxLobbies) continue;
        PbReader entry = {f.data, f.size, 0, 0};
        PbField e;
        uint64_t id = 0;
        int32_t max = 0, type = 0, flags = 0, members = 0;
        const uint8_t* kv = 0;
        size_t kv_n = 0;
        while (PbNext(&entry, &e)) {
          if (e.number == 1 && e.wire == 1) id = e.value;
          if (e.number == 2 && e.wire == 0) max = (int32_t)e.value;
          if (e.number == 3 && e.wire == 0) type = (int32_t)e.value;
          if (e.number == 4 && e.wire == 0) flags = (int32_t)e.value;
          if (e.number == 5 && e.wire == 2) kv = e.data, kv_n = e.size;
          if (e.number == 6 && e.wire == 0) members = (int32_t)e.value;
        }
        Lobby* lobby = entry.bad ? 0 : FindLobby(id, 1);
        if (!lobby) continue;
        lobby->max_members = max;
        lobby->type = type;
        lobby->flags = flags;
        lobby->num_members = members;
        if (!lobby->joined) {
          KvClear(&lobby->data);
          if (kv) KvDecodeBinary(kv, kv_n, &lobby->data);
          PbReader again = {f.data, f.size, 0, 0};
          while (PbNext(&again, &e))
            if (e.number == 11 && e.wire == 2) KvDecodePair(e.data, e.size, &lobby->data);
        }
        g_list[count++] = id;
      }
      g_list_count = count;
      const uint64_t call = g_pending_list;
      g_pending_list = 0;
      LeaveCriticalSection(&g_lock);
      LogLobby("lobby list", 0, eresult == 1 ? count : -eresult);
      const uint32_t matching = (uint32_t)count;
      PostResult(call, kCallbackLobbyMatchList, &matching, sizeof matching);
      break;
    }
    case kEMsgClientMMSLobbyData: {
      uint64_t lobby_id = 0, owner = 0;
      int32_t members = -1, max = 0, type = 0, flags = 0;
      const uint8_t* kv = 0;
      size_t kv_n = 0;
      int has_members = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 1) lobby_id = f.value;
        if (f.number == 3 && f.wire == 0) members = (int32_t)f.value;
        if (f.number == 4 && f.wire == 0) max = (int32_t)f.value;
        if (f.number == 5 && f.wire == 0) type = (int32_t)f.value;
        if (f.number == 6 && f.wire == 0) flags = (int32_t)f.value;
        if (f.number == 7 && f.wire == 1) owner = f.value;
        if (f.number == 8 && f.wire == 2) kv = f.data, kv_n = f.size;
        if (f.number == 9) has_members = 1;
      }
      EnterCriticalSection(&g_lock);
      Lobby* lobby = FindLobby(lobby_id, 1);
      if (lobby) {
        if (owner) lobby->owner = owner;
        lobby->max_members = max;
        lobby->type = type;
        lobby->flags = flags;
        KvClear(&lobby->data);
        if (kv) KvDecodeBinary(kv, kv_n, &lobby->data);
        if (has_members) lobby->member_count = 0;
        PbReader again = {body, n, 0, 0};
        while (PbNext(&again, &f)) {
          if (f.number == 9 && f.wire == 2) ReadMember(lobby, f.data, f.size, 5);
          if (f.number == 12 && f.wire == 2) KvDecodePair(f.data, f.size, &lobby->data);
        }
        lobby->num_members = members >= 0 ? members : lobby->member_count;
      }
      LeaveCriticalSection(&g_lock);
      PostLobbyUpdated(lobby_id, lobby_id);
      break;
    }
    case kEMsgClientMMSUserJoinedLobby:
    case kEMsgClientMMSUserLeftLobby: {
      uint64_t lobby_id = 0, user = 0;
      const uint8_t* name = 0;
      size_t name_n = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 1) lobby_id = f.value;
        if (f.number == 3 && f.wire == 1) user = f.value;
        if (f.number == 4 && f.wire == 2) name = f.data, name_n = f.size;
      }
      const int joined = emsg == kEMsgClientMMSUserJoinedLobby;
      EnterCriticalSection(&g_lock);
      Lobby* lobby = FindLobby(lobby_id, 0);
      if (lobby && joined) {
        Member* member = FindMember(lobby, user, 1);
        if (member && name) member->name = Dup((const char*)name, name_n);
      } else if (lobby) {
        RemoveMember(lobby, user);
      }
      if (lobby) lobby->num_members = lobby->member_count;
      LeaveCriticalSection(&g_lock);
      LogLobby(joined ? "lobby member joined" : "lobby member left", lobby_id, 0);
      struct {
        uint64_t lobby, changed, by;
        uint32_t state, pad;
      } update = {lobby_id, user, user, joined ? 1u : 2u, 0};
      PostCallback(kCallbackLobbyChatUpdate, &update, sizeof update);
      break;
    }
    case kEMsgClientMMSLobbyChatMsg: {
      uint64_t lobby_id = 0, sender = 0;
      const uint8_t* message = 0;
      size_t message_n = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 1) lobby_id = f.value;
        if (f.number == 3 && f.wire == 1) sender = f.value;
        if (f.number == 4 && f.wire == 2) message = f.data, message_n = f.size;
      }
      if (message_n > 4096) break;
      ChatEntry* entry = Alloc(sizeof(ChatEntry));
      if (!entry) break;
      entry->lobby = lobby_id;
      entry->sender = sender;
      entry->size = (uint32_t)message_n;
      if (message_n) memcpy(entry->bytes, message, message_n);
      EnterCriticalSection(&g_lock);
      entry->id = g_next_chat_id++;
      if (g_chat_count == kMaxChat) {
        Free(g_chat[0]);
        memmove(g_chat, g_chat + 1, sizeof(ChatEntry*) * (kMaxChat - 1));
        --g_chat_count;
      }
      g_chat[g_chat_count++] = entry;
      LeaveCriticalSection(&g_lock);
      struct {
        uint64_t lobby, user;
        uint8_t type, pad[3];
        uint32_t chat_id;
      } chat = {lobby_id, sender, 1, {0}, (uint32_t)entry->id};
      PostCallback(kCallbackLobbyChatMsg, &chat, sizeof chat);
      break;
    }
    case kEMsgClientMMSLobbyGameServerSet: {
      uint64_t lobby_id = 0, server = 0;
      uint32_t ip = 0, port = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 1) lobby_id = f.value;
        if (f.number == 3 && f.wire == 0) ip = (uint32_t)f.value;
        if (f.number == 4 && f.wire == 0) port = (uint32_t)f.value;
        if (f.number == 5 && f.wire == 1) server = f.value;
        if (f.number == 6 && f.wire == 2) {
          PbReader address = {f.data, f.size, 0, 0};
          PbField a;
          while (PbNext(&address, &a))
            if (a.number == 1 && a.wire == 5) ip = (uint32_t)a.value;
        }
      }
      EnterCriticalSection(&g_lock);
      Lobby* lobby = FindLobby(lobby_id, 0);
      if (lobby) {
        lobby->server_id = server;
        lobby->server_ip = ip;
        lobby->server_port = (uint16_t)port;
      }
      LeaveCriticalSection(&g_lock);
      LogLobby("lobby game server", lobby_id, port);
      struct {
        uint64_t lobby, server;
        uint32_t ip;
        uint16_t port, pad;
      } created = {lobby_id, server, ip, (uint16_t)port, 0};
      PostCallback(kCallbackLobbyGameCreated, &created, sizeof created);
      break;
    }
    case kEMsgClientMMSInviteToLobby: {
      /* An invite: with no Steam overlay to accept it, the bridge accepts it at once. */
      uint64_t lobby_id = 0, from = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 1) lobby_id = f.value;
        if (f.number == 3 && f.wire == 1) from = f.value;
      }
      LogLobby("lobby invite received, accepting", lobby_id, 0);
      struct {
        uint64_t user, lobby, game;
      } invite = {from, lobby_id, g_config.app_id};
      PostCallback(kCallbackLobbyInvite, &invite, sizeof invite);
      struct {
        uint64_t lobby, from;
      } join = {lobby_id, from};
      PostCallback(kCallbackGameLobbyJoinRequested, &join, sizeof join);
      break;
    }
    case kEMsgClientMMSLeaveLobbyResponse:
    case kEMsgClientMMSSetLobbyDataResponse:
    case kEMsgClientMMSSetLobbyOwnerResponse: {
      uint64_t lobby_id = 0;
      int32_t eresult = 2;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 1) lobby_id = f.value;
        if (f.number == 3 && f.wire == 0) eresult = (int32_t)f.value;
      }
      if (eresult != 1 || emsg != kEMsgClientMMSSetLobbyDataResponse)
        LogLobby(emsg == kEMsgClientMMSLeaveLobbyResponse ? "lobby left"
                 : emsg == kEMsgClientMMSSetLobbyOwnerResponse ? "lobby owner set"
                                                                 : "lobby data refused",
                 lobby_id, eresult);
      break;
    }
    default:
      break;
  }
}

/* ---- Friends and presence (ISteamFriends) ------------------------------------------------------ */

#define kMaxFriends 1024
typedef struct {
  uint64_t id, game_id, lobby;
  uint32_t relationship, state, game_app, server_ip, server_port;
  char* name;
} Friend;
static Friend g_friends[kMaxFriends];
static int g_friend_count;

static Friend* FindFriend(uint64_t id, int create) {
  for (int i = 0; i < g_friend_count; ++i)
    if (g_friends[i].id == id) return &g_friends[i];
  if (!create || g_friend_count == kMaxFriends) return 0;
  Friend* f = &g_friends[g_friend_count++];
  memset(f, 0, sizeof *f);
  f->id = id;
  return f;
}

static void SendPresence(void) {
  Buf status = {0};
  PbUint(&status, 1, 1);  /* online */
  SendAndFree(kEMsgClientChangeStatus, &status);
  Buf game = {0}, played = {0};
  PbFixed64(&game, 2, g_config.app_id);  /* CGameID of a Steam app: its app ID */
  PbBytes(&played, 1, game.p, game.n);
  BufFree(&game);
  SendAndFree(kEMsgClientGamesPlayed, &played);
  Log("presence: online, playing the game");
}

static void RequestFriendData(void) {
  Buf body = {0};
  /* Status, name, presence, game extra info, rich presence. */
  PbUint(&body, 1, 1 | 2 | 16 | 256 | 4096);
  EnterCriticalSection(&g_lock);
  int count = 0;
  for (int i = 0; i < g_friend_count; ++i)
    if (g_friends[i].relationship == 3) {
      PbFixed64(&body, 2, g_friends[i].id);
      ++count;
    }
  LeaveCriticalSection(&g_lock);
  if (count) SendAndFree(kEMsgClientRequestFriendData, &body);
  else BufFree(&body);
}

static void HandleFriendMessage(uint32_t emsg, const uint8_t* body, size_t n) {
  PbReader r = {body, n, 0, 0};
  PbField f;
  if (emsg == kEMsgClientFriendsList) {
    int incremental = 0;
    while (PbNext(&r, &f))
      if (f.number == 1 && f.wire == 0) incremental = (int)f.value;
    EnterCriticalSection(&g_lock);
    if (!incremental)
      for (int i = 0; i < g_friend_count; ++i) g_friends[i].relationship = 0;
    PbReader again = {body, n, 0, 0};
    while (PbNext(&again, &f)) {
      if (f.number != 2 || f.wire != 2) continue;
      PbReader entry = {f.data, f.size, 0, 0};
      PbField e;
      uint64_t id = 0;
      uint32_t relationship = 0;
      while (PbNext(&entry, &e)) {
        if (e.number == 1 && e.wire == 1) id = e.value;
        if (e.number == 2 && e.wire == 0) relationship = (uint32_t)e.value;
      }
      if ((id >> 52) != 0x011) continue;  /* individual accounts only, not groups */
      Friend* fr = FindFriend(id, 1);
      if (fr) fr->relationship = relationship;
    }
    int friends = 0;
    for (int i = 0; i < g_friend_count; ++i) friends += g_friends[i].relationship == 3;
    LeaveCriticalSection(&g_lock);
    LogValue("friends list: friends=", (uint64_t)friends);
    RequestFriendData();
    return;
  }
  if (emsg == kEMsgClientPersonaState) {
    int count = 0;
    while (PbNext(&r, &f)) {
      if (f.number != 2 || f.wire != 2) continue;
      PbReader entry = {f.data, f.size, 0, 0};
      PbField e;
      uint64_t id = 0, game_id = 0, lobby = 0;
      uint32_t state = 0, app = 0, ip = 0, port = 0;
      const uint8_t* name = 0;
      size_t name_n = 0;
      int has_state = 0;
      while (PbNext(&entry, &e)) {
        if (e.number == 1 && e.wire == 1) id = e.value;
        if (e.number == 2 && e.wire == 0) state = (uint32_t)e.value, has_state = 1;
        if (e.number == 3 && e.wire == 0) app = (uint32_t)e.value;
        if (e.number == 4 && e.wire == 0) ip = (uint32_t)e.value;
        if (e.number == 5 && e.wire == 0) port = (uint32_t)e.value;
        if (e.number == 15 && e.wire == 2) name = e.data, name_n = e.size;
        if (e.number == 56 && e.wire == 1) game_id = e.value;
        if (e.number == 73 && e.wire == 1) lobby = e.value;
      }
      if (entry.bad || !id) continue;
      EnterCriticalSection(&g_lock);
      if (id == g_session_steam_id) {
        if (name) CopyValue(g_my_name, sizeof g_my_name, (const char*)name, name_n);
      } else {
        Friend* fr = FindFriend(id, 1);
        if (fr) {
          if (has_state) fr->state = state;
          if (name) fr->name = Dup((const char*)name, name_n);
          fr->game_app = app;
          fr->game_id = game_id ? game_id : app;
          fr->server_ip = ip;
          fr->server_port = port;
          fr->lobby = lobby;
        }
      }
      LeaveCriticalSection(&g_lock);
      struct {
        uint64_t id;
        int32_t flags, pad;
      } change = {id, 1 | 2 | 16, 0};
      PostCallback(kCallbackPersonaStateChange, &change, sizeof change);
      ++count;
    }
    (void)count;
    return;
  }
  if (emsg == kEMsgClientFriendMsgIncoming) {
    uint64_t from = 0;
    int32_t type = 0;
    while (PbNext(&r, &f)) {
      if (f.number == 1 && f.wire == 1) from = f.value;
      if (f.number == 2 && f.wire == 0) type = (int32_t)f.value;
    }
    if (type == 3) LogValue("game invite from a friend, chat entry type=", (uint64_t)type);
    (void)from;
  }
}

/* ---- Incoming messages ------------------------------------------------------------------------ */

static void HandleFrame(const uint8_t* data, size_t size, int depth);
static void PostAuthResult(uint64_t steam_id, uint32_t response);

static void HandleMulti(const uint8_t* body, size_t n, int depth) {
  PbReader r = {body, n, 0, 0};
  PbField f;
  uint64_t unzipped = 0;
  const uint8_t* payload = 0;
  size_t payload_size = 0;
  while (PbNext(&r, &f)) {
    if (f.number == 1 && f.wire == 0) unzipped = f.value;
    if (f.number == 2 && f.wire == 2) payload = f.data, payload_size = f.size;
  }
  if (r.bad || !payload) return;
  Buf inflated = {0};
  if (unzipped) {
    if (!Gunzip(payload, payload_size, (size_t)unzipped, &inflated)) {
      Log("multi: bad gzip");
      BufFree(&inflated);
      return;
    }
    payload = inflated.p;
    payload_size = inflated.n;
  }
  size_t at = 0;
  while (payload_size - at >= 4) {
    const uint32_t len = LE32(payload + at);
    at += 4;
    if (len > payload_size - at) break;
    HandleFrame(payload + at, len, depth + 1);
    at += len;
  }
  BufFree(&inflated);
}

static void HandleFrame(const uint8_t* data, size_t size, int depth) {
  if (depth > 2 || size < 8) return;
  const uint32_t raw = LE32(data);
  if (!(raw & kProtoMask)) return;
  const uint32_t emsg = raw & ~kProtoMask;
  const uint32_t header_size = LE32(data + 4);
  if (header_size > size - 8) return;
  uint64_t steam_id = 0;
  int32_t session_id = 0;
  PbReader hr = {data + 8, header_size, 0, 0};
  PbField f;
  while (PbNext(&hr, &f)) {
    if (f.number == 1 && f.wire == 1) steam_id = f.value;
    if (f.number == 2 && f.wire == 0) session_id = (int32_t)f.value;
  }
  const uint8_t* body = data + 8 + header_size;
  const size_t n = size - 8 - header_size;
  PbReader r = {body, n, 0, 0};

  if (emsg >= kEMsgClientMMSCreateLobby && emsg <= kEMsgClientMMSSetLobbyLinked) {
    HandleLobbyMessage(emsg, body, n);
    return;
  }
  switch (emsg) {
    case kEMsgMulti:
      HandleMulti(body, n, depth);
      break;
    case kEMsgClientFriendsList:
    case kEMsgClientPersonaState:
    case kEMsgClientFriendMsgIncoming:
      HandleFriendMessage(emsg, body, n);
      break;
    case kEMsgClientLogOnResponse: {
      int32_t eresult = 2;
      uint32_t heartbeat = 0, legacy = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 1 && f.wire == 0) eresult = (int32_t)f.value;
        if (f.number == 2 && f.wire == 0) legacy = (uint32_t)f.value;
        if (f.number == 3 && f.wire == 0) heartbeat = (uint32_t)f.value;
      }
      LogValue("logon eresult=", (uint64_t)(uint32_t)eresult);
      if (eresult != 1) break;
      EnterCriticalSection(&g_lock);
      if (steam_id) g_session_steam_id = steam_id;
      g_session_id = session_id;
      g_heartbeat_seconds = heartbeat ? heartbeat : legacy ? legacy : 9;
      g_logged_on = 1;
      g_logon_tick = GetTickCount64();
      ++g_connections;
      LeaveCriticalSection(&g_lock);
      if (steam_id != g_config.steam_id) Log("logon: the session's SteamID differs from Playden's account");
      Buf request = {0};
      PbUint(&request, 1, g_config.app_id);
      SendAndFree(kEMsgClientGetAppOwnershipTicket, &request);
      SendPresence();
      break;
    }
    case kEMsgClientLoggedOff: {
      int32_t eresult = 2;
      while (PbNext(&r, &f))
        if (f.number == 1 && f.wire == 0) eresult = (int32_t)f.value;
      LogValue("logged off eresult=", (uint64_t)(uint32_t)eresult);
      EnterCriticalSection(&g_lock);
      g_logged_on = 0;
      LeaveCriticalSection(&g_lock);
      break;
    }
    case kEMsgClientGameConnectTokens: {
      uint32_t keep = 10;
      EnterCriticalSection(&g_lock);
      while (PbNext(&r, &f)) {
        if (f.number == 1 && f.wire == 0) keep = (uint32_t)f.value;
        if (f.number == 2 && f.wire == 2 && f.size == 8) {
          if (g_token_count == kMaxTokens) {
            memmove(g_tokens, g_tokens + 1, sizeof(uint64_t) * (kMaxTokens - 1));
            --g_token_count;
          }
          g_tokens[g_token_count++] = LE32(f.data) | (uint64_t)LE32(f.data + 4) << 32;
        }
      }
      if (!keep) keep = 1;
      if (keep > kMaxTokens) keep = kMaxTokens;
      if ((uint32_t)g_token_count > keep) {
        const int drop = g_token_count - (int)keep;
        memmove(g_tokens, g_tokens + drop, sizeof(uint64_t) * keep);
        g_token_count = (int)keep;
      }
      const int count = g_token_count;
      LeaveCriticalSection(&g_lock);
      LogValue("game connect tokens=", (uint64_t)count);
      break;
    }
    case kEMsgClientGetAppOwnershipTicketResponse: {
      uint32_t eresult = 2, app = 0;
      const uint8_t* ticket = 0;
      size_t ticket_size = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 1 && f.wire == 0) eresult = (uint32_t)f.value;
        if (f.number == 2 && f.wire == 0) app = (uint32_t)f.value;
        if (f.number == 3 && f.wire == 2) ticket = f.data, ticket_size = f.size;
      }
      Buf line = {0};
      LogText(&line, "ownership ticket app=");
      LogNumber(&line, app);
      LogText(&line, " eresult=");
      LogNumber(&line, eresult);
      LogText(&line, " bytes=");
      LogNumber(&line, ticket_size);
      LogLine(&line);
      if (eresult != 1 || app != g_config.app_id || !ticket || ticket_size > kMaxTicketBytes) break;
      EnterCriticalSection(&g_lock);
      memcpy(g_ownership, ticket, ticket_size);
      g_ownership_size = (uint32_t)ticket_size;
      LeaveCriticalSection(&g_lock);
      break;
    }
    case kEMsgClientAuthListAck: {
      uint32_t sequence = 0;
      while (PbNext(&r, &f))
        if (f.number == 3 && f.wire == 0) sequence = (uint32_t)f.value;
      EnterCriticalSection(&g_lock);
      if (sequence > g_auth_acked) g_auth_acked = sequence;
      LeaveCriticalSection(&g_lock);
      LogValue("auth list ack sequence=", sequence);
      break;
    }
    case kEMsgClientTicketAuthComplete: {
      uint64_t who = 0;
      uint32_t estate = 0, response = 0, crc = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 1 && f.wire == 1) who = f.value;
        if (f.number == 3 && f.wire == 0) estate = (uint32_t)f.value;
        if (f.number == 4 && f.wire == 0) response = (uint32_t)f.value;
        if (f.number == 6 && f.wire == 0) crc = (uint32_t)f.value;
      }
      Buf line = {0};
      LogText(&line, "ticket auth complete estate=");
      LogNumber(&line, estate);
      LogText(&line, " response=");
      LogNumber(&line, response);
      LogText(&line, " crc=");
      LogNumber(&line, crc);
      LogText(&line, who == g_session_steam_id ? " (own ticket)" : " (another player's)");
      LogLine(&line);
      if (who && who != g_session_steam_id && response) PostAuthResult(who, response);
      break;
    }
    case kEMsgClientRequestEncryptedAppTicketResponse: {
      int32_t eresult = 2;
      const uint8_t* ticket = 0;
      size_t ticket_size = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 2 && f.wire == 0) eresult = (int32_t)f.value;
        if (f.number == 3 && f.wire == 2) ticket = f.data, ticket_size = f.size;
      }
      LogValue("encrypted app ticket eresult=", (uint64_t)(uint32_t)eresult);
      EnterCriticalSection(&g_lock);
      if (eresult == 1 && ticket && ticket_size <= kMaxTicketBytes) {
        memcpy(g_encrypted, ticket, ticket_size);
        g_encrypted_size = (uint32_t)ticket_size;
        g_encrypted_state = 2;
      } else {
        g_encrypted_state = 3;
      }
      LeaveCriticalSection(&g_lock);
      break;
    }
    default:
      break;
  }
}

/* ---- Connection ------------------------------------------------------------------------------- */

static void WideFrom(WCHAR* out, size_t cap, const char* s, size_t n) {
  size_t i = 0;
  for (; i < n && i + 1 < cap; ++i) out[i] = (WCHAR)(uint8_t)s[i];
  out[i] = 0;
}

/* GET https://api.steampowered.com<path>; the body, or empty with the status logged. */
static void HttpGet(HINTERNET http, const WCHAR* path, Buf* text) {
  HINTERNET connection = WinHttpConnect(http, L"api.steampowered.com", 443, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
  DWORD status = 0, status_size = sizeof status;
  if (request && WinHttpSendRequest(request, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(request, 0) &&
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &status_size, 0) &&
      status == 200) {
    uint8_t chunk[4096];
    DWORD read = 0;
    while (text->n < (1u << 20) && WinHttpReadData(request, chunk, sizeof chunk, &read) && read) BufPut(text, chunk, read);
  } else {
    LogValue("http: status=", status);
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
}
static int CountOccurrences(const Buf* text, const char* needle) {
  const size_t n = Strlen(needle);
  int count = 0;
  for (size_t at = 0; !text->failed && at + n <= text->n; ++at)
    if (!memcmp(text->p + at, needle, n)) ++count;
  return count;
}

/* The CM list (host:port strings) from Steam's directory. */
static int FetchEndpoints(HINTERNET http, char endpoints[][128], int max) {
  Buf text = {0};
  HttpGet(http, L"/ISteamDirectory/GetCMListForConnect/v1/?cmtype=websockets&maxcount=16&format=json", &text);
  int count = 0;
  static const char kKey[] = "\"endpoint\":\"";
  for (size_t at = 0; !text.failed && at + sizeof kKey < text.n && count < max; ++at) {
    if (memcmp(text.p + at, kKey, sizeof kKey - 1)) continue;
    const size_t start = at + sizeof kKey - 1;
    size_t end = start;
    while (end < text.n && text.p[end] != '"') ++end;
    if (end < text.n && end - start < 128) CopyValue(endpoints[count++], 128, (const char*)text.p + start, end - start);
    at = end;
  }
  BufFree(&text);
  return count;
}

/* Steam Datagram Relay: what a game asking for Steam networking would need. The bridge only checks that
 * the relay list is reachable and logs its size; it does not relay traffic. */
static void CheckRelays(HINTERNET http) {
  WCHAR path[128];
  static const char kPrefix[] = "/ISteamApps/GetSDRConfig/v1/?appid=";
  Buf query = {0};
  LogText(&query, kPrefix);
  LogNumber(&query, g_config.app_id);
  WideFrom(path, 128, (const char*)query.p, query.n);
  BufFree(&query);
  Buf text = {0};
  HttpGet(http, path, &text);
  LogValue("steam relays: relay addresses listed=", (uint64_t)CountOccurrences(&text, "\"ipv4\""));
  BufFree(&text);
}

static HINTERNET OpenSocket(HINTERNET http, const char* endpoint, HINTERNET* connection_out) {
  const size_t n = Strlen(endpoint);
  size_t colon = n;
  while (colon && endpoint[colon - 1] != ':') --colon;
  if (!colon) return 0;
  WCHAR host[128];
  WideFrom(host, 128, endpoint, colon - 1);
  const uint16_t port = (uint16_t)ParseNumber(endpoint + colon, n - colon);
  HINTERNET connection = WinHttpConnect(http, host, port ? port : 443, 0);
  if (!connection) return 0;
  HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/cmsocket/", 0, 0, 0, WINHTTP_FLAG_SECURE);
  HINTERNET socket = 0;
  if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, 0, 0) &&
      WinHttpSendRequest(request, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(request, 0))
    socket = WinHttpWebSocketCompleteUpgrade(request, 0);
  if (request) WinHttpCloseHandle(request);
  if (!socket) {
    WinHttpCloseHandle(connection);
    return 0;
  }
  *connection_out = connection;
  return socket;
}

static void SendLogon(void) {
  Buf hello = {0};
  PbUint(&hello, 1, kProtocolVersion);
  SendAndFree(kEMsgClientHello, &hello);
  /* A login ID of the bridge's own: a logon with Playden's would replace Playden's session. */
  Buf address = {0}, logon = {0};
  PbFixed32(&address, 1, 0x50440000u | (g_config.app_id & 0xffffu) | 1u);
  PbUint(&logon, 1, kProtocolVersion);
  PbUint(&logon, 3, 0);
  PbUint(&logon, 5, kClientPackageVersion);
  PbString(&logon, 6, "english");
  PbUint(&logon, 7, 0);
  PbUint(&logon, 8, 1);
  PbBytes(&logon, 11, address.p, address.n);
  PbString(&logon, 50, g_config.account);
  PbString(&logon, 96, "Playden");
  PbUint(&logon, 102, 1);
  PbString(&logon, 108, g_config.token);
  SendMessage(kEMsgClientLogon, &logon);
  if (logon.p) memset(logon.p, 0, logon.n);
  BufFree(&logon);
  BufFree(&address);
}

static DWORD WINAPI HeartbeatThread(void* http) {
  int relays_checked = 0;
  for (;;) {
    EnterCriticalSection(&g_lock);
    const uint32_t seconds = g_heartbeat_seconds;
    const int logged_on = g_logged_on;
    LeaveCriticalSection(&g_lock);
    if (logged_on && !relays_checked) {
      relays_checked = 1;
      CheckRelays(http);
    }
    Sleep((seconds ? seconds : 9) * 1000u);
    if (logged_on) SendMessage(kEMsgClientHeartBeat, 0);
  }
}

static DWORD WINAPI SessionThread(void* unused) {
  (void)unused;
  HINTERNET http = WinHttpOpen(L"Playden", 0, 0, 0, 0);
  if (!http) {
    Log("winhttp unavailable: online features off");
    return 0;
  }
  WinHttpSetTimeouts(http, 15000, 15000, 15000, 0);
  CreateThread(0, 0, HeartbeatThread, http, 0, 0);
  DWORD wait_ms = 2000;
  uint8_t* message = Alloc(1u << 16);
  Buf assembled = {0};
  for (;;) {
    static char endpoints[16][128];
    const int count = FetchEndpoints(http, endpoints, 16);
    LogValue("directory endpoints=", (uint64_t)count);
    HINTERNET connection = 0, socket = 0;
    for (int i = 0; i < count && i < 4 && !socket; ++i) socket = OpenSocket(http, endpoints[i], &connection);
    if (socket) {
      Log("connected");
      EnterCriticalSection(&g_send_lock);
      g_socket = socket;
      LeaveCriticalSection(&g_send_lock);
      EnterCriticalSection(&g_lock);
      g_session_steam_id = kAnonymousSteamId;
      g_session_id = 0;
      g_token_count = 0;
      LeaveCriticalSection(&g_lock);
      SendLogon();
      for (;;) {
        DWORD read = 0;
        int type = 0;
        if (WinHttpWebSocketReceive(socket, message, 1u << 16, &read, &type) != 0 || type == WS_CLOSE) break;
        BufPut(&assembled, message, read);
        if (assembled.failed || assembled.n > (16u << 20)) break;
        if (type == WS_BINARY_FRAGMENT) continue;
        if (type == WS_BINARY_MESSAGE) HandleFrame(assembled.p, assembled.n, 0);
        assembled.n = 0;
      }
      BufFree(&assembled);
      Log("disconnected");
      EnterCriticalSection(&g_lock);
      const int was_logged_on = g_logged_on;
      g_logged_on = 0;
      LeaveCriticalSection(&g_lock);
      EnterCriticalSection(&g_send_lock);
      g_socket = 0;
      LeaveCriticalSection(&g_send_lock);
      WinHttpWebSocketClose(socket, 1000, 0, 0);
      WinHttpCloseHandle(socket);
      if (was_logged_on) wait_ms = 2000;
    } else {
      Log("could not connect to Steam");
    }
    if (connection) WinHttpCloseHandle(connection);
    Sleep(wait_ms);
    if (wait_ms < 120000) wait_ms *= 2;
  }
}

/* ---- Tickets ---------------------------------------------------------------------------------- */

static int WaitUntil(int (*ready)(void), DWORD timeout_ms) {
  const uint64_t end = GetTickCount64() + timeout_ms;
  for (;;) {
    EnterCriticalSection(&g_lock);
    const int ok = ready();
    LeaveCriticalSection(&g_lock);
    if (ok) return 1;
    if (GetTickCount64() >= end) return 0;
    Sleep(25);
  }
}
static int TicketInputsReady(void) { return g_logged_on && g_ownership_size && g_token_count; }
static int LoggedOn(void) { return g_logged_on; }
static uint32_t g_wanted_ack;
static int AuthListAcked(void) { return g_auth_acked >= g_wanted_ack; }
static int EncryptedDone(void) { return g_encrypted_state >= 2; }

static uint32_t TicketCrc(const uint8_t* p, size_t n) {
  uint32_t crc = 0xffffffffu;
  while (n--) {
    crc ^= *p++;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

/* Builds a session ticket for handle; 0 when Steam is not reachable in time. The ticket layout is
 * Playden's steam/backend/game_tickets.h BuildSessionTicket. */
static Ticket* MakeTicket(uint32_t handle) {
  if (!g_config.online) return 0;
  if (!WaitUntil(TicketInputsReady, 15000)) {
    Log("ticket: Steam not ready (logon, ownership ticket or connect token missing)");
    return 0;
  }
  Ticket* ticket = Alloc(sizeof(Ticket));
  if (!ticket) return 0;
  uint64_t now = 0;
  GetSystemTimeAsFileTime(&now);
  Buf b = {0};
  EnterCriticalSection(&g_lock);
  const uint64_t token = g_tokens[0];
  memmove(g_tokens, g_tokens + 1, sizeof(uint64_t) * (size_t)(--g_token_count));
  BufLE32(&b, 20);
  BufLE64(&b, token);
  BufLE64(&b, g_session_steam_id);
  BufLE32(&b, (uint32_t)(UDiv64(now, 10000000u, 0) - 11644473600ull));
  BufLE32(&b, 24);
  BufLE32(&b, 1);
  BufLE32(&b, 2);
  BufLE32(&b, 0);
  BufLE32(&b, 0);
  BufLE32(&b, (uint32_t)(GetTickCount64() - g_logon_tick));
  BufLE32(&b, g_connections);
  BufPut(&b, g_ownership, g_ownership_size);
  LeaveCriticalSection(&g_lock);
  if (b.failed || b.n > kMaxTicketBytes) {
    BufFree(&b);
    Free(ticket);
    return 0;
  }
  ticket->handle = handle;
  ticket->size = (uint32_t)b.n;
  memcpy(ticket->bytes, b.p, b.n);
  BufFree(&b);
  return ticket;
}

static void EncodeAuthTicket(Buf* body, const Ticket* ticket) {
  Buf entry = {0};
  PbUint(&entry, 1, ticket->steam_id ? 1 : 0);  /* estate */
  PbUint(&entry, 2, 1);
  if (ticket->steam_id) PbFixed64(&entry, 3, ticket->steam_id);
  PbFixed64(&entry, 4, g_config.app_id);
  PbUint(&entry, 5, 1);
  PbUint(&entry, 6, TicketCrc(ticket->bytes, ticket->size));
  PbBytes(&entry, 7, ticket->bytes, ticket->size);
  PbBytes(body, 4, entry.p, entry.n);
  BufFree(&entry);
}

/* Sends the auth list (every live ticket, the game's own and other players') and, with wait, waits
 * for Steam's ack. Call without g_lock. */
static void PublishAuthList(int wait) {
  Buf body = {0};
  EnterCriticalSection(&g_lock);
  const uint32_t sequence = ++g_auth_sequence;
  PbUint(&body, 1, (uint64_t)g_token_count);
  PbUint(&body, 2, sequence);
  PbUint(&body, 3, 0);
  for (int i = 0; i < g_ticket_count; ++i) EncodeAuthTicket(&body, g_tickets[i]);
  for (int i = 0; i < g_remote_count; ++i) EncodeAuthTicket(&body, g_remote[i]);
  PbUint(&body, 5, g_config.app_id);
  PbUint(&body, 6, sequence);
  const int count = g_ticket_count + g_remote_count;
  LeaveCriticalSection(&g_lock);
  if (SendMessage(kEMsgClientAuthList, &body) && wait) {
    g_wanted_ack = sequence;
    if (!WaitUntil(AuthListAcked, 5000)) LogValue("auth list not acknowledged, tickets=", (uint64_t)count);
  }
  BufFree(&body);
}

static void AddTicket(Ticket* ticket) {
  EnterCriticalSection(&g_lock);
  if (g_ticket_count == kMaxTickets) {
    Free(g_tickets[0]);
    memmove(g_tickets, g_tickets + 1, sizeof(Ticket*) * (kMaxTickets - 1));
    --g_ticket_count;
  }
  g_tickets[g_ticket_count++] = ticket;
  LeaveCriticalSection(&g_lock);
  PublishAuthList(1);
}

static Ticket* FindTicket(uint32_t handle) {
  for (int i = 0; i < g_ticket_count; ++i)
    if (g_tickets[i]->handle == handle) return g_tickets[i];
  return 0;
}

static void PostAuthResult(uint64_t steam_id, uint32_t response) {
  struct {
    uint64_t id;
    uint32_t response, pad;
    uint64_t owner;
  } result = {steam_id, response, 0, steam_id};
  PostCallback(kCallbackValidateAuthTicket, &result, sizeof result);
}

/* ---- ISteamUser ------------------------------------------------------------------------------- */

typedef uint32_t(METHOD* GetAuthSessionTicketFn)(void*, void*, int, uint32_t*, const void*);
typedef uint32_t(METHOD* GetAuthSessionTicketOldFn)(void*, void*, int, uint32_t*);
typedef uint32_t(METHOD* GetAuthTicketForWebApiFn)(void*, const char*);
typedef int(METHOD* BeginAuthSessionFn)(void*, const void*, int, uint64_t);
typedef void(METHOD* EndAuthSessionFn)(void*, uint64_t);
typedef void(METHOD* CancelAuthTicketFn)(void*, uint32_t);
typedef uint64_t(METHOD* RequestEncryptedAppTicketFn)(void*, void*, int);
typedef int8_t(METHOD* GetEncryptedAppTicketFn)(void*, void*, int, uint32_t*);

enum { kUser, kUtils, kMatchmaking, kFriends };
/* Methods past the ones the bridge answers still reach the emulator, so the copy covers the whole table: a copied
 * table is only as long as this, and a slot left empty is a call through null. Reading past the end of a real table
 * stays inside the emulator's read-only data. */
#define kVtableEntries 128
typedef struct {
  void** original;
  void* table[kVtableEntries];
  int kind, version;
} Vtable;
static Vtable g_vtables[16];
static int g_vtable_count;
static CRITICAL_SECTION g_patch_lock;

static Vtable* VtableOf(void* self) {
  void** current = *(void***)self;
  for (int i = 0; i < g_vtable_count; ++i)
    if (g_vtables[i].table == (void*)current) return &g_vtables[i];
  return 0;
}
#define ORIGINAL(type, self, slot) ((type)VtableOf(self)->original[slot])

/* Vtable slots by ISteamUser version: SteamUser023 adds GetAuthTicketForWebApi at 14. */
static int SlotSessionTicket(int v) { (void)v; return 13; }
static int SlotWebApi(int v) { return v >= 23 ? 14 : -1; }
static int SlotBeginAuth(int v) { return v >= 23 ? 15 : 14; }
static int SlotEndAuth(int v) { return v >= 23 ? 16 : 15; }
static int SlotCancel(int v) { return v >= 23 ? 17 : 16; }
static int SlotRequestEncrypted(int v) { return v >= 23 ? 21 : 20; }
static int SlotGetEncrypted(int v) { return v >= 23 ? 22 : 21; }

static uint32_t TicketFinish(void* buffer, int max, uint32_t* size, uint32_t handle) {
  if (!handle) return handle;
  Ticket* ticket = MakeTicket(handle);
  if (!ticket) return handle;  /* the emulator's ticket stays */
  if (ticket->size > (uint32_t)max) {
    Log("ticket: the game's buffer is too small");
    Free(ticket);
    return handle;
  }
  memcpy(buffer, ticket->bytes, ticket->size);
  if (size) *size = ticket->size;
  AddTicket(ticket);
  LogValue("session ticket handle=", handle);
  return handle;
}
static METHOD uint32_t UserGetAuthSessionTicket(void* self, void* buffer, int max, uint32_t* size, const void* identity) {
  Vtable* v = VtableOf(self);
  return TicketFinish(buffer, max, size,
                      ((GetAuthSessionTicketFn)v->original[SlotSessionTicket(v->version)])(self, buffer, max, size, identity));
}
/* SteamUser021 and earlier take no identity; on 32-bit the callee pops its own arguments, so the count matters. */
static METHOD uint32_t UserGetAuthSessionTicketOld(void* self, void* buffer, int max, uint32_t* size) {
  Vtable* v = VtableOf(self);
  return TicketFinish(buffer, max, size,
                      ((GetAuthSessionTicketOldFn)v->original[SlotSessionTicket(v->version)])(self, buffer, max, size));
}

static METHOD uint32_t UserGetAuthTicketForWebApi(void* self, const char* identity) {
  Vtable* v = VtableOf(self);
  const uint32_t handle = ((GetAuthTicketForWebApiFn)v->original[SlotWebApi(v->version)])(self, identity);
  if (!handle) return handle;
  Ticket* ticket = MakeTicket(handle);
  if (ticket) {
    AddTicket(ticket);
    LogValue("web api ticket handle=", handle);
  }
  return handle;
}

static METHOD void UserCancelAuthTicket(void* self, uint32_t handle) {
  Vtable* v = VtableOf(self);
  int removed = 0;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_ticket_count; ++i) {
    if (g_tickets[i]->handle != handle) continue;
    Free(g_tickets[i]);
    memmove(g_tickets + i, g_tickets + i + 1, sizeof(Ticket*) * (size_t)(g_ticket_count - i - 1));
    --g_ticket_count;
    removed = 1;
    break;
  }
  LeaveCriticalSection(&g_lock);
  if (removed) PublishAuthList(1);
  ((CancelAuthTicketFn)v->original[SlotCancel(v->version)])(self, handle);
}

/* Another player's ticket (this game hosts): Steam checks it once it is on the auth list and answers
 * with ClientTicketAuthComplete. A ticket in Steam's format is accepted at once, as Steam's client
 * does, and a refusal from Steam is passed on as ValidateAuthTicketResponse_t. */
static METHOD int UserBeginAuthSession(void* self, const void* data, int size, uint64_t steam_id) {
  Vtable* v = VtableOf(self);
  const int emulator = ((BeginAuthSessionFn)v->original[SlotBeginAuth(v->version)])(self, data, size, steam_id);
  const uint8_t* p = data;
  const int steam_format = g_config.online && size >= 52 && size <= kMaxTicketBytes && LE32(p) == 20 &&
                           (LE32(p + 12) | (uint64_t)LE32(p + 16) << 32) == steam_id;
  if (!steam_format) return emulator;
  Ticket* ticket = Alloc(sizeof(Ticket));
  if (!ticket) return emulator;
  ticket->steam_id = steam_id;
  ticket->size = (uint32_t)size;
  memcpy(ticket->bytes, data, (size_t)size);
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_remote_count; ++i)
    if (g_remote[i]->steam_id == steam_id) {
      Free(g_remote[i]);
      g_remote[i--] = g_remote[--g_remote_count];
    }
  if (g_remote_count == kMaxRemoteTickets) {
    Free(g_remote[0]);
    g_remote[0] = g_remote[--g_remote_count];
  }
  g_remote[g_remote_count++] = ticket;
  LeaveCriticalSection(&g_lock);
  PublishAuthList(0);
  LogValue("auth session begun for another player, emulator result=", (uint64_t)(uint32_t)emulator);
  if (emulator != 0) PostAuthResult(steam_id, 0);  /* the emulator refused it: answer OK ourselves */
  return 0;
}

static METHOD void UserEndAuthSession(void* self, uint64_t steam_id) {
  Vtable* v = VtableOf(self);
  int removed = 0;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_remote_count; ++i)
    if (g_remote[i]->steam_id == steam_id) {
      Free(g_remote[i]);
      g_remote[i--] = g_remote[--g_remote_count];
      removed = 1;
    }
  LeaveCriticalSection(&g_lock);
  if (removed) PublishAuthList(0);
  ((EndAuthSessionFn)v->original[SlotEndAuth(v->version)])(self, steam_id);
}

static METHOD uint64_t UserRequestEncryptedAppTicket(void* self, void* data, int size) {
  Vtable* v = VtableOf(self);
  if (g_config.online && size >= 0 && size <= kMaxTicketBytes && WaitUntil(TicketInputsReady, 15000)) {
    EnterCriticalSection(&g_lock);
    g_encrypted_state = 1;
    LeaveCriticalSection(&g_lock);
    Buf body = {0};
    PbUint(&body, 1, g_config.app_id);
    if (size) PbBytes(&body, 2, data, (size_t)size);
    if (SendMessage(kEMsgClientRequestEncryptedAppTicket, &body) && !WaitUntil(EncryptedDone, 10000))
      Log("encrypted app ticket: no answer");
    BufFree(&body);
  }
  return ((RequestEncryptedAppTicketFn)v->original[SlotRequestEncrypted(v->version)])(self, data, size);
}

static METHOD int8_t UserGetEncryptedAppTicket(void* self, void* buffer, int max, uint32_t* size) {
  Vtable* v = VtableOf(self);
  EnterCriticalSection(&g_lock);
  const int have = g_encrypted_state == 2 && max >= 0 && g_encrypted_size <= (uint32_t)max;
  if (have) {
    memcpy(buffer, g_encrypted, g_encrypted_size);
    if (size) *size = g_encrypted_size;
  }
  LeaveCriticalSection(&g_lock);
  if (have) return 1;
  return ((GetEncryptedAppTicketFn)v->original[SlotGetEncrypted(v->version)])(self, buffer, max, size);
}

/* ---- ISteamUtils: the bridge's API calls ------------------------------------------------------ */

typedef int8_t(METHOD* IsAPICallCompletedFn)(void*, uint64_t, int8_t*);
typedef int(METHOD* GetAPICallFailureReasonFn)(void*, uint64_t);
typedef int8_t(METHOD* GetAPICallResultFn)(void*, uint64_t, void*, int, int, int8_t*);
#define kSlotIsAPICallCompleted 10
#define kSlotGetAPICallFailureReason 11
#define kSlotGetAPICallResult 12

static METHOD int8_t UtilsIsAPICallCompleted(void* self, uint64_t call, int8_t* failed) {
  if (!IsOurCall(call)) return ORIGINAL(IsAPICallCompletedFn, self, kSlotIsAPICallCompleted)(self, call, failed);
  EnterCriticalSection(&g_dispatch_lock);
  const int done = FindResultLocked(call, 0) != 0;
  LeaveCriticalSection(&g_dispatch_lock);
  if (failed) *failed = 0;
  return (int8_t)done;
}
static METHOD int UtilsGetAPICallFailureReason(void* self, uint64_t call) {
  if (!IsOurCall(call)) return ORIGINAL(GetAPICallFailureReasonFn, self, kSlotGetAPICallFailureReason)(self, call);
  return -1;  /* k_ESteamAPICallFailureNone */
}
static METHOD int8_t UtilsGetAPICallResult(void* self, uint64_t call, void* out, int size, int expected, int8_t* failed) {
  if (!IsOurCall(call))
    return ORIGINAL(GetAPICallResultFn, self, kSlotGetAPICallResult)(self, call, out, size, expected, failed);
  return CopyResult(call, out, size, expected, failed);
}

/* ---- ISteamMatchmaking ------------------------------------------------------------------------ */

static void AddFilter(const char* key, const char* value, int32_t comparison, int32_t type) {
  Buf filter = {0};
  PbString(&filter, 1, key ? key : "");
  PbString(&filter, 2, value ? value : "");
  PbUint(&filter, 3, (uint64_t)(int64_t)comparison);
  PbUint(&filter, 4, (uint64_t)(int64_t)type);
  EnterCriticalSection(&g_lock);
  PbBytes(&g_filters, 6, filter.p, filter.n);
  LeaveCriticalSection(&g_lock);
  BufFree(&filter);
}
static void NumberText(char* out, int64_t value) {
  Buf text = {0};
  if (value < 0) {
    LogText(&text, "-");
    value = -value;
  }
  LogNumber(&text, (uint64_t)value);
  CopyValue(out, 24, text.failed ? "" : (const char*)text.p, text.failed ? 0 : text.n);
  BufFree(&text);
}

static METHOD uint64_t MmRequestLobbyList(void* self) {
  (void)self;
  const uint64_t call = NewCall();
  if (!WaitUntil(LoggedOn, 10000)) {
    Log("lobby list: not signed in to Steam");
    const uint32_t none = 0;
    PostResult(call, kCallbackLobbyMatchList, &none, sizeof none);
    return call;
  }
  Buf body = {0};
  EnterCriticalSection(&g_lock);
  PbUint(&body, 1, g_config.app_id);
  PbUint(&body, 3, (uint64_t)(int64_t)g_filter_count);
  PbUint(&body, 4, 0);
  BufPut(&body, g_filters.p, g_filters.n);
  BufFree(&g_filters);
  g_filter_count = 50;
  g_pending_list = call;
  LeaveCriticalSection(&g_lock);
  SendAndFree(kEMsgClientMMSGetLobbyList, &body);
  Log("lobby list requested");
  return call;
}
static METHOD void MmAddStringFilter(void* self, const char* key, const char* value, int comparison) {
  (void)self;
  AddFilter(key, value, comparison, 0);
}
static METHOD void MmAddNumericalFilter(void* self, const char* key, int value, int comparison) {
  (void)self;
  char text[24];
  NumberText(text, value);
  AddFilter(key, text, comparison, 1);
}
static METHOD void MmAddNearValueFilter(void* self, const char* key, int value) {
  (void)self;
  char text[24];
  NumberText(text, value);
  AddFilter(key, text, 0, 3);
}
static METHOD void MmAddSlotsAvailableFilter(void* self, int slots) {
  (void)self;
  char text[24];
  NumberText(text, slots);
  AddFilter("", text, 0, 2);
}
static METHOD void MmAddDistanceFilter(void* self, int distance) {
  (void)self;
  char text[24];
  NumberText(text, distance);
  AddFilter("", text, 0, 4);
}
static METHOD void MmAddResultCountFilter(void* self, int count) {
  (void)self;
  EnterCriticalSection(&g_lock);
  g_filter_count = count;
  LeaveCriticalSection(&g_lock);
}
static METHOD void MmAddCompatibleMembersFilter(void* self, uint64_t lobby) {
  (void)self;
  (void)lobby;
}
static METHOD uint64_t* MmGetLobbyByIndex(void* self, uint64_t* out, int index) {
  (void)self;
  EnterCriticalSection(&g_lock);
  *out = index >= 0 && index < g_list_count ? g_list[index] : 0;
  LeaveCriticalSection(&g_lock);
  return out;
}
static METHOD uint64_t MmCreateLobby(void* self, int type, int max_members) {
  (void)self;
  const uint64_t call = NewCall();
  if (!WaitUntil(LoggedOn, 10000)) {
    Log("create lobby: not signed in to Steam");
    struct {
      int32_t result, pad;
      uint64_t lobby;
    } failed = {3 /* k_EResultNoConnection */, 0, 0};
    PostResult(call, kCallbackLobbyCreated, &failed, sizeof failed);
    PostCallback(kCallbackLobbyCreated, &failed, sizeof failed);
    return call;
  }
  Buf body = {0};
  EnterCriticalSection(&g_lock);
  g_pending_create = call;
  g_pending_create_type = type;
  g_pending_create_max = max_members;
  PbUint(&body, 1, g_config.app_id);
  PbUint(&body, 2, (uint64_t)(int64_t)max_members);
  PbUint(&body, 3, (uint64_t)(int64_t)type);
  PbUint(&body, 4, 0);
  PbUint(&body, 5, 0);
  PbString(&body, 8, PersonaName());
  LeaveCriticalSection(&g_lock);
  SendAndFree(kEMsgClientMMSCreateLobby, &body);
  LogLobby("create lobby requested", 0, type);
  return call;
}
static METHOD uint64_t MmJoinLobby(void* self, uint64_t lobby) {
  (void)self;
  const uint64_t call = NewCall();
  if (!WaitUntil(LoggedOn, 10000)) {
    Log("join lobby: not signed in to Steam");
    LobbyEnterData enter = {lobby, 0, 0, {0}, 2 /* k_EChatRoomEnterResponseDoesntExist */, 0};
    PostResult(call, kCallbackLobbyEnter, &enter, sizeof enter);
    return call;
  }
  Buf body = {0};
  EnterCriticalSection(&g_lock);
  int slot = 0;
  for (int i = 0; i < 8; ++i)
    if (!g_pending_join[i].call) slot = i;
  g_pending_join[slot].lobby = lobby;
  g_pending_join[slot].call = call;
  PbUint(&body, 1, g_config.app_id);
  PbFixed64(&body, 2, lobby);
  PbString(&body, 3, PersonaName());
  LeaveCriticalSection(&g_lock);
  SendAndFree(kEMsgClientMMSJoinLobby, &body);
  LogLobby("join lobby requested", lobby, 0);
  return call;
}
static METHOD void MmLeaveLobby(void* self, uint64_t lobby_id) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  if (lobby) lobby->joined = 0;
  LeaveCriticalSection(&g_lock);
  Buf body = {0};
  PbUint(&body, 1, g_config.app_id);
  PbFixed64(&body, 2, lobby_id);
  SendAndFree(kEMsgClientMMSLeaveLobby, &body);
}
static METHOD int8_t MmInviteUserToLobby(void* self, uint64_t lobby, uint64_t user) {
  (void)self;
  Buf body = {0};
  PbUint(&body, 1, g_config.app_id);
  PbFixed64(&body, 2, lobby);
  PbFixed64(&body, 3, user);
  LogLobby("invite sent", lobby, 0);
  return (int8_t)SendAndFree(kEMsgClientMMSInviteToLobby, &body);
}
static METHOD int MmGetNumLobbyMembers(void* self, uint64_t lobby_id) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  const int count = !lobby ? 0 : lobby->joined ? lobby->member_count : lobby->num_members;
  LeaveCriticalSection(&g_lock);
  return count;
}
static METHOD uint64_t* MmGetLobbyMemberByIndex(void* self, uint64_t* out, uint64_t lobby_id, int index) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  *out = lobby && index >= 0 && index < lobby->member_count ? lobby->members[index].id : 0;
  LeaveCriticalSection(&g_lock);
  return out;
}
static METHOD const char* MmGetLobbyData(void* self, uint64_t lobby_id, const char* key) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  const char* value = lobby && key ? KvGet(&lobby->data, key) : "";
  LeaveCriticalSection(&g_lock);
  return value;
}
static METHOD int8_t MmSetLobbyData(void* self, uint64_t lobby_id, const char* key, const char* value) {
  (void)self;
  if (!key || !value) return 0;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  if (lobby) KvSet(&lobby->data, key, Strlen(key), value, Strlen(value));
  LeaveCriticalSection(&g_lock);
  if (!lobby) return 0;
  SendLobbyData(lobby_id, 0);
  PostLobbyUpdated(lobby_id, lobby_id);
  return 1;
}
static METHOD int MmGetLobbyDataCount(void* self, uint64_t lobby_id) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  const int count = lobby ? lobby->data.count : 0;
  LeaveCriticalSection(&g_lock);
  return count;
}
static METHOD int8_t MmGetLobbyDataByIndex(void* self, uint64_t lobby_id, int index, char* key, int key_size, char* value,
                                           int value_size) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  const int ok = lobby && index >= 0 && index < lobby->data.count && key_size > 0 && value_size > 0;
  if (ok) {
    CopyValue(key, (size_t)key_size, lobby->data.items[index].key, Strlen(lobby->data.items[index].key) < (size_t)key_size
                                                                         ? Strlen(lobby->data.items[index].key) : (size_t)key_size - 1);
    CopyValue(value, (size_t)value_size, lobby->data.items[index].value,
              Strlen(lobby->data.items[index].value) < (size_t)value_size ? Strlen(lobby->data.items[index].value)
                                                                          : (size_t)value_size - 1);
  }
  LeaveCriticalSection(&g_lock);
  return (int8_t)ok;
}
static METHOD int8_t MmDeleteLobbyData(void* self, uint64_t lobby_id, const char* key) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  const int removed = lobby && key && KvDelete(&lobby->data, key);
  LeaveCriticalSection(&g_lock);
  if (removed) SendLobbyData(lobby_id, 0);
  return (int8_t)removed;
}
static METHOD const char* MmGetLobbyMemberData(void* self, uint64_t lobby_id, uint64_t user, const char* key) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  Member* member = lobby ? FindMember(lobby, user, 0) : 0;
  const char* value = member && key ? KvGet(&member->data, key) : "";
  LeaveCriticalSection(&g_lock);
  return value;
}
static METHOD void MmSetLobbyMemberData(void* self, uint64_t lobby_id, const char* key, const char* value) {
  (void)self;
  if (!key || !value) return;
  const uint64_t me = SelfId();
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  Member* member = lobby ? FindMember(lobby, me, 1) : 0;
  if (member) KvSet(&member->data, key, Strlen(key), value, Strlen(value));
  LeaveCriticalSection(&g_lock);
  if (member) {
    SendLobbyData(lobby_id, me);
    PostLobbyUpdated(lobby_id, me);
  }
}
static METHOD int8_t MmSendLobbyChatMsg(void* self, uint64_t lobby, const void* data, int size) {
  (void)self;
  if (size < 0 || size > 4096) return 0;
  Buf body = {0};
  PbUint(&body, 1, g_config.app_id);
  PbFixed64(&body, 2, lobby);
  PbFixed64(&body, 3, 0);
  PbBytes(&body, 4, data, (size_t)size);
  return (int8_t)SendAndFree(kEMsgClientMMSSendLobbyChatMsg, &body);
}
static METHOD int MmGetLobbyChatEntry(void* self, uint64_t lobby, int chat_id, uint64_t* user, void* out, int size,
                                      int* type) {
  (void)self;
  (void)lobby;
  int copied = 0;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < g_chat_count; ++i) {
    if (g_chat[i]->id != chat_id) continue;
    copied = size < 0 ? 0 : (uint32_t)size < g_chat[i]->size ? size : (int)g_chat[i]->size;
    if (copied) memcpy(out, g_chat[i]->bytes, (size_t)copied);
    if (user) *user = g_chat[i]->sender;
    if (type) *type = 1;
  }
  LeaveCriticalSection(&g_lock);
  return copied;
}
static METHOD int8_t MmRequestLobbyData(void* self, uint64_t lobby) {
  (void)self;
  Buf body = {0};
  PbUint(&body, 1, g_config.app_id);
  PbFixed64(&body, 2, lobby);
  return (int8_t)SendAndFree(kEMsgClientMMSGetLobbyData, &body);
}
static METHOD void MmSetLobbyGameServer(void* self, uint64_t lobby, uint32_t ip, uint16_t port, uint64_t server) {
  (void)self;
  Buf body = {0}, address = {0};
  PbUint(&body, 1, g_config.app_id);
  PbFixed64(&body, 2, lobby);
  PbUint(&body, 3, ip);
  PbUint(&body, 4, port);
  PbFixed64(&body, 5, server);
  PbFixed32(&address, 1, ip);
  PbBytes(&body, 6, address.p, address.n);
  BufFree(&address);
  SendAndFree(kEMsgClientMMSSetLobbyGameServer, &body);
  LogLobby("lobby game server set", lobby, port);
}
static METHOD int8_t MmGetLobbyGameServer(void* self, uint64_t lobby_id, uint32_t* ip, uint16_t* port, uint64_t* server) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  const int ok = lobby && (lobby->server_ip || lobby->server_id);
  if (ok) {
    if (ip) *ip = lobby->server_ip;
    if (port) *port = lobby->server_port;
    if (server) *server = lobby->server_id;
  }
  LeaveCriticalSection(&g_lock);
  return (int8_t)ok;
}
static int8_t UpdateLobby(uint64_t lobby_id, int field, int32_t value) {
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  if (lobby && field == 0) lobby->max_members = value;
  if (lobby && field == 1) lobby->type = value;
  LeaveCriticalSection(&g_lock);
  if (lobby) SendLobbyData(lobby_id, 0);
  return lobby != 0;
}
static METHOD int8_t MmSetLobbyMemberLimit(void* self, uint64_t lobby, int max) {
  (void)self;
  return UpdateLobby(lobby, 0, max);
}
static METHOD int MmGetLobbyMemberLimit(void* self, uint64_t lobby_id) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  const int max = lobby ? lobby->max_members : 0;
  LeaveCriticalSection(&g_lock);
  return max;
}
static METHOD int8_t MmSetLobbyType(void* self, uint64_t lobby, int type) {
  (void)self;
  return UpdateLobby(lobby, 1, type);
}
static METHOD int8_t MmSetLobbyJoinable(void* self, uint64_t lobby_id, int8_t joinable) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  if (lobby) lobby->joinable = joinable != 0;
  LeaveCriticalSection(&g_lock);
  LogLobby("lobby joinable", lobby_id, joinable != 0);
  return lobby != 0;
}
static METHOD uint64_t* MmGetLobbyOwner(void* self, uint64_t* out, uint64_t lobby_id) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Lobby* lobby = FindLobby(lobby_id, 0);
  *out = lobby ? lobby->owner : 0;
  LeaveCriticalSection(&g_lock);
  return out;
}
static METHOD int8_t MmSetLobbyOwner(void* self, uint64_t lobby, uint64_t owner) {
  (void)self;
  Buf body = {0};
  PbUint(&body, 1, g_config.app_id);
  PbFixed64(&body, 2, lobby);
  PbFixed64(&body, 3, owner);
  return (int8_t)SendAndFree(kEMsgClientMMSSetLobbyOwner, &body);
}
static METHOD int8_t MmSetLinkedLobby(void* self, uint64_t lobby, uint64_t other) {
  (void)self;
  Buf body = {0};
  PbUint(&body, 1, g_config.app_id);
  PbFixed64(&body, 2, lobby);
  PbFixed64(&body, 3, other);
  return (int8_t)SendAndFree(kEMsgClientMMSSetLobbyLinked, &body);
}

/* ISteamMatchmaking009 slots 4-37 (the favorites, 0-3, stay the emulator's). */
static void* const kMatchmakingMethods[38] = {
    0, 0, 0, 0,
    (void*)MmRequestLobbyList, (void*)MmAddStringFilter, (void*)MmAddNumericalFilter, (void*)MmAddNearValueFilter,
    (void*)MmAddSlotsAvailableFilter, (void*)MmAddDistanceFilter, (void*)MmAddResultCountFilter,
    (void*)MmAddCompatibleMembersFilter, (void*)MmGetLobbyByIndex, (void*)MmCreateLobby, (void*)MmJoinLobby,
    (void*)MmLeaveLobby, (void*)MmInviteUserToLobby, (void*)MmGetNumLobbyMembers, (void*)MmGetLobbyMemberByIndex,
    (void*)MmGetLobbyData, (void*)MmSetLobbyData, (void*)MmGetLobbyDataCount, (void*)MmGetLobbyDataByIndex,
    (void*)MmDeleteLobbyData, (void*)MmGetLobbyMemberData, (void*)MmSetLobbyMemberData, (void*)MmSendLobbyChatMsg,
    (void*)MmGetLobbyChatEntry, (void*)MmRequestLobbyData, (void*)MmSetLobbyGameServer, (void*)MmGetLobbyGameServer,
    (void*)MmSetLobbyMemberLimit, (void*)MmGetLobbyMemberLimit, (void*)MmSetLobbyType, (void*)MmSetLobbyJoinable,
    (void*)MmGetLobbyOwner, (void*)MmSetLobbyOwner, (void*)MmSetLinkedLobby,
};

/* ---- ISteamFriends ---------------------------------------------------------------------------- */

static METHOD int FrGetFriendCount(void* self, int flags) {
  (void)self;
  (void)flags;
  EnterCriticalSection(&g_lock);
  int count = 0;
  for (int i = 0; i < g_friend_count; ++i) count += g_friends[i].relationship == 3;
  LeaveCriticalSection(&g_lock);
  return count;
}
static METHOD uint64_t* FrGetFriendByIndex(void* self, uint64_t* out, int index, int flags) {
  (void)self;
  (void)flags;
  *out = 0;
  EnterCriticalSection(&g_lock);
  for (int i = 0, seen = 0; i < g_friend_count; ++i)
    if (g_friends[i].relationship == 3 && seen++ == index) *out = g_friends[i].id;
  LeaveCriticalSection(&g_lock);
  return out;
}
static METHOD int FrGetFriendRelationship(void* self, uint64_t id) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Friend* f = FindFriend(id, 0);
  const int relationship = f ? (int)f->relationship : 0;
  LeaveCriticalSection(&g_lock);
  return relationship;
}
static METHOD int FrGetFriendPersonaState(void* self, uint64_t id) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Friend* f = FindFriend(id, 0);
  const int state = f ? (int)f->state : 0;
  LeaveCriticalSection(&g_lock);
  return state;
}
static METHOD const char* FrGetFriendPersonaName(void* self, uint64_t id) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Friend* f = FindFriend(id, 0);
  const char* name = f && f->name ? f->name : "";
  if (id == g_session_steam_id) name = PersonaName();
  LeaveCriticalSection(&g_lock);
  return name;
}
/* FriendGameInfo_t: {u64 game, u32 ip, u16 port, u16 query port, u64 lobby}. */
static METHOD int8_t FrGetFriendGamePlayed(void* self, uint64_t id, void* out) {
  (void)self;
  EnterCriticalSection(&g_lock);
  Friend* f = FindFriend(id, 0);
  const int playing = f && f->game_id;
  if (playing && out) {
    uint8_t* p = out;
    memset(p, 0, 24);
    memcpy(p, &f->game_id, 8);
    memcpy(p + 8, &f->server_ip, 4);
    const uint16_t port = (uint16_t)f->server_port;
    memcpy(p + 12, &port, 2);
    memcpy(p + 16, &f->lobby, 8);
  }
  LeaveCriticalSection(&g_lock);
  return (int8_t)playing;
}
static void* const kFriendsMethods[9] = {
    0, 0, 0, (void*)FrGetFriendCount, (void*)FrGetFriendByIndex, (void*)FrGetFriendRelationship,
    (void*)FrGetFriendPersonaState, (void*)FrGetFriendPersonaName, (void*)FrGetFriendGamePlayed,
};

/* ---- Interfaces ------------------------------------------------------------------------------- */

static int VersionAfter(const char* version, const char* prefix) {
  if (!StartsWith(version, prefix)) return -1;
  const char* digits = version + Strlen(prefix);
  for (const char* d = digits; *d; ++d)
    if (*d < '0' || *d > '9') return -1;
  return *digits ? (int)ParseNumber(digits, Strlen(digits)) : -1;
}

/* Points an interface object at a copy of its vtable with the bridge's methods in place. */
static void* WrapInterface(void* object, const char* version) {
  if (!object || !version || !g_config.online) return object;
  int kind = -1, count = 0, v;
  if ((v = VersionAfter(version, "SteamUser")) >= 20 && v <= 23) kind = kUser, count = v >= 23 ? 33 : 32;
  else if ((v = VersionAfter(version, "SteamUtils")) >= 9 && v <= 11) kind = kUtils, count = 13;
  else if ((v = VersionAfter(version, "SteamMatchMaking")) == 9) kind = kMatchmaking, count = 38;
  else if ((v = VersionAfter(version, "SteamFriends")) >= 15 && v <= 18) kind = kFriends, count = 9;
  if (kind < 0) {
    if (StartsWith(version, "SteamNetworking")) {
      static char seen[8][48];
      EnterCriticalSection(&g_patch_lock);
      int known = 0, free_slot = -1;
      for (int i = 0; i < 8; ++i) {
        if (seen[i][0] && StrEq(seen[i], version)) known = 1;
        if (!seen[i][0] && free_slot < 0) free_slot = i;
      }
      if (!known && free_slot >= 0) CopyValue(seen[free_slot], 48, version, Strlen(version) < 47 ? Strlen(version) : 47);
      LeaveCriticalSection(&g_patch_lock);
      if (!known) {
        Buf line = {0};
        LogText(&line, "game uses ");
        LogText(&line, version);
        LogText(&line, ": Steam networking is not bridged; connections through Steam's relays will not open");
        LogLine(&line);
      }
    }
    return object;
  }
  EnterCriticalSection(&g_patch_lock);
  if (!VtableOf(object) && g_vtable_count < 16) {
    Vtable* table = &g_vtables[g_vtable_count];
    void** original = *(void***)object;
    table->original = original;
    table->kind = kind;
    table->version = v;
    for (int i = 0; i < kVtableEntries; ++i) table->table[i] = original[i];
    if (kind == kUser) {
      table->table[SlotSessionTicket(v)] = v >= 22 ? (void*)UserGetAuthSessionTicket : (void*)UserGetAuthSessionTicketOld;
      if (SlotWebApi(v) >= 0) table->table[SlotWebApi(v)] = (void*)UserGetAuthTicketForWebApi;
      table->table[SlotBeginAuth(v)] = (void*)UserBeginAuthSession;
      table->table[SlotEndAuth(v)] = (void*)UserEndAuthSession;
      table->table[SlotCancel(v)] = (void*)UserCancelAuthTicket;
      table->table[SlotRequestEncrypted(v)] = (void*)UserRequestEncryptedAppTicket;
      table->table[SlotGetEncrypted(v)] = (void*)UserGetEncryptedAppTicket;
    } else if (kind == kUtils) {
      table->table[kSlotIsAPICallCompleted] = (void*)UtilsIsAPICallCompleted;
      table->table[kSlotGetAPICallFailureReason] = (void*)UtilsGetAPICallFailureReason;
      table->table[kSlotGetAPICallResult] = (void*)UtilsGetAPICallResult;
    } else {
      void* const* methods = kind == kMatchmaking ? kMatchmakingMethods : kFriendsMethods;
      for (int i = 0; i < count; ++i)
        if (methods[i]) table->table[i] = methods[i];
    }
    ++g_vtable_count;
    *(void***)object = table->table;
    Buf line = {0};
    LogText(&line, "bridged ");
    LogText(&line, version);
    LogLine(&line);
  }
  LeaveCriticalSection(&g_patch_lock);
  return object;
}

/* ---- Callbacks: GetTicketForWebApiResponse_t carries the ticket bytes ------------------------- */

#define kWebApiTicketBytes 2560

/* Puts the real ticket into a GetTicketForWebApiResponse_t {handle, eresult, size, bytes[2560]}. */
static void PatchWebApiResponse(void* param) {
  if (!param) return;
  uint8_t* p = param;
  const uint32_t handle = LE32(p);
  EnterCriticalSection(&g_lock);
  Ticket* ticket = FindTicket(handle);
  if (ticket && ticket->size <= kWebApiTicketBytes) {
    const int32_t ok = 1;
    const int32_t size = (int32_t)ticket->size;
    memcpy(p + 4, &ok, 4);
    memcpy(p + 8, &size, 4);
    memcpy(p + 12, ticket->bytes, ticket->size);
  }
  LeaveCriticalSection(&g_lock);
}

typedef void(METHOD* RunOneFn)(void*, void*);
typedef struct {
  void** original;
  void* table[3];
} CallbackVtable;
static CallbackVtable g_callback_vtables[16];
static int g_callback_vtable_count;

static CallbackVtable* CallbackVtableOf(void* self) {
  void** current = *(void***)self;
  for (int i = 0; i < g_callback_vtable_count; ++i)
    if (g_callback_vtables[i].table == (void*)current) return &g_callback_vtables[i];
  return 0;
}
/* CCallbackBase's Run(void*, bool, SteamAPICall_t) (slot 0 under MSVC) and Run(void*) (slot 1). */
static METHOD void CallbackRunResult(void* self, void* param, int8_t failed, uint64_t call) {
  PatchWebApiResponse(param);
  ((RunResultFn)CallbackVtableOf(self)->original[0])(self, param, failed, call);
}
static METHOD void CallbackRunOne(void* self, void* param) {
  PatchWebApiResponse(param);
  ((RunOneFn)CallbackVtableOf(self)->original[1])(self, param);
}

/* ---- Exports ---------------------------------------------------------------------------------- */

void* g_slots[SLOT_COUNT];
__declspec(dllexport) void* g_pSteamClientGameServer;

static uint64_t MissingExport(void) { return 0; }

typedef void* (*FindOrCreateUserInterfaceFn)(int32_t, const char*);
typedef void* (*CreateInterfaceFn)(const char*);
typedef void* (*AccessorFn)(void);
typedef void (*RegisterCallbackFn)(void*, int);
typedef void (*UnregisterCallbackFn)(void*);
typedef void (*RegisterCallResultFn)(void*, uint64_t);
typedef void (*UnregisterCallResultFn)(void*, uint64_t);
typedef void (*RunCallbacksFn)(void);
typedef int8_t (*GetNextCallbackFn)(int32_t, void*);
typedef void (*FreeLastCallbackFn)(int32_t);
typedef int8_t (*ManualGetAPICallResultFn)(int32_t, uint64_t, void*, int, int, int8_t*);

void* SteamInternal_FindOrCreateUserInterface(int32_t user, const char* version) {
  return WrapInterface(((FindOrCreateUserInterfaceFn)g_slots[SLOT_SteamInternal_FindOrCreateUserInterface])(user, version),
                       version);
}
void* SteamInternal_CreateInterface(const char* version) {
  return WrapInterface(((CreateInterfaceFn)g_slots[SLOT_SteamInternal_CreateInterface])(version), version);
}
#define ACCESSOR(name, version) \
  void* name(void) { return WrapInterface(((AccessorFn)g_slots[SLOT_##name])(), version); }
ACCESSOR(SteamAPI_SteamUser_v020, "SteamUser020")
ACCESSOR(SteamAPI_SteamUser_v021, "SteamUser021")
ACCESSOR(SteamAPI_SteamUser_v022, "SteamUser022")
ACCESSOR(SteamAPI_SteamUser_v023, "SteamUser023")
ACCESSOR(SteamAPI_SteamUtils_v009, "SteamUtils009")
ACCESSOR(SteamAPI_SteamUtils_v010, "SteamUtils010")
ACCESSOR(SteamAPI_SteamUtils_v011, "SteamUtils011")
ACCESSOR(SteamAPI_SteamMatchmaking_v009, "SteamMatchMaking009")
ACCESSOR(SteamAPI_SteamFriends_v017, "SteamFriends017")
ACCESSOR(SteamAPI_SteamFriends_v018, "SteamFriends018")
ACCESSOR(SteamAPI_SteamNetworking_v006, "SteamNetworking006")

void SteamAPI_RegisterCallback(void* callback, int id) {
  if (callback && g_config.online) {
    EnterCriticalSection(&g_dispatch_lock);
    if (g_registered_count < 512) {
      g_registered[g_registered_count].object = callback;
      g_registered[g_registered_count++].id = id;
    }
    LeaveCriticalSection(&g_dispatch_lock);
  }
  if (callback && id == kCallbackWebApiTicket && g_config.online) {
    EnterCriticalSection(&g_patch_lock);
    void** original = *(void***)callback;
    CallbackVtable* table = 0;
    for (int i = 0; i < g_callback_vtable_count && !table; ++i)
      if (g_callback_vtables[i].original == original || g_callback_vtables[i].table == (void*)original)
        table = &g_callback_vtables[i];
    if (!table && g_callback_vtable_count < 16) {
      table = &g_callback_vtables[g_callback_vtable_count++];
      table->original = original;
      table->table[0] = (void*)CallbackRunResult;
      table->table[1] = (void*)CallbackRunOne;
      table->table[2] = original[2];
    }
    if (table) *(void***)callback = table->table;
    LeaveCriticalSection(&g_patch_lock);
  }
  ((RegisterCallbackFn)g_slots[SLOT_SteamAPI_RegisterCallback])(callback, id);
}
void SteamAPI_UnregisterCallback(void* callback) {
  EnterCriticalSection(&g_dispatch_lock);
  for (int i = 0; i < g_registered_count; ++i)
    if (g_registered[i].object == callback) g_registered[i--] = g_registered[--g_registered_count];
  LeaveCriticalSection(&g_dispatch_lock);
  ((UnregisterCallbackFn)g_slots[SLOT_SteamAPI_UnregisterCallback])(callback);
}
void SteamAPI_RegisterCallResult(void* callback, uint64_t call) {
  if (!IsOurCall(call)) {
    ((RegisterCallResultFn)g_slots[SLOT_SteamAPI_RegisterCallResult])(callback, call);
    return;
  }
  EnterCriticalSection(&g_dispatch_lock);
  if (g_registered_result_count < 128) {
    g_registered_results[g_registered_result_count].object = callback;
    g_registered_results[g_registered_result_count++].call = call;
  }
  /* Already finished and kept for polling: queue it again for this object. */
  Item* done = 0;
  for (int i = 0; i < g_done_count && !done; ++i)
    if (g_done[i]->call == call) {
      done = g_done[i];
      memmove(g_done + i, g_done + i + 1, sizeof(Item*) * (size_t)(--g_done_count - i));
    }
  if (done && g_queue_count < kMaxQueued) g_queue[g_queue_count++] = done;
  else if (done) Free(done);
  LeaveCriticalSection(&g_dispatch_lock);
}
void SteamAPI_UnregisterCallResult(void* callback, uint64_t call) {
  if (!IsOurCall(call)) {
    ((UnregisterCallResultFn)g_slots[SLOT_SteamAPI_UnregisterCallResult])(callback, call);
    return;
  }
  EnterCriticalSection(&g_dispatch_lock);
  for (int i = 0; i < g_registered_result_count; ++i)
    if (g_registered_results[i].object == callback && g_registered_results[i].call == call)
      g_registered_results[i--] = g_registered_results[--g_registered_result_count];
  LeaveCriticalSection(&g_dispatch_lock);
}
void SteamAPI_RunCallbacks(void) {
  ((RunCallbacksFn)g_slots[SLOT_SteamAPI_RunCallbacks])();
  if (g_config.online) DispatchOurs();
}

/* CallbackMsg_t: {HSteamUser, int iCallback, uint8* pubParam, int cubParam}. */
static void FillMessage(void* message, int id, void* param, uint32_t size) {
  uint8_t* m = message;
  const int32_t user = 1;
  memcpy(m, &user, 4);
  memcpy(m + 4, &id, 4);
  memcpy(m + 8, &param, sizeof param);
  memcpy(m + 8 + sizeof param, &size, 4);
}
int8_t SteamAPI_ManualDispatch_GetNextCallback(int32_t pipe, void* message) {
  const int8_t got = ((GetNextCallbackFn)g_slots[SLOT_SteamAPI_ManualDispatch_GetNextCallback])(pipe, message);
  if (!g_config.online || !message) return got;
  g_manual = 1;
  if (got) {
    const uint8_t* m = message;
    void* param = 0;
    memcpy(&param, m + 8, sizeof param);
    if ((int32_t)LE32(m + 4) == kCallbackWebApiTicket) PatchWebApiResponse(param);
    return got;
  }
  Item* item = PopQueued();
  if (!item) return 0;
  if (item->call) {
    /* A call result: kept for ManualDispatch_GetAPICallResult, announced by SteamAPICallCompleted_t. */
    Item* completed = Alloc(sizeof(Item));
    if (!completed) {
      Free(item);
      return 0;
    }
    completed->id = kCallbackSteamAPICallCompleted;
    completed->size = 16;
    memcpy(completed->data, &item->call, 8);
    memcpy(completed->data + 8, &item->id, 4);
    memcpy(completed->data + 12, &item->size, 4);
    EnterCriticalSection(&g_dispatch_lock);
    KeepDoneLocked(item);
    LeaveCriticalSection(&g_dispatch_lock);
    item = completed;
  }
  g_manual_last = item;
  FillMessage(message, item->id, item->data, item->size);
  return 1;
}
void SteamAPI_ManualDispatch_FreeLastCallback(int32_t pipe) {
  if (g_manual_last) {
    Free(g_manual_last);
    g_manual_last = 0;
    return;
  }
  ((FreeLastCallbackFn)g_slots[SLOT_SteamAPI_ManualDispatch_FreeLastCallback])(pipe);
}
int8_t SteamAPI_ManualDispatch_GetAPICallResult(int32_t pipe, uint64_t call, void* out, int size, int expected,
                                                int8_t* failed) {
  if (!IsOurCall(call))
    return ((ManualGetAPICallResultFn)g_slots[SLOT_SteamAPI_ManualDispatch_GetAPICallResult])(pipe, call, out, size,
                                                                                               expected, failed);
  return CopyResult(call, out, size, expected, failed);
}

/* ---- The flat C API ----------------------------------------------------------------------------- */

/* Games and libraries built on Steam's flat API (SteamAPI_ISteamUser_*) call these exports, and the emulator's do not
 * go through the interface table the bridge replaced, so the ticket calls come here too. An interface the bridge did
 * not wrap goes straight to the emulator. Each one logs its first call. */
static void LogFlat(const char* name, int* seen) {
  if (*seen) return;
  *seen = 1;
  Buf line = {0};
  LogText(&line, "game calls the flat API: ");
  LogText(&line, name);
  LogLine(&line);
}
static Vtable* FlatUser(void* self) { return g_config.online && self ? VtableOf(self) : 0; }
uint32_t SteamAPI_ISteamUser_GetAuthSessionTicket(void* self, void* buffer, int max, uint32_t* size, const void* identity) {
  static int seen;
  LogFlat("GetAuthSessionTicket", &seen);
  Vtable* v = FlatUser(self);
  if (!v)
    return ((uint32_t(*)(void*, void*, int, uint32_t*, const void*))g_slots[SLOT_SteamAPI_ISteamUser_GetAuthSessionTicket])(
        self, buffer, max, size, identity);
  return v->version >= 22 ? UserGetAuthSessionTicket(self, buffer, max, size, identity)
                          : UserGetAuthSessionTicketOld(self, buffer, max, size);
}
uint32_t SteamAPI_ISteamUser_GetAuthTicketForWebApi(void* self, const char* identity) {
  static int seen;
  LogFlat("GetAuthTicketForWebApi", &seen);
  Vtable* v = FlatUser(self);
  if (!v || SlotWebApi(v->version) < 0)
    return ((uint32_t(*)(void*, const char*))g_slots[SLOT_SteamAPI_ISteamUser_GetAuthTicketForWebApi])(self, identity);
  return UserGetAuthTicketForWebApi(self, identity);
}
int SteamAPI_ISteamUser_BeginAuthSession(void* self, const void* data, int size, uint64_t steam_id) {
  static int seen;
  LogFlat("BeginAuthSession", &seen);
  if (!FlatUser(self))
    return ((int (*)(void*, const void*, int, uint64_t))g_slots[SLOT_SteamAPI_ISteamUser_BeginAuthSession])(self, data, size,
                                                                                                         steam_id);
  return UserBeginAuthSession(self, data, size, steam_id);
}
void SteamAPI_ISteamUser_EndAuthSession(void* self, uint64_t steam_id) {
  if (!FlatUser(self)) {
    ((void (*)(void*, uint64_t))g_slots[SLOT_SteamAPI_ISteamUser_EndAuthSession])(self, steam_id);
    return;
  }
  UserEndAuthSession(self, steam_id);
}
void SteamAPI_ISteamUser_CancelAuthTicket(void* self, uint32_t handle) {
  if (!FlatUser(self)) {
    ((void (*)(void*, uint32_t))g_slots[SLOT_SteamAPI_ISteamUser_CancelAuthTicket])(self, handle);
    return;
  }
  UserCancelAuthTicket(self, handle);
}
uint64_t SteamAPI_ISteamUser_RequestEncryptedAppTicket(void* self, void* data, int size) {
  static int seen;
  LogFlat("RequestEncryptedAppTicket", &seen);
  if (!FlatUser(self))
    return ((uint64_t(*)(void*, void*, int))g_slots[SLOT_SteamAPI_ISteamUser_RequestEncryptedAppTicket])(self, data, size);
  return UserRequestEncryptedAppTicket(self, data, size);
}
int8_t SteamAPI_ISteamUser_GetEncryptedAppTicket(void* self, void* buffer, int max, uint32_t* size) {
  if (!FlatUser(self))
    return ((int8_t(*)(void*, void*, int, uint32_t*))g_slots[SLOT_SteamAPI_ISteamUser_GetEncryptedAppTicket])(self, buffer, max,
                                                                                                           size);
  return UserGetEncryptedAppTicket(self, buffer, max, size);
}

BOOL WINAPI DllMain(HANDLE instance, DWORD reason, void* reserved) {
  (void)reserved;
  if (reason != 1) return 1;
  InitializeCriticalSection(&g_lock);
  InitializeCriticalSection(&g_send_lock);
  InitializeCriticalSection(&g_log_lock);
  InitializeCriticalSection(&g_patch_lock);
  InitializeCriticalSection(&g_dispatch_lock);
  const DWORD n = GetModuleFileNameW(instance, g_dir, 512);
  DWORD cut = 0;
  for (DWORD i = 0; i < n; ++i)
    if (g_dir[i] == '\\' || g_dir[i] == '/') cut = i + 1;
  g_dir[cut] = 0;
  WCHAR path[600];
  PathIn(path, 600, L"steam_settings\\playden_online.log");
  g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, 0, 0);
  PathIn(path, 600, kEmulatorName);
  HANDLE emulator = LoadLibraryW(path);
  if (!emulator) Log("the emulator DLL beside the bridge is missing: every Steam call fails");
  int missing = 0;
  for (int i = 0; i < SLOT_COUNT; ++i) {
    g_slots[i] = emulator ? GetProcAddress(emulator, kSlotNames[i]) : 0;
    if (!g_slots[i]) {
      g_slots[i] = (void*)MissingExport;
      ++missing;
    }
  }
  if (missing) LogValue("emulator exports missing=", (uint64_t)missing);
  LoadConfig();
  if (!g_config.online) {
    Log("start: no online settings, emulator only");
    return 1;
  }
  LogValue("start: online app=", g_config.app_id);
  AddVectoredExceptionHandler(0, CrashTrail);
  CreateThread(0, 0, SessionThread, 0, 0, 0);
  return 1;
}
