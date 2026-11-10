// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only payload: dumps the code around the local-process calls, so their arguments can be read offline.
// It calls none of them. Output: /data/local-process-probe/dump-<symbol>.bin and dump.log.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/mdbg.h>

#define DIR "/data/local-process-probe"
enum { kWindow = 64 * 1024, kPage = 4096 };

int sceSystemServiceAddLocalProcess();
int sceSystemServiceAddLocalProcessForJvm();
int sceLncUtilAddLocalProcess();

static uint8_t page[kPage];

static void Dump(FILE* log, const char* name, intptr_t address) {
  char path[128];
  snprintf(path, sizeof path, DIR "/dump-%s.bin", name);
  const int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  const intptr_t start = address & ~(intptr_t)(kPage - 1);
  int pages = 0, failed = 0;
  for (intptr_t at = start; out >= 0 && at < start + kWindow; at += kPage) {
    const int rc = mdbg_copyout(getpid(), at, page, kPage);
    if (rc != 0) {
      failed++;
      break;
    }
    if (write(out, page, kPage) != kPage) break;
    pages++;
  }
  if (out >= 0) close(out);
  fprintf(log, "%s address=0x%lx start=0x%lx pages=%d failed=%d\n", name, (long)address, (long)start, pages, failed);
  fflush(log);
}

int main(void) {
  mkdir(DIR, 0777);
  chmod(DIR, 0777);
  FILE* log = fopen(DIR "/dump.log", "w");
  if (log == NULL) return 1;
  Dump(log, "sceSystemServiceAddLocalProcess", (intptr_t)&sceSystemServiceAddLocalProcess);
  Dump(log, "sceSystemServiceAddLocalProcessForJvm", (intptr_t)&sceSystemServiceAddLocalProcessForJvm);
  Dump(log, "sceLncUtilAddLocalProcess", (intptr_t)&sceLncUtilAddLocalProcess);
  // The strings and defaults those functions read, about 0x33000 after them in this firmware's library.
  Dump(log, "rodata", (intptr_t)&sceSystemServiceAddLocalProcess + 0x32cb0);
  Dump(log, "rodata2", (intptr_t)&sceSystemServiceAddLocalProcess + 0x32cb0 + kWindow);
  fprintf(log, "done\n");
  fclose(log);
  return 0;
}
