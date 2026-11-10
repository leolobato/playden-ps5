// SPDX-License-Identifier: GPL-3.0-or-later
// One Wine server across two processes. Built twice: with PROBE_TITLE it is the title, which runs Wine and its in-process server
// and hands a second client socket to its child; without, it is the child, whose ntdll connects with that socket through
// server-shim.c. WINE_PS5_SERVER_DIRECT=0 and the shared-sync switches keep every request on the socket.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pw_wine_start.h"
#include "share.h"

#define PROBE_APP "/app0"
#define PROBE_DATA "/data/homebrew/FAKE77099"
#ifdef PROBE_TITLE
#define ROLE "title"
#define GRANT_OK PROBE_DATA "/grant-ok-title"
#define RUNTIME PROBE_DATA "/win/wine/lib/wine/x86_64-unix"
#define PROGRAM "Z:\\data\\homebrew\\FAKE77099\\ipc-owner.exe"
#else
#define ROLE "child"
#define GRANT_OK PROBE_DATA "/grant-ok-child"
#define RUNTIME PROBE_DATA "/client/lib/wine/x86_64-unix"
#define PROGRAM "Z:\\data\\homebrew\\FAKE77099\\ipc-peer.exe"
#endif

int32_t sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, const void* option, int* result);
int sceKernelGetModuleInfo(int32_t handle, void* info);
void _init_env(void* parameters);
int setenv(const char* name, const char* value, int overwrite);
#ifdef PROBE_TITLE
int sceSystemServiceAddLocalProcess(int app_id, const char* path, char* const* argv, const void* param);
int sceKernelGetAppInfo(int pid, void* info);
#endif
int sceKernelVirtualQuery(const void* address, int flags, void* info, size_t info_size);

static int log_fd = -1;
static void Text(const char* text) { (void)!write(log_fd, text, strlen(text)); }
static void Log(const char* text, long value) {
  char line[160];
  const int size = snprintf(line, sizeof line, "%s=%ld\n", text, value);
  (void)!write(log_fd, line, (size_t)size);
}

static int SetEnv(const char* name, const char* value) { return setenv(name, value, 1); }

struct ThreadStart { void (*entry)(void*); void* arg; };
static struct ThreadStart thread_start;
static void* Trampoline(void* unused) {
  (void)unused;
  Text("calling __wine_main\n");
  thread_start.entry(thread_start.arg);
  Text("__wine_main returned\n");
  return NULL;
}
static int StartThread(void (*entry)(void*), void* arg, size_t stack_bytes) {
  pthread_attr_t attr;
  pthread_t thread;
  thread_start.entry = entry;
  thread_start.arg = arg;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack_bytes);
  return pthread_create(&thread, &attr, Trampoline, NULL);
}

static void OutputSink(const char* text, size_t size) { (void)!write(log_fd, text, size); }

static void WritePid(const char* path) {
  char text[16];
  const int size = snprintf(text, sizeof text, "%d", getpid());
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  (void)!write(fd, text, (size_t)size);
  close(fd);
}

static void WaitForGrant(void) {
  // This path only resolves once the root view is in place, and the payload writes the file after the whole grant worked.
  struct stat st;
  int waited = 0;
  while (waited < 900 && stat(GRANT_OK, &st) != 0) {
    usleep(100000);
    waited++;
  }
  Log("waited tenths for the grant", waited);
  if (waited >= 900) {
    Text("no grant: stopping\n");
    _exit(3);
  }
  Log("prefix stat", stat(PROBE_DATA "/prefix2", &st));
  Log("prefix owner", (long)st.st_uid);
  Log("uid", (long)getuid());
}

static PwWineStart start;

struct QueryInfo { void* start; void* end; int64_t offset; int protection; int memory_type; uint32_t flags; char name[32]; };

// Wine's x64 syscall thunks call through this slot. With 16k host pages it shares a page with KUSER_SHARED_DATA, which the server
// maps for every process, so each process's ntdll writes the same memory.
static void LogDispatcherSlot(void) {
  const uintptr_t address = 0x7ffe1000u;
  struct QueryInfo query;
  memset(&query, 0, sizeof query);
  char line[120];
  const int status = sceKernelVirtualQuery((const void*)address, 0, &query, sizeof query);
  if (status != 0 || (uintptr_t)query.start > address || (query.protection & 1) == 0) return;
  const int size = snprintf(line, sizeof line, "dispatcher slot=%#lx\n", (unsigned long)*(const volatile uintptr_t*)address);
  (void)!write(log_fd, line, (size_t)size);
}

#ifdef PROBE_TITLE
struct LocalProcessParam {
  uint32_t size, field4, field8;
  uint64_t field_c;
  uint32_t field14;
  uint64_t field18;
  uint32_t field20;
  char name[0x20];
  uint32_t field44;
} __attribute__((packed));

static void HandOver(int listener) {
  void* (*wine_dlopen)(const char*, int) = (void* (*)(const char*, int))(uintptr_t)pw_prx_lookup(start.descriptor, "dlopen");
  void* (*wine_dlsym)(void*, const char*) = (void* (*)(void*, const char*))(uintptr_t)pw_prx_lookup(start.descriptor, "dlsym");
  Log("found dlopen", wine_dlopen != NULL);
  Log("found dlsym", wine_dlsym != NULL);
  if (wine_dlopen == NULL || wine_dlsym == NULL) return;
  void* module = wine_dlopen(RUNTIME "/wineserver.so", 2);
  Log("server module", module != NULL);
  if (module == NULL) return;
  int (*connect_server)(const char*) = (int (*)(const char*))wine_dlsym(module, "pw_wineserver_connect");
  Log("found pw_wineserver_connect", connect_server != NULL);
  if (connect_server == NULL) return;
  const int client = connect_server(PROBE_DATA "/win/wine/share/wine/nls");
  Log("second client socket", client);
  if (client < 0) return;

  uint8_t info[256];
  memset(info, 0, sizeof info);
  sceKernelGetAppInfo(getpid(), info);
  int app_id = 0;
  memcpy(&app_id, info, sizeof app_id);
  struct LocalProcessParam param;
  memset(&param, 0, sizeof param);
  param.size = sizeof param;
  param.field4 = 0xffffffffu;
  param.field8 = 1;
  param.field_c = 0xffffffffu;
  char arg0[] = "/app0/bdj.elf";
  char* const argv[] = {arg0, NULL};
  Log("add local process", sceSystemServiceAddLocalProcess(app_id, arg0, argv, &param));
  const int peer = listener >= 0 ? accept(listener, NULL, NULL) : -1;
  Log("child connected", peer);
  if (peer >= 0) Log("sent the socket", SendFd(peer, client));
}
#endif

__attribute__((visibility("default"))) void _start(void* parameters, void* teardown) {
  (void)teardown;
  log_fd = open(PROBE_APP "/" ROLE ".log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  Log(ROLE " started pid", getpid());
  _init_env(parameters);
  FILE* err = freopen(PROBE_APP "/" ROLE "-stderr.log", "w", stderr);
  if (err != NULL) setvbuf(err, NULL, _IONBF, 0);
  FILE* out = freopen(PROBE_APP "/" ROLE "-stdout.log", "w", stdout);
  if (out != NULL) setvbuf(out, NULL, _IONBF, 0);

#ifdef PROBE_TITLE
  WritePid(PROBE_APP "/title-pid");
#else
  WritePid(PROBE_APP "/child-pid");
#endif
  WaitForGrant();
#ifdef PROBE_TITLE
  // A run that fails early must not leave the last run's results looking like its own.
  unlink(PROBE_DATA "/prefix2/drive_c/probe-owner.txt");
  unlink(PROBE_DATA "/prefix2/drive_c/probe-peer.txt");
#endif

  // /data refuses socket files (errno 45), so the rendezvous goes in whichever memory-backed folder takes one. Both processes
  // have the same view after the grant.
  static const char* const kRendezvous[] = {"/tmp/playden-probe.sock", "/system_tmp/playden-probe.sock", "/var/run/playden-probe.sock",
                                            "/dev/playden-probe.sock", "/mnt/playden-probe.sock"};
  enum { kRendezvousCount = sizeof kRendezvous / sizeof kRendezvous[0] };
  struct sockaddr_un address;
  char fd_text[16] = "";
#ifdef PROBE_TITLE
  int listener = -1;
  for (int i = 0; i < kRendezvousCount && listener < 0; i++) {
    const int candidate = socket(AF_UNIX, SOCK_STREAM, 0);
    ProbeAddress(&address, kRendezvous[i]);
    unlink(kRendezvous[i]);
    const int bound = bind(candidate, (struct sockaddr*)&address, sizeof address);
    const int bind_errno = errno;
    Text(kRendezvous[i]);
    Text("\n");
    Log("  bind", bound);
    Log("  errno", bind_errno);
    if (bound == 0 && listen(candidate, 1) == 0) listener = candidate;
    else close(candidate);
  }
#else
  int sock = -1, connected = -1, connect_errno = 0;
  for (int i = 0; i < 100 && connected != 0; i++) {
    for (int p = 0; p < kRendezvousCount && connected != 0; p++) {
      if (sock >= 0) close(sock);
      sock = socket(AF_UNIX, SOCK_STREAM, 0);
      ProbeAddress(&address, kRendezvous[p]);
      connected = connect(sock, (struct sockaddr*)&address, sizeof address);
      connect_errno = errno;
      if (connected == 0) Text(kRendezvous[p]), Text("\n");
    }
    if (connected != 0) usleep(100000);
  }
  Log("connect", connected);
  Log("connect errno", connect_errno);
  const int server_fd = ReceiveFd(sock);
  Log("received the server socket", server_fd);
  Log("receive errno", errno);
  if (server_fd < 0) _exit(4);
  snprintf(fd_text, sizeof fd_text, "%d", server_fd);
#endif
  const PwWineStartEnv extra[] = {
      {"WINEDEBUG", "fixme-all,err+all,+seh,+loaddll,+timestamp"},
      {"WINEARCH", "wow64"},
      {"HOME", PROBE_DATA "/prefix2"},
      {"USER", "prospero"},
      {"WINE_PS5_TRACE_STARTUP", "1"},
      {"WINE_PS5_SERVER_DIRECT", "0"},
      {"WINE_PS5_MUTEX_FAST", "0"},
      {"WINE_PS5_MUTEX_SHARED", "0"},
      {"WINE_PS5_SYNC_SHARED", "0"},
      {"PW_SECTIONS_FLEXIBLE", "1"},  // Direct-memory sections are pipes that only the creating process can map.
      {"PROBE_SERVER_FD", fd_text},
  };
  static const char* const wine_argv[] = {"wine", PROGRAM, NULL};
  static const PwWineStartOps ops = {sceKernelLoadStartModule, sceKernelGetModuleInfo, SetEnv, StartThread};
  PwWineStartConfig config;
  memset(&config, 0, sizeof config);
  config.ntdll_path = RUNTIME "/ntdll.prx";
  config.ntdll_dir = RUNTIME;
  config.prefix = PROBE_DATA "/prefix2";
  config.extra_env = extra;
  config.extra_env_count = sizeof extra / sizeof extra[0];
  config.argc = 2;
  config.argv = wine_argv;
  config.stack_bytes = 16u << 20;
#ifndef PROBE_TITLE
  // The title has this stub at the lowest module address. Taking the same place keeps ntdll.prx, and so the dispatcher address both
  // processes write to the shared slot, the same in both.
  int stub_result = 0;
  Log("libc stub", sceKernelLoadStartModule(PROBE_DATA "/sce_module/libc.prx", 0, NULL, 0, NULL, &stub_result));
#endif
  int status = pw_wine_start_load(&start, &config, &ops);
  Log("load status", status);
  Log("load stage", start.stage);
  if (status == 0) {
    void (*set_sink)(void (*)(const char*, size_t)) =
        (void (*)(void (*)(const char*, size_t)))(uintptr_t)pw_prx_lookup(start.descriptor, "__wine_ps5_set_output_sink");
    if (set_sink != NULL) set_sink(OutputSink);
    status = pw_wine_start_environment(&start, &config, &ops);
    Log("environment status", status);
  }
  if (status == 0) {
    status = pw_wine_start_run(&start, &config, &ops);
    Log("run status", status);
  }
#ifdef PROBE_TITLE
  // Give the title's program time to create the event and section, then bring in the child.
  sleep(8);
  LogDispatcherSlot();
  HandOver(listener);
#endif
  for (int i = 0; i < 120; i++) {
    sleep(1);
    if (i % 5 == 0) {
      Log(ROLE " alive", i);
      LogDispatcherSlot();
    }
  }
  _exit(0);
}
