// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only payload: copies address ranges of a running title. /data/local-process-probe/target.txt holds the PID, then
// one "name start_hex size_hex" per line. Output: dump-<name>.bin and dump-target.log.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <ps5/mdbg.h>

#define DIR "/data/local-process-probe"
static uint8_t page[4096];

int main(void) {
  FILE* in = fopen(DIR "/target.txt", "r");
  FILE* log = fopen(DIR "/dump-target.log", "w");
  if (in == NULL || log == NULL) return 1;
  int pid = 0;
  if (fscanf(in, "%d", &pid) != 1) return 2;
  char name[64];
  unsigned long start, size;
  while (fscanf(in, "%63s %lx %lx", name, &start, &size) == 3) {
    char path[128];
    snprintf(path, sizeof path, DIR "/dump-%s.bin", name);
    const int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    unsigned long done = 0;
    int rc = 0;
    for (; out >= 0 && done < size; done += sizeof page) {
      rc = mdbg_copyout(pid, (intptr_t)(start + done), page, sizeof page);
      if (rc != 0) break;
      if (write(out, page, sizeof page) != sizeof page) break;
    }
    if (out >= 0) close(out);
    fprintf(log, "%s start=0x%lx bytes=0x%lx rc=%d\n", name, start, done, rc);
    fflush(log);
  }
  fprintf(log, "done pid=%d\n", pid);
  fclose(log);
  return 0;
}
