// SPDX-License-Identifier: GPL-3.0-or-later
// Runs in the title, as a game would: creates a named event (and, with PROBE_SECTION, a named shared section), then waits for the program in the child.
#include "ipc.h"

static DWORD __stdcall SetLater(void* event) {
  Sleep(500);
  SetEvent(event);
  return 0;
}

void __stdcall Entry(void) {
  HANDLE log = OpenLog("C:\\probe-owner.txt");
  HANDLE event = CreateEventA(0, 1, 0, "ProbeEvent");
  Note(log, "CreateEvent", event != 0, GetLastError());
#ifdef PROBE_SECTION
  HANDLE section = CreateFileMappingA((HANDLE)(long long)-1, 0, 4 /* PAGE_READWRITE */, 0, 4096, "ProbeSection");
  Note(log, "CreateFileMapping", section != 0, GetLastError());
  volatile DWORD* shared = MapViewOfFile(section, 2 /* FILE_MAP_WRITE */, 0, 0, 4096);
  Note(log, "MapViewOfFile", shared != 0, GetLastError());
  if (shared != 0) shared[0] = 0x11223344;
#else
  volatile DWORD* shared = 0;
#endif
  Note(log, "owner pid", GetCurrentProcessId(), 0);
  Note(log, "ready", 1, 0);
  // Controls before the child arrives: a wait that times out, then a wake-up from this process's own thread.
  HANDLE own = CreateEventA(0, 1, 0, 0);
  Note(log, "wait with timeout (0x102 = timed out)", WaitForSingleObject(own, 500), GetLastError());
  HANDLE thread = CreateThread(0, 0, SetLater, own, 0, 0);
  Note(log, "CreateThread", thread != 0, GetLastError());
  Note(log, "wait woken by own thread (0 = signaled)", WaitForSingleObject(own, 5000), GetLastError());
  const DWORD waited = WaitForSingleObject(event, 180000);
  Note(log, "WaitForSingleObject (0 = signaled by the other process)", waited, GetLastError());
  if (shared != 0) Note(log, "the other process wrote", shared[1], 0);
  CloseHandle(log);
  Sleep(2000);
  ExitProcess(0);
}
