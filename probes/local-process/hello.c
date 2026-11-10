// SPDX-License-Identifier: GPL-3.0-or-later
// A Windows program for the probe: proves Wine ran it in the child, and stays alive for a few seconds.
typedef void* HANDLE;
typedef unsigned long DWORD;
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);
__declspec(dllimport) void __stdcall Sleep(DWORD);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);

void __stdcall Entry(void) {
  static const char text[] = "hello from Windows code in the local process\r\n";
  DWORD written = 0;
  HANDLE file = CreateFileA("C:\\probe-hello.txt", 0x40000000 /* GENERIC_WRITE */, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
  if (file != (HANDLE)(long long)-1) {
    WriteFile(file, text, sizeof text - 1, &written, 0);
    CloseHandle(file);
  }
  (void)GetCurrentProcessId();
  Sleep(4000);
  ExitProcess(7);
}
