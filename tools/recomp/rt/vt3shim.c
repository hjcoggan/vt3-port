/* vt3shim: kernel32 subset for the prototype. Each shim is a guest-visible
 * function reached through the icall table. Arguments are read from the guest
 * stack. A stdcall shim pops its own arguments, a cdecl one leaves them.
 */
#include "vt3rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#define ARG(c, i) rd32((c)->esp + 4u + 4u * (uint32_t)(i))
/* stdcall return: pop the return address and n argument bytes. */
#define STD_RET(c, nbytes) ((c)->esp += 4u + (uint32_t)(nbytes))

#define VT_HEAP_BASE 0x20000000u
#define VT_HEAP_LIMIT 0x40000000u
#define VT_PROCESS_HEAP 0x30000000u
#define VT_STD_IN 0x100u
#define VT_STD_OUT 0x101u
#define VT_STD_ERR 0x102u

static uint32_t heap_next = VT_HEAP_BASE;
static uint32_t cmdline_addr = 0;
static struct timeval t_start;

static uint32_t heap_alloc(uint32_t size)
{
    uint32_t p = heap_next;
    uint32_t need = ((size + 7u) & ~7u) + 8u;
    if (p + need > VT_HEAP_LIMIT || p + need < p) return 0;
    heap_next = p + need;
    memset(vt_mem + p, 0, size);
    return p;
}

void vt_shim_init(void)
{
    gettimeofday(&t_start, NULL);
    static const char cmd[] = "vt3.exe";
    cmdline_addr = heap_alloc((uint32_t)sizeof cmd);
    memcpy(vt_mem + cmdline_addr, cmd, sizeof cmd);
}

void vt_shim_fini(void) {}

/* HANDLE GetStdHandle(DWORD nStdHandle) */
static void imp_GetStdHandle(vt3_cpu *c)
{
    uint32_t n = ARG(c, 0);
    c->eax = n == (uint32_t)-10 ? VT_STD_IN : n == (uint32_t)-11 ? VT_STD_OUT :
             n == (uint32_t)-12 ? VT_STD_ERR : 0xFFFFFFFFu;
    STD_RET(c, 4);
}

/* BOOL WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED) */
static void imp_WriteFile(vt3_cpu *c)
{
    uint32_t h = ARG(c, 0), buf = ARG(c, 1), n = ARG(c, 2), pw = ARG(c, 3);
    int fd = h == VT_STD_OUT ? 1 : h == VT_STD_ERR ? 2 : -1;
    uint32_t done = 0;
    if (fd >= 0) {
        while (done < n) {
            ssize_t w = write(fd, vt_mem + buf + done, n - done);
            if (w <= 0) break;
            done += (uint32_t)w;
        }
    }
    if (pw) wr32(pw, done);
    c->eax = fd >= 0 ? 1u : 0u;
    STD_RET(c, 20);
}

/* void ExitProcess(UINT uExitCode) */
static void imp_ExitProcess(vt3_cpu *c)
{
    fflush(stdout);
    exit((int)ARG(c, 0));
}

/* DWORD GetTickCount(void) */
static void imp_GetTickCount(vt3_cpu *c)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    long ms = (now.tv_sec - t_start.tv_sec) * 1000L + (now.tv_usec - t_start.tv_usec) / 1000L;
    c->eax = (uint32_t)ms + 1000u; /* never zero, so tests can check it */
    STD_RET(c, 0);
}

/* HANDLE GetProcessHeap(void) */
static void imp_GetProcessHeap(vt3_cpu *c)
{
    c->eax = VT_PROCESS_HEAP;
    STD_RET(c, 0);
}

/* LPVOID HeapAlloc(HANDLE, DWORD flags, SIZE_T size) */
static void imp_HeapAlloc(vt3_cpu *c)
{
    c->eax = heap_alloc(ARG(c, 2));
    STD_RET(c, 12);
}

/* BOOL HeapFree(HANDLE, DWORD flags, LPVOID) */
static void imp_HeapFree(vt3_cpu *c)
{
    c->eax = 1u; /* bump allocator: frees are no-ops in the prototype */
    STD_RET(c, 12);
}

/* int lstrlenA(LPCSTR) */
static void imp_lstrlenA(vt3_cpu *c)
{
    uint32_t p = ARG(c, 0), n = 0;
    while (rd8(p + n)) n++;
    c->eax = n;
    STD_RET(c, 4);
}

/* HMODULE GetModuleHandleA(LPCSTR) */
static void imp_GetModuleHandleA(vt3_cpu *c)
{
    c->eax = 0x00400000u;
    STD_RET(c, 4);
}

/* LPSTR GetCommandLineA(void) */
static void imp_GetCommandLineA(vt3_cpu *c)
{
    c->eax = cmdline_addr;
    STD_RET(c, 0);
}

/* void Sleep(DWORD ms) */
static void imp_Sleep(vt3_cpu *c)
{
    usleep(ARG(c, 0) * 1000u);
    STD_RET(c, 4);
}

/* DWORD GetLastError(void) */
static void imp_GetLastError(vt3_cpu *c)
{
    c->eax = 0;
    STD_RET(c, 0);
}

typedef struct {
    const char *name;
    vt3_fnptr fn;
} shim_ent;

static const shim_ent shims[] = {
    {"GetStdHandle", imp_GetStdHandle},
    {"WriteFile", imp_WriteFile},
    {"ExitProcess", imp_ExitProcess},
    {"GetTickCount", imp_GetTickCount},
    {"GetProcessHeap", imp_GetProcessHeap},
    {"HeapAlloc", imp_HeapAlloc},
    {"HeapFree", imp_HeapFree},
    {"lstrlenA", imp_lstrlenA},
    {"GetModuleHandleA", imp_GetModuleHandleA},
    {"GetCommandLineA", imp_GetCommandLineA},
    {"Sleep", imp_Sleep},
    {"GetLastError", imp_GetLastError},
};

/* Import names arrive as "DLL.dll!Name" or as "Name". Match the bare name. */
vt3_fnptr vt_shim_lookup(const char *name)
{
    const char *bare = name;
    for (const char *p = name; *p; p++)
        if (*p == '!') bare = p + 1;
    for (size_t i = 0; i < sizeof shims / sizeof shims[0]; i++)
        if (strcmp(shims[i].name, bare) == 0) return shims[i].fn;
    return NULL;
}
