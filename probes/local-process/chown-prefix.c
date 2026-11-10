// SPDX-License-Identifier: GPL-3.0-or-later
// Payload: gives the probe's scratch Wine prefix to the title's uid, which Wine's server checks. Touches only that folder.
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "/data/homebrew/FAKE77099/prefix2"

int main(void) {
  FILE* log = fopen("/data/local-process-probe/chown.log", "w");
  if (log == NULL) return 1;
  struct stat st;
  if (stat(PREFIX, &st) != 0) {
    fprintf(log, "no prefix\n");
    fclose(log);
    return 2;
  }
  fprintf(log, "before uid=%d gid=%d\n", (int)st.st_uid, (int)st.st_gid);
  fprintf(log, "chown=%d\n", chown(PREFIX, 1, 1));
  const int after = stat(PREFIX, &st);
  fprintf(log, "after stat=%d uid=%d gid=%d\n", after, after == 0 ? (int)st.st_uid : -1, after == 0 ? (int)st.st_gid : -1);
  fclose(log);
  return 0;
}
