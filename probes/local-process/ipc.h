// SPDX-License-Identifier: GPL-3.0-or-later
// What the two probe programs share: kernel32 imports and a small log.
#pragma once
typedef void* HANDLE;
typedef unsigned long DWORD;
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) int __stdcall FlushFileBuffers(HANDLE);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);
__declspec(dllimport) DWORD __stdcall GetLastError(void);
__declspec(dllimport) void __stdcall Sleep(DWORD);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);
__declspec(dllimport) HANDLE __stdcall CreateEventA(void*, int, int, const char*);
__declspec(dllimport) HANDLE __stdcall OpenEventA(DWORD, int, const char*);
__declspec(dllimport) int __stdcall SetEvent(HANDLE);
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) HANDLE __stdcall CreateThread(void*, unsigned long long, DWORD(__stdcall*)(void*), void*, DWORD, DWORD*);
__declspec(dllimport) HANDLE __stdcall CreateFileMappingA(HANDLE, void*, DWORD, DWORD, DWORD, const char*);
__declspec(dllimport) HANDLE __stdcall OpenFileMappingA(DWORD, int, const char*);
__declspec(dllimport) void* __stdcall MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, unsigned long long);

static HANDLE OpenLog(const char* path) {
  return CreateFileA(path, 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
}

static void Hex(char** at, unsigned long long value) {
  static const char digits[] = "0123456789abcdef";
  *(*at)++ = '0';
  *(*at)++ = 'x';
  for (int shift = 28; shift >= 0; shift -= 4) *(*at)++ = digits[(value >> shift) & 15];
}

static void Note(HANDLE log, const char* text, unsigned long long value, DWORD error) {
  char line[160], *at = line;
  while (*text != 0 && at < line + 100) *at++ = *text++;
  *at++ = ' ';
  Hex(&at, value);
  *at++ = ' ';
  *at++ = 'e';
  Hex(&at, error);
  *at++ = '\r';
  *at++ = '\n';
  DWORD written = 0;
  WriteFile(log, line, (DWORD)(at - line), &written, 0);
  FlushFileBuffers(log);
}
