// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 1 of the helper-process probe: one call to sceSystemServiceAddLocalProcess from a title.
// The parameter block repeats the defaults that libSceSystemService fills in (FW 13.60, read from its code).
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PROBE_CHILD
#define PROBE_CHILD "/app0/child.bin"
#endif

#include "report.h"
#include "share.h"

#define LOG "/app0/parent.log"

int sceSystemServiceAddLocalProcess(int app_id, const char* path, char* const* argv, const void* param);
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
int sceSystemServiceHideSplashScreen(void);
int sceKernelGetAppInfo(int pid, void* info);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t* address, size_t* bytes);
int sceKernelAvailableFlexibleMemorySize(size_t* bytes);

static void ReportMemory(const char* when) {
  size_t flexible = 0, direct = 0;
  int64_t where = 0;
  sceKernelAvailableFlexibleMemorySize(&flexible);
  sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), 0x4000, &where, &direct);
  Report("parent", LOG, "memory %s flexible=%zu MiB direct=%zu MiB of %zu MiB", when, flexible >> 20, direct >> 20,
         sceKernelGetDirectMemorySize() >> 20);
}

struct LocalProcessParam {
  uint32_t size;       // +0x00
  uint32_t field4;     // +0x04, copied to +0x08 of the request
  uint32_t field8;     // +0x08, copied to +0x04 of the request
  uint64_t field_c;    // +0x0c, copied to +0x10 of the request
  uint32_t field14;    // +0x14, not read
  uint64_t field18;    // +0x18
  uint32_t field20;    // +0x20
  char name[0x20];     // +0x24
  uint32_t field44;    // +0x44
} __attribute__((packed));
_Static_assert(sizeof(struct LocalProcessParam) == 0x48, "the library reads 0x48 bytes");

int main(void) {
  sceSystemServiceHideSplashScreen();
  uint8_t info[256];
  memset(info, 0, sizeof info);
  const int info_rc = sceKernelGetAppInfo(getpid(), info);
  int app_id = 0;
  memcpy(&app_id, info, sizeof app_id);
  Report("parent", LOG, "started uid=%d euid=%d info_rc=0x%x app_id=0x%x", (int)getuid(), (int)geteuid(), info_rc, app_id);

#ifdef PROBE_HOLD
  // No call: stay alive so that a payload can read this process's libraries.
  for (int i = 0; i < 180; i++) {
    sleep(1);
    if (i % 30 == 0) Report("parent", LOG, "holding %d", i);
  }
  sceSystemServiceLoadExec("exit", NULL);
  return 0;
#endif
  struct LocalProcessParam param;
  memset(&param, 0, sizeof param);
  param.size = sizeof param;
  param.field4 = 0xffffffffu;
  param.field8 = 1;
  param.field_c = 0xffffffffu;

  struct stat st;
  Report("parent", LOG, "child file stat=%d", stat(PROBE_CHILD, &st));
  char arg0[] = PROBE_CHILD, arg1[] = "hello-from-parent";
  char* const argv[] = {arg0, arg1, NULL};
  unlink("/app0/child-pid");  // The kill test must only ever read this run's child.
  ReportMemory("before");
  Report("parent", LOG, "calling");
  const int rc = sceSystemServiceAddLocalProcess(app_id, PROBE_CHILD, argv, &param);
  Report("parent", LOG, "returned rc=0x%x (%d)", rc, rc);

#ifdef PROBE_COUNT
  // How many can run at once: keep adding until the console refuses. Each is told apart by the shared log.
  for (int i = 1; i < PROBE_COUNT; i++) {
    const int more = sceSystemServiceAddLocalProcess(app_id, PROBE_CHILD, argv, &param);
    Report("parent", LOG, "extra child %d rc=0x%x", i, more);
    if (more < 0) break;
  }
  ReportMemory("after the extra children");
#endif
#ifdef PROBE_KILL
  // Can the title end its child? The child writes its PID into the title folder.
  sleep(3);
  {
    char text[16] = {0};
    const int pid_fd = open("/app0/child-pid", O_RDONLY);
    const long got = pid_fd >= 0 ? read(pid_fd, text, sizeof text - 1) : -1;
    if (pid_fd >= 0) close(pid_fd);
    int target = 0;
    for (int i = 0; text[i] >= '0' && text[i] <= '9'; i++) target = target * 10 + (text[i] - '0');
    Report("parent", LOG, "child pid file got=%ld pid=%d", got, target);
    errno = 0;
    const int by_signal = target > 0 ? kill(target, SIGKILL) : -1;
    Report("parent", LOG, "kill rc=%d errno=%d", by_signal, errno);
  }
#endif
#ifdef PROBE_SHARE
  const int named = shm_open(PROBE_SHM_NAME, O_RDWR | O_CREAT, 0666);
  Report("parent", LOG, "named shm=%d errno=%d", named, errno);
  if (named >= 0) {
    ftruncate(named, 4096);
    volatile uint32_t* page = (volatile uint32_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, named, 0);
    if (page != MAP_FAILED) page[0] = 0x44444444;
  }
  int listener = -1;
  for (int i = 0; i < kSocketPathCount && listener < 0; i++) {
    const int candidate = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    ProbeAddress(&address, kSocketPaths[i]);
    unlink(kSocketPaths[i]);
    const int bound = bind(candidate, (struct sockaddr*)&address, sizeof address);
    const int bind_errno = errno;
    const int listening = bound == 0 ? listen(candidate, 1) : -1;
    Report("parent", LOG, "path=%s bind=%d errno=%d listen=%d", kSocketPaths[i], bound, bind_errno, listening);
    if (listening == 0) listener = candidate; else close(candidate);
  }
  const int peer = listener >= 0 ? accept(listener, NULL, NULL) : -1;
  Report("parent", LOG, "accept=%d errno=%d", peer, errno);
  char hello = 0;
  const long got_hello = peer >= 0 ? read(peer, &hello, 1) : -1;
  Report("parent", LOG, "child said %c rc=%ld", hello, got_hello);
  const int memory = shm_open(SHM_ANON, O_RDWR, 0600);
  const int sized = ftruncate(memory, kShareSize);
  volatile uint32_t* shared = (volatile uint32_t*)mmap(NULL, kShareSize, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
  Report("parent", LOG, "shm=%d truncate=%d map=%p errno=%d", memory, sized, (void*)shared, errno);
  if (shared != MAP_FAILED) {
    shared[0] = 0x11111111;
    shared[kShareSize / 4 - 1] = 0x33333333;
    const int sent = SendFd(peer, memory);
    Report("parent", LOG, "sent fd rc=%d errno=%d", sent, errno);
    for (int i = 0; i < 100 && shared[1] != 0x22222222; i++) usleep(100000);
    Report("parent", LOG, "child wrote 0x%x (want 0x22222222)", shared[1]);
    // A wake-up across processes: sleep on a word in the shared page until the child changes it and wakes this thread.
    struct timespec wait_limit = {10, 0}, before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    const int waited = _umtx_op((void*)&shared[2], kUmtxWaitUint, 0, (void*)sizeof wait_limit, &wait_limit);
    const int wait_errno = errno;
    clock_gettime(CLOCK_MONOTONIC, &after);
    Report("parent", LOG, "umtx wait rc=%d errno=%d word=%u ms=%ld", waited, wait_errno, shared[2],
           (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000);
    // Open files: fill this process's table, tell the child, and keep them open while it fills its own.
    static int files[2048];
    int opened = 0;
    while (opened < 2000 && (files[opened] = open("/app0/sce_sys/param.json", O_RDONLY)) >= 0) opened++;
    const int open_errno = errno;
    shared[3] = (uint32_t)opened + 1;
    // Hold them until the child has filled its own table, then free them so that the log can be written.
    for (int i = 0; i < 200 && shared[4] == 0; i++) usleep(50000);
    errno = 0;
    const int another = open("/app0/sce_sys/param.json", O_RDONLY);
    for (int i = 0; i < opened; i++) close(files[i]);
    Report("parent", LOG, "parent opened %d files, errno=%d; child opened %u; parent's next open=%d", opened, open_errno, shared[4] - 1,
           another);
  }
#endif
#ifndef PROBE_PARENT_SECONDS
#define PROBE_PARENT_SECONDS 12
#endif
  for (int i = 0; i < PROBE_PARENT_SECONDS; i++) {
    sleep(1);
    Report("parent", LOG, "waiting %d child_log=%d", i, stat("/app0/child.log", &st));
    ReportMemory("during");
  }
  Report("parent", LOG, "leaving through LoadExec exit");
  sceSystemServiceLoadExec("exit", NULL);
  return 0;
}
