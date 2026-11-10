// SPDX-License-Identifier: GPL-3.0-or-later
// Stands in for wineserver.prx in the child: instead of starting a server, ntdll gets the client socket that the title's server
// made for it. The child puts the descriptor number in PROBE_SERVER_FD before Wine starts.
#include <stdlib.h>

__attribute__((visibility("default"))) int pw_wineserver_connect(const char* nls_dir) {
  (void)nls_dir;
  const char* text = getenv("PROBE_SERVER_FD");
  return text != NULL ? atoi(text) : -1;
}

__attribute__((visibility("default"))) int pw_wine_thread_register(long tid, void* thread) {
  (void)tid;
  (void)thread;
  return 0;
}
