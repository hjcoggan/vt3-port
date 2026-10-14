/* SetFilePointerEx, 64-bit reads and the error results that Win32 defines.
 *
 * The test opens its own executable, prog.exe, from the working directory. The harness runs
 * the Wine build and the translated build in the same directory, so both read the same bytes.
 * The result is the exit code: 42 when every check holds, otherwise 100 plus the number of
 * failed checks.
 */
#include "tcommon.h"

__declspec(dllimport) void *__stdcall CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, void *);
__declspec(dllimport) int __stdcall ReadFile(void *, void *, DWORD, DWORD *, void *);
__declspec(dllimport) int __stdcall SetFilePointerEx(void *, long long, long long *, DWORD);
__declspec(dllimport) DWORD __stdcall GetLastError(void);

#define GENERIC_READ 0x80000000ul
#define FILE_SHARE_READ 0x1ul
#define OPEN_EXISTING 3ul
#define FILE_ATTRIBUTE_NORMAL 0x80ul
#define FILE_BEGIN 0ul
#define FILE_CURRENT 1ul
#define FILE_END 2ul
#define ERROR_INVALID_HANDLE 6ul
#define ERROR_INVALID_PARAMETER 87ul
#define ERROR_NEGATIVE_SEEK 131ul

/* Reads four bytes at the current position. Returns the byte count, or -1. */
static int read4(void *h, unsigned char *b)
{
    DWORD n = 0;
    if (!ReadFile(h, b, 4, &n, 0)) return -1;
    return (int)n;
}

static unsigned long le32(const unsigned char *b)
{
    return (unsigned long)b[0] | ((unsigned long)b[1] << 8) |
           ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
}

int vt_main(void)
{
    int fails = 0;
    unsigned char b[4];
    long long pos = -1;

    void *h = CreateFileA("prog.exe", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, 0);
    if (h == (void *)0xFFFFFFFFul) return 2;

    /* e_lfanew sits at offset 0x3C of the PE image. */
    if (!SetFilePointerEx(h, 0x3C, &pos, FILE_BEGIN) || pos != 0x3C) fails++;
    if (read4(h, b) != 4) fails++;
    unsigned long lfanew = le32(b);

    /* The PE signature, "PE\0\0", is at e_lfanew. The 64-bit distance is not truncated. */
    if (!SetFilePointerEx(h, (long long)lfanew, &pos, FILE_BEGIN) || pos != (long long)lfanew) fails++;
    if (read4(h, b) != 4) fails++;
    if (le32(b) != 0x00004550ul) fails++;

    /* FILE_CURRENT: after the read the position is lfanew + 4, so going back 8 gives lfanew - 4. */
    if (!SetFilePointerEx(h, -8, &pos, FILE_CURRENT) || pos != (long long)lfanew - 4) fails++;

    /* FILE_END: a negative distance from the end is valid, and the position is non-negative. */
    if (!SetFilePointerEx(h, -4, &pos, FILE_END) || pos < 4) fails++;

    /* Before the start of the file: FALSE, ERROR_NEGATIVE_SEEK, and *new_pos is not written. */
    long long untouched = -1;
    if (SetFilePointerEx(h, -1, &untouched, FILE_BEGIN)) fails++;
    if (GetLastError() != ERROR_NEGATIVE_SEEK) fails++;
    if (untouched != -1) fails++;

    /* An unknown method is ERROR_INVALID_PARAMETER. */
    if (SetFilePointerEx(h, 0, 0, 7)) fails++;
    if (GetLastError() != ERROR_INVALID_PARAMETER) fails++;

    /* A handle that was never opened is ERROR_INVALID_HANDLE. */
    if (SetFilePointerEx((void *)0x1234, 0, 0, FILE_BEGIN)) fails++;
    if (GetLastError() != ERROR_INVALID_HANDLE) fails++;

    return fails == 0 ? 42 : 100 + fails;
}
