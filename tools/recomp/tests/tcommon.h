/* Test support for the recompiler suite. Libc-free: the only imports are kernel32
 * functions, so the same exe runs under Wine and through the translator.
 * Output goes through WriteFile on the stdout handle.
 */
#ifndef TCOMMON_H
#define TCOMMON_H

typedef unsigned long DWORD;

__declspec(dllimport) void *__stdcall GetStdHandle(DWORD);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int) __attribute__((noreturn));
__declspec(dllimport) DWORD __stdcall GetTickCount(void);
__declspec(dllimport) void *__stdcall GetProcessHeap(void);
__declspec(dllimport) void *__stdcall HeapAlloc(void *, DWORD, unsigned long);
__declspec(dllimport) int __stdcall HeapFree(void *, DWORD, void *);

int vt_main(void);

/* Entry point: no CRT. The exe's entry symbol is __vt_start (see run_tests.py). */
void __attribute__((noreturn)) _vt_start(void)
{
    ExitProcess((unsigned int)vt_main());
}

/* Minimal libc symbols that gcc may emit for struct copies and zeroing. */
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int v, unsigned long n)
{
    unsigned char *dp = d;
    while (n--) *dp++ = (unsigned char)v;
    return d;
}

static void out_bytes(const char *s, DWORD n)
{
    DWORD w = 0;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0);
}

static void out_str(const char *s)
{
    DWORD n = 0;
    while (s[n]) n++;
    out_bytes(s, n);
}

static void out_nl(void)
{
    out_bytes("\n", 1);
}

static void out_u32(unsigned int v)
{
    char b[12];
    int i = 11;
    do {
        b[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    out_bytes(b + i, (DWORD)(11 - i));
}

static void out_i32(int v)
{
    if (v < 0) {
        out_bytes("-", 1);
        out_u32((unsigned int)0 - (unsigned int)v);
    } else {
        out_u32((unsigned int)v);
    }
}

static void out_hex32(unsigned int v)
{
    static const char hx[] = "0123456789abcdef";
    char b[8];
    for (int i = 7; i >= 0; i--) {
        b[i] = hx[v & 15u];
        v >>= 4;
    }
    out_bytes(b, 8);
}

/* Print a labelled integer on one line. */
static void kv(const char *k, int v)
{
    out_str(k);
    out_str("=");
    out_i32(v);
    out_nl();
}

static void kvx(const char *k, unsigned int v)
{
    out_str(k);
    out_str("=");
    out_hex32(v);
    out_nl();
}

#endif /* TCOMMON_H */
