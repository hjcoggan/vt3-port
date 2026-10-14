/* ReadFile with an OVERLAPPED block, then GetOverlappedResult.
 *
 * The test reads its own executable, prog.exe, from the working directory. Both builds read the
 * same bytes. Each failed check prints "Fnn" on stdout. The exit code is 42 when every check holds,
 * otherwise 100 plus the number of failed checks.
 */
#include "tcommon.h"

__declspec(dllimport) void *__stdcall CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, void *);
__declspec(dllimport) int __stdcall ReadFile(void *, void *, DWORD, DWORD *, void *);
__declspec(dllimport) int __stdcall GetOverlappedResult(void *, void *, DWORD *, int);
__declspec(dllimport) int __stdcall CloseHandle(void *);

#define GENERIC_READ 0x80000000ul
#define FILE_SHARE_READ 0x1ul
#define OPEN_EXISTING 3ul
#define FILE_ATTRIBUTE_NORMAL 0x80ul
#define INVALID ((void *)0xFFFFFFFFul)

/* The 32-bit OVERLAPPED layout. */
typedef struct {
    DWORD internal;
    DWORD internal_high;
    DWORD offset;
    DWORD offset_high;
    void *event;
} ov_t;

static int fails;

/* Count a failed check and print its number. */
static void check(int id, int ok)
{
    DWORD n = 0;
    char m[4];
    if (ok) return;
    fails++;
    m[0] = 'F';
    m[1] = (char)('0' + id / 10);
    m[2] = (char)('0' + id % 10);
    m[3] = '\n';
    WriteFile(GetStdHandle((DWORD)-11), m, 4, &n, 0);
}

int vt_main(void)
{
    ov_t ov;
    DWORD n = 0;
    unsigned char b[4];
    void *h;
    unsigned long lfanew;

    h = CreateFileA("prog.exe", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    check(1, h != INVALID);

    /* 2-3: read four bytes at offset 0x3C, which holds e_lfanew. The read completes inside
     * ReadFile, and GetOverlappedResult reports four bytes. */
    memset(&ov, 0, sizeof ov);
    ov.offset = 0x3C;
    check(2, ReadFile(h, b, 4, 0, &ov) != 0);
    check(3, GetOverlappedResult(h, &ov, &n, 0) != 0 && n == 4);
    lfanew = (unsigned long)b[0] | ((unsigned long)b[1] << 8) |
             ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);

    /* 4-6: the PE signature at e_lfanew. bytes_read is given too, and it must match. */
    memset(&ov, 0, sizeof ov);
    ov.offset = lfanew;
    check(4, ReadFile(h, b, 4, &n, &ov) != 0 && n == 4);
    check(5, b[0] == 'P' && b[1] == 'E' && b[2] == 0 && b[3] == 0);
    check(6, GetOverlappedResult(h, &ov, &n, 1) != 0 && n == 4);

    CloseHandle(h);
    return fails == 0 ? 42 : 100 + fails;
}
