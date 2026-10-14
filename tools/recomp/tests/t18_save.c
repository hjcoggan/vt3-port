/* CreateFileA with write access and the creation dispositions, then reads of what was written.
 *
 * The test works in its own directory (the harness runs both builds there), so the files it
 * creates are local to the run. The result is the exit code: 42 when every check holds,
 * otherwise 100 plus the number of failed checks.
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
#define TRUNCATE_EXISTING 5ul
#define FILE_ATTRIBUTE_NORMAL 0x80ul
#define ERROR_PATH_NOT_FOUND 3ul
#define ERROR_FILE_NOT_FOUND 2ul
#define ERROR_INVALID_PARAMETER 87ul
#define ERROR_FILE_EXISTS 80ul
#define ERROR_ALREADY_EXISTS 183ul

static void *open_file(const char *name, DWORD access, DWORD disposition)
{
    return CreateFileA(name, access, 0, 0, disposition, FILE_ATTRIBUTE_NORMAL, 0);
}

int vt_main(void)
{
    int fails = 0;
    DWORD n = 0;
    char buf[8];
    void *h;

    /* A new file: a valid handle, ERROR_SUCCESS, and the bytes written. */
    h = open_file("save_test.bin", GENERIC_WRITE, CREATE_ALWAYS);
    if (h == (void *)0xFFFFFFFFul || GetLastError() != 0) fails++;
    if (h != (void *)0xFFFFFFFFul) {
        if (!WriteFile(h, "ABCDEFGH", 8, &n, 0) || n != 8) fails++;
        CloseHandle(h);
    }

    /* CREATE_NEW on a name that exists: no handle, ERROR_FILE_EXISTS. */
    h = open_file("save_test.bin", GENERIC_WRITE, CREATE_NEW);
    if (h != (void *)0xFFFFFFFFul) fails++;
    if (GetLastError() != ERROR_FILE_EXISTS) fails++;

    /* CREATE_ALWAYS on a name that exists: the file is truncated, and the error is
     * ERROR_ALREADY_EXISTS while the handle is still valid. */
    h = open_file("save_test.bin", GENERIC_WRITE, CREATE_ALWAYS);
    if (h == (void *)0xFFFFFFFFul) fails++;
    if (GetLastError() != ERROR_ALREADY_EXISTS) fails++;
    if (h != (void *)0xFFFFFFFFul) {
        if (GetFileSize(h, 0) != 0) fails++;
        CloseHandle(h);
    }

    /* OPEN_ALWAYS on a new name creates it. Read it back through a read handle. */
    h = open_file("save_new.bin", GENERIC_WRITE, OPEN_ALWAYS);
    if (h == (void *)0xFFFFFFFFul || GetLastError() != 0) fails++;
    if (h != (void *)0xFFFFFFFFul) {
        if (!WriteFile(h, "WXYZ", 4, &n, 0) || n != 4) fails++;
        CloseHandle(h);
    }
    h = open_file("save_new.bin", GENERIC_READ, OPEN_EXISTING);
    if (h == (void *)0xFFFFFFFFul) fails++;
    if (h != (void *)0xFFFFFFFFul) {
        if (!ReadFile(h, buf, 4, &n, 0) || n != 4) fails++;
        if (buf[0] != 'W' || buf[3] != 'Z') fails++;
        if (GetFileSize(h, 0) != 4) fails++;
        CloseHandle(h);
    }

    /* The directory of a new file must exist: ERROR_PATH_NOT_FOUND. */
    h = open_file("no_such_dir\\x.bin", GENERIC_WRITE, CREATE_NEW);
    if (h != (void *)0xFFFFFFFFul) fails++;
    if (GetLastError() != ERROR_PATH_NOT_FOUND) fails++;

    /* TRUNCATE_EXISTING needs write access: ERROR_INVALID_PARAMETER. */
    h = open_file("save_test.bin", GENERIC_READ, TRUNCATE_EXISTING);
    if (h != (void *)0xFFFFFFFFul) fails++;
    if (GetLastError() != ERROR_INVALID_PARAMETER) fails++;

    /* OPEN_EXISTING on a missing name: ERROR_FILE_NOT_FOUND. */
    h = open_file("missing.bin", GENERIC_READ, OPEN_EXISTING);
    if (h != (void *)0xFFFFFFFFul) fails++;
    if (GetLastError() != ERROR_FILE_NOT_FOUND) fails++;

    return fails == 0 ? 42 : 100 + fails;
}
