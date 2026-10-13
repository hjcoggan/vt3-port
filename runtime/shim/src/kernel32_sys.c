/* kernel32: system information, version and locale queries, string and case helpers,
 * virtual memory, synchronisation objects, and the thread and exception stubs that
 * the game's startup path calls.
 *
 * Single-threaded runtime. Thread creation fails with ERROR_CALL_NOT_IMPLEMENTED.
 * Wait calls return immediately. Events and mutexes are handles without state.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "win32/kernel32.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#define VT3_PAGE 4096u
#define VT3_VM_BASE 0x30000000u   /* VirtualAlloc hands out pages from here upward */
#define VT3_VM_LIMIT 0x3F000000u
#define VT3_WAIT_OBJECT_0 0u

static uint32_t vm_next = VT3_VM_BASE;

static void zero_guest(GPTR p, uint32_t n)
{
    if (vt3s_guest_range_ok(p, n)) memset(vt3_g2h(p), 0, n);
}

static void put_bytes(GPTR p, const void *src, uint32_t n)
{
    if (vt3s_guest_range_ok(p, n)) memcpy(vt3_g2h(p), src, n);
}

static size_t guest_len8(GPTR s)
{
    if (!s || !vt3s_guest_range_ok(s, 1)) return 0;
    return strnlen((const char *)vt3_g2h(s), vt3_guest_size() - s);
}

/* ---- system and version --------------------------------------------------- */

void WINAPI GetSystemTimeAsFileTime(LPVOID ft)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* FILETIME counts 100 ns ticks from 1601-01-01. Unix time starts 1970. */
    uint64_t t = (uint64_t)tv.tv_sec * 10000000ull + (uint64_t)tv.tv_usec * 10ull + 116444736000000000ull;
    vt3s_store_u64(ft, t);
}

/* SYSTEM_INFO: wProcessorArchitecture, wReserved, dwPageSize, lpMinimumApplicationAddress,
 * lpMaximumApplicationAddress, dwActiveProcessorMask, dwNumberOfProcessors,
 * dwProcessorType, dwAllocationGranularity, wProcessorLevel, wProcessorRevision. */
void WINAPI GetSystemInfo(LPVOID si)
{
    zero_guest(si, 36);
    vt3s_store_u32(si + 4, VT3_PAGE);                /* dwPageSize */
    vt3s_store_u32(si + 8, 0x00010000u);             /* lpMinimumApplicationAddress */
    vt3s_store_u32(si + 12, 0x7FFEFFFFu);            /* lpMaximumApplicationAddress */
    vt3s_store_u32(si + 16, 1u);                     /* dwActiveProcessorMask */
    vt3s_store_u32(si + 20, 1u);                     /* dwNumberOfProcessors */
    vt3s_store_u32(si + 24, 586u);                   /* dwProcessorType */
    vt3s_store_u32(si + 28, 65536u);                 /* dwAllocationGranularity */
    if (vt3s_guest_range_ok(si, 36)) {
        ((uint16_t *)vt3_g2h(si))[16] = 6;           /* wProcessorLevel (a Pentium Pro class CPU) */
    }
}

void WINAPI GetStartupInfoA(LPVOID si)
{
    zero_guest(si, 68);
    vt3s_store_u32(si, 68u); /* cb */
}

BOOL WINAPI GetVersionExA(LPVOID info)
{
    /* OSVERSIONINFOA: cb, major, minor, build, platform, szCSDVersion[128]. */
    if (!vt3s_guest_range_ok(info, 20)) return FALSE;
    vt3s_store_u32(info + 4, 5u);
    vt3s_store_u32(info + 8, 1u);
    vt3s_store_u32(info + 12, 2600u);
    vt3s_store_u32(info + 16, 2u);      /* VER_PLATFORM_WIN32_NT */
    zero_guest(info + 20, 128);
    return TRUE;
}

DWORD WINAPI GetVersion(void)
{
    /* Low byte major, next byte minor, high word build; top bit clear for NT. */
    return (2600u << 16) | (1u << 8) | 5u;
}

DWORD WINAPI GetModuleFileNameA(HANDLE module, LPSTR buf, DWORD size)
{
    (void)module;
    static const char path[] = "C:\\VT3\\VT3.exe";
    DWORD n = (DWORD)(sizeof path - 1);
    if (!buf || size == 0) return 0;
    if (n >= size) n = size - 1;
    put_bytes(buf, path, n);
    if (vt3s_guest_range_ok(buf, n + 1)) ((char *)vt3_g2h(buf))[n] = 0;
    return n;
}

UINT WINAPI GetACP(void) { return 1252u; }
UINT WINAPI GetOEMCP(void) { return 437u; }
UINT WINAPI GetConsoleOutputCP(void) { return 437u; }
UINT WINAPI GetConsoleCP(void) { return 437u; }

BOOL WINAPI GetCPInfo(UINT cp, LPVOID info)
{
    if (cp != 0 && cp != 1252u && cp != 437u) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* CPINFO: MaxCharSize, DefaultChar[2], LeadByte[12]. */
    zero_guest(info, 18);
    vt3s_store_u32(info, 1u);
    if (vt3s_guest_range_ok(info, 18)) ((uint8_t *)vt3_g2h(info))[4] = '?';
    return TRUE;
}

/* ---- character classes, case mapping, and string comparison --------------- */

#define C1_UPPER 0x0001u
#define C1_LOWER 0x0002u
#define C1_DIGIT 0x0004u
#define C1_SPACE 0x0008u
#define C1_PUNCT 0x0010u
#define C1_CNTRL 0x0020u
#define C1_BLANK 0x0040u
#define C1_XDIGIT 0x0080u
#define C1_ALPHA 0x0100u
#define CT_CTYPE1 1u

static uint16_t ctype1(uint32_t c)
{
    uint16_t f = 0;
    if (c >= 0x80) return 0;
    if (isupper((int)c)) f |= C1_UPPER | C1_ALPHA;
    if (islower((int)c)) f |= C1_LOWER | C1_ALPHA;
    if (isdigit((int)c)) f |= C1_DIGIT;
    if (isspace((int)c)) f |= C1_SPACE;
    if (ispunct((int)c)) f |= C1_PUNCT;
    if (iscntrl((int)c)) f |= C1_CNTRL;
    if (c == ' ' || c == '\t') f |= C1_BLANK;
    if (isxdigit((int)c)) f |= C1_XDIGIT;
    return f;
}

BOOL WINAPI GetStringTypeA(DWORD locale, DWORD type, LPCSTR src, int n, LPVOID out)
{
    (void)locale;
    size_t len = n < 0 ? guest_len8(src) + 1 : (size_t)n;
    if (!vt3s_guest_range_ok(src, (DWORD)len) || !vt3s_guest_range_ok(out, (DWORD)(2 * len))) return FALSE;
    const uint8_t *s = vt3_g2h(src);
    for (size_t i = 0; i < len; i++) {
        ((uint16_t *)vt3_g2h(out))[i] = type == CT_CTYPE1 ? ctype1(s[i]) : 0;
    }
    return TRUE;
}

BOOL WINAPI GetStringTypeW(DWORD type, LPCVOID src, int n, LPVOID out)
{
    size_t len = n < 0 ? 0 : (size_t)n;
    if (n < 0) {
        const uint16_t *w = vt3s_guest_range_ok(src, 2) ? vt3_g2h(src) : NULL;
        while (w && w[len]) len++;
        len++;
    }
    if (!vt3s_guest_range_ok(src, (DWORD)(2 * len)) || !vt3s_guest_range_ok(out, (DWORD)(2 * len))) return FALSE;
    const uint16_t *w = vt3_g2h(src);
    uint16_t *o = vt3_g2h(out);
    for (size_t i = 0; i < len; i++) o[i] = type == CT_CTYPE1 ? ctype1(w[i] < 0x80 ? w[i] : 0) : 0;
    return TRUE;
}

#define LCMAP_LOWERCASE 0x00000100u
#define LCMAP_UPPERCASE 0x00000200u
#define NORM_IGNORECASE 0x00000001u

int WINAPI LCMapStringA(DWORD locale, DWORD flags, LPCSTR src, int srclen, LPSTR dst, int dstlen)
{
    (void)locale;
    size_t n = srclen < 0 ? guest_len8(src) + 1 : (size_t)srclen;
    if (dstlen == 0 || !dst) return (int)n;
    if (n > (size_t)dstlen) n = (size_t)dstlen;
    if (!vt3s_guest_range_ok(src, (DWORD)n) || !vt3s_guest_range_ok(dst, (DWORD)n)) return 0;
    const uint8_t *s = vt3_g2h(src);
    uint8_t *d = vt3_g2h(dst);
    for (size_t i = 0; i < n; i++) {
        int c = s[i];
        if (flags & LCMAP_LOWERCASE) c = tolower(c);
        if (flags & LCMAP_UPPERCASE) c = toupper(c);
        d[i] = (uint8_t)c;
    }
    return (int)n;
}

int WINAPI LCMapStringW(DWORD locale, DWORD flags, LPCVOID src, int srclen, LPVOID dst, int dstlen)
{
    (void)locale;
    size_t n = srclen < 0 ? 0 : (size_t)srclen;
    if (srclen < 0) {
        const uint16_t *w = vt3s_guest_range_ok(src, 2) ? vt3_g2h(src) : NULL;
        while (w && w[n]) n++;
        n++;
    }
    if (dstlen == 0 || !dst) return (int)n;
    if (n > (size_t)dstlen) n = (size_t)dstlen;
    if (!vt3s_guest_range_ok(src, (DWORD)(2 * n)) || !vt3s_guest_range_ok(dst, (DWORD)(2 * n))) return 0;
    const uint16_t *s = vt3_g2h(src);
    uint16_t *d = vt3_g2h(dst);
    for (size_t i = 0; i < n; i++) {
        int c = s[i];
        if (flags & LCMAP_LOWERCASE) c = tolower(c);
        if (flags & LCMAP_UPPERCASE) c = toupper(c);
        d[i] = (uint16_t)c;
    }
    return (int)n;
}

/* Returns CSTR_LESS_THAN (1), CSTR_EQUAL (2), or CSTR_GREATER_THAN (3). */
static int cmp_units(const uint16_t *a, size_t na, const uint16_t *b, size_t nb, int ic)
{
    size_t n = na < nb ? na : nb;
    for (size_t i = 0; i < n; i++) {
        int x = a[i], y = b[i];
        if (ic) { x = tolower(x); y = tolower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : (na < nb ? 1 : 3);
}

int WINAPI CompareStringA(DWORD locale, DWORD flags, LPCSTR s1, int n1, LPCSTR s2, int n2)
{
    (void)locale;
    size_t a = n1 < 0 ? guest_len8(s1) : (size_t)n1;
    size_t b = n2 < 0 ? guest_len8(s2) : (size_t)n2;
    if (!vt3s_guest_range_ok(s1, (DWORD)a) || !vt3s_guest_range_ok(s2, (DWORD)b)) return 0;
    uint16_t *x = malloc(2 * (a + 1)), *y = malloc(2 * (b + 1));
    if (!x || !y) { free(x); free(y); return 0; }
    for (size_t i = 0; i < a; i++) x[i] = ((const uint8_t *)vt3_g2h(s1))[i];
    for (size_t i = 0; i < b; i++) y[i] = ((const uint8_t *)vt3_g2h(s2))[i];
    int r = cmp_units(x, a, y, b, (flags & NORM_IGNORECASE) != 0);
    free(x);
    free(y);
    return r;
}

int WINAPI CompareStringW(DWORD locale, DWORD flags, LPCVOID s1, int n1, LPCVOID s2, int n2)
{
    (void)locale;
    size_t a = n1 < 0 ? 0 : (size_t)n1, b = n2 < 0 ? 0 : (size_t)n2;
    if (n1 < 0) { const uint16_t *w = vt3_g2h(s1); while (w[a]) a++; }
    if (n2 < 0) { const uint16_t *w = vt3_g2h(s2); while (w[b]) b++; }
    if (!vt3s_guest_range_ok(s1, (DWORD)(2 * a)) || !vt3s_guest_range_ok(s2, (DWORD)(2 * b))) return 0;
    return cmp_units(vt3_g2h(s1), a, vt3_g2h(s2), b, (flags & NORM_IGNORECASE) != 0);
}

BOOL WINAPI GetLocaleInfoA(DWORD locale, DWORD type, LPSTR buf, int n)
{
    (void)locale; (void)type;
    if (!buf || n < 2) return 0;
    put_bytes(buf, "C", 2);
    return 2;
}

/* ---- handles and files ---------------------------------------------------- */

UINT WINAPI SetHandleCount(UINT n) { return n; }

DWORD WINAPI GetFileType(HANDLE h)
{
    vfile *f = vt3s_handle_get(h, VT3S_H_FILE);
    if (!f) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    return f->std ? 2u : 1u; /* FILE_TYPE_CHAR for the console, FILE_TYPE_DISK otherwise */
}

BOOL WINAPI FlushFileBuffers(HANDLE h)
{
    return vt3s_handle_get(h, VT3S_H_FILE) ? TRUE : FALSE;
}

BOOL WINAPI GetFileSizeEx(HANDLE h, LPLONGLONG size)
{
    DWORD low = GetFileSize(h, 0);   /* the high word is not reported: files are under 4 GiB */
    if (low == 0xFFFFFFFFu) return FALSE;
    vt3s_store_u64(size, low);
    return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR dir, LPVOID free_avail, LPVOID total, LPVOID total_free)
{
    (void)dir;
    const uint64_t big = 4ull << 30; /* 4 GiB, enough for the installer's checks */
    if (free_avail) vt3s_store_u64(free_avail, big);
    if (total) vt3s_store_u64(total, big);
    if (total_free) vt3s_store_u64(total_free, big);
    return TRUE;
}

DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD len, LPSTR buf, LPVOID filepart)
{
    size_t n = guest_len8(name);
    if (filepart) vt3s_store_u32(filepart, 0);
    if (!buf || len == 0) return (DWORD)n + 1;
    if (n >= len) return (DWORD)n + 1;
    put_bytes(buf, vt3_g2h(name), (DWORD)n + 1);
    return (DWORD)n;
}

BOOL WINAPI WriteConsoleA(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPVOID reserved)
{
    (void)reserved;
    return WriteFile(h, buf, n, written, 0);
}

/* ---- environment and time zone -------------------------------------------- */

LPVOID WINAPI GetEnvironmentStringsA(void)
{
    LPVOID p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 4);
    return p;
}

LPVOID WINAPI GetEnvironmentStringsW(void)
{
    LPVOID p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 4);
    return p;
}

BOOL WINAPI FreeEnvironmentStringsA(LPVOID block) { (void)block; return TRUE; }
BOOL WINAPI FreeEnvironmentStringsW(LPVOID block) { (void)block; return TRUE; }

BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    (void)name; (void)value;
    return TRUE;
}

DWORD WINAPI GetTimeZoneInformation(LPVOID tz)
{
    zero_guest(tz, 172);
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}

/* ---- memory --------------------------------------------------------------- */

#define VT3_MEM_COMMIT 0x1000u
#define VT3_MEM_RESERVE 0x2000u

LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
    (void)addr; (void)protect;
    if (!(type & (VT3_MEM_COMMIT | VT3_MEM_RESERVE)) || size == 0) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint32_t bytes = (size + VT3_PAGE - 1) & ~(VT3_PAGE - 1);
    if (vm_next + bytes > VT3_VM_LIMIT || vm_next + bytes < vm_next) {
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    uint32_t p = vm_next;
    vm_next += bytes;
    memset(vt3_g2h(p), 0, bytes); /* the arena is zero-filled, but not on reuse */
    return p;
}

BOOL WINAPI VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    (void)addr; (void)size; (void)type;
    return TRUE; /* pages are not returned to the arena */
}

SIZE_T WINAPI VirtualQuery(LPVOID addr, LPVOID info, SIZE_T len)
{
    (void)addr;
    if (len < 28) return 0;
    zero_guest(info, 28); /* MEMORY_BASIC_INFORMATION, reporting nothing committed */
    return 28;
}

/* ---- synchronisation ------------------------------------------------------ */

static HANDLE new_sync_handle(void)
{
    static int dummy;
    return vt3s_handle_new(VT3S_H_SYNC, &dummy);
}

HANDLE WINAPI CreateEventA(LPVOID attrs, BOOL manual, BOOL initial, LPCSTR name)
{
    (void)attrs; (void)manual; (void)initial; (void)name;
    return new_sync_handle();
}

HANDLE WINAPI CreateMutexA(LPVOID attrs, BOOL owner, LPCSTR name)
{
    (void)attrs; (void)owner; (void)name;
    return new_sync_handle();
}

BOOL WINAPI SetEvent(HANDLE h) { return vt3s_handle_get(h, VT3S_H_SYNC) ? TRUE : FALSE; }
BOOL WINAPI ResetEvent(HANDLE h) { return vt3s_handle_get(h, VT3S_H_SYNC) ? TRUE : FALSE; }
BOOL WINAPI PulseEvent(HANDLE h) { return vt3s_handle_get(h, VT3S_H_SYNC) ? TRUE : FALSE; }
BOOL WINAPI ReleaseMutex(HANDLE h) { return vt3s_handle_get(h, VT3S_H_SYNC) ? TRUE : FALSE; }

/* Single thread: nothing else can signal, so a wait returns at once as signalled. */
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms)
{
    (void)ms;
    if (!vt3s_handle_get(h, VT3S_H_SYNC)) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu; /* WAIT_FAILED */
    }
    return VT3_WAIT_OBJECT_0;
}

/* ---- threads and processes ------------------------------------------------ */

HANDLE WINAPI CreateThread(LPVOID attrs, SIZE_T stack, LPVOID start, LPVOID param, DWORD flags, LPDWORD tid)
{
    (void)attrs; (void)stack; (void)start; (void)param; (void)flags;
    if (tid) vt3s_store_u32(tid, 0);
    vt3s_set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    VT3_ERROR("shim: CreateThread is not implemented: the runtime is single-threaded");
    return 0;
}

void WINAPI ExitThread(DWORD code)
{
    fflush(stdout);
    exit((int)code);
}

BOOL WINAPI GetExitCodeThread(HANDLE h, LPDWORD code)
{
    (void)h;
    if (code) vt3s_store_u32(code, 0);
    return FALSE;
}

DWORD WINAPI ResumeThread(HANDLE h) { (void)h; return 0; }
DWORD WINAPI SuspendThread(HANDLE h) { (void)h; return 0; }
BOOL WINAPI SetThreadPriority(HANDLE h, int prio) { (void)h; (void)prio; return TRUE; }
int WINAPI GetThreadPriority(HANDLE h) { (void)h; return 0; }
DWORD WINAPI SetThreadIdealProcessor(HANDLE h, DWORD cpu) { (void)h; (void)cpu; return 0; }

HANDLE WINAPI GetCurrentProcess(void) { return 0xFFFFFFFFu; }

BOOL WINAPI TerminateProcess(HANDLE h, UINT code)
{
    (void)h;
    fflush(stdout);
    exit((int)code);
}

void WINAPI DebugBreak(void) {}

BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }

void WINAPI OutputDebugStringA(LPCSTR s)
{
    size_t n = guest_len8(s);
    if (s && vt3s_guest_range_ok(s, (DWORD)n)) fprintf(stderr, "%.*s", (int)n, (const char *)vt3_g2h(s));
}

void WINAPI RaiseException(DWORD code, DWORD flags, DWORD nargs, LPVOID args)
{
    (void)flags; (void)nargs; (void)args;
    fflush(stdout);
    fprintf(stderr, "vt3: RaiseException 0x%08x: the runtime has no structured exception support\n", code);
    exit(94);
}

LPVOID WINAPI SetUnhandledExceptionFilter(LPVOID filter) { (void)filter; return 0; }
LONG WINAPI UnhandledExceptionFilter(LPVOID info) { (void)info; return 0; /* EXCEPTION_CONTINUE_SEARCH */ }

/* RtlUnwind and the SEH walk are not implemented. Calls return without unwinding. */
void WINAPI RtlUnwind(LPVOID target, LPVOID ip, LPVOID rec, LPVOID value)
{
    (void)target; (void)ip; (void)rec; (void)value;
}

/* ---- strings -------------------------------------------------------------- */

LPSTR WINAPI lstrcatA(LPSTR dst, LPCSTR src)
{
    size_t d = guest_len8(dst), s = guest_len8(src);
    if (!vt3s_guest_range_ok(dst, (DWORD)(d + s + 1))) return dst;
    memcpy((char *)vt3_g2h(dst) + d, vt3_g2h(src), s + 1);
    return dst;
}

int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b)
{
    size_t na = guest_len8(a), nb = guest_len8(b);
    size_t n = na < nb ? na : nb;
    for (size_t i = 0; i < n; i++) {
        int x = tolower(((const uint8_t *)vt3_g2h(a))[i]);
        int y = tolower(((const uint8_t *)vt3_g2h(b))[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return na == nb ? 0 : (na < nb ? -1 : 1);
}

LPSTR WINAPI lstrcpynA(LPSTR dst, LPCSTR src, int n)
{
    if (n <= 0 || !dst) return dst;
    size_t s = guest_len8(src);
    size_t c = s < (size_t)(n - 1) ? s : (size_t)(n - 1);
    if (!vt3s_guest_range_ok(dst, (DWORD)c + 1)) return dst;
    memcpy(vt3_g2h(dst), vt3_g2h(src), c);
    ((char *)vt3_g2h(dst))[c] = 0;
    return dst;
}

/* ---- libraries ------------------------------------------------------------ */

/* System DLLs count as loaded, so the CRT and the game get nonzero module handles.
 * Handles are opaque. Nothing is loaded from disk. mscoree.dll stays absent: the
 * CRT uses its absence to decide the process is not managed code. */
static const char *const system_dlls[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll",
    "shell32.dll", "comdlg32.dll", "d3d9.dll", "d3dx9_32.dll", "dsound.dll", "winmm.dll",
    "dinput8.dll", "msvcrt.dll", "ntdll.dll", "version.dll", "ws2_32.dll", "wsock32.dll",
    "imm32.dll", NULL
};

HANDLE vt3s_system_module(LPCSTR name)
{
    if (!name) return 0;
    size_t n = guest_len8(name);
    if (!vt3s_guest_range_ok(name, (DWORD)n)) return 0;
    const char *s = vt3_g2h(name);
    for (uint32_t i = 0; system_dlls[i]; i++) {
        if (strlen(system_dlls[i]) == n && strncasecmp(system_dlls[i], s, n) == 0)
            return 0x7E000000u + 0x10000u * (i + 1);
    }
    return 0;
}

HANDLE WINAPI LoadLibraryA(LPCSTR name)
{
    HANDLE h = vt3s_system_module(name);
    vt3s_set_last_error(h ? ERROR_SUCCESS : ERROR_MOD_NOT_FOUND);
    return h;
}

LPVOID WINAPI GetProcAddress(HANDLE module, LPCSTR name)
{
    (void)module; (void)name;
    vt3s_set_last_error(127u); /* ERROR_PROC_NOT_FOUND */
    return 0;
}
