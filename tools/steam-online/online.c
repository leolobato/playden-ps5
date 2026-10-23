/* Playden's online bridge: a steam_api64.dll that hands every call to gbe_fork (steam_api64_gbe.dll
 * beside it) and adds what the emulator cannot do, a logged-on Steam session for the game:
 *
 *   ISteamUser::GetAuthSessionTicket / GetAuthTicketForWebApi: real session tickets (game connect
 *     token, SteamID, app ownership ticket), activated with Steam through the auth list, so a game's
 *     servers and backends (dedicated servers, Jagex accounts) accept them.
 *   ISteamUser::CancelAuthTicket: takes the ticket off the auth list.
 *   ISteamUser::RequestEncryptedAppTicket / GetEncryptedAppTicket: Steam's encrypted app ticket.
 *
 * Everything else (lobbies, friends, P2P networking) is still the emulator's. The session logs on to a
 * CM over WebSocket (WinHTTP) with the refresh token Playden writes to steam_settings/playden_online.txt,
 * on a thread of its own. Built freestanding (no C runtime) by build.sh; online.c and the generated stubs
 * are the whole DLL. The log (steam_settings/playden_online.log) never holds a token or ticket bytes. */

#include <stddef.h>
#include <stdint.h>

#include "slots.h"

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

DECLSPEC_IMPORT BOOL WINAPI CloseHandle(HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI CreateFileW(const WCHAR*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
DECLSPEC_IMPORT HANDLE WINAPI CreateThread(void*, size_t, DWORD(WINAPI*)(void*), void*, DWORD, DWORD*);
DECLSPEC_IMPORT void WINAPI EnterCriticalSection(CRITICAL_SECTION*);
DECLSPEC_IMPORT DWORD WINAPI GetModuleFileNameW(HANDLE, WCHAR*, DWORD);
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
static void LogNumber(Buf* line, uint64_t value) {
  char digits[24];
  int n = 0;
  do digits[n++] = (char)('0' + value % 10); while ((value /= 10) && n < 24);
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
  kEMsgClientLogOnResponse = 751,
  kEMsgClientLoggedOff = 757,
  kEMsgClientGameConnectTokens = 779,
  kEMsgClientGetAppOwnershipTicket = 857,
  kEMsgClientGetAppOwnershipTicketResponse = 858,
  kEMsgClientTicketAuthComplete = 5429,
  kEMsgClientAuthList = 5432,
  kEMsgClientLogon = 5514,
  kEMsgClientRequestEncryptedAppTicket = 5526,
  kEMsgClientRequestEncryptedAppTicketResponse = 5527,
  kEMsgClientAuthListAck = 5575,
  kEMsgClientHello = 9805,
};
#define kProtoMask 0x80000000u
#define kProtocolVersion 65580u
#define kClientPackageVersion 1771u
#define kAnonymousSteamId 0x0110000100000000ull
#define kMaxTokens 32
#define kMaxTickets 64
#define kMaxTicketBytes 4096

typedef struct {
  uint32_t handle;
  uint32_t size;
  uint8_t bytes[kMaxTicketBytes];
} Ticket;

static CRITICAL_SECTION g_lock;  /* everything below */
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

static void HandleFrame(const uint8_t* data, size_t size, int depth);

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
  int32_t header_eresult = 1;
  PbReader hr = {data + 8, header_size, 0, 0};
  PbField f;
  while (PbNext(&hr, &f)) {
    if (f.number == 1 && f.wire == 1) steam_id = f.value;
    if (f.number == 2 && f.wire == 0) session_id = (int32_t)f.value;
    if (f.number == 13 && f.wire == 0) header_eresult = (int32_t)f.value;
  }
  const uint8_t* body = data + 8 + header_size;
  const size_t n = size - 8 - header_size;
  PbReader r = {body, n, 0, 0};

  switch (emsg) {
    case kEMsgMulti:
      HandleMulti(body, n, depth);
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
      SendMessage(kEMsgClientGetAppOwnershipTicket, &request);
      BufFree(&request);
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
      uint32_t estate = 0, eresult = 0;
      while (PbNext(&r, &f)) {
        if (f.number == 4 && f.wire == 0) estate = (uint32_t)f.value;
        if (f.number == 5 && f.wire == 0) eresult = (uint32_t)f.value;
      }
      Buf line = {0};
      LogText(&line, "ticket auth complete estate=");
      LogNumber(&line, estate);
      LogText(&line, " eresult=");
      LogNumber(&line, eresult);
      LogLine(&line);
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
      (void)header_eresult;
      break;
  }
}

static void WideFrom(WCHAR* out, size_t cap, const char* s, size_t n) {
  size_t i = 0;
  for (; i < n && i + 1 < cap; ++i) out[i] = (WCHAR)(uint8_t)s[i];
  out[i] = 0;
}

/* The CM list (host:port strings) from Steam's directory. */
static int FetchEndpoints(HINTERNET http, char endpoints[][128], int max) {
  int count = 0;
  HINTERNET connection = WinHttpConnect(http, L"api.steampowered.com", 443, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET",
                                                      L"/ISteamDirectory/GetCMListForConnect/v1/"
                                                      L"?cmtype=websockets&maxcount=16&format=json",
                                                      0, 0, 0, WINHTTP_FLAG_SECURE)
                                 : 0;
  Buf text = {0};
  DWORD status = 0, status_size = sizeof status;
  if (request && WinHttpSendRequest(request, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(request, 0) &&
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &status_size, 0) &&
      status == 200) {
    uint8_t chunk[4096];
    DWORD read = 0;
    while (text.n < (1u << 20) && WinHttpReadData(request, chunk, sizeof chunk, &read) && read) BufPut(&text, chunk, read);
  } else {
    LogValue("directory: http status=", status);
  }
  static const char kKey[] = "\"endpoint\":\"";
  for (size_t at = 0; !text.failed && at + sizeof kKey < text.n && count < max; ++at) {
    if (memcmp(text.p + at, kKey, sizeof kKey - 1)) continue;
    const size_t start = at + sizeof kKey - 1;
    size_t end = start;
    while (end < text.n && text.p[end] != '"') ++end;
    if (end < text.n && end - start < 128) {
      CopyValue(endpoints[count++], 128, (const char*)text.p + start, end - start);
    }
    at = end;
  }
  BufFree(&text);
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  return count;
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
  SendMessage(kEMsgClientHello, &hello);
  BufFree(&hello);
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

static DWORD WINAPI HeartbeatThread(void* unused) {
  (void)unused;
  for (;;) {
    EnterCriticalSection(&g_lock);
    const uint32_t seconds = g_heartbeat_seconds;
    const int logged_on = g_logged_on;
    LeaveCriticalSection(&g_lock);
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
  CreateThread(0, 0, HeartbeatThread, 0, 0, 0);
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
  BufLE32(&b, (uint32_t)(now / 10000000ull - 11644473600ull));
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

/* Sends the auth list (every live ticket) and waits for Steam's ack. Call without g_lock. */
static void PublishAuthList(void) {
  Buf body = {0};
  EnterCriticalSection(&g_lock);
  const uint32_t sequence = ++g_auth_sequence;
  PbUint(&body, 1, (uint64_t)g_token_count);
  PbUint(&body, 2, sequence);
  PbUint(&body, 3, 0);
  for (int i = 0; i < g_ticket_count; ++i) {
    Buf entry = {0};
    PbUint(&entry, 1, 0);
    PbUint(&entry, 2, 1);
    PbFixed64(&entry, 4, g_config.app_id);
    PbUint(&entry, 5, 1);
    PbUint(&entry, 6, TicketCrc(g_tickets[i]->bytes, g_tickets[i]->size));
    PbBytes(&entry, 7, g_tickets[i]->bytes, g_tickets[i]->size);
    PbBytes(&body, 4, entry.p, entry.n);
    BufFree(&entry);
  }
  PbUint(&body, 5, g_config.app_id);
  PbUint(&body, 6, sequence);
  const int count = g_ticket_count;
  LeaveCriticalSection(&g_lock);
  if (SendMessage(kEMsgClientAuthList, &body)) {
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
  PublishAuthList();
}

static Ticket* FindTicket(uint32_t handle) {
  for (int i = 0; i < g_ticket_count; ++i)
    if (g_tickets[i]->handle == handle) return g_tickets[i];
  return 0;
}

/* ---- ISteamUser ------------------------------------------------------------------------------- */

typedef uint32_t (*GetAuthSessionTicketFn)(void*, void*, int, uint32_t*, const void*);
typedef uint32_t (*GetAuthTicketForWebApiFn)(void*, const char*);
typedef void (*CancelAuthTicketFn)(void*, uint32_t);
typedef uint64_t (*RequestEncryptedAppTicketFn)(void*, void*, int);
typedef int8_t (*GetEncryptedAppTicketFn)(void*, void*, int, uint32_t*);

#define kVtableEntries 40
typedef struct {
  void** original;
  void* table[kVtableEntries];
  int version;
} UserVtable;
static UserVtable g_user_vtables[8];
static int g_user_vtable_count;

/* Vtable slots by interface version: SteamUser023 adds GetAuthTicketForWebApi at 14. */
static int SlotSessionTicket(int v) { (void)v; return 13; }
static int SlotWebApi(int v) { return v >= 23 ? 14 : -1; }
static int SlotCancel(int v) { return v >= 23 ? 17 : 16; }
static int SlotRequestEncrypted(int v) { return v >= 23 ? 21 : 20; }
static int SlotGetEncrypted(int v) { return v >= 23 ? 22 : 21; }

static UserVtable* VtableOf(void* self) {
  void** current = *(void***)self;
  for (int i = 0; i < g_user_vtable_count; ++i)
    if (g_user_vtables[i].table == (void*)current) return &g_user_vtables[i];
  return 0;
}

static uint32_t UserGetAuthSessionTicket(void* self, void* buffer, int max, uint32_t* size, const void* identity) {
  UserVtable* v = VtableOf(self);
  const uint32_t handle = ((GetAuthSessionTicketFn)v->original[SlotSessionTicket(v->version)])(self, buffer, max, size, identity);
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

static uint32_t UserGetAuthTicketForWebApi(void* self, const char* identity) {
  UserVtable* v = VtableOf(self);
  const uint32_t handle = ((GetAuthTicketForWebApiFn)v->original[SlotWebApi(v->version)])(self, identity);
  if (!handle) return handle;
  Ticket* ticket = MakeTicket(handle);
  if (ticket) {
    AddTicket(ticket);
    LogValue("web api ticket handle=", handle);
  }
  return handle;
}

static void UserCancelAuthTicket(void* self, uint32_t handle) {
  UserVtable* v = VtableOf(self);
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
  if (removed) PublishAuthList();
  ((CancelAuthTicketFn)v->original[SlotCancel(v->version)])(self, handle);
}

static uint64_t UserRequestEncryptedAppTicket(void* self, void* data, int size) {
  UserVtable* v = VtableOf(self);
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

static int8_t UserGetEncryptedAppTicket(void* self, void* buffer, int max, uint32_t* size) {
  UserVtable* v = VtableOf(self);
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

static CRITICAL_SECTION g_patch_lock;

/* Points an ISteamUser020-023 object at a copy of its vtable with the ticket methods replaced. */
static void* WrapUser(void* user, const char* version) {
  if (!user || !version || !g_config.online || !StartsWith(version, "SteamUser0")) return user;
  const int v = (int)ParseNumber(version + 9, 3);
  if (v < 20 || v > 23) return user;
  EnterCriticalSection(&g_patch_lock);
  if (!VtableOf(user) && g_user_vtable_count < 8) {
    UserVtable* table = &g_user_vtables[g_user_vtable_count];
    void** original = *(void***)user;
    table->original = original;
    table->version = v;
    for (int i = 0; i < (v >= 23 ? 33 : 32); ++i) table->table[i] = original[i];
    table->table[SlotSessionTicket(v)] = (void*)UserGetAuthSessionTicket;
    if (SlotWebApi(v) >= 0) table->table[SlotWebApi(v)] = (void*)UserGetAuthTicketForWebApi;
    table->table[SlotCancel(v)] = (void*)UserCancelAuthTicket;
    table->table[SlotRequestEncrypted(v)] = (void*)UserRequestEncryptedAppTicket;
    table->table[SlotGetEncrypted(v)] = (void*)UserGetEncryptedAppTicket;
    ++g_user_vtable_count;
    *(void***)user = table->table;
    LogValue("wrapped SteamUser0", (uint64_t)v);
  }
  LeaveCriticalSection(&g_patch_lock);
  return user;
}

/* ---- Callbacks: GetTicketForWebApiResponse_t carries the ticket bytes ------------------------- */

#define kCallbackWebApiTicket 168
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

typedef void (*CallbackRunFn)(void*, void*, uint64_t, uint64_t);
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
/* CCallbackBase's two Run overloads (their vtable order differs between compilers): both get the
 * callback's data second. */
static void CallbackRun0(void* self, void* param, uint64_t a, uint64_t b) {
  PatchWebApiResponse(param);
  ((CallbackRunFn)CallbackVtableOf(self)->original[0])(self, param, a, b);
}
static void CallbackRun1(void* self, void* param, uint64_t a, uint64_t b) {
  PatchWebApiResponse(param);
  ((CallbackRunFn)CallbackVtableOf(self)->original[1])(self, param, a, b);
}

/* ---- Exports ---------------------------------------------------------------------------------- */

void* g_slots[SLOT_COUNT];
__declspec(dllexport) void* g_pSteamClientGameServer;

static uint64_t MissingExport(void) { return 0; }

typedef void* (*FindOrCreateUserInterfaceFn)(int32_t, const char*);
typedef void* (*CreateInterfaceFn)(const char*);
typedef void* (*SteamUserFn)(void);
typedef void (*RegisterCallbackFn)(void*, int);
typedef int8_t (*GetNextCallbackFn)(int32_t, void*);

void* SteamInternal_FindOrCreateUserInterface(int32_t user, const char* version) {
  return WrapUser(((FindOrCreateUserInterfaceFn)g_slots[SLOT_SteamInternal_FindOrCreateUserInterface])(user, version), version);
}
void* SteamInternal_CreateInterface(const char* version) {
  return WrapUser(((CreateInterfaceFn)g_slots[SLOT_SteamInternal_CreateInterface])(version), version);
}
void* SteamAPI_SteamUser_v020(void) { return WrapUser(((SteamUserFn)g_slots[SLOT_SteamAPI_SteamUser_v020])(), "SteamUser020"); }
void* SteamAPI_SteamUser_v021(void) { return WrapUser(((SteamUserFn)g_slots[SLOT_SteamAPI_SteamUser_v021])(), "SteamUser021"); }
void* SteamAPI_SteamUser_v022(void) { return WrapUser(((SteamUserFn)g_slots[SLOT_SteamAPI_SteamUser_v022])(), "SteamUser022"); }
void* SteamAPI_SteamUser_v023(void) { return WrapUser(((SteamUserFn)g_slots[SLOT_SteamAPI_SteamUser_v023])(), "SteamUser023"); }

void SteamAPI_RegisterCallback(void* callback, int id) {
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
      table->table[0] = (void*)CallbackRun0;
      table->table[1] = (void*)CallbackRun1;
      table->table[2] = original[2];
    }
    if (table) *(void***)callback = table->table;
    LeaveCriticalSection(&g_patch_lock);
  }
  ((RegisterCallbackFn)g_slots[SLOT_SteamAPI_RegisterCallback])(callback, id);
}

/* CallbackMsg_t: {HSteamUser, int iCallback, uint8* pubParam, int cubParam}. */
int8_t SteamAPI_ManualDispatch_GetNextCallback(int32_t pipe, void* message) {
  const int8_t got = ((GetNextCallbackFn)g_slots[SLOT_SteamAPI_ManualDispatch_GetNextCallback])(pipe, message);
  if (got && message && g_config.online) {
    const uint8_t* m = message;
    void* param = 0;
    memcpy(&param, m + 8, sizeof param);
    if ((int32_t)LE32(m + 4) == kCallbackWebApiTicket) PatchWebApiResponse(param);
  }
  return got;
}

BOOL WINAPI DllMain(HANDLE instance, DWORD reason, void* reserved) {
  (void)reserved;
  if (reason != 1) return 1;
  InitializeCriticalSection(&g_lock);
  InitializeCriticalSection(&g_send_lock);
  InitializeCriticalSection(&g_log_lock);
  InitializeCriticalSection(&g_patch_lock);
  const DWORD n = GetModuleFileNameW(instance, g_dir, 512);
  DWORD cut = 0;
  for (DWORD i = 0; i < n; ++i)
    if (g_dir[i] == '\\' || g_dir[i] == '/') cut = i + 1;
  g_dir[cut] = 0;
  WCHAR path[600];
  PathIn(path, 600, L"steam_settings\\playden_online.log");
  g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, 0, 0);
  PathIn(path, 600, L"steam_api64_gbe.dll");
  HANDLE emulator = LoadLibraryW(path);
  if (!emulator) Log("steam_api64_gbe.dll is missing: every Steam call fails");
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
  CreateThread(0, 0, SessionThread, 0, 0, 0);
  return 1;
}
