/*
 * Self-test. Run as `vt3rt --selftest [PATH]`.
 *
 * Unit checks run with no game data: letterbox maths, the ring buffer, the
 * guest heap (including coalescing), and Win32 path mapping.
 *
 * PATH is optional. It can be a raw .adx file, or a CAPX .xpac archive, in
 * which case the first ADX entry is used. Pass a path outside the repo.
 * The ADX check decodes the stream, counts the frames up to the end marker,
 * and checks the duration against the header.
 *
 * Exit codes: 0 all passed, 1 a check failed, 77 unit checks passed but no
 * ADX path was given (ctest reports this as SKIPPED).
 */
#include "selftest.h"

#include "vt3/vt3_adx.h"
#include "vt3/vt3_audio.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_renderer.h"
#include "vt3shim/shim.h"
#include "win32/kernel32.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            g_failures++;                                 \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                 \
            fputc('\n', stderr);                          \
        }                                                 \
    } while (0)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- letterbox ---------------------------------------------------------- */

static void test_letterbox(void)
{
    vt3_rect r;

    /* 2x window at 640x480: an exact 2x fit. */
    r = vt3_letterbox(1280, 960, 640, 480, 4, 3, 1);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1280 && r.h == 960, "2x exact: got %d,%d %dx%d", r.x, r.y, r.w, r.h);

    /* 1920x1080, integer: box 1440x1080, s=2, image 1280x960 centred. */
    r = vt3_letterbox(1920, 1080, 640, 480, 4, 3, 1);
    CHECK(r.w == 1280 && r.h == 960 && r.x == 320 && r.y == 60, "1080p integer: got %d,%d %dx%d", r.x, r.y, r.w, r.h);

    /* Same window, fractional: box 1440x1080, centred, no bars at top or bottom. */
    r = vt3_letterbox(1920, 1080, 640, 480, 4, 3, 0);
    CHECK(r.w == 1440 && r.h == 1080 && r.x == 240 && r.y == 0, "1080p fractional: got %d,%d %dx%d", r.x, r.y, r.w, r.h);

    /* Tall window: bars top and bottom. Fractional. */
    r = vt3_letterbox(1000, 1000, 640, 480, 4, 3, 0);
    CHECK(r.w == 1000 && r.h == 750 && r.x == 0 && r.y == 125, "tall fractional: got %d,%d %dx%d", r.x, r.y, r.w, r.h);

    /* Window too small for an integer scale: falls back to fractional. */
    r = vt3_letterbox(400, 300, 640, 480, 4, 3, 1);
    CHECK(r.w == 400 && r.h == 300, "too small for integer: got %dx%d", r.w, r.h);

    /* Degenerate input gives an empty rect. */
    r = vt3_letterbox(0, 0, 640, 480, 4, 3, 0);
    CHECK(r.w == 0 && r.h == 0, "zero dst: got %dx%d", r.w, r.h);

    printf("PASS letterbox\n");
}

/* ---- ring buffer -------------------------------------------------------- */

static void test_ringbuf(void)
{
    vt3_ringbuf *rb = vt3_rb_create(10);
    CHECK(rb != NULL, "rb_create failed");
    if (!rb) return;

    uint8_t in[16], out[16];
    for (int i = 0; i < 16; i++) in[i] = (uint8_t)i;

    CHECK(vt3_rb_write(rb, in, 7) == 7, "write 7");
    CHECK(vt3_rb_read(rb, out, 4) == 4 && memcmp(out, in, 4) == 0, "read 4");
    /* 3 left in the ring, 7 free: writing 9 accepts 7 and wraps. */
    CHECK(vt3_rb_write(rb, in + 7, 9) == 7, "write 9 accepts 7");
    CHECK(vt3_rb_used(rb) == 10 && vt3_rb_space(rb) == 0, "full: used=%zu", vt3_rb_used(rb));
    CHECK(vt3_rb_read(rb, out, 10) == 10, "read 10");
    CHECK(memcmp(out, in + 4, 10) == 0, "wrapped data order");

    vt3_rb_destroy(rb);
    printf("PASS ringbuf\n");
}

/* ---- heap --------------------------------------------------------------- */

static void test_heap(void)
{
    HANDLE ph = GetProcessHeap();
    CHECK(ph != 0, "no process heap");

    GPTR a = HeapAlloc(ph, HEAP_ZERO_MEMORY, 100);
    GPTR b = HeapAlloc(ph, 0, 200);
    GPTR c = HeapAlloc(ph, 0, 300);
    CHECK(a && b && c, "alloc failed");
    CHECK(a != b && b != c && a != c, "allocations overlap");
    CHECK(a >= VT3_GUEST_HEAP_BASE, "heap address 0x%08x below heap base", a);

    const uint8_t *za = vt3_g2h(a);
    int zeroed = 1;
    for (int i = 0; i < 100; i++) zeroed &= (za[i] == 0);
    CHECK(zeroed, "HEAP_ZERO_MEMORY not zeroed");
    CHECK(HeapSize(ph, 0, a) == 100, "HeapSize %u", (unsigned)HeapSize(ph, 0, a));

    memset(vt3_g2h(a), 0xAB, 100);
    GPTR a2 = HeapReAlloc(ph, 0, a, 5000);
    CHECK(a2 != 0, "realloc grow failed");
    if (a2) {
        const uint8_t *z = vt3_g2h(a2);
        CHECK(z[0] == 0xAB && z[99] == 0xAB, "realloc lost data");
    }

    /* Freeing everything should coalesce back into one span, so a large
     * request then succeeds from the start of the heap. */
    /* On a successful realloc the old block 'a' was already freed by it. */
    GPTR first = a2 ? a2 : a;
    CHECK(HeapFree(ph, 0, first), "free first");
    CHECK(HeapFree(ph, 0, b), "free b");
    CHECK(HeapFree(ph, 0, c), "free c");
    CHECK(!HeapFree(ph, 0, c), "double free should fail");
    GPTR big = HeapAlloc(ph, 0, 3000);
    CHECK(big != 0, "coalesced alloc failed");
    CHECK(big == a, "coalesced span not reused from its start: 0x%08x (want 0x%08x)", big, a);
    HeapFree(ph, 0, big);

    printf("PASS heap\n");
}

/* ---- Win32 file path mapping ------------------------------------------- */

static GPTR guest_cstr(HANDLE heap, const char *s)
{
    size_t n = strlen(s) + 1;
    GPTR p = HeapAlloc(heap, 0, (SIZE_T)n);
    if (p) memcpy(vt3_g2h(p), s, n);
    return p;
}

static void test_file_mapping(void)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char root[512], dir[600], file[700];
    snprintf(root, sizeof root, "%s/vt3selftest.XXXXXX", tmp);
    if (!mkdtemp(root)) {
        CHECK(0, "mkdtemp: %s", strerror(errno));
        return;
    }
    snprintf(dir, sizeof dir, "%s/Data", root);
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/Data/Sub", root);
    mkdir(dir, 0700);
    snprintf(file, sizeof file, "%s/Hello.TXT", dir);
    FILE *f = fopen(file, "wb");
    CHECK(f != NULL, "write fixture");
    if (f) {
        fputs("vt3\n", f);
        fclose(f);
    }

    vt3shim_set_game_root(root);
    HANDLE ph = GetProcessHeap();
    GPTR name = guest_cstr(ph, "data\\sub\\hello.txt");   /* wrong case on purpose */
    HANDLE h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileA case-insensitive lookup failed (err %u)", (unsigned)GetLastError());
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(GetFileSize(h, 0) == 4, "GetFileSize");
        GPTR buf = HeapAlloc(ph, 0, 16);
        GPTR nread = HeapAlloc(ph, 0, 4);
        uint32_t got_bytes = 0;
        CHECK(ReadFile(h, buf, 4, nread, 0), "ReadFile failed");
        memcpy(&got_bytes, vt3_g2h(nread), 4);
        CHECK(got_bytes == 4, "ReadFile returned %u bytes", (unsigned)got_bytes);
        CHECK(memcmp(vt3_g2h(buf), "vt3\n", 4) == 0, "ReadFile contents");
        CHECK(CloseHandle(h), "CloseHandle");
        HeapFree(ph, 0, buf);
        HeapFree(ph, 0, nread);
    }
    GPTR missing = guest_cstr(ph, "data\\nope.bin");
    CHECK(CreateFileA(missing, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0) == INVALID_HANDLE_VALUE, "missing file opened");
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND, "missing file error %u", (unsigned)GetLastError());
    HeapFree(ph, 0, name);
    HeapFree(ph, 0, missing);

    unlink(file);
    snprintf(dir, sizeof dir, "%s/Data/Sub", root);
    rmdir(dir);
    snprintf(dir, sizeof dir, "%s/Data", root);
    rmdir(dir);
    rmdir(root);
    vt3shim_set_game_root(NULL);
    printf("PASS file path mapping\n");
}

/* ---- ADX ---------------------------------------------------------------- */

/* Loads the first ADX stream from a raw .adx or a CAPX archive. */
static int load_first_adx(const char *path, uint8_t **out, size_t *out_len, char *label, size_t label_cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) {
        fclose(f);
        return -1;
    }

    if (memcmp(magic, "CAPX", 4) == 0) {
        uint8_t hdr[16];
        fseek(f, 0, SEEK_SET);
        if (fread(hdr, 1, 16, f) != 16) {
            fclose(f);
            return -1;
        }
        uint32_t count = le32(hdr + 4), table = le32(hdr + 8);
        for (uint32_t i = 0; i < count; i++) {
            uint8_t rec[16];
            if (fseek(f, (long)(table + (uint64_t)i * 16), SEEK_SET) != 0) break;
            if (fread(rec, 1, 16, f) != 16) break;
            uint32_t off = le32(rec + 4), size = le32(rec + 8);
            uint8_t two[2];
            if (size < VT3_ADX_MIN_HEADER_BYTES || size > (64u << 20)) continue;
            if (fseek(f, (long)off, SEEK_SET) != 0) continue;
            if (fread(two, 1, 2, f) != 2) continue;
            if (two[0] != 0x80 || two[1] != 0x00) continue;

            uint8_t *buf = malloc(size);
            if (!buf) break;
            fseek(f, (long)off, SEEK_SET);
            if (fread(buf, 1, size, f) != size) {
                free(buf);
                break;
            }
            snprintf(label, label_cap, "CAPX entry %u", i);
            *out = buf;
            *out_len = size;
            fclose(f);
            return 0;
        }
        fclose(f);
        return -1;
    }

    /* Raw ADX file: read it all. */
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < VT3_ADX_MIN_HEADER_BYTES || n > (256L << 20)) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    snprintf(label, label_cap, "raw file");
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

static int test_adx(const char *path)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    char label[64] = "";
    if (load_first_adx(path, &buf, &len, label, sizeof label) != 0) {
        fprintf(stderr, "  FAIL: no ADX stream found in %s\n", path);
        g_failures++;
        return 0;
    }

    vt3_adx_header h;
    int rc = vt3_adx_parse_header(buf, len, &h);
    CHECK(rc == VT3_ADX_OK, "header parse: %d", rc);
    if (rc != VT3_ADX_OK) {
        free(buf);
        return 0;
    }

    uint32_t total = h.total_samples;
    size_t frame_bytes = (size_t)VT3_ADX_BLOCK_BYTES * h.channels;
    uint32_t blocks_needed = (total + VT3_ADX_SAMPLES_PER_BLOCK - 1) / VT3_ADX_SAMPLES_PER_BLOCK;
    CHECK(total > 0, "zero samples");
    CHECK(h.data_offset + (size_t)blocks_needed * frame_bytes <= len,
          "stream truncated: need %zu bytes, have %zu", h.data_offset + (size_t)blocks_needed * frame_bytes, len);

    /* Whole-buffer decode, trimmed to the header's sample count. */
    int16_t *pcm = malloc((size_t)total * h.channels * sizeof(int16_t));
    if (!pcm) {
        free(buf);
        CHECK(0, "out of memory for PCM");
        return 0;
    }
    int64_t got = vt3_adx_decode_all(buf, len, pcm, total, NULL);
    CHECK(got == (int64_t)total, "decode_all wrote %lld of %u samples", (long long)got, total);

    /* Frame walk: count frames up to the end marker, and confirm the streaming
     * decoder reproduces decode_all's output. */
    vt3_adx_decoder d;
    rc = vt3_adx_decoder_init(&d, buf, len);
    CHECK(rc == VT3_ADX_OK, "decoder init");
    uint32_t frames = 0;
    int saw_end = 0;
    int16_t tmp[VT3_ADX_SAMPLES_PER_BLOCK * VT3_ADX_MAX_CHANNELS];
    size_t pos = h.data_offset;
    size_t mismatches = 0;
    while (pos + frame_bytes <= len) {
        rc = vt3_adx_decode_frame(&d, buf + pos, tmp);
        if (rc == VT3_ADX_END_OF_STREAM) {
            saw_end = 1;
            break;
        }
        if (rc != VT3_ADX_OK) break;
        for (uint32_t s = 0; s < VT3_ADX_SAMPLES_PER_BLOCK; s++) {
            uint64_t idx = (uint64_t)frames * VT3_ADX_SAMPLES_PER_BLOCK + s;
            if (idx >= total) break;
            for (int c = 0; c < h.channels; c++) {
                if (tmp[s * h.channels + c] != pcm[idx * h.channels + c]) mismatches++;
            }
        }
        frames++;
        pos += frame_bytes;
    }
    CHECK(mismatches == 0, "streaming and whole-buffer decode differ at %zu values", mismatches);
    CHECK(frames == blocks_needed, "frames before end %u, expected %u", frames, blocks_needed);

    /* Duration: header sample count over the sample rate, cross-checked against
     * the decoded frame count. */
    double duration = vt3_adx_duration_seconds(&h);
    double from_frames = (double)frames * VT3_ADX_SAMPLES_PER_BLOCK / (double)h.sample_rate;
    CHECK(isfinite(duration) && duration > 0.0, "duration not positive");
    CHECK(fabs(duration * h.sample_rate - (double)total) < 1e-6, "duration arithmetic");
    CHECK(from_frames >= duration - 1e-9 && from_frames - duration < 32.0 / h.sample_rate + 1e-9,
          "duration %.6f s vs %.6f s from frame count", duration, from_frames);

    printf("PASS adx: %s, %d ch @ %u Hz, %u samples, duration %.6f s (%u frames, end marker %s)\n",
           label, h.channels, h.sample_rate, total, duration, frames, saw_end ? "seen" : "not seen");

    free(pcm);
    free(buf);
    return 1;
}

/* ---- coefficients ------------------------------------------------------- */

static void test_coefs(void)
{
    int32_t c[2];
    vt3_adx_compute_coefs(500, 48000, c);
    CHECK(c[0] > 7000 && c[0] < 7700, "c0 out of range: %d", c[0]);
    CHECK(c[1] < -2900 && c[1] > -3600, "c1 out of range: %d", c[1]);
    printf("PASS adx coefficients (48 kHz, 500 Hz): %d %d\n", c[0], c[1]);
}

/* ---- entry -------------------------------------------------------------- */

int vt3_selftest_run(const char *adx_path)
{
    g_failures = 0;
    if (vt3_guest_init(VT3_GUEST_DEFAULT_BYTES) != 0 || vt3shim_init() != 0) {
        fprintf(stderr, "selftest: guest/shim init failed\n");
        return 1;
    }

    test_letterbox();
    test_ringbuf();
    test_coefs();
    test_heap();
    test_file_mapping();

    int adx_ran = 0;
    if (adx_path) adx_ran = test_adx(adx_path);

    vt3shim_shutdown();
    vt3_guest_shutdown();

    if (g_failures) {
        printf("selftest: %d check(s) failed\n", g_failures);
        return 1;
    }
    if (!adx_path) {
        printf("selftest: unit checks passed; ADX skipped (no path given)\n");
        return 77;
    }
    if (!adx_ran) return 1;
    printf("selftest: all checks passed\n");
    return 0;
}
