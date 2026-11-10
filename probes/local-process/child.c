// SPDX-License-Identifier: GPL-3.0-or-later
// The trivial child: says it runs, with what rights, and exits by itself after a few seconds.
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "report.h"

#define LOG "/app0/child.log"

int main(int argc, char** argv) {
  Report("child", LOG, "started argc=%d argv1=%s ppid=%d uid=%d euid=%d", argc, argc > 1 ? argv[1] : "-", (int)getppid(),
         (int)getuid(), (int)geteuid());
  for (int i = 0; i < 5; i++) {
    sleep(1);
    Report("child", LOG, "alive %d ppid=%d", i, (int)getppid());
  }
  Report("child", LOG, "exiting");
  return 0;
}
