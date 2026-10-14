/* user32: windows and the message queue.
 *
 * A window is a fake handle that remembers its class's window procedure. CreateWindowEx,
 * ShowWindow and DestroyWindow send their messages to that procedure now, as SendMessage
 * does, in the order Windows uses. The window system is the SDL window: PeekMessage,
 * GetMessage and Present pump its events, the close, resize and focus events become WM_
 * messages in the queue, and DispatchMessage hands them to the window procedure.
 * Keyboard and mouse input stay with the input layer and are not turned into WM_ messages.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "win32/kernel32.h"
#include "win32/user32.h"

#include <ctype.h>
#include <string.h>

#define VT3_SM_CXSCREEN 0
#define VT3_SM_CYSCREEN 1
#define VT3_SCREEN_W 640
#define VT3_SCREEN_H 480
#define VT3_FAKE_HWND 0x00010000u
#define VT3_ATOM_BASE 0xC000u
#define VT3_MAX_CLASS 32
#define VT3_MAX_WND 32
#define VT3_MSG_CAP 256
#define VT3_CW_USEDEFAULT 0x80000000u
#define VT3_WS_VISIBLE 0x10000000u
#define VT3_CREATESTRUCT_BYTES 48u   /* CREATESTRUCTA */
#define VT3_RECT_BYTES 16u
#define VT3_MSG_BYTES 28u            /* MSG: hwnd, message, wParam, lParam, time, pt */

/* Window messages and constants (winuser.h). */
#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_SHOWWINDOW 0x0018u
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCCALCSIZE 0x0083u
#define WA_INACTIVE 0u
#define WA_ACTIVE 1u
#define SW_HIDE 0
#define PM_REMOVE 0x0001u
#define ERROR_CLASS_DOES_NOT_EXIST 1411u
#define ERROR_INVALID_WINDOW_HANDLE 1400u

typedef struct vt3_class {
    int used;
    char name[64];
    uint32_t atom, wndproc;
} vt3_class;

typedef struct vt3_wnd {
    int used, destroying, visible;
    uint32_t hwnd, wndproc, style, exstyle;
    int x, y, w, h;
} vt3_wnd;

typedef struct vt3_qmsg {
    uint32_t hwnd, message, wparam, lparam;
} vt3_qmsg;

static vt3_class classes[VT3_MAX_CLASS];
static vt3_wnd wnds[VT3_MAX_WND];
static vt3_qmsg queue[VT3_MSG_CAP];
static int q_head, q_count;
static uint32_t next_hwnd = VT3_FAKE_HWND;
static int logged_window;

static uint32_t mk_lparam(int lo, int hi)
{
    return ((uint32_t)hi << 16) | ((uint32_t)lo & 0xFFFFu);
}

/* Copies a guest NUL-terminated string. Returns 0 when p is not in guest memory. */
static int guest_str(GPTR p, char *dst, size_t cap)
{
    if (!p || p >= vt3_guest_size()) return 0;
    size_t room = vt3_guest_size() - p;
    if (room > cap - 1) room = cap - 1;
    const char *s = vt3_g2h(p);
    size_t n = strnlen(s, room);
    memcpy(dst, s, n);
    dst[n] = 0;
    return 1;
}

static int name_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        if (!*a) return 1;
    }
}

/* ---- window classes ----------------------------------------------------------- */

/* WNDCLASSEXA (cbSize first, 48 bytes) or WNDCLASSA (40 bytes). The window procedure is kept. */
static uint32_t register_class(GPTR wc, int ex)
{
    uint32_t proc_off = ex ? 8u : 4u;
    uint32_t name_off = ex ? 40u : 36u;
    if (!vt3s_guest_range_ok(wc, ex ? 48u : 40u)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char name[64];
    if (!guest_str(vt3s_load_u32(wc + name_off), name, sizeof name)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < VT3_MAX_CLASS; i++) {
        if (classes[i].used && name_eq(classes[i].name, name)) return classes[i].atom;
        if (!classes[i].used && slot < 0) slot = i;
    }
    if (slot < 0) {
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    vt3_class *c = &classes[slot];
    memset(c, 0, sizeof *c);
    c->used = 1;
    memcpy(c->name, name, sizeof c->name);
    c->atom = VT3_ATOM_BASE + (uint32_t)slot + 1u;
    c->wndproc = vt3s_load_u32(wc + proc_off);
    VT3_INFO("RegisterClass: \"%s\" procedure 0x%08x", name, c->wndproc);
    return c->atom;
}

ATOM WINAPI RegisterClassExA(LPVOID wndclass) { return register_class((GPTR)(uintptr_t)wndclass, 1); }
ATOM WINAPI RegisterClassA(LPVOID wndclass) { return register_class((GPTR)(uintptr_t)wndclass, 0); }

BOOL WINAPI UnregisterClassA(LPCSTR name, HANDLE inst)
{
    (void)inst;
    char buf[64];
    if (!guest_str(name, buf, sizeof buf)) return FALSE;
    for (int i = 0; i < VT3_MAX_CLASS; i++)
        if (classes[i].used && name_eq(classes[i].name, buf)) {
            memset(&classes[i], 0, sizeof classes[i]);
            return TRUE;
        }
    return FALSE;
}

/* The class an LPCSTR names: a registered name, or an atom in the low word. -1 if none. */
static int class_find(GPTR cls)
{
    if (!cls) return -1;
    if (cls < 0x10000u) {
        for (int i = 0; i < VT3_MAX_CLASS; i++)
            if (classes[i].used && classes[i].atom == cls) return i;
        return -1;
    }
    char name[64];
    if (!guest_str(cls, name, sizeof name)) return -1;
    for (int i = 0; i < VT3_MAX_CLASS; i++)
        if (classes[i].used && name_eq(classes[i].name, name)) return i;
    return -1;
}

/* ---- windows and sending messages ------------------------------------------------ */

static vt3_wnd *find_wnd(uint32_t hwnd)
{
    if (!hwnd) return NULL;
    for (int i = 0; i < VT3_MAX_WND; i++)
        if (wnds[i].used && wnds[i].hwnd == hwnd) return &wnds[i];
    return NULL;
}

static int window_slot(void)
{
    for (int i = 0; i < VT3_MAX_WND; i++)
        if (!wnds[i].used) return i;
    return -1;
}

static uint32_t main_hwnd(void)
{
    for (int i = 0; i < VT3_MAX_WND; i++)
        if (wnds[i].used && !wnds[i].destroying) return wnds[i].hwnd;
    return 0;
}

/* Sends a message to a window's procedure now, as SendMessage does. Returns the procedure's
 * result, or 0 when the window has no procedure. */
static uint32_t send_wm(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l)
{
    vt3_wnd *wd = find_wnd(hwnd);
    if (!wd || !wd->wndproc) return 0;
    uint32_t args[4] = { hwnd, msg, w, l };
    uint32_t r = vt3s_call_guest_nested(wd->wndproc, args, 4);
    VT3_DEBUG("send 0x%04x to 0x%08x: w 0x%x l 0x%x -> 0x%x", (unsigned)msg, hwnd,
              (unsigned)w, (unsigned)l, (unsigned)r);
    return r;
}

HANDLE WINAPI CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR name, DWORD style,
                              int x, int y, int w, int h, HANDLE parent, HANDLE menu,
                              HANDLE inst, LPVOID param)
{
    int ci = class_find(cls);
    if (ci < 0) {
        VT3_WARN("CreateWindowExA: class 0x%08x is not registered", (unsigned)cls);
        vt3s_set_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    int si = window_slot();
    if (si < 0) {
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    vt3_wnd *wd = &wnds[si];
    memset(wd, 0, sizeof *wd);
    wd->used = 1;
    wd->hwnd = next_hwnd += 4u;
    wd->wndproc = classes[ci].wndproc;
    wd->style = style;
    wd->exstyle = exstyle;
    wd->x = (uint32_t)x == VT3_CW_USEDEFAULT ? 0 : x;
    wd->y = (uint32_t)y == VT3_CW_USEDEFAULT ? 0 : y;
    wd->w = (uint32_t)w == VT3_CW_USEDEFAULT ? VT3_SCREEN_W : w;
    wd->h = (uint32_t)h == VT3_CW_USEDEFAULT ? VT3_SCREEN_H : h;
    uint32_t hwnd = wd->hwnd;
    if (!logged_window) {
        logged_window = 1;
        VT3_INFO("CreateWindowExA: window 0x%08x, class \"%s\"", hwnd, classes[ci].name);
    }
    if (!wd->wndproc) return hwnd;   /* no procedure: nothing to send */

    /* The creation parameters live in a guest block for the duration of the messages. */
    GPTR block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, VT3_CREATESTRUCT_BYTES + VT3_RECT_BYTES);
    if (!block) {
        memset(wd, 0, sizeof *wd);
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    GPTR cs = block, rc = block + VT3_CREATESTRUCT_BYTES;
    vt3s_store_u32(cs + 0, (uint32_t)(uintptr_t)param);
    vt3s_store_u32(cs + 4, (uint32_t)inst);
    vt3s_store_u32(cs + 8, (uint32_t)menu);
    vt3s_store_u32(cs + 12, (uint32_t)parent);
    vt3s_store_u32(cs + 16, (uint32_t)h);
    vt3s_store_u32(cs + 20, (uint32_t)w);
    vt3s_store_u32(cs + 24, (uint32_t)y);
    vt3s_store_u32(cs + 28, (uint32_t)x);
    vt3s_store_u32(cs + 32, style);
    vt3s_store_u32(cs + 36, (uint32_t)name);
    vt3s_store_u32(cs + 40, (uint32_t)cls);
    vt3s_store_u32(cs + 44, exstyle);
    vt3s_store_u32(rc + 0, 0);
    vt3s_store_u32(rc + 4, 0);
    vt3s_store_u32(rc + 8, (uint32_t)wd->w);
    vt3s_store_u32(rc + 12, (uint32_t)wd->h);

    /* The order is Windows': NCCREATE (FALSE fails the window), NCCALCSIZE, CREATE (-1 fails it),
     * then SIZE and MOVE. */
    int ok = send_wm(hwnd, WM_NCCREATE, 0, cs) != 0;
    if (ok) send_wm(hwnd, WM_NCCALCSIZE, 0, rc);
    if (ok) ok = (int32_t)send_wm(hwnd, WM_CREATE, 0, cs) != -1;
    if (ok) {
        send_wm(hwnd, WM_SIZE, 0, mk_lparam(wd->w, wd->h));
        send_wm(hwnd, WM_MOVE, 0, mk_lparam(wd->x, wd->y));
    }
    HeapFree(GetProcessHeap(), 0, block);
    if (!ok) {
        VT3_WARN("CreateWindowExA: the window procedure refused window 0x%08x", hwnd);
        vt3_wnd *dead = find_wnd(hwnd);
        if (dead) memset(dead, 0, sizeof *dead);
        vt3s_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    return hwnd;
}

BOOL WINAPI DestroyWindow(HANDLE h)
{
    vt3_wnd *wd = find_wnd(h);
    if (!wd) {
        vt3s_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    if (wd->destroying) return TRUE;
    wd->destroying = 1;
    send_wm(h, WM_DESTROY, 0, 0);
    send_wm(h, WM_NCDESTROY, 0, 0);
    vt3_wnd *dead = find_wnd(h);
    if (dead) memset(dead, 0, sizeof *dead);
    return TRUE;
}

BOOL WINAPI ShowWindow(HANDLE h, int cmd)
{
    vt3_wnd *wd = find_wnd(h);
    if (!wd) {
        vt3s_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    int was = wd->visible;
    int now = cmd != SW_HIDE;
    int cw = wd->w, ch = wd->h;
    wd->visible = now;
    if (now && !was) {
        send_wm(h, WM_SHOWWINDOW, 1, 0);
        send_wm(h, WM_SIZE, 0, mk_lparam(cw, ch));
        send_wm(h, WM_ACTIVATE, WA_ACTIVE, 0);
        send_wm(h, WM_SETFOCUS, 0, 0);
    } else if (!now && was) {
        send_wm(h, WM_SHOWWINDOW, 0, 0);
    }
    return was;
}

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
HANDLE WINAPI GetForegroundWindow(void)
{
    uint32_t h = main_hwnd();
    return h ? h : VT3_FAKE_HWND;
}
BOOL WINAPI ScreenToClient(HANDLE h, LPVOID pt) { (void)h; (void)pt; return TRUE; }
BOOL WINAPI GetCursorPos(LPVOID pt)
{
    if (vt3s_guest_range_ok((GPTR)(uintptr_t)pt, 8)) {
        vt3s_store_u32((GPTR)(uintptr_t)pt, 0);
        vt3s_store_u32((GPTR)(uintptr_t)pt + 4, 0);
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

/* ---- the message queue ------------------------------------------------------------ */

static void post(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l)
{
    if (q_count == VT3_MSG_CAP) {
        VT3_WARN("message queue full: dropped message 0x%04x", (unsigned)msg);
        return;
    }
    vt3_qmsg *m = &queue[(q_head + q_count) % VT3_MSG_CAP];
    m->hwnd = hwnd;
    m->message = msg;
    m->wparam = w;
    m->lparam = l;
    q_count++;
}

/* Takes the first queued message that matches. A zero hwnd matches any window, and zero min and
 * max match any message. */
static int take(uint32_t hwnd, UINT min, UINT max, int remove, vt3_qmsg *out)
{
    for (int i = 0; i < q_count; i++) {
        vt3_qmsg *m = &queue[(q_head + i) % VT3_MSG_CAP];
        if (hwnd && m->hwnd != hwnd) continue;
        if ((min || max) && (m->message < min || m->message > max)) continue;
        *out = *m;
        if (remove) {
            for (int j = i; j + 1 < q_count; j++)
                queue[(q_head + j) % VT3_MSG_CAP] = queue[(q_head + j + 1) % VT3_MSG_CAP];
            q_count--;
        }
        return 1;
    }
    return 0;
}

static void write_msg(GPTR p, const vt3_qmsg *m)
{
    if (!p) return;
    vt3s_store_u32(p + 0, m->hwnd);
    vt3s_store_u32(p + 4, m->message);
    vt3s_store_u32(p + 8, m->wparam);
    vt3s_store_u32(p + 12, m->lparam);
    vt3s_store_u32(p + 16, vt3s_now_ms());
    vt3s_store_u32(p + 20, 0);
    vt3s_store_u32(p + 24, 0);
}

static int read_msg(GPTR p, vt3_qmsg *m)
{
    if (!p || !vt3s_guest_range_ok(p, VT3_MSG_BYTES)) return 0;
    m->hwnd = vt3s_load_u32(p + 0);
    m->message = vt3s_load_u32(p + 4);
    m->wparam = vt3s_load_u32(p + 8);
    m->lparam = vt3s_load_u32(p + 12);
    return 1;
}

/* Pumps the SDL window and queues its events as WM_ messages for the main window. */
static void pump(void)
{
    vt3_event ev;
    vt3_platform_pump_events();
    while (vt3_platform_next_event(&ev)) {
        uint32_t h = main_hwnd();
        if (!h) continue;
        switch (ev.kind) {
        case VT3_EV_CLOSE:
            post(h, WM_CLOSE, 0, 0);
            break;
        case VT3_EV_RESIZE:
            post(h, WM_SIZE, 0, mk_lparam(ev.w, ev.h));
            break;
        case VT3_EV_FOCUS:
            post(h, WM_ACTIVATE, WA_ACTIVE, 0);
            post(h, WM_SETFOCUS, 0, 0);
            break;
        case VT3_EV_BLUR:
            post(h, WM_ACTIVATE, WA_INACTIVE, 0);
            post(h, WM_KILLFOCUS, 0, 0);
            break;
        default:
            break;
        }
    }
}

BOOL WINAPI PeekMessageA(LPVOID msg, HANDLE h, UINT min, UINT max, UINT remove)
{
    vt3s_spin_point();   /* a polling loop must let the other guest threads run */
    pump();
    vt3_qmsg m;
    if (!take(h, min, max, (remove & PM_REMOVE) != 0, &m)) return FALSE;
    write_msg((GPTR)(uintptr_t)msg, &m);
    return TRUE;
}

/* Waits for a message. Sleep releases the guest lock while it waits. Returns 0 for WM_QUIT. */
BOOL WINAPI GetMessageA(LPVOID msg, HANDLE h, UINT min, UINT max)
{
    vt3_qmsg m;
    for (;;) {
        pump();
        if (take(h, min, max, 1, &m)) break;
        Sleep(1);
    }
    write_msg((GPTR)(uintptr_t)msg, &m);
    return m.message != WM_QUIT;
}

/* Sends the message in the MSG to its window procedure. */
LONG WINAPI DispatchMessageA(LPVOID msg)
{
    vt3_qmsg m;
    if (!read_msg((GPTR)(uintptr_t)msg, &m)) return 0;
    return (LONG)send_wm(m.hwnd, m.message, m.wparam, m.lparam);
}

BOOL WINAPI TranslateMessage(LPVOID msg) { (void)msg; return FALSE; }

/* The default procedure: the parts of the default processing a game depends on. */
LONG WINAPI DefWindowProcA(HANDLE h, UINT msg, UINT w, LONG l)
{
    (void)w; (void)l;
    switch (msg) {
    case WM_NCCREATE:
        return 1;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    default:
        return 0;
    }
}

void WINAPI PostQuitMessage(int code)
{
    post(0, WM_QUIT, (uint32_t)code, 0);
}
