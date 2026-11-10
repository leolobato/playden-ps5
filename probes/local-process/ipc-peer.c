// SPDX-License-Identifier: GPL-3.0-or-later
// Runs in the child, as a Steam host would: opens the title's event (and, with PROBE_SECTION, section) by name through the shared Wine server.
#include "ipc.h"

void __stdcall Entry(void) {
  HANDLE log = OpenLog("C:\\probe-peer.txt");
  Note(log, "peer pid", GetCurrentProcessId(), 0);
  HANDLE event = OpenEventA(2 /* EVENT_MODIFY_STATE */, 0, "ProbeEvent");
  Note(log, "OpenEvent", event != 0, GetLastError());
#ifdef PROBE_SECTION
  HANDLE section = OpenFileMappingA(2 /* FILE_MAP_WRITE */, 0, "ProbeSection");
  Note(log, "OpenFileMapping", section != 0, GetLastError());
  volatile DWORD* shared = section != 0 ? MapViewOfFile(section, 2, 0, 0, 4096) : 0;
  Note(log, "MapViewOfFile", shared != 0, GetLastError());
  if (shared != 0) {
    Note(log, "the title wrote", shared[0], 0);
    shared[1] = 0x55667788;
  }
#endif
  Note(log, "waiting before SetEvent", 15000, 0);
  Sleep(15000);
  if (event != 0) Note(log, "SetEvent", SetEvent(event), GetLastError());
  CloseHandle(log);
  ExitProcess(21);
}
