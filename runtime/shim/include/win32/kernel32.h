/*
 * kernel32 subset used by the shim skeleton. Each function here is either
 * implemented in shim/src/kernel32_*.c or logged as unimplemented on first
 * call (vt3s_unimpl). Add prototypes as the game needs them.
 */
#ifndef VT3_WIN32_KERNEL32_H
#define VT3_WIN32_KERNEL32_H

#include "win32/types.h"

/* Heap. Backed by the guest arena (see vt3_guest.h). */
HANDLE WINAPI GetProcessHeap(void);
HANDLE WINAPI HeapCreate(DWORD options, SIZE_T initial_size, SIZE_T maximum_size);
BOOL   WINAPI HeapDestroy(HANDLE heap);
GPTR   WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
GPTR   WINAPI HeapReAlloc(HANDLE heap, DWORD flags, GPTR mem, SIZE_T bytes);
BOOL   WINAPI HeapFree(HANDLE heap, DWORD flags, GPTR mem);
SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, GPTR mem);

/* File I/O. Read-only for now. Paths map onto the game root, case-insensitively. */
HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                          DWORD disposition, DWORD flags, HANDLE template_file);
BOOL   WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD bytes_read, LPOVERLAPPED ovl);
BOOL   WINAPI WriteFile(HANDLE file, LPCVOID buffer, DWORD bytes, LPDWORD bytes_written, LPOVERLAPPED ovl);
DWORD  WINAPI SetFilePointer(HANDLE file, LONG distance, LPLONG distance_high, DWORD method);
DWORD  WINAPI GetFileSize(HANDLE file, LPDWORD size_high);
BOOL   WINAPI CloseHandle(HANDLE handle);

/* Errors. */
DWORD  WINAPI GetLastError(void);
void   WINAPI SetLastError(DWORD error);

/* Timers. */
DWORD  WINAPI GetTickCount(void);
BOOL   WINAPI QueryPerformanceCounter(LPLONGLONG count);
BOOL   WINAPI QueryPerformanceFrequency(LPLONGLONG frequency);
void   WINAPI Sleep(DWORD milliseconds);

#endif /* VT3_WIN32_KERNEL32_H */
