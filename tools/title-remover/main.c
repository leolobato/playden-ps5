// main.c - Playden's one-shot title remover payload.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playden (FAKE77031) cannot load libSceAppInstUtil itself, and ShadowMountPlus
// refuses to uninstall while any title runs. This payload, which Playden sends to
// the payload loader on 127.0.0.1:9021, unregisters one of Playden's game titles
// (FAKE78xxx) and exits. It reads one request from REQUEST_DIR:
//
//   request   "title=FAKE78002\nnonce=<16..64 of [0-9a-f-]>\n"
//
// The directory and the request must be owned by Playden's user (the owner of
// its private 0700 state folder /data/playden/runtime-state; the title folder
// itself belongs to whoever deployed it), must not be links, and must not be
// writable by others. The result goes next to the request, written as that user:
//
//   result    "nonce=<nonce>\ncode=<8 hex digits>\n"   (code 0 = accepted)
//
// The native uninstall completes asynchronously; Playden confirms it in app.db.
// Order matters (results/20261007-native-uninstall): initialize AppInstUtil, then
// take ShadowMountPlus's authid, then uninstall.
#include <ps5/kernel.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUEST_DIR "/data/playden/title-remover"
#define PLAYDEN_STATE "/data/playden/runtime-state"
#define AUTHID_INSTALLER 0x4800000000000006ull  // ShadowMountPlus main.c:54

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppUnInstall(const char *title_id);

static int private_entry(const char *path, uid_t owner, int directory) {
  struct stat info;
  if (lstat(path, &info) != 0 || info.st_uid != owner || (info.st_mode & 0022) != 0) return 0;
  return directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size < 256;
}

static int valid_title(const char *title) {
  if (strlen(title) != 9 || strncmp(title, "FAKE78", 6) != 0) return 0;
  for (int i = 6; i < 9; ++i)
    if (title[i] < '0' || title[i] > '9') return 0;
  return 1;
}

static int valid_nonce(const char *nonce) {
  const size_t length = strlen(nonce);
  if (length < 16 || length > 64) return 0;
  for (size_t i = 0; i < length; ++i)
    if (!((nonce[i] >= '0' && nonce[i] <= '9') || (nonce[i] >= 'a' && nonce[i] <= 'f') || nonce[i] == '-')) return 0;
  return 1;
}

// Reads "key=value" from the request text into value (bounded).
static int field(const char *text, const char *key, char *value, size_t size) {
  const size_t key_length = strlen(key);
  for (const char *line = text; *line;) {
    const char *end = strchr(line, '\n');
    const size_t length = end ? (size_t)(end - line) : strlen(line);
    if (length > key_length && strncmp(line, key, key_length) == 0 && line[key_length] == '=') {
      const size_t value_length = length - key_length - 1;
      if (value_length >= size) return 0;
      memcpy(value, line + key_length + 1, value_length);
      value[value_length] = 0;
      return 1;
    }
    if (!end) break;
    line = end + 1;
  }
  return 0;
}

static void write_result(uid_t owner, const char *nonce, unsigned code) {
  // Write as Playden's user so the result sits in its directory like the request.
  kernel_set_ucred_uid(-1, owner);
  char text[128];
  const int length = snprintf(text, sizeof text, "nonce=%s\ncode=%08x\n", nonce, code);
  const int fd = open(REQUEST_DIR "/result.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if (fd < 0) return;
  const int written = (int)write(fd, text, (size_t)length) == length;
  fsync(fd);
  close(fd);
  if (written) rename(REQUEST_DIR "/result.tmp", REQUEST_DIR "/result");
  else unlink(REQUEST_DIR "/result.tmp");
}

int main(void) {
  struct stat playden;
  if (lstat(PLAYDEN_STATE, &playden) != 0 || !S_ISDIR(playden.st_mode) || (playden.st_mode & 0777) != 0700) return 1;
  const uid_t owner = playden.st_uid;
  if (!private_entry(REQUEST_DIR, owner, 1) || !private_entry(REQUEST_DIR "/request", owner, 0)) return 2;
  char text[256] = {0};
  const int fd = open(REQUEST_DIR "/request", O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return 3;
  const ssize_t count = read(fd, text, sizeof text - 1);
  close(fd);
  if (count <= 0) return 3;
  char title[16], nonce[72];
  if (!field(text, "title", title, sizeof title) || !field(text, "nonce", nonce, sizeof nonce) ||
      !valid_title(title) || !valid_nonce(nonce))
    return 4;
  unsigned code = (unsigned)sceAppInstUtilInitialize();
  if (code == 0) {
    kernel_set_ucred_authid(-1, AUTHID_INSTALLER);
    code = (unsigned)sceAppInstUtilAppUnInstall(title);
  }
  write_result(owner, nonce, code);
  return code == 0 ? 0 : 5;
}
