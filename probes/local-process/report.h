// SPDX-License-Identifier: GPL-3.0-or-later
// Reports over UDP to the Mac and into a file, so a result survives a title that dies right after a call.
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef PROBE_MAC_IP
#define PROBE_MAC_IP "10.0.1.72"
#endif
#define PROBE_PORT 47100

static inline void Report(const char* who, const char* path, const char* format, ...) {
  char line[512];
  int used = snprintf(line, sizeof line, "%s pid=%d ", who, (int)getpid());
  va_list args;
  va_start(args, format);
  used += vsnprintf(line + used, sizeof line - (size_t)used, format, args);
  va_end(args);
  if (used > (int)sizeof line - 2) used = (int)sizeof line - 2;
  line[used++] = '\n';
  const int out = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (out >= 0) {
    (void)!write(out, line, (size_t)used);
    close(out);
  }
  const int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock >= 0) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(PROBE_PORT);
    inet_pton(AF_INET, PROBE_MAC_IP, &to.sin_addr);
    sendto(sock, line, (size_t)used, 0, (struct sockaddr*)&to, sizeof to);
    close(sock);
  }
}
