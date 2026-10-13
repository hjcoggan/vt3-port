/* user32: window and message stubs. The runtime has no window system yet. Windows
 * are fake handles, and the message queue is always empty. The game's start-up code
 * gets the success values it checks for, and the first window creation is logged.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "win32/user32.h"

#define VT3_SM_CXSCREEN 0
#define VT3_SM_CYSCREEN 1
#define VT3_SCREEN_W 640
#define VT3_SCREEN_H 480
#define VT3_FAKE_HWND 0x00010000u

static uint32_t next_hwnd = VT3_FAKE_HWND;
static int logged_window;

ATOM WINAPI RegisterClassExA(LPVOID wndclass) { (void)wndclass; return 1; }
ATOM WINAPI RegisterClassA(LPVOID wndclass) { (void)wndclass; return 1; }
BOOL WINAPI UnregisterClassA(LPCSTR name, HANDLE inst) { (void)name; (void)inst; return TRUE; }

HANDLE WINAPI CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR name, DWORD style,
                              int x, int y, int w, int h, HANDLE parent, HANDLE menu,
                              HANDLE inst, LPVOID param)
{
    (void)exstyle; (void)cls; (void)name; (void)style; (void)x; (void)y; (void)w; (void)h;
    (void)parent; (void)menu; (void)inst; (void)param;
    if (!logged_window) {
        logged_window = 1;
        VT3_INFO("shim: CreateWindowExA returns a fake window (no window system yet)");
    }
    return next_hwnd += 4u;
}

BOOL WINAPI DestroyWindow(HANDLE h) { (void)h; return TRUE; }
BOOL WINAPI ShowWindow(HANDLE h, int cmd) { (void)h; (void)cmd; return TRUE; }
BOOL WINAPI UpdateWindow(HANDLE h) { (void)h; return TRUE; }
BOOL WINAPI SetWindowPos(HANDLE h, HANDLE after, int x, int y, int cx, int cy, UINT flags)
{
    (void)h; (void)after; (void)x; (void)y; (void)cx; (void)cy; (void)flags;
    return TRUE;
}
BOOL WINAPI AdjustWindowRectEx(LPVOID rect, DWORD style, BOOL menu, DWORD exstyle)
{
    (void)rect; (void)style; (void)menu; (void)exstyle;
    return TRUE;
}
BOOL WINAPI SetForegroundWindow(HANDLE h) { (void)h; return TRUE; }
HANDLE WINAPI FindWindowA(LPCSTR cls, LPCSTR name) { (void)cls; (void)name; return 0; }
HANDLE WINAPI GetForegroundWindow(void) { return VT3_FAKE_HWND; }
BOOL WINAPI ScreenToClient(HANDLE h, LPVOID pt) { (void)h; (void)pt; return TRUE; }
BOOL WINAPI GetCursorPos(LPVOID pt)
{
    if (vt3s_guest_range_ok(pt, 8)) {
        vt3s_store_u32(pt, 0);
        vt3s_store_u32(pt + 4, 0);
    }
    return TRUE;
}
int WINAPI ShowCursor(BOOL show) { (void)show; return 0; }

int WINAPI GetSystemMetrics(int index)
{
    if (index == VT3_SM_CXSCREEN) return VT3_SCREEN_W;
    if (index == VT3_SM_CYSCREEN) return VT3_SCREEN_H;
    return 0;
}

/* The queue is always empty, so PeekMessage never reports input. */
BOOL WINAPI PeekMessageA(LPVOID msg, HANDLE h, UINT min, UINT max, UINT remove)
{
    (void)msg; (void)h; (void)min; (void)max; (void)remove;
    return FALSE;
}

LONG WINAPI DispatchMessageA(LPVOID msg) { (void)msg; return 0; }
BOOL WINAPI TranslateMessage(LPVOID msg) { (void)msg; return FALSE; }
LONG WINAPI DefWindowProcA(HANDLE h, UINT msg, UINT w, LONG l)
{
    (void)h; (void)msg; (void)w; (void)l;
    return 0;
}
void WINAPI PostQuitMessage(int code) { (void)code; }
