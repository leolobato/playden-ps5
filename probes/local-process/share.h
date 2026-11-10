// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 3 of the probe: pass a shared-memory descriptor to the child with SCM_RIGHTS, and check both sides see each other's writes.
#pragma once
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char* const kSocketPaths[] = {"/app0/probe.sock", "/temp0/probe.sock", "/download0/probe.sock", "/tmp/probe.sock",
                                           "/data/probe.sock", "/hostapp/probe.sock", "/user/probe.sock", "/dev/probe.sock"};
enum { kSocketPathCount = sizeof kSocketPaths / sizeof kSocketPaths[0] };
#define PROBE_SHM_NAME "/playden-probe-shm"
enum { kShareSize = 2 * 1024 * 1024 };
// Words in the shared page: [2] the wake-up test, [3] and [4] the open-file counts plus one, set once each side filled its table.
enum { kUmtxWaitUint = 11, kUmtxWake = 3 };
int _umtx_op(void* object, int op, unsigned long value, void* size, void* timeout);

static inline void ProbeAddress(struct sockaddr_un* address, const char* path) {
  for (size_t i = 0; i < sizeof *address; i++) ((char*)address)[i] = 0;
  address->sun_family = AF_UNIX;
  address->sun_len = sizeof *address;
  for (size_t i = 0; path[i] != 0; i++) address->sun_path[i] = path[i];
}

static inline int SendFd(int sock, int fd) {
  char byte = 'F';
  struct iovec io = {&byte, 1};
  char control[CMSG_SPACE(sizeof(int))];
  memset(control, 0, sizeof control);
  struct msghdr message;
  memset(&message, 0, sizeof message);
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof control;
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(header), &fd, sizeof fd);
  return sendmsg(sock, &message, 0) == 1 ? 0 : -1;
}

static inline int ReceiveFd(int sock) {
  char byte = 0;
  struct iovec io = {&byte, 1};
  char control[CMSG_SPACE(sizeof(int))];
  struct msghdr message;
  memset(&message, 0, sizeof message);
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof control;
  if (recvmsg(sock, &message, 0) != 1) return -1;
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  if (header == NULL || header->cmsg_type != SCM_RIGHTS) return -2;
  int fd = -3;
  memcpy(&fd, CMSG_DATA(header), sizeof fd);
  return fd;
}
