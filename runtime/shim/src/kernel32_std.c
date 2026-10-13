/* kernel32: console handles, process and module queries, strings, TLS, critical
 * sections, interlocked operations, and codepage conversion.
 *
 * The runtime is single-threaded. Critical sections and interlocked operations are
 * therefore plain guest-memory updates. They keep the Win32 field layout, so game
 * code that reads the fields sees the values it expects.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "win32/kernel32.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VT3_TLS_SLOTS 64
#define VT3_CP_ACP 0u
#define VT3_CP_UTF8 65001u

static HANDLE std_handles[3];          /* stdin, stdout, stderr */
static GPTR cmdline_addr;

/* Length of a guest string, bounded by the arena. */
static size_t guest_strlen(GPTR s, size_t limit)
{
    if (!vt3s_guest_range_ok(s, 1)) return 0;
    size_t room = vt3_guest_size() - s;
    if (limit && limit < room) room = limit;
    return strnlen((const char *)vt3_g2h(s), room);
}

HANDLE WINAPI GetStdHandle(DWORD n)
{
    int idx;
    int fd;
    switch (n) {
    case (DWORD)-10: idx = 0; fd = 0; break;
    case (DWORD)-11: idx = 1; fd = 1; break;
    case (DWORD)-12: idx = 2; fd = 2; break;
    default:
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (!std_handles[idx]) {
        vfile *f = calloc(1, sizeof *f);
        if (!f) return INVALID_HANDLE_VALUE;
        f->fd = fd;
        f->std = 1;
        std_handles[idx] = vt3s_handle_new(VT3S_H_FILE, f);
        if (!std_handles[idx]) {
            free(f);
            return INVALID_HANDLE_VALUE;
        }
    }
    vt3s_set_last_error(ERROR_SUCCESS);
    return std_handles[idx];
}

void WINAPI ExitProcess(UINT code)
{
    fflush(stdout);
    exit((int)code);
}

int WINAPI lstrlenA(LPCSTR s)
{
    return s ? (int)guest_strlen(s, 0) : 0;
}

LPSTR WINAPI lstrcpyA(LPSTR dst, LPCSTR src)
{
    if (!dst || !src) return dst;
    size_t n = guest_strlen(src, 0);
    if (!vt3s_guest_range_ok(dst, (DWORD)n + 1)) return dst;
    memcpy(vt3_g2h(dst), vt3_g2h(src), n);
    ((char *)vt3_g2h(dst))[n] = 0;
    return dst;
}

/* The main module is the only one. NULL means the exe, which is at its ImageBase. */
HANDLE WINAPI GetModuleHandleA(LPCSTR name)
{
    if (!name) {
        vt3s_set_last_error(ERROR_SUCCESS);
        return VT3_GUEST_IMAGE_BASE;
    }
    HANDLE h = vt3s_system_module(name);
    vt3s_set_last_error(h ? ERROR_SUCCESS : ERROR_MOD_NOT_FOUND);
    return h;
}

LPSTR WINAPI GetCommandLineA(void)
{
    if (!cmdline_addr) {
        static const char cmd[] = "vt3.exe";
        cmdline_addr = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sizeof cmd);
        if (cmdline_addr) memcpy(vt3_g2h(cmdline_addr), cmd, sizeof cmd);
    }
    return cmdline_addr;
}

DWORD WINAPI GetCurrentProcessId(void)
{
    return (DWORD)getpid();
}

LONG WINAPI InterlockedIncrement(LPLONG addend)
{
    vt3s_spin_point();
    uint32_t v = vt3s_load_u32(addend) + 1u;
    vt3s_store_u32(addend, v);
    return (LONG)v;
}

LONG WINAPI InterlockedDecrement(LPLONG addend)
{
    vt3s_spin_point();
    uint32_t v = vt3s_load_u32(addend) - 1u;
    vt3s_store_u32(addend, v);
    return (LONG)v;
}

LONG WINAPI InterlockedExchange(LPLONG target, LONG value)
{
    vt3s_spin_point();
    uint32_t old = vt3s_load_u32(target);
    vt3s_store_u32(target, (uint32_t)value);
    return (LONG)old;
}

/* Codepage conversion. ASCII and UTF-8 pass through for code points below 0x80.
 * Anything else becomes '?'. Enough for the game's ASCII paths, not a full
 * converter. */
int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCVOID wide, int wide_len,
                               LPSTR multi, int multi_len, LPCSTR defchar, LPVOID used)
{
    (void)flags; (void)defchar; (void)used;
    if (cp != VT3_CP_ACP && cp != VT3_CP_UTF8) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!wide) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!vt3s_guest_range_ok(wide, 2)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint16_t *w = vt3_g2h(wide);
    uint32_t units = 0;
    if (wide_len == -1) {
        size_t cap = (vt3_guest_size() - wide) / 2;
        while (units < cap && w[units]) units++;
        units++; /* include the terminator */
    } else {
        units = (uint32_t)wide_len;
    }
    if (!vt3s_guest_range_ok(wide, 2 * units)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (multi_len == 0 || !multi) return (int)units;
    uint32_t n = units < (uint32_t)multi_len ? units : (uint32_t)multi_len;
    if (!vt3s_guest_range_ok(multi, n)) return 0;
    uint8_t *out = vt3_g2h(multi);
    for (uint32_t i = 0; i < n; i++) out[i] = w[i] < 0x80u ? (uint8_t)w[i] : '?';
    vt3s_set_last_error(ERROR_SUCCESS);
    return (int)n;
}

int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR multi, int multi_len,
                               LPVOID wide, int wide_len)
{
    (void)flags;
    if (cp != VT3_CP_ACP && cp != VT3_CP_UTF8) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!multi) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t units;
    if (multi_len == -1) units = guest_strlen(multi, 0) + 1;
    else units = (size_t)multi_len;
    if (!vt3s_guest_range_ok(multi, (DWORD)units)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (wide_len == 0 || !wide) return (int)units;
    size_t n = units < (size_t)wide_len ? units : (size_t)wide_len;
    if (!vt3s_guest_range_ok(wide, (DWORD)(2 * n))) return 0;
    const uint8_t *in = vt3_g2h(multi);
    uint16_t *out = vt3_g2h(wide);
    for (size_t i = 0; i < n; i++) out[i] = in[i] < 0x80u ? in[i] : (uint16_t)'?';
    vt3s_set_last_error(ERROR_SUCCESS);
    return (int)n;
}
