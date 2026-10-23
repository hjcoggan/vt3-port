/* Shim init and shutdown, handle table, error state, guest memory helpers. */
#include "shim_internal.h"
#include "vt3shim/shim.h"
#include "vt3/vt3_log.h"
#include "win32/kernel32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hentry {
    vt3s_hkind kind;
    void *obj;
} hentry;

static hentry *g_handles;
static uint32_t g_handle_count;     /* slots allocated, used or not */
static __thread DWORD g_last_error;   /* per guest thread */
static char g_root[1024] = "./";

static int slot_of(HANDLE h)
{
    if (h == 0 || (h & 3u) != 0) return -1;
    uint32_t i = h / 4u - 1u;
    if (i >= g_handle_count) return -1;
    return (int)i;
}

HANDLE vt3s_handle_new(vt3s_hkind kind, void *obj)
{
    uint32_t slot = g_handle_count;
    for (uint32_t i = 0; i < g_handle_count; i++) {
        if (g_handles[i].kind == VT3S_H_NONE) {
            slot = i;
            break;
        }
    }
    if (slot == g_handle_count) {
        uint32_t cap = g_handle_count ? g_handle_count * 2 : 64;
        hentry *n = realloc(g_handles, cap * sizeof *n);
        if (!n) return 0;
        memset(n + g_handle_count, 0, (cap - g_handle_count) * sizeof *n);
        g_handles = n;
        g_handle_count = cap;
    }
    g_handles[slot].kind = kind;
    g_handles[slot].obj = obj;
    return (slot + 1) * 4u;
}

void *vt3s_handle_get(HANDLE h, vt3s_hkind kind)
{
    int s = slot_of(h);
    if (s < 0 || g_handles[s].kind != kind) return NULL;
    return g_handles[s].obj;
}

void *vt3s_handle_take(HANDLE h, vt3s_hkind kind)
{
    int s = slot_of(h);
    if (s < 0 || g_handles[s].kind != kind) return NULL;
    void *obj = g_handles[s].obj;
    g_handles[s].kind = VT3S_H_NONE;
    g_handles[s].obj = NULL;
    return obj;
}

__thread uint32_t vt3s_guest_caller;
__thread uint32_t vt3s_guest_esp;

/* Only the traces read the two above (VT3_TRACE_VIEWPORT, VT3_PIXEL_TEX, VT3_TRACE_NAN,
 * VT3_TRACE_LOCK). The generated adapters would otherwise store them on every shim call, and a
 * thread-local store on Android is a call into the emulated-TLS runtime: two per call, 2.1% of
 * in-match CPU plus 1.0% in pthread_getspecific in the profile. Off unless one of those traces
 * is asked for, so the default build pays one predictable branch instead. */
int vt3s_guest_ctx_on;

void vt3s_guest_backtrace(char *buf, size_t size)
{
    size_t pos = 0;
    int found = 0;
    if (size) buf[0] = 0;
    if (!vt3s_guest_esp) return;
    /* The image's code runs from 0x401000 to about 0x6C0000 (the data sections follow). */
    for (uint32_t i = 0; i < 256 && found < 8 && pos + 12 < size; i++) {
        uint32_t a = vt3s_guest_esp + 4u * i;
        if (!vt3s_guest_range_ok(a, 4)) break;
        uint32_t w = vt3s_load_u32(a);
        if (w < 0x00401000u || w >= 0x006C0000u) continue;
        pos += (size_t)snprintf(buf + pos, size - pos, "%s%06x", found ? " " : "", (unsigned)w);
        found++;
    }
}

/* VT3_TRACE_SLOW=<ms> read once: the threshold for the slow-hold / slow-call trace. */
int vt3s_slow_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        const char *s = getenv("VT3_TRACE_SLOW");
        ms = s ? atoi(s) : 0;
        if (ms < 0) ms = 0;
    }
    return ms;
}

void vt3s_set_last_error(DWORD err)
{
    g_last_error = err;
}

DWORD WINAPI GetLastError(void)
{
    return g_last_error;
}

void WINAPI SetLastError(DWORD error)
{
    g_last_error = error;
}

void vt3s_unimpl(const char *api)
{
    /* Each API name is logged once, so a hot path does not flood the log. */
    static const char *seen[256];
    static int nseen;
    for (int i = 0; i < nseen; i++) {
        if (strcmp(seen[i], api) == 0) return;
    }
    if (nseen < 256) seen[nseen++] = api;
    VT3_WARN("unimplemented: %s", api);
}

int vt3s_guest_range_ok(GPTR p, DWORD bytes)
{
    if (p == 0) return 0;
    uint64_t end = (uint64_t)p + bytes;
    return end <= vt3_guest_size();
}

void vt3s_store_u32(GPTR p, uint32_t v)
{
    if (!vt3s_guest_range_ok(p, 4)) return;
    memcpy(vt3_g2h(p), &v, 4);
}

void vt3s_store_u64(GPTR p, uint64_t v)
{
    if (!vt3s_guest_range_ok(p, 8)) return;
    memcpy(vt3_g2h(p), &v, 8);
}

uint32_t vt3s_load_u32(GPTR p)
{
    uint32_t v = 0;
    if (vt3s_guest_range_ok(p, 4)) memcpy(&v, vt3_g2h(p), 4);
    return v;
}

const char *vt3s_game_root(void)
{
    return g_root;
}

int vt3shim_init(void)
{
    if (vt3_guest_size() < VT3_GUEST_HEAP_BASE + (1u << 20)) {
        VT3_ERROR("shim: guest arena too small for the heap");
        return -1;
    }
    vt3s_heap_module_init();
    vt3s_display_prepare();   /* window up and a display-sized frame for the game before it runs */
    vt3s_guest_ctx_on = getenv("VT3_TRACE_VIEWPORT") || getenv("VT3_PIXEL_TEX") ||
                        getenv("VT3_TRACE_NAN") || getenv("VT3_TRACE_LOCK") ||
                        getenv("VT3_TRACE_SLOW");   /* its slow-read lines print a guest backtrace */
    VT3_INFO("shim: initialised (guest arena %zu MiB)", vt3_guest_size() >> 20);
    return 0;
}

void vt3shim_shutdown(void)
{
    vt3s_heap_module_shutdown();
    free(g_handles);
    g_handles = NULL;
    g_handle_count = 0;
}

void vt3shim_set_game_root(const char *path)
{
    if (!path || !*path) {
        strcpy(g_root, "./");
        return;
    }
    size_t n = strlen(path);
    if (n >= sizeof g_root - 1) n = sizeof g_root - 2;
    memcpy(g_root, path, n);
    /* Trailing slash so the path join is simple. */
    if (g_root[n - 1] == '/') {
        g_root[n] = '\0';
    } else {
        g_root[n] = '/';
        g_root[n + 1] = '\0';
    }
}
