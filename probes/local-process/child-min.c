// SPDX-License-Identifier: GPL-3.0-or-later
// A child that imports only libkernel: no libc.prx and no C runtime, because skipping the preload also skips libc.
#include <stddef.h>
#ifdef PROBE_SHARE
#include <errno.h>
#include <stdint.h>
#include "share.h"
int* __error(void);
#endif

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#ifdef PROBE_MEM
int32_t sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, const void* option, int* result);
int sceKernelReserveVirtualRange(void** address, size_t bytes, int flags, size_t alignment);
int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t* offset);
int sceKernelMapDirectMemory(void** address, size_t bytes, int protection, int flags, int64_t offset, size_t alignment);
int sceKernelMprotect(const void* address, size_t bytes, int protection);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t* address, size_t* bytes);
int sceKernelAvailableFlexibleMemorySize(size_t* bytes);
#endif

static size_t Length(const char* text) {
  size_t size = 0;
  while (text[size] != 0) size++;
  return size;
}

static void Log(const char* text, long value) {
  char line[128];
  size_t used = 0;
  for (size_t i = 0; text[i] != 0 && used < 80; i++) line[used++] = text[i];
  line[used++] = '=';
  char digits[24];
  int count = 0;
  unsigned long magnitude = value < 0 ? (unsigned long)-value : (unsigned long)value;
  do {
    digits[count++] = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  if (value < 0) line[used++] = '-';
  while (count > 0) line[used++] = digits[--count];
  line[used++] = '\n';
  const int fd = open("/app0/child.log", 0x0209 /* O_WRONLY | O_CREAT | O_APPEND */, 0666);
  if (fd >= 0) {
    write(fd, line, used);
    close(fd);
  }
}

__attribute__((visibility("default"))) void _start(void* parameters, void* teardown) {
  (void)teardown;
  Log("child started argc", (long)*(int*)parameters);
  char** argv = (char**)((char*)parameters + 8);
  if (argv[1] != NULL) Log("argv1 length", (long)Length(argv[1]));
  Log("pid", getpid());
#ifdef PROBE_KILL
  {
    const int fd = open("/app0/child-pid", 0x0601 /* O_WRONLY | O_CREAT | O_TRUNC */, 0666);
    char digits[16];
    int count = 0, pid = getpid();
    do {
      digits[count++] = (char)('0' + pid % 10);
      pid /= 10;
    } while (pid != 0);
    while (fd >= 0 && count > 0) write(fd, &digits[--count], 1);
    if (fd >= 0) close(fd);
  }
#endif
  Log("ppid", getppid());
  Log("uid", getuid());
  Log("euid", geteuid());
#ifdef PROBE_SHARE
  const int named = shm_open(PROBE_SHM_NAME, O_RDWR, 0);
  Log("named shm", named);
  Log("named errno", *__error());
  usleep(500000);
  if (named >= 0) {
    volatile uint32_t* page = (volatile uint32_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, named, 0);
    if (page != MAP_FAILED) Log("named shm parent wrote", (long)page[0]);
  }
  int sock = -1, connected = -1;
  for (int i = 0; i < 40 && connected != 0; i++) {
    for (int p = 0; p < kSocketPathCount && connected != 0; p++) {
      if (sock >= 0) close(sock);
      sock = socket(AF_UNIX, SOCK_STREAM, 0);
      struct sockaddr_un address;
      ProbeAddress(&address, kSocketPaths[p]);
      connected = connect(sock, (struct sockaddr*)&address, sizeof address);
      if (connected == 0) Log("connected path index", p);
    }
    if (connected != 0) usleep(100000);
  }
  Log("socket", sock);
  Log("connect", connected);
  Log("connect errno", *__error());
  const int pid_sent = write(sock, "P", 1) == 1;
  Log("sent byte", pid_sent);
  const int memory = ReceiveFd(sock);
  Log("received fd", memory);
  Log("receive errno", *__error());
  if (memory >= 0) {
    volatile uint32_t* shared = (volatile uint32_t*)mmap(NULL, kShareSize, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
    Log("mapped", shared != MAP_FAILED);
    Log("map errno", *__error());
    if (shared != MAP_FAILED) {
      Log("parent wrote", (long)shared[0]);
      Log("parent wrote at end", (long)shared[kShareSize / 4 - 1]);
      shared[1] = 0x22222222;
      Log("child wrote", (long)shared[1]);
      usleep(1200000);
      shared[2] = 1;
      Log("umtx wake woke", _umtx_op((void*)&shared[2], kUmtxWake, 1, NULL, NULL));
      for (int i = 0; i < 300 && shared[3] == 0; i++) usleep(50000);
      static int files[2048];
      int opened = 0;
      while (opened < 2000 && (files[opened] = open("/app0/sce_sys/param.json", 0)) >= 0) opened++;
      const int open_errno = *__error();
      shared[4] = (uint32_t)opened + 1;
      for (int i = 0; i < opened; i++) close(files[i]);
      Log("parent had opened", (long)shared[3] - 1);
      Log("child opened", opened);
      Log("child open errno", open_errno);
    }
  }
#endif
#ifdef PROBE_MEM
  {
    int start_result = 0;
    const int32_t libc = sceKernelLoadStartModule("/app0/sce_module/libc.prx", 0, NULL, 0, NULL, &start_result);
    Log("libc handle", libc);
    Log("libc start result", start_result);
    size_t flexible = 0, available = 0;
    int64_t where = 0;
    Log("flexible rc", sceKernelAvailableFlexibleMemorySize(&flexible));
    Log("flexible MiB", (long)(flexible >> 20));
    Log("direct total MiB", (long)(sceKernelGetDirectMemorySize() >> 20));
    Log("direct rc", sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), 0x4000, &where, &available));
    Log("direct available MiB", (long)(available >> 20));
    // The areas Wine reserves: fixed and refusing to overwrite (0x10 | 0x80), as patches 0570, 0895 and 0982.
    void* low = (void*)0x68000000ul;
    Log("reserve 0x68000000 rc", sceKernelReserveVirtualRange(&low, 0x17000000, 0x90, 0x4000));
    void* first = (void*)0x10000ul;
    Log("reserve 0x10000 rc", sceKernelReserveVirtualRange(&first, 0x400000, 0x90, 0x4000));
    size_t high_bytes = 0;
    for (size_t bytes = (size_t)512 << 30; bytes >= (size_t)16 << 30; bytes >>= 1) {
      void* high = (void*)0x1000000000ul;
      if (sceKernelReserveVirtualRange(&high, bytes, 0x90, 0x4000) == 0) {
        high_bytes = bytes;
        break;
      }
    }
    Log("high area GiB", (long)(high_bytes >> 30));
    // Whose budget: take 256 MiB of direct memory and hold it. The parent logs its own view meanwhile.
    int64_t offset = 0;
    const int allocated = sceKernelAllocateMainDirectMemory((size_t)256 << 20, 0x4000, 0x0c, &offset);
    Log("allocate 256 MiB rc", allocated);
    Log("direct offset MiB", (long)(offset >> 20));
    // Code in direct memory: map writable, write `mov eax, 42; ret`, make it executable, call it.
    if (allocated == 0) {
      void* code = (void*)0x1000000000ul;
      const int mapped = sceKernelMapDirectMemory(&code, 0x4000, 0x03, 0x10, offset, 0x4000);
      Log("map direct rc", mapped);
      if (mapped == 0) {
        static const unsigned char kCode[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
        for (size_t i = 0; i < sizeof kCode; i++) ((volatile unsigned char*)code)[i] = kCode[i];
        const int protect = sceKernelMprotect(code, 0x4000, 0x05);
        Log("mprotect rx rc", protect);
        if (protect == 0) Log("ran generated code, returned", ((int (*)(void))code)());
      }
    }
    Log("direct after, rc", sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), 0x4000, &where, &available));
    Log("direct available after MiB", (long)(available >> 20));
    Log("flexible after rc", sceKernelAvailableFlexibleMemorySize(&flexible));
    Log("flexible after MiB", (long)(flexible >> 20));
  }
#endif
#ifndef PROBE_CHILD_SECONDS
#define PROBE_CHILD_SECONDS 5
#endif
  for (int i = 0; i < PROBE_CHILD_SECONDS; i++) {
    sleep(1);
    Log("alive", i);
  }
  Log("exiting", 0);
  _exit(0);
}
