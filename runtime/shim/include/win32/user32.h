/*
 * user32 subset. Prototypes only. The implementations are in shim/src/user32_stub.c
 * (MessageBoxA) and shim/src/user32_window.c (window and message stubs).
 */
#ifndef VT3_WIN32_USER32_H
#define VT3_WIN32_USER32_H

#include "win32/types.h"

typedef UINT ATOM;

/* Returns IDOK (1) without showing anything. */
int    WINAPI MessageBoxA(HANDLE hwnd, LPCSTR text, LPCSTR caption, UINT type);

/* Window classes and windows. Fake handles, no window system. */
ATOM   WINAPI RegisterClassExA(LPVOID wndclass);
ATOM   WINAPI RegisterClassA(LPVOID wndclass);
BOOL   WINAPI UnregisterClassA(LPCSTR name, HANDLE inst);
HANDLE WINAPI CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR name, DWORD style,
                              int x, int y, int w, int h, HANDLE parent, HANDLE menu,
                              HANDLE inst, LPVOID param);
BOOL   WINAPI DestroyWindow(HANDLE h);
BOOL   WINAPI ShowWindow(HANDLE h, int cmd);
BOOL   WINAPI UpdateWindow(HANDLE h);
BOOL   WINAPI SetWindowPos(HANDLE h, HANDLE after, int x, int y, int cx, int cy, UINT flags);
BOOL   WINAPI AdjustWindowRectEx(LPVOID rect, DWORD style, BOOL menu, DWORD exstyle);
BOOL   WINAPI SetForegroundWindow(HANDLE h);
HANDLE WINAPI FindWindowA(LPCSTR cls, LPCSTR name);
HANDLE WINAPI GetForegroundWindow(void);
BOOL   WINAPI ScreenToClient(HANDLE h, LPVOID pt);
BOOL   WINAPI GetCursorPos(LPVOID pt);
int    WINAPI ShowCursor(BOOL show);
int    WINAPI GetSystemMetrics(int index);

/* Messages. The queue is always empty. */
BOOL   WINAPI PeekMessageA(LPVOID msg, HANDLE h, UINT min, UINT max, UINT remove);
LONG   WINAPI DispatchMessageA(LPVOID msg);
BOOL   WINAPI TranslateMessage(LPVOID msg);
LONG   WINAPI DefWindowProcA(HANDLE h, UINT msg, UINT w, LONG l);
void   WINAPI PostQuitMessage(int code);

#endif /* VT3_WIN32_USER32_H */
