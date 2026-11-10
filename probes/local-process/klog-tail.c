// SPDX-License-Identifier: GPL-3.0-or-later
// Payload: copies the kernel log to a file for a fixed time, so a title that dies leaves its reason behind.
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  const int in = open("/dev/klog", O_RDONLY | O_NONBLOCK);
  const int out = open("/data/local-process-probe/klog.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out < 0) return 1;
  if (in < 0) {
    (void)!write(out, "cannot open /dev/klog\n", 22);
    return 2;
  }
  static char buffer[8192];
  const time_t end = time(NULL) + 75;
  while (time(NULL) < end) {
    const ssize_t got = read(in, buffer, sizeof buffer);
    if (got > 0) {
      (void)!write(out, buffer, (size_t)got);
    } else {
      usleep(100000);
    }
  }
  close(out);
  close(in);
  return 0;
}
