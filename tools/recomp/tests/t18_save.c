/* CreateFileA with write access and the creation dispositions, then reads of what was written.
 *
 * The harness runs the Wine build and the translated build in the same directory, so each run
 * uses file names built from GetTickCount. The two runs never see each other's files. Each failed
 * check prints "Fnn" with its number on stdout, so the harness compares which checks failed as
 * well as the exit code. The exit code is 42 when every check holds, otherwise 100 plus the number
 * of failed checks.
 *
 * Not tested here: TRUNCATE_EXISTING with read-only access. Wine opens the file without an error,
 * and the shim returns ERROR_INVALID_PARAMETER, so the two differ and the test asserts neither.
 */
#include "tcommon.h"

__declspec(dllimport) void *__stdcall CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, void *);
__declspec(dllimport) int __stdcall ReadFile(void *, void *, DWORD, DWORD *, void *);
__declspec(dllimport) DWORD __stdcall GetFileSize(void *, DWORD *);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) DWORD __stdcall GetLastError(void);

#define GENERIC_READ 0x80000000ul
#define GENERIC_WRITE 0x40000000ul
#define CREATE_NEW 1ul
#define CREATE_ALWAYS 2ul
#define OPEN_EXISTING 3ul
#define OPEN_ALWAYS 4ul
#define FILE_ATTRIBUTE_NORMAL 0x80ul
#define INVALID ((void *)0xFFFFFFFFul)

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

/* "vt3_<tag>_<tick>.bin" in out. */
static void make_name(char *out, char tag, DWORD tick)
{
    char digits[12];
    int k = 0, p = 0;
    DWORD v = tick;
    const char *head = "vt3_";
    const char *tail = ".bin";
    do {
        digits[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (*head) out[p++] = *head++;
    out[p++] = tag;
    out[p++] = '_';
    while (k) out[p++] = digits[--k];
    while (*tail) out[p++] = *tail++;
    out[p] = '\0';
}

static void *open_file(const char *name, DWORD access, DWORD disposition)
{
    return CreateFileA(name, access, 0, 0, disposition, FILE_ATTRIBUTE_NORMAL, 0);
}

int vt_main(void)
{
    DWORD n = 0;
    char buf[8];
    char fa[40], fb[40];
    void *h;

    make_name(fa, 'a', GetTickCount());
    make_name(fb, 'b', GetTickCount());

    /* 1-2: a new file gives a valid handle, ERROR_SUCCESS, and the bytes are written. */
    h = open_file(fa, GENERIC_WRITE, CREATE_ALWAYS);
    check(1, h != INVALID && GetLastError() == 0);
    if (h != INVALID) {
        check(2, WriteFile(h, "ABCDEFGH", 8, &n, 0) && n == 8);
        CloseHandle(h);
    }

    /* 3-4: CREATE_NEW on a name that exists gives no handle and ERROR_FILE_EXISTS. */
    h = open_file(fa, GENERIC_WRITE, CREATE_NEW);
    check(3, h == INVALID);
    check(4, GetLastError() == 80);

    /* 5-7: CREATE_ALWAYS on a name that exists truncates it. The error is ERROR_ALREADY_EXISTS
     * while the handle is still valid. */
    h = open_file(fa, GENERIC_WRITE, CREATE_ALWAYS);
    check(5, h != INVALID);
    check(6, GetLastError() == 183);
    if (h != INVALID) {
        check(7, GetFileSize(h, 0) == 0);
        CloseHandle(h);
    }

    /* 8-13: OPEN_ALWAYS on a new name creates it. The bytes read back through a read handle. */
    h = open_file(fb, GENERIC_WRITE, OPEN_ALWAYS);
    check(8, h != INVALID && GetLastError() == 0);
    if (h != INVALID) {
        check(9, WriteFile(h, "WXYZ", 4, &n, 0) && n == 4);
        CloseHandle(h);
    }
    h = open_file(fb, GENERIC_READ, OPEN_EXISTING);
    check(10, h != INVALID);
    if (h != INVALID) {
        check(11, ReadFile(h, buf, 4, &n, 0) && n == 4);
        check(12, buf[0] == 'W' && buf[3] == 'Z');
        check(13, GetFileSize(h, 0) == 4);
        CloseHandle(h);
    }

    /* 14-15: the directory of a new file must exist: ERROR_PATH_NOT_FOUND. */
    h = open_file("no_such_dir\\x.bin", GENERIC_WRITE, CREATE_NEW);
    check(14, h == INVALID);
    check(15, GetLastError() == 3);

    /* 18-19: OPEN_EXISTING on a missing name: ERROR_FILE_NOT_FOUND. */
    h = open_file("missing.bin", GENERIC_READ, OPEN_EXISTING);
    check(18, h == INVALID);
    check(19, GetLastError() == 2);

    return fails == 0 ? 42 : 100 + fails;
}
