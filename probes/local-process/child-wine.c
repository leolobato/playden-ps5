// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4 of the probe: the child loads ntdll.prx and enters __wine_main, as the title shell does. Wine's in-process
// server runs in this child, so it shares nothing with the title. Imports libkernel and the system C library only.
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pw_wine_start.h"

#define PROBE_APP "/app0"
#define PROBE_DATA "/data/homebrew/FAKE77099"
#define PROBE_RUNTIME PROBE_DATA "/win/wine/lib/wine/x86_64-unix"

int32_t sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, const void* option, int* result);
int sceKernelGetModuleInfo(int32_t handle, void* info);

static int log_fd = -1;
static void Text(const char* text) { (void)!write(log_fd, text, strlen(text)); }
static void Log(const char* text, long value) {
  char digits[24];
  int count = 0;
  unsigned long magnitude = value < 0 ? (unsigned long)-value : (unsigned long)value;
  do {
    digits[count++] = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  Text(text);
  Text("=");
  if (value < 0) Text("-");
  while (count > 0) (void)!write(log_fd, &digits[--count], 1);
  Text("\n");
}

void _init_env(void* parameters);
int setenv(const char* name, const char* value, int overwrite);
static int SetEnv(const char* name, const char* value) { return setenv(name, value, 1); }

struct ThreadStart { void (*entry)(void*); void* arg; };
static struct ThreadStart thread_start;
static void* Trampoline(void* unused) {
  (void)unused;
  Text("thread entered, calling __wine_main\n");
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

static PwWineStart start;

__attribute__((visibility("default"))) void _start(void* parameters, void* teardown) {
  (void)teardown;
  log_fd = open(PROBE_APP "/child.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
  Log("child started pid", getpid());
  _init_env(parameters);
  Text("libc initialised\n");
  // The console protects descriptors 0-3 in a local process, so Wine's messages are redirected at the stdio level.
  FILE* err = freopen(PROBE_APP "/wine-stderr.log", "a", stderr);
  FILE* out = freopen(PROBE_APP "/wine-stdout.log", "a", stdout);
  Log("freopen stderr ok", err != NULL);
  Log("freopen stdout ok", out != NULL);
  if (err != NULL) setvbuf(err, NULL, _IONBF, 0);
  if (out != NULL) setvbuf(out, NULL, _IONBF, 0);
  fprintf(stderr, "stderr redirected\n");

  // A real title has the /data grant: the real root view, where the bottle shows its true owner. Here a payload
  // (grant-child.c) gives the same view once it sees this process's PID, and Wine then uses /data paths as the shell does.
  {
    char text[16];
    const int size = snprintf(text, sizeof text, "%d", getpid());
    const int pid_fd = open(PROBE_APP "/child-pid", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    (void)!write(pid_fd, text, (size_t)size);
    close(pid_fd);
    struct stat st;
    int waited = 0;
    // This path only resolves once the root view is in place, and the payload writes the file after the whole grant worked.
    while (waited < 200 && stat(PROBE_DATA "/grant-ok-child", &st) != 0) {
      usleep(100000);
      waited++;
    }
    Log("waited tenths for the grant", waited);
    if (waited >= 200) _exit(3);
    Log("uid", (long)getuid());
    Log("prefix stat", stat(PROBE_DATA "/prefix", &st));
    Log("prefix owner", (long)st.st_uid);
  }
  const char* const prefix = PROBE_DATA "/prefix";

  const PwWineStartEnv extra[] = {
      {"WINEDEBUG", "fixme-all,err+all"},
      {"WINEARCH", "wow64"},
      {"HOME", prefix},
      {"USER", "prospero"},
      {"WINE_PS5_TRACE_STARTUP", "1"},
  };
  static const char* const wine_argv[] = {"wine", "Z:\\data\\homebrew\\FAKE77099\\hello.exe", NULL};
  static const PwWineStartOps ops = {sceKernelLoadStartModule, sceKernelGetModuleInfo, SetEnv, StartThread};
  PwWineStartConfig config;
  memset(&config, 0, sizeof config);
  config.ntdll_path = PROBE_RUNTIME "/ntdll.prx";
  config.ntdll_dir = PROBE_RUNTIME;
  config.prefix = prefix;
  config.extra_env = extra;
  config.extra_env_count = sizeof extra / sizeof extra[0];
  config.argc = 2;
  config.argv = wine_argv;
  config.stack_bytes = 16u << 20;
  int status = pw_wine_start_load(&start, &config, &ops);
  Log("load status", status);
  Log("load stage", start.stage);
  Log("module", start.module);
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
  for (int i = 0; i < 90; i++) {
    sleep(1);
    Log("wine child alive", i);
  }
  _exit(0);
}
