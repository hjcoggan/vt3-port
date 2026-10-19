/*
 * libvt3install: the native half of the on-device installer (Installer.java).
 *
 * extractIso: reads the PC disc image straight from the file descriptor Android's file picker gives,
 * and unpacks its InstallShield cabinets (data1.hdr, data1.cab, data2.cab) into <out>/<group>/...,
 * the same tree `unshield -d data2 x data2.cab` makes on a PC. unshield reads the cabinets through
 * IO callbacks that map them onto their ISO9660 extents, so nothing is copied out of the ISO first.
 *
 * flattenPe: lays a PE file out as the guest image (byte 0 at ImageBase, SizeOfImage bytes), as
 * tools/recomp/recomp.py does for image.bin. Installer.java checks the result's SHA-256.
 */
#include "libunshield.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VDIR "/iso" /* unshield sees the disc's root directory as this path */
#define MAXF 64

typedef struct { char name[64]; int64_t off, size; } IsoFile;
typedef struct { int fd; int n; IsoFile f[MAXF]; } Iso;
typedef struct { int virt; FILE *fp; const IsoFile *e; int64_t pos; } Handle;
typedef struct { int i; struct dirent d; } VDir;

static Iso g_iso;
static volatile int g_done, g_total;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Root directory only: the cabinets sit in the disc's root. Returns the number of files found. */
static int iso_read_root(Iso *iso, int fd)
{
    uint8_t pvd[2048], *dir;
    iso->fd = fd;
    iso->n = 0;
    if (pread(fd, pvd, sizeof pvd, 16 * 2048) != sizeof pvd || memcmp(pvd + 1, "CD001", 5)) return -1;
    uint32_t lba = le32(pvd + 156 + 2), len = le32(pvd + 156 + 10);
    if (len > 1 << 20 || !(dir = malloc(len))) return -1;
    if (pread(fd, dir, len, (off_t)lba * 2048) != (ssize_t)len) { free(dir); return -1; }
    for (uint32_t o = 0; o < len && iso->n < MAXF;) {
        uint8_t rl = dir[o];
        if (!rl) { o = (o / 2048 + 1) * 2048; continue; } /* records never straddle a sector */
        uint8_t nl = dir[o + 32];
        if (!(dir[o + 25] & 2) && nl && nl < sizeof iso->f[0].name) {
            IsoFile *f = &iso->f[iso->n++];
            memcpy(f->name, dir + o + 33, nl);
            f->name[nl] = 0;
            char *semi = strchr(f->name, ';');
            if (semi) *semi = 0;
            f->off = (int64_t)le32(dir + o + 2) * 2048;
            f->size = le32(dir + o + 10);
        }
        o += rl;
    }
    free(dir);
    return iso->n;
}

static const IsoFile *iso_find(const char *path)
{
    if (strncmp(path, VDIR "/", sizeof VDIR)) return NULL;
    for (int i = 0; i < g_iso.n; i++)
        if (!strcasecmp(g_iso.f[i].name, path + sizeof VDIR)) return &g_iso.f[i];
    return NULL;
}

static void *cb_fopen(const char *name, const char *modes, void *u)
{
    (void)u;
    Handle *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    if ((h->e = iso_find(name))) { h->virt = 1; return h; }
    if ((h->fp = fopen(name, modes))) return h;
    free(h);
    return NULL;
}

static int cb_fseek(void *f, long off, int whence, void *u)
{
    (void)u;
    Handle *h = f;
    if (!h->virt) return fseek(h->fp, off, whence);
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? h->pos + off : h->e->size + off;
    if (p < 0) return -1;
    h->pos = p;
    return 0;
}

static long cb_ftell(void *f, void *u) { (void)u; Handle *h = f; return h->virt ? (long)h->pos : ftell(h->fp); }

static size_t cb_fread(void *ptr, size_t size, size_t n, void *f, void *u)
{
    (void)u;
    Handle *h = f;
    if (!h->virt) return fread(ptr, size, n, h->fp);
    if (!size || h->pos >= h->e->size) return 0;
    int64_t want = (int64_t)(size * n), left = h->e->size - h->pos;
    if (want > left) want = left;
    ssize_t got = pread(g_iso.fd, ptr, (size_t)want, h->e->off + h->pos);
    if (got <= 0) return 0;
    h->pos += got;
    return (size_t)got / size;
}

static size_t cb_fwrite(const void *ptr, size_t size, size_t n, void *f, void *u)
{
    (void)u;
    Handle *h = f;
    return h->virt ? 0 : fwrite(ptr, size, n, h->fp);
}

static int cb_fclose(void *f, void *u)
{
    (void)u;
    Handle *h = f;
    int r = h->virt ? 0 : fclose(h->fp);
    free(h);
    return r;
}

static void *cb_opendir(const char *name, void *u)
{
    (void)u;
    if (strcmp(name, VDIR)) return NULL;
    return calloc(1, sizeof(VDir));
}

static int cb_closedir(void *d, void *u) { (void)u; free(d); return 0; }

static struct dirent *cb_readdir(void *dp, void *u)
{
    (void)u;
    VDir *d = dp;
    if (d->i >= g_iso.n) return NULL;
    snprintf(d->d.d_name, sizeof d->d.d_name, "%s", g_iso.f[d->i++].name);
    return &d->d;
}

static const UnshieldIoCallbacks g_cb = {
    cb_fopen, cb_fseek, cb_ftell, cb_fread, cb_fwrite, cb_fclose, cb_opendir, cb_closedir, cb_readdir,
};

static int mkdirs(char *path) /* mkdir -p on the parent directories of path */
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int r = mkdir(path, 0755);
        *p = '/';
        if (r && errno != EEXIST) return -1;
    }
    return 0;
}

/* Output names follow the unshield CLI: <out>/<file group>/<directory>/<file>, with '\' -> '/' and
 * ' ', '<', '>', '[', ']' -> '_' after <out>, so the tree matches the one the PC build reads. */
static int save(Unshield *u, const char *out, const char *group, int i)
{
    char path[1024];
    const char *dir = unshield_file_directory(u, i) >= 0 ? unshield_directory_name(u, unshield_file_directory(u, i)) : "";
    const char *name = unshield_file_name(u, i);
    int n = snprintf(path, sizeof path, "%s/%s/%s%s", out, group, dir ? dir : "", dir && *dir ? "/" : "");
    if (n < 0 || n + strlen(name) >= sizeof path || strchr(name, '/') || strchr(name, '\\')) return -1;
    for (char *p = path + strlen(out); *p; p++) {
        if (*p == '\\') *p = '/';
        else if (strchr(" <>[]", *p) || !isprint((unsigned char)*p)) *p = '_';
        if (p[0] == '.' && p[1] == '.') return -1; /* never write outside <out> */
    }
    if (!strcmp(name, "..") || mkdirs(path)) return -1;
    strcat(path, name); /* like the CLI, the file name itself is not rewritten */
    return unshield_file_save(u, i, path) ? 0 : -1;
}

/* Returns the number of files written, or a negative error: -1 not an ISO, -2 no cabinets, -3 bad file. */
int vt3_extract_iso(int fd, const char *out)
{
    g_done = g_total = 0;
    unshield_set_log_level(UNSHIELD_LOG_LEVEL_LOWEST);
    if (iso_read_root(&g_iso, fd) < 0) return -1;
    Unshield *u = unshield_open2(VDIR "/data1.hdr", &g_cb, NULL);
    if (!u) return -2;
    for (int g = 0; g < unshield_file_group_count(u); g++) {
        UnshieldFileGroup *fg = unshield_file_group_get(u, g);
        if (fg) g_total += fg->last_file - fg->first_file + 1;
    }
    int rc = 0;
    for (int g = 0; g < unshield_file_group_count(u) && rc >= 0; g++) {
        UnshieldFileGroup *fg = unshield_file_group_get(u, g);
        for (unsigned i = fg ? fg->first_file : 1; fg && i <= fg->last_file; i++, g_done++) {
            if (!unshield_file_is_valid(u, i)) continue;
            if (save(u, out, fg->name, i)) { rc = -3; break; }
            rc++;
        }
    }
    unshield_close(u);
    return rc;
}

/* PE -> flat guest image. Returns a malloc'd buffer of *size bytes, or NULL. */
uint8_t *vt3_flatten_pe(const uint8_t *d, size_t n, size_t *size)
{
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return NULL;
    uint32_t pe = le32(d + 0x3c);
    if ((size_t)pe + 0xf8 > n || memcmp(d + pe, "PE\0\0", 4)) return NULL;
    unsigned nsec = d[pe + 6] | d[pe + 7] << 8, ohsz = d[pe + 20] | d[pe + 21] << 8;
    const uint8_t *oh = d + pe + 24;
    uint32_t img = le32(oh + 56), hdrs = le32(oh + 60), falign = le32(oh + 36);
    size_t sec = pe + 24 + ohsz;
    if (img > 64u << 20 || sec + nsec * 40 > n) return NULL;
    uint8_t *m = calloc(1, img);
    if (!m) return NULL;
    memcpy(m, d, hdrs < n && hdrs < img ? hdrs : 0);
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = d + sec + i * 40;
        uint32_t vsize = le32(s + 8), va = le32(s + 12), raw = le32(s + 16), ptr = le32(s + 20);
        if (falign >= 512) ptr &= ~511u; /* pefile rounds PointerToRawData down to 512, as Windows does */
        if (ptr >= n || va >= img) continue;
        size_t len = raw;
        if (len > n - ptr) len = n - ptr;
        if (vsize && len > vsize) len = vsize;
        if (len > img - va) len = img - va;
        memcpy(m + va, d + ptr, len);
    }
    *size = img;
    return m;
}

#ifdef __ANDROID__
#include <jni.h>

JNIEXPORT jint JNICALL Java_net_vt3port_game_Installer_extractIso(JNIEnv *env, jclass c, jint fd, jstring out)
{
    (void)c;
    const char *o = (*env)->GetStringUTFChars(env, out, NULL);
    int rc = vt3_extract_iso(fd, o);
    (*env)->ReleaseStringUTFChars(env, out, o);
    return rc;
}

JNIEXPORT jint JNICALL Java_net_vt3port_game_Installer_progress(JNIEnv *env, jclass c)
{
    (void)env, (void)c;
    return g_total ? g_done * 1000 / g_total : 0;
}

JNIEXPORT jbyteArray JNICALL Java_net_vt3port_game_Installer_flattenPe(JNIEnv *env, jclass c, jbyteArray exe)
{
    (void)c;
    jsize n = (*env)->GetArrayLength(env, exe);
    jbyte *d = (*env)->GetByteArrayElements(env, exe, NULL);
    size_t size = 0;
    uint8_t *m = vt3_flatten_pe((const uint8_t *)d, (size_t)n, &size);
    (*env)->ReleaseByteArrayElements(env, exe, d, JNI_ABORT);
    if (!m) return NULL;
    jbyteArray r = (*env)->NewByteArray(env, (jsize)size);
    if (r) (*env)->SetByteArrayRegion(env, r, 0, (jsize)size, (const jbyte *)m);
    free(m);
    return r;
}
#endif

#ifdef VT3_INSTALL_TEST
/* Host self-check: install_test ISO OUT_DIR [EXE IMAGE_OUT] */
#include <fcntl.h>
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    if (argc >= 5) {
        FILE *f = fopen(argv[3], "rb");
        static uint8_t buf[16 << 20];
        size_t n = fread(buf, 1, sizeof buf, f), sz;
        fclose(f);
        uint8_t *m = vt3_flatten_pe(buf, n, &sz);
        f = fopen(argv[4], "wb");
        fwrite(m, 1, sz, f);
        fclose(f);
        printf("image %zu bytes\n", sz);
    }
    int fd = open(argv[1], O_RDONLY);
    int rc = vt3_extract_iso(fd, argv[2]);
    printf("extracted %d (of %d)\n", rc, g_total);
    return rc < 0;
}
#endif
