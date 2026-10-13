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

/* Console, process, and module. */
HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
void   WINAPI ExitProcess(UINT code);
HANDLE WINAPI GetModuleHandleA(LPCSTR name);
LPSTR  WINAPI GetCommandLineA(void);
DWORD  WINAPI GetCurrentProcessId(void);
DWORD  WINAPI GetCurrentThreadId(void);

/* Strings and codepages. */
int    WINAPI lstrlenA(LPCSTR s);
LPSTR  WINAPI lstrcpyA(LPSTR dst, LPCSTR src);
int    WINAPI WideCharToMultiByte(UINT codepage, DWORD flags, LPCVOID wide, int wide_len,
                                  LPSTR multi, int multi_len, LPCSTR defchar, LPVOID used);
int    WINAPI MultiByteToWideChar(UINT codepage, DWORD flags, LPCSTR multi, int multi_len,
                                  LPVOID wide, int wide_len);

/* Thread-local storage. */
DWORD  WINAPI TlsAlloc(void);
LPVOID WINAPI TlsGetValue(DWORD index);
BOOL   WINAPI TlsSetValue(DWORD index, LPVOID value);
BOOL   WINAPI TlsFree(DWORD index);

/* Critical sections (the argument is a CRITICAL_SECTION in guest memory). */
void   WINAPI InitializeCriticalSection(LPVOID cs);
void   WINAPI EnterCriticalSection(LPVOID cs);
void   WINAPI LeaveCriticalSection(LPVOID cs);
void   WINAPI DeleteCriticalSection(LPVOID cs);

/* Interlocked operations. */
LONG   WINAPI InterlockedIncrement(LPLONG addend);
LONG   WINAPI InterlockedDecrement(LPLONG addend);
LONG   WINAPI InterlockedExchange(LPLONG target, LONG value);


/* System, version, and locale. */
void   WINAPI GetSystemTimeAsFileTime(LPVOID ft);
void   WINAPI GetStartupInfoA(LPVOID si);
void   WINAPI GetSystemInfo(LPVOID si);
BOOL   WINAPI GetVersionExA(LPVOID info);
DWORD  WINAPI GetVersion(void);
DWORD  WINAPI GetModuleFileNameA(HANDLE module, LPSTR buf, DWORD size);
UINT   WINAPI GetACP(void);
UINT   WINAPI GetOEMCP(void);
UINT   WINAPI GetConsoleOutputCP(void);
UINT   WINAPI GetConsoleCP(void);
BOOL   WINAPI GetCPInfo(UINT cp, LPVOID info);
BOOL   WINAPI GetStringTypeA(DWORD locale, DWORD type, LPCSTR src, int n, LPVOID out);
BOOL   WINAPI GetStringTypeW(DWORD type, LPCVOID src, int n, LPVOID out);
int    WINAPI LCMapStringA(DWORD locale, DWORD flags, LPCSTR src, int srclen, LPSTR dst, int dstlen);
int    WINAPI LCMapStringW(DWORD locale, DWORD flags, LPCVOID src, int srclen, LPVOID dst, int dstlen);
int    WINAPI CompareStringA(DWORD locale, DWORD flags, LPCSTR s1, int n1, LPCSTR s2, int n2);
int    WINAPI CompareStringW(DWORD locale, DWORD flags, LPCVOID s1, int n1, LPCVOID s2, int n2);
BOOL   WINAPI GetLocaleInfoA(DWORD locale, DWORD type, LPSTR buf, int n);

/* Handles, files, and the console. */
UINT   WINAPI SetHandleCount(UINT n);
DWORD  WINAPI GetFileType(HANDLE h);
BOOL   WINAPI FlushFileBuffers(HANDLE h);
BOOL   WINAPI GetFileSizeEx(HANDLE h, LPLONGLONG size);
BOOL   WINAPI GetDiskFreeSpaceExA(LPCSTR dir, LPVOID free_avail, LPVOID total, LPVOID total_free);
DWORD  WINAPI GetFullPathNameA(LPCSTR name, DWORD len, LPSTR buf, LPVOID filepart);
BOOL   WINAPI WriteConsoleA(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPVOID reserved);

/* Environment and time zone. */
LPVOID WINAPI GetEnvironmentStringsA(void);
LPVOID WINAPI GetEnvironmentStringsW(void);
BOOL   WINAPI FreeEnvironmentStringsA(LPVOID block);
BOOL   WINAPI FreeEnvironmentStringsW(LPVOID block);
BOOL   WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
DWORD  WINAPI GetTimeZoneInformation(LPVOID tz);

/* Virtual memory. */
LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL   WINAPI VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
SIZE_T WINAPI VirtualQuery(LPVOID addr, LPVOID info, SIZE_T len);

/* Events, mutexes, waits. */
HANDLE WINAPI CreateEventA(LPVOID attrs, BOOL manual, BOOL initial, LPCSTR name);
HANDLE WINAPI CreateMutexA(LPVOID attrs, BOOL owner, LPCSTR name);
BOOL   WINAPI SetEvent(HANDLE h);
BOOL   WINAPI ResetEvent(HANDLE h);
BOOL   WINAPI PulseEvent(HANDLE h);
BOOL   WINAPI ReleaseMutex(HANDLE h);
DWORD  WINAPI WaitForSingleObject(HANDLE h, DWORD ms);
DWORD  WINAPI WaitForMultipleObjects(DWORD count, LPVOID handles, BOOL wait_all, DWORD ms);
HANDLE WINAPI GetCurrentThread(void);
BOOL   WINAPI SwitchToThread(void);

/* Threads and processes. */
HANDLE WINAPI CreateThread(LPVOID attrs, SIZE_T stack, LPVOID start, LPVOID param, DWORD flags, LPDWORD tid);
void   WINAPI ExitThread(DWORD code);
BOOL   WINAPI GetExitCodeThread(HANDLE h, LPDWORD code);
DWORD  WINAPI ResumeThread(HANDLE h);
DWORD  WINAPI SuspendThread(HANDLE h);
BOOL   WINAPI SetThreadPriority(HANDLE h, int prio);
int    WINAPI GetThreadPriority(HANDLE h);
DWORD  WINAPI SetThreadIdealProcessor(HANDLE h, DWORD cpu);
HANDLE WINAPI GetCurrentProcess(void);
BOOL   WINAPI TerminateProcess(HANDLE h, UINT code);
void   WINAPI DebugBreak(void);
BOOL   WINAPI IsDebuggerPresent(void);
void   WINAPI OutputDebugStringA(LPCSTR s);
void   WINAPI RaiseException(DWORD code, DWORD flags, DWORD nargs, LPVOID args);
LPVOID WINAPI SetUnhandledExceptionFilter(LPVOID filter);
LONG   WINAPI UnhandledExceptionFilter(LPVOID info);
void   WINAPI RtlUnwind(LPVOID target, LPVOID ip, LPVOID rec, LPVOID value);

/* Strings and libraries. */
LPSTR  WINAPI lstrcatA(LPSTR dst, LPCSTR src);
int    WINAPI lstrcmpiA(LPCSTR a, LPCSTR b);
LPSTR  WINAPI lstrcpynA(LPSTR dst, LPCSTR src, int n);
HANDLE WINAPI LoadLibraryA(LPCSTR name);
LPVOID WINAPI GetProcAddress(HANDLE module, LPCSTR name);

#endif /* VT3_WIN32_KERNEL32_H */
