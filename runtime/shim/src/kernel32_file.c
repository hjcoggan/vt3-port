/*
 * Read-only file I/O. Win32 paths map onto the game root. Matching is
 * case-insensitive at each path component, because the original data was
 * written against a case-insensitive filesystem and Android and Linux are not.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "win32/kernel32.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>


/*
 * Resolve a Win32 path (drive letter and backslashes allowed) under the game
 * root. Each component is matched exactly first, then case-insensitively.
 * Returns 0 and writes out[] on success.
 */
static int resolve_path(const char *win, char *out, size_t cap)
{
    if (strlen(win) >= 2 && isalpha((unsigned char)win[0]) && win[1] == ':') win += 2;

    char dir[PATH_MAX];
    const char *root = vt3s_game_root();
    size_t rl = strlen(root);
    if (rl > 0 && root[rl - 1] == '/') rl--;   /* dir has no trailing slash */
    if (rl + 1 >= sizeof dir) return -1;
    memcpy(dir, root, rl);
    dir[rl] = '\0';

    const char *p = win;
    while (*p == '\\' || *p == '/') p++;
    while (*p) {
        char comp[NAME_MAX + 1];
        size_t n = 0;
        while (*p && *p != '\\' && *p != '/') {
            if (n < NAME_MAX) comp[n++] = *p;
            p++;
        }
        comp[n] = '\0';
        while (*p == '\\' || *p == '/') p++;
        if (n == 0 || strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) return -1;   /* no escaping the root */

        char cand[PATH_MAX];
        int len = snprintf(cand, sizeof cand, "%s/%s", dir, comp);
        if (len < 0 || (size_t)len >= sizeof cand) return -1;

        struct stat st;
        if (stat(cand, &st) == 0) {
            memcpy(dir, cand, (size_t)len + 1);
            continue;
        }

        DIR *d = opendir(dir);
        if (!d) return -1;
        const struct dirent *de;
        int found = 0;
        while ((de = readdir(d)) != NULL) {
            if (strcasecmp(de->d_name, comp) == 0) {
                len = snprintf(cand, sizeof cand, "%s/%s", dir, de->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
        if (!found || (size_t)len >= sizeof cand) return -1;
        memcpy(dir, cand, (size_t)len + 1);
    }

    size_t dl = strlen(dir);
    if (dl + 1 > cap) return -1;
    memcpy(out, dir, dl + 1);
    return 0;
}

HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                          DWORD disposition, DWORD flags, HANDLE template_file)
{
    (void)share;
    (void)sa;
    (void)flags;
    (void)template_file;

    const char *win = (const char *)vt3_g2h(name);
    if (!win) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (access & GENERIC_WRITE) {
        vt3s_unimpl("CreateFileA(GENERIC_WRITE)");
        vt3s_set_last_error(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (disposition != OPEN_EXISTING) {
        vt3s_unimpl("CreateFileA(disposition != OPEN_EXISTING)");
        vt3s_set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
        return INVALID_HANDLE_VALUE;
    }

    char path[PATH_MAX];
    if (resolve_path(win, path, sizeof path) != 0) {
        VT3_DEBUG("CreateFileA: not found: %s", win);
        vt3s_set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        vt3s_set_last_error(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        vt3s_set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    vfile *f = calloc(1, sizeof *f);
    if (!f) {
        close(fd);
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    f->fd = fd;
    f->size = (fstat(fd, &st) == 0) ? (int64_t)st.st_size : 0;

    HANDLE h = vt3s_handle_new(VT3S_H_FILE, f);
    if (!h) {
        close(fd);
        free(f);
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    vt3s_set_last_error(ERROR_SUCCESS);
    return h;
}

BOOL WINAPI ReadFile(HANDLE hf, LPVOID buffer, DWORD bytes, LPDWORD bytes_read, LPOVERLAPPED ovl)
{
    vfile *f = vt3s_handle_get(hf, VT3S_H_FILE);
    if (!f) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (ovl) {
        vt3s_unimpl("ReadFile(overlapped)");
        vt3s_set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }
    if (bytes && !vt3s_guest_range_ok(buffer, bytes)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ssize_t r = bytes ? read(f->fd, vt3_g2h(buffer), bytes) : 0;
    if (r < 0) {
        vt3s_set_last_error((DWORD)errno);
        return FALSE;
    }
    vt3s_store_u32(bytes_read, (uint32_t)r);
    vt3s_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI WriteFile(HANDLE hf, LPCVOID buffer, DWORD bytes, LPDWORD bytes_written, LPOVERLAPPED ovl)
{
    (void)ovl;
    vfile *f = vt3s_handle_get(hf, VT3S_H_FILE);
    if (!f) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (bytes && !vt3s_guest_range_ok(buffer, bytes)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const uint8_t *src = bytes ? vt3_g2h(buffer) : NULL;
    DWORD done = 0;
    while (done < bytes) {
        ssize_t w = write(f->fd, src + done, bytes - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (DWORD)w;
    }
    if (bytes_written) vt3s_store_u32(bytes_written, done);
    vt3s_set_last_error(done == bytes ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
    return done == bytes ? TRUE : FALSE;
}

DWORD WINAPI SetFilePointer(HANDLE hf, LONG distance, LPLONG distance_high, DWORD method)
{
    vfile *f = vt3s_handle_get(hf, VT3S_H_FILE);
    if (!f) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return INVALID_SET_FILE_POINTER;
    }
    int whence;
    switch (method) {
    case FILE_BEGIN:   whence = SEEK_SET; break;
    case FILE_CURRENT: whence = SEEK_CUR; break;
    case FILE_END:     whence = SEEK_END; break;
    default:
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
    }
    int64_t off = (int64_t)(int32_t)distance;
    if (distance_high) off += (int64_t)(int32_t)vt3s_load_u32(distance_high) << 32;

    off_t pos = lseek(f->fd, (off_t)off, whence);
    if (pos < 0) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
    }
    vt3s_store_u32(distance_high, (uint32_t)((uint64_t)pos >> 32));
    vt3s_set_last_error(ERROR_SUCCESS);
    return (DWORD)((uint64_t)pos & 0xFFFFFFFFu);
}

DWORD WINAPI GetFileSize(HANDLE hf, LPDWORD size_high)
{
    vfile *f = vt3s_handle_get(hf, VT3S_H_FILE);
    if (!f) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return INVALID_SET_FILE_POINTER;
    }
    vt3s_store_u32(size_high, (uint32_t)((uint64_t)f->size >> 32));
    return (DWORD)((uint64_t)f->size & 0xFFFFFFFFu);
}

/*
 * Directory search. FindFirstFileA takes a snapshot of the directory and sorts the
 * matches case-insensitively by name, which is the order NTFS reports them in.
 * Matching follows the Win32 rules: '*' matches any run of characters, '?' matches
 * one character, and the comparison ignores case. The pattern "*.*" matches every
 * name, including names without an extension, as it does on Windows.
 *
 * Not done: 8.3 short names. cAlternateFileName is always empty, which is what
 * Windows reports for a name that is already valid 8.3. Creation time is the
 * modification time, because the host has no portable birth time.
 */
typedef struct vfind_entry {
    char name[NAME_MAX + 1];
    DWORD attrs;
    int64_t size;
    int64_t mtime;
    int64_t atime;
} vfind_entry;

typedef struct vfind {
    vfind_entry *e;
    size_t n;
    size_t pos;     /* index of the entry the next FindNextFileA returns */
} vfind;

static int wild_match(const char *pat, const char *s)
{
    if (strcmp(pat, "*.*") == 0) return 1;
    const char *star_p = NULL, *star_s = NULL;
    while (*s) {
        if (*pat == '*') {
            star_p = ++pat;
            star_s = s;
        } else if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) {
            pat++;
            s++;
        } else if (star_p) {
            pat = star_p;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*pat == '*') pat++;
    return *pat == '\0';
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const vfind_entry *)a)->name, ((const vfind_entry *)b)->name);
}

static void find_free(vfind *s)
{
    if (!s) return;
    free(s->e);
    free(s);
}

/* Snapshot the entries of dir whose names match pat. On failure returns NULL and sets *err. */
static vfind *find_scan(const char *dir, const char *pat, DWORD *err)
{
    DIR *d = opendir(dir);
    if (!d) {
        *err = ERROR_PATH_NOT_FOUND;
        return NULL;
    }
    vfind *s = calloc(1, sizeof *s);
    if (!s) {
        closedir(d);
        *err = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    size_t cap = 0;
    const struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!wild_match(pat, de->d_name)) continue;
        char full[PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", dir, de->d_name) >= (int)sizeof full) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;   /* dangling symlink: not listed */
        if (s->n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            vfind_entry *ne = realloc(s->e, ncap * sizeof *ne);
            if (!ne) {
                closedir(d);
                find_free(s);
                *err = ERROR_NOT_ENOUGH_MEMORY;
                return NULL;
            }
            s->e = ne;
            cap = ncap;
        }
        vfind_entry *e = &s->e[s->n++];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        int is_dir = S_ISDIR(st.st_mode);
        e->attrs = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (!(st.st_mode & S_IWUSR)) e->attrs |= FILE_ATTRIBUTE_READONLY;
        e->size = is_dir ? 0 : (int64_t)st.st_size;
        e->mtime = (int64_t)st.st_mtime;
        e->atime = (int64_t)st.st_atime;
    }
    closedir(d);
    if (s->n == 0) {
        find_free(s);
        *err = ERROR_FILE_NOT_FOUND;
        return NULL;
    }
    qsort(s->e, s->n, sizeof *s->e, by_name);
    return s;
}

/* FILETIME counts 100 ns ticks from 1601-01-01 UTC. Unix time starts 1970. */
static uint64_t unix_to_filetime(int64_t t)
{
    return ((uint64_t)t + 11644473600ull) * 10000000ull;
}

/* Write a WIN32_FIND_DATAA. The caller has checked that the 320 bytes are in guest memory. */
static void find_fill(GPTR out, const vfind_entry *e)
{
    uint8_t *p = vt3_g2h(out);
    memset(p, 0, WIN32_FIND_DATAA_SIZE);
    vt3s_store_u32(out + 0, e->attrs);
    vt3s_store_u64(out + 4, unix_to_filetime(e->mtime));    /* ftCreationTime */
    vt3s_store_u64(out + 12, unix_to_filetime(e->atime));   /* ftLastAccessTime */
    vt3s_store_u64(out + 20, unix_to_filetime(e->mtime));   /* ftLastWriteTime */
    vt3s_store_u32(out + 28, (uint32_t)((uint64_t)e->size >> 32));
    vt3s_store_u32(out + 32, (uint32_t)((uint64_t)e->size & 0xFFFFFFFFu));
    /* dwReserved0 and dwReserved1 stay zero. cFileName is at 44, and cAlternateFileName
     * (14 bytes) stays empty. */
    snprintf((char *)p + 44, MAX_PATH, "%s", e->name);
}

HANDLE WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
    const char *win = (const char *)vt3_g2h(pattern);
    if (!win) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (!vt3s_guest_range_ok(data, WIN32_FIND_DATAA_SIZE)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (strlen(win) >= 2 && isalpha((unsigned char)win[0]) && win[1] == ':') win += 2;

    /* Split at the last separator. The directory part is resolved; the rest is the pattern. */
    const char *last = NULL;
    for (const char *q = win; *q; q++) {
        if (*q == '\\' || *q == '/') last = q;
    }
    const char *leaf = last ? last + 1 : win;
    if (*leaf == '\0') {
        vt3s_set_last_error(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    size_t dl = last ? (size_t)(last - win) : 0;
    char dirpart[PATH_MAX];
    if (dl >= sizeof dirpart) {
        vt3s_set_last_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    memcpy(dirpart, win, dl);
    dirpart[dl] = '\0';

    char dir[PATH_MAX];
    if (resolve_path(dirpart, dir, sizeof dir) != 0) {
        VT3_DEBUG("FindFirstFileA: no directory for %s", win);
        vt3s_set_last_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    DWORD err = ERROR_FILE_NOT_FOUND;
    vfind *s = find_scan(dir, leaf, &err);
    if (!s) {
        vt3s_set_last_error(err);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = vt3s_handle_new(VT3S_H_FIND, s);
    if (!h) {
        find_free(s);
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    VT3_DEBUG("FindFirstFileA: %s: %zu match(es), first \"%s\"", win, s->n, s->e[0].name);
    find_fill(data, &s->e[0]);
    s->pos = 1;
    vt3s_set_last_error(ERROR_SUCCESS);
    return h;
}

BOOL WINAPI FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data)
{
    vfind *s = vt3s_handle_get(find, VT3S_H_FIND);
    if (!s) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!vt3s_guest_range_ok(data, WIN32_FIND_DATAA_SIZE)) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (s->pos >= s->n) {
        vt3s_set_last_error(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    find_fill(data, &s->e[s->pos++]);
    vt3s_set_last_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI FindClose(HANDLE find)
{
    vfind *s = vt3s_handle_take(find, VT3S_H_FIND);
    if (!s) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    find_free(s);
    return TRUE;
}

BOOL WINAPI CloseHandle(HANDLE h)
{
    if (vt3s_handle_get(h, VT3S_H_SYNC)) {
        free(vt3s_handle_take(h, VT3S_H_SYNC));
        return TRUE;
    }
    if (vt3s_handle_get(h, VT3S_H_THREAD)) {
        vt3s_handle_take(h, VT3S_H_THREAD);   /* the thread object outlives the handle */
        return TRUE;
    }
    if (vt3s_handle_get(h, VT3S_H_FIND)) {
        find_free(vt3s_handle_take(h, VT3S_H_FIND));
        return TRUE;
    }
    vfile *f = vt3s_handle_take(h, VT3S_H_FILE);
    if (!f) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!f->std) close(f->fd);
    free(f);
    return TRUE;
}
