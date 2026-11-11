/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shc.h"

static size_t Length(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}

int ShcTextEqual(const char* a, const char* b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}

int ShcTextEqualPrefix(const char* text, const char* prefix) {
  while (*prefix)
    if (*text++ != *prefix++) return 1;
  return 0;
}

static int Key(const char* line, size_t eq, const char* key) {
  if (Length(key) != eq) return 0;
  for (size_t i = 0; i < eq; ++i)
    if (line[i] != key[i]) return 0;
  return 1;
}

/* Decimal digits only; 0 for anything else, which no field accepts. */
static uint64_t Number(const char* s, size_t n) {
  uint64_t value = 0;
  if (!n || n > 20) return 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return 0;
    value = value * 10 + (uint64_t)(s[i] - '0');
  }
  return value;
}

static void Copy(char* out, size_t cap, const char* s, size_t n) {
  if (n >= cap) {
    out[0] = 0;  /* too long: the field stays empty and the session is refused */
    return;
  }
  for (size_t i = 0; i < n; ++i) out[i] = s[i];
  out[n] = 0;
}

static int LowerHex(const char* text, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return 0;
  return 1;
}

static int Is(const char* value, size_t n, const char* word) { return Key(value, n, word); }

static int Invites(const char* value, size_t n) {
  if (Is(value, n, "join")) return SHC_INVITES_JOIN;
  if (Is(value, n, "ask")) return SHC_INVITES_ASK;
  if (Is(value, n, "show")) return SHC_INVITES_SHOW;
  return SHC_INVITES_OFF;
}

int ShcParseSession(const char* text, size_t n, ShcSession* session) {
  memset(session, 0, sizeof *session);
  session->persona = SHC_PERSONA_ONLINE;
  size_t at = 0;
  while (at < n) {
    size_t end = at;
    while (end < n && text[end] != '\n' && text[end] != '\r') ++end;
    const char* line = text + at;
    const size_t length = end - at;
    size_t eq = 0;
    while (eq < length && line[eq] != '=') ++eq;
    if (eq < length) {
      const char* value = line + eq + 1;
      const size_t vn = length - eq - 1;
      if (Key(line, eq, "app_id")) session->app_id = (uint32_t)Number(value, vn);
      else if (Key(line, eq, "steam_id")) session->steam_id = Number(value, vn);
      else if (Key(line, eq, "account_name")) Copy(session->account, sizeof session->account, value, vn);
      else if (Key(line, eq, "refresh_token")) Copy(session->token, sizeof session->token, value, vn);
      else if (Key(line, eq, "keep")) session->keep = Number(value, vn) != 0;
      else if (Key(line, eq, "invites")) session->invites = Invites(value, vn);
      else if (Key(line, eq, "persona")) session->persona = Is(value, vn, "invisible") ? SHC_PERSONA_INVISIBLE : SHC_PERSONA_ONLINE;
      else if (Key(line, eq, "adapters")) session->adapters_off = Is(value, vn, "off");
      else if (Key(line, eq, "notice")) session->notice = Is(value, vn, "1");
      else if (Key(line, eq, "client_sha256") && vn == 64 && LowerHex(value, vn)) Copy(session->client_sha256, sizeof session->client_sha256, value, vn);
    }
    at = end + 1;
  }
  return session->app_id && session->steam_id && session->account[0] && session->token[0];
}

void ShcWipeSession(ShcSession* session) {
  volatile uint8_t* p = (volatile uint8_t*)session;
  for (size_t i = 0; i < sizeof *session; ++i) p[i] = 0;
}

int ShcParseLaunch(const char* text, size_t n, int64_t now, ShcLaunch* launch) {
  int written = 0;
  int64_t at_time = 0;
  memset(launch, 0, sizeof *launch);
  size_t at = 0;
  while (at < n) {
    size_t end = at;
    while (end < n && text[end] != '\n' && text[end] != '\r') ++end;
    const char* line = text + at;
    const size_t length = end - at;
    size_t eq = 0;
    while (eq < length && line[eq] != '=') ++eq;
    if (eq < length) {
      const char* value = line + eq + 1;
      const size_t vn = length - eq - 1;
      const uint64_t number = Number(value, vn);
      if (Key(line, eq, "written") && number && number < ((uint64_t)1 << 62)) {
        written = 1;
        at_time = (int64_t)number;
      } else if (Key(line, eq, "connect_lobby") && number) {
        launch->connect_lobby = number;
      } else if (Key(line, eq, "invite") && number && launch->invite_count < SHC_LAUNCH_MAX_INVITES) {
        int seen = 0;
        for (int i = 0; i < launch->invite_count; ++i) seen |= launch->invites[i] == number;
        if (!seen) launch->invites[launch->invite_count++] = number;
      }
    }
    at = end + 1;
  }
  if (!written || at_time - now > SHC_LAUNCH_MAX_AGE_SECONDS || now - at_time > SHC_LAUNCH_MAX_AGE_SECONDS ||
      (!launch->connect_lobby && !launch->invite_count)) {
    memset(launch, 0, sizeof *launch);
    return 0;
  }
  return 1;
}

int ShcInviteAction(int invites, int title_can_ask, int title_can_show) {
  switch (invites) {
    case SHC_INVITES_JOIN: return SHC_INVITE_JOIN;
    case SHC_INVITES_ASK: return title_can_ask ? SHC_INVITE_ASK : SHC_INVITE_IGNORE;
    case SHC_INVITES_SHOW: return title_can_show ? SHC_INVITE_SHOW : SHC_INVITE_IGNORE;
    default: return SHC_INVITE_IGNORE;
  }
}

typedef struct {
  char* out;
  size_t cap, n;
} Text;
static void Add(Text* text, const char* s) {
  for (; *s && text->n + 1 < text->cap; ++s) text->out[text->n++] = (uint8_t)*s < 0x20 || *s == 0x7f ? ' ' : *s;
  if (text->cap) text->out[text->n] = 0;
}
static void AddNumber(Text* text, uint64_t value) {
  char digits[24];
  int n = 0;
  do digits[n++] = (char)('0' + value % 10), value /= 10;
  while (value);
  while (n && text->n + 1 < text->cap) text->out[text->n++] = digits[--n];
  if (text->cap) text->out[text->n] = 0;
}

void ShcInviteText(int kind, const char* name, char* out, size_t cap) {
  Text text = {out, cap, 0};
  int visible = 0;
  if (!cap) return;
  out[0] = 0;
  for (const char* s = name ? name : ""; *s; ++s) visible |= (uint8_t)*s > ' ' && *s != 0x7f;
  if (kind == SHC_TEXT_JOINING) {
    Add(&text, "Joining ");
    Add(&text, visible ? name : "a friend");
    Add(&text, "'s game.");
    return;
  }
  Add(&text, visible ? name : "A friend");
  Add(&text, " invited you to their game.");
  if (kind == SHC_TEXT_ASK) {
    Add(&text, " Press L3 + R3 within ");
    AddNumber(&text, SHC_QUESTION_SECONDS);
    Add(&text, " seconds to join.");
  }
}

void ShcInvitedText(int count, char* out, size_t cap) {
  Text text = {out, cap, 0};
  if (!cap) return;
  out[0] = 0;
  Add(&text, "Invited ");
  AddNumber(&text, (uint64_t)(count > 0 ? count : 0));
  Add(&text, count == 1 ? " friend to your game." : " friends to your game.");
}

void ShcOnlineText(uint32_t persona, const char* name, char* out, size_t cap) {
  Text text = {out, cap, 0};
  int visible = 0;
  if (!cap) return;
  out[0] = 0;
  for (const char* s = name ? name : ""; *s; ++s) visible |= (uint8_t)*s > ' ' && *s != 0x7f;
  const int invisible = persona == SHC_PERSONA_INVISIBLE;
  Add(&text, invisible ? "Signed in to Steam" : "Online on Steam");
  if (visible) {
    Add(&text, " as ");
    Add(&text, name);
  }
  Add(&text, invisible ? ", invisible to friends." : ".");
}

void ShcQuestionLine(uint64_t id, const char* question, char* out, size_t cap) {
  Text text = {out, cap, 0};
  if (!cap) return;
  out[0] = 0;
  AddNumber(&text, id);
  Add(&text, " ");
  AddNumber(&text, SHC_QUESTION_SECONDS);
  Add(&text, " ");
  if (cap < 2) return;
  text.cap = cap - 1; /* room for the line end */
  Add(&text, question);
  out[text.n++] = '\n';
  out[text.n] = 0;
}

int ShcParseAnswer(const char* text, size_t n, uint64_t* id, int* accepted) {
  size_t end = 0, digits = 0;
  while (end < n && text[end] != '\n' && text[end] != '\r') ++end;
  while (digits < end && text[digits] >= '0' && text[digits] <= '9') ++digits;
  if (!digits || digits + 1 >= end || text[digits] != ' ') return 0;
  const uint64_t value = Number(text, digits);
  if (!value) return 0;
  if (Key(text + digits + 1, end - digits - 1, "accepted")) *accepted = 1;
  else if (Key(text + digits + 1, end - digits - 1, "expired")) *accepted = 0;
  else return 0;
  *id = value;
  return 1;
}

size_t ShcChangeStatusMessage(uint32_t state, uint8_t* out, size_t cap) {
  const uint32_t emsg = 716u | 0x80000000u; /* ClientChangeStatus, protobuf */
  size_t n = 0;
  uint8_t body[8];
  size_t body_n = 0;
  body[body_n++] = 0x08; /* field 1, varint: persona_state */
  do {
    uint8_t byte = (uint8_t)(state & 0x7f);
    state >>= 7;
    body[body_n++] = (uint8_t)(byte | (state ? 0x80 : 0));
  } while (state);
  if (cap < 8 + body_n) return 0;
  for (int i = 0; i < 4; ++i) out[n++] = (uint8_t)(emsg >> (8 * i));
  for (int i = 0; i < 4; ++i) out[n++] = 0; /* header length */
  for (size_t i = 0; i < body_n; ++i) out[n++] = body[i];
  return n;
}

int ShcLobbySeen(ShcLobbyWatch* watch, uint64_t lobby) {
  if (!lobby) return 0;
  watch->last = lobby;
  if (watch->first) return 0;
  watch->first = lobby;
  return 1;
}

/* ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------- */

static const uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t Rotr(uint32_t v, int n) { return v >> n | v << (32 - n); }

static void Block(ShcSha256* hash, const uint8_t* p) {
  uint32_t w[64], s[8];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t a = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t b = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + a + w[i - 7] + b;
  }
  for (int i = 0; i < 8; ++i) s[i] = hash->state[i];
  for (int i = 0; i < 64; ++i) {
    const uint32_t e1 = Rotr(s[4], 6) ^ Rotr(s[4], 11) ^ Rotr(s[4], 25);
    const uint32_t choose = (s[4] & s[5]) ^ (~s[4] & s[6]);
    const uint32_t t1 = s[7] + e1 + choose + kRound[i] + w[i];
    const uint32_t e0 = Rotr(s[0], 2) ^ Rotr(s[0], 13) ^ Rotr(s[0], 22);
    const uint32_t majority = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
    const uint32_t t2 = e0 + majority;
    s[7] = s[6], s[6] = s[5], s[5] = s[4], s[4] = s[3] + t1;
    s[3] = s[2], s[2] = s[1], s[1] = s[0], s[0] = t1 + t2;
  }
  for (int i = 0; i < 8; ++i) hash->state[i] += s[i];
}

void ShcSha256Init(ShcSha256* hash) {
  static const uint32_t kInitial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (int i = 0; i < 8; ++i) hash->state[i] = kInitial[i];
  hash->bytes = 0;
  hash->used = 0;
}

void ShcSha256Update(ShcSha256* hash, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  hash->bytes += n;
  while (n) {
    if (!hash->used && n >= 64) {
      Block(hash, p);
      p += 64, n -= 64;
      continue;
    }
    hash->block[hash->used++] = *p++;
    --n;
    if (hash->used == 64) {
      Block(hash, hash->block);
      hash->used = 0;
    }
  }
}

void ShcSha256Final(ShcSha256* hash, uint8_t digest[32]) {
  const uint64_t bits = hash->bytes * 8;
  uint8_t pad = 0x80;
  ShcSha256Update(hash, &pad, 1);
  pad = 0;
  while (hash->used != 56) ShcSha256Update(hash, &pad, 1);
  for (int i = 7; i >= 0; --i) {
    const uint8_t byte = (uint8_t)(bits >> (8 * i));
    ShcSha256Update(hash, &byte, 1);
  }
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) digest[4 * i + j] = (uint8_t)(hash->state[i] >> (24 - 8 * j));
}

void ShcHex(const uint8_t digest[32], char out[65]) {
  static const char kDigits[] = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[2 * i] = kDigits[digest[i] >> 4];
    out[2 * i + 1] = kDigits[digest[i] & 15];
  }
  out[64] = 0;
}
