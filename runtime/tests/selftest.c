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
#include "vt3/vt3_input.h"
#include "vt3/vt3_renderer.h"
#include "vt3shim/shim.h"
#include "win32/dinput.h"
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

/* The shim's DXT decoder (shim/src/d3d9_format.c). BC1 colour 0 holds red in its top five bits. */
int d3d9_to_bgra(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst);

static void test_dxt_colour(void)
{
    /* DXT1 4x4 blocks: colour 0 = pure red (0xF800), colour 1 = pure blue (0x001F), 0 > 1 so four colours.
     * Index word 0 selects colour 0 everywhere; 0x55555555 selects colour 1 everywhere. */
    static const uint8_t red_blk[8] = { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t blue_blk[8] = { 0x00, 0xF8, 0x1F, 0x00, 0x55, 0x55, 0x55, 0x55 };
    uint8_t bgra[4 * 4 * 4];
    CHECK(d3d9_to_bgra(0x31545844u, red_blk, 8, 4, 4, bgra) == 0, "DXT1 red block: decode failed");
    for (int p = 0; p < 16; p++) {
        const uint8_t *px = bgra + 4 * p;
        CHECK(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255,
              "DXT1 red pixel %d decoded as B,G,R,A %u,%u,%u,%u", p, px[0], px[1], px[2], px[3]);
    }
    CHECK(d3d9_to_bgra(0x31545844u, blue_blk, 8, 4, 4, bgra) == 0, "DXT1 blue block: decode failed");
    CHECK(bgra[0] == 255 && bgra[1] == 0 && bgra[2] == 0 && bgra[3] == 255,
          "DXT1 blue pixel decoded as B,G,R,A %u,%u,%u,%u", bgra[0], bgra[1], bgra[2], bgra[3]);
}

static void test_coefs(void)
{
    int32_t c[2];
    vt3_adx_compute_coefs(500, 48000, c);
    CHECK(c[0] > 7000 && c[0] < 7700, "c0 out of range: %d", c[0]);
    CHECK(c[1] < -2900 && c[1] > -3600, "c1 out of range: %d", c[1]);
    printf("PASS adx coefficients (48 kHz, 500 Hz): %d %d\n", c[0], c[1]);
}

/* ---- DirectInput8 Xbox 360 layout --------------------------------------- */

static uint32_t g_load32(GPTR p)
{
    uint32_t v;
    memcpy(&v, vt3_g2h(p), 4);
    return v;
}

static void g_store32(GPTR p, uint32_t v)
{
    memcpy(vt3_g2h(p), &v, 4);
}

static void test_dinput8_xbox360_controls(void)
{
    HANDLE ph = GetProcessHeap();
    GPTR iid_di8  = HeapAlloc(ph, HEAP_ZERO_MEMORY, 16);
    GPTR guid_kb  = HeapAlloc(ph, HEAP_ZERO_MEMORY, 16);
    GPTR guid_pad = HeapAlloc(ph, HEAP_ZERO_MEMORY, 16);
    GPTR out_ptr  = HeapAlloc(ph, HEAP_ZERO_MEMORY, 4);
    GPTR df_kb    = HeapAlloc(ph, HEAP_ZERO_MEMORY, 24);
    GPTR odf_kb   = HeapAlloc(ph, HEAP_ZERO_MEMORY, 256 * 16);
    GPTR df_pad   = HeapAlloc(ph, HEAP_ZERO_MEMORY, 24);
    GPTR odf_pad  = HeapAlloc(ph, HEAP_ZERO_MEMORY, 20 * 16);
    GPTR prop_rng = HeapAlloc(ph, HEAP_ZERO_MEMORY, 24);
    GPTR state_kb = HeapAlloc(ph, HEAP_ZERO_MEMORY, 256);
    GPTR state_pad= HeapAlloc(ph, HEAP_ZERO_MEMORY, 80);

    static const uint8_t k_iid_di8[16]  = { 0x30,0x80,0x79,0xBF, 0x3A,0x48, 0xA2,0x4D, 0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00 };
    static const uint8_t k_guid_kb[16]  = { 0x61,0x2B,0x1D,0x6F, 0xA0,0xD5, 0xCF,0x11, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
    static const uint8_t k_guid_pad[16] = { 0x70,0x2B,0x1D,0x6F, 0xA0,0xD5, 0xCF,0x11, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 };
    memcpy(vt3_g2h(iid_di8),  k_iid_di8,  16);
    memcpy(vt3_g2h(guid_kb),  k_guid_kb,  16);
    memcpy(vt3_g2h(guid_pad), k_guid_pad, 16);

    CHECK(DirectInput8Create(0, 0x0800, iid_di8, out_ptr, 0) == 0, "DirectInput8Create failed");
    GPTR di = g_load32(out_ptr);
    CHECK(di != 0, "DirectInput8Create returned null");

    CHECK(IDirectInput8A_CreateDevice(di, guid_kb, out_ptr, 0) == 0, "CreateDevice(Keyboard) failed");
    GPTR dev_kb = g_load32(out_ptr);
    CHECK(IDirectInput8A_CreateDevice(di, guid_pad, out_ptr, 0) == 0, "CreateDevice(Gamepad) failed");
    GPTR dev_pad = g_load32(out_ptr);

    /* Configure c_dfDIKeyboard (256 buttons at offsets 0..255). */
    g_store32(df_kb + 0, 24);
    g_store32(df_kb + 4, 16);
    g_store32(df_kb + 8, 2);
    g_store32(df_kb + 12, 256);
    g_store32(df_kb + 16, 256);
    g_store32(df_kb + 20, odf_kb);
    for (uint32_t i = 0; i < 256; i++) {
        g_store32(odf_kb + i * 16 + 4, i);
        g_store32(odf_kb + i * 16 + 8, 0x0000000Cu); /* DIDFT_BUTTON */
    }
    CHECK(IDirectInputDevice8A_SetDataFormat(dev_kb, df_kb) == 0, "SetDataFormat(kb)");
    CHECK(IDirectInputDevice8A_Acquire(dev_kb) == 0, "Acquire(kb)");

    /* Configure DIJOYSTATE: 8 axes (0..28), 1 POV (32), 11 buttons (48..58). */
    g_store32(df_pad + 0, 24);
    g_store32(df_pad + 4, 16);
    g_store32(df_pad + 8, 1);
    g_store32(df_pad + 12, 80);
    g_store32(df_pad + 16, 20);
    g_store32(df_pad + 20, odf_pad);
    for (uint32_t i = 0; i < 8; i++) {
        g_store32(odf_pad + i * 16 + 4, i * 4);
        g_store32(odf_pad + i * 16 + 8, 0x00000003u); /* DIDFT_AXIS */
    }
    g_store32(odf_pad + 8 * 16 + 4, 32);
    g_store32(odf_pad + 8 * 16 + 8, 0x00000010u);     /* DIDFT_POV */
    for (uint32_t i = 0; i < 11; i++) {
        g_store32(odf_pad + (9 + i) * 16 + 4, 48 + i);
        g_store32(odf_pad + (9 + i) * 16 + 8, 0x0000000Cu); /* DIDFT_BUTTON */
    }
    CHECK(IDirectInputDevice8A_SetDataFormat(dev_pad, df_pad) == 0, "SetDataFormat(pad)");
    /* Set DIPROP_RANGE to -32767..32767, as VT3.exe's EnumObjects callback (0x00401180) does. */
    g_store32(prop_rng + 0, 24);
    g_store32(prop_rng + 4, 16);
    g_store32(prop_rng + 16, (uint32_t)(-32767));
    g_store32(prop_rng + 20, 32767);
    CHECK(IDirectInputDevice8A_SetProperty(dev_pad, 4, prop_rng) == 0, "SetProperty(DIPROP_RANGE)");
    CHECK(IDirectInputDevice8A_Acquire(dev_pad) == 0, "Acquire(pad)");

    const uint8_t *pad_buf = vt3_g2h(state_pad);
    const uint8_t *kb_buf  = vt3_g2h(state_kb);

    /* Verify VT3.exe's FUN_004018c0 & FUN_00476300/FUN_004763c0 with default pad table [12,13,14,15,2,0,3,1,4,7]
     * against the official Xbox 360 controls:
     *   A (VT3_BTN_A) -> Button 0 (offset 48, slot 5 = 0x85605a -> raw 0x40000): Top Spin / Menu Select / World Tour Open
     *   B (VT3_BTN_B) -> Button 2 (offset 50, slot 4 = 0x856058 -> raw 0x100000): Slice / Drop Shot / Menu Cancel
     *   X (VT3_BTN_X) -> Button 2 (offset 50, slot 4 = 0x856058 -> raw 0x100000): Slice / Drop Shot / Menu Cancel
     *   Y (VT3_BTN_Y) -> Button 3 (offset 51, slot 6 = 0x85605c -> raw 0x200000): Lob / World Tour Return Home
     *   LB/RB/LT/RT   -> Button 4 (offset 52, slot 8 = 0x856060 -> raw 0x20000000): Instruct COM Partner / Incoming Mail
     *   RB/RT         -> DIK_LMENU (0x38 -> raw 0x800 -> action 0x400000): World Tour Tour Calendar
     *   Start         -> Button 7 (offset 55, slot 9 = 0x856062 -> raw 0x20): Pause / Open Main Menu
     *   Back          -> Button 6 (offset 54) AND DIK_V (0x2F -> raw 0x2000): Change Camera View
     *   Button 1 (offset 49, slot 7 = 0x85605e -> raw 0x10 = Pause match) must remain 0 for all face/shoulder buttons.
     */
    vt3_input_init();

    vt3_input_set_pad_button(VT3_BTN_A, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    CHECK(pad_buf[48 + 0] == 0x80 && pad_buf[48 + 1] == 0 && pad_buf[48 + 2] == 0 && pad_buf[48 + 3] == 0,
          "Xbox 360 A must map to DI button 0 (Top Spin / Select)");
    vt3_input_set_pad_button(VT3_BTN_A, 0);

    vt3_input_set_pad_button(VT3_BTN_B, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    CHECK(pad_buf[48 + 2] == 0x80 && pad_buf[48 + 0] == 0 && pad_buf[48 + 1] == 0,
          "Xbox 360 B must map to DI button 2 (Slice / Cancel)");
    vt3_input_set_pad_button(VT3_BTN_B, 0);

    vt3_input_set_pad_button(VT3_BTN_X, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    CHECK(pad_buf[48 + 2] == 0x80 && pad_buf[48 + 0] == 0 && pad_buf[48 + 1] == 0,
          "Xbox 360 X must map to DI button 2 (Slice / Cancel)");
    vt3_input_set_pad_button(VT3_BTN_X, 0);

    vt3_input_set_pad_button(VT3_BTN_Y, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    CHECK(pad_buf[48 + 3] == 0x80 && pad_buf[48 + 0] == 0 && pad_buf[48 + 1] == 0 && pad_buf[48 + 2] == 0,
          "Xbox 360 Y must map to DI button 3 (Lob / World Tour Home)");
    vt3_input_set_pad_button(VT3_BTN_Y, 0);

    /* LB, RB, LT, RT all activate DI button 4 (Instruct COM partner / Incoming Mail),
     * and RB/RT also assert DIK_LMENU (0x38, World Tour Tour Calendar). */
    vt3_input_set_pad_button(VT3_BTN_LB, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    CHECK(pad_buf[48 + 4] == 0x80 && pad_buf[48 + 1] == 0,
          "Xbox 360 LB must map to DI button 4 (COM partner / Incoming Mail)");
    vt3_input_set_pad_button(VT3_BTN_LB, 0);

    vt3_input_set_pad_button(VT3_BTN_RB, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    IDirectInputDevice8A_GetDeviceState(dev_kb, 256, state_kb);
    CHECK(pad_buf[48 + 4] == 0x80 && pad_buf[48 + 5] == 0x80 && pad_buf[48 + 1] == 0 && kb_buf[0x38] == 0x80,
          "Xbox 360 RB must map to DI button 4, button 5, and DIK_LMENU (Tour Calendar)");
    vt3_input_set_pad_button(VT3_BTN_RB, 0);

    vt3_input_set_axis(VT3_AXIS_LT, 0.8f);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    CHECK(pad_buf[48 + 4] == 0x80 && pad_buf[48 + 1] == 0,
          "Xbox 360 LT must activate DI button 4 (COM partner / Incoming Mail)");
    vt3_input_set_axis(VT3_AXIS_LT, 0.0f);

    vt3_input_set_axis(VT3_AXIS_RT, 0.8f);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    IDirectInputDevice8A_GetDeviceState(dev_kb, 256, state_kb);
    CHECK(pad_buf[48 + 4] == 0x80 && pad_buf[48 + 1] == 0 && kb_buf[0x38] == 0x80,
          "Xbox 360 RT must activate DI button 4 (COM partner) and DIK_LMENU (Tour Calendar)");
    vt3_input_set_axis(VT3_AXIS_RT, 0.0f);

    /* Start -> DI button 7 (Pause / Open Main Menu). */
    vt3_input_set_pad_button(VT3_BTN_START, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    CHECK(pad_buf[48 + 7] == 0x80, "Xbox 360 Start must map to DI button 7 (Pause)");
    vt3_input_set_pad_button(VT3_BTN_START, 0);

    /* Back -> DI button 6 AND keyboard DIK_V (0x2F, Change camera). */
    vt3_input_set_pad_button(VT3_BTN_BACK, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    IDirectInputDevice8A_GetDeviceState(dev_kb, 256, state_kb);
    CHECK(pad_buf[48 + 6] == 0x80 && kb_buf[0x2F] == 0x80,
          "Xbox 360 Back must map to DI button 6 and DIK_V (0x2F, Change camera)");
    vt3_input_set_pad_button(VT3_BTN_BACK, 0);

    /* D-pad -> POV hat centidegrees; Left stick -> X (0), Y (8) as read by FUN_004018c0. */
    vt3_input_set_pad_button(VT3_BTN_UP | VT3_BTN_RIGHT, 1);
    IDirectInputDevice8A_GetDeviceState(dev_pad, 80, state_pad);
    CHECK(g_load32(state_pad + 32) == 4500u, "D-pad UP+RIGHT POV = 4500");
    vt3_input_set_pad_button(VT3_BTN_UP | VT3_BTN_RIGHT, 0);

    IDirectInputDevice8A_Release(dev_kb);
    IDirectInputDevice8A_Release(dev_pad);
    IDirectInput8A_Release(di);

    HeapFree(ph, 0, state_pad);
    HeapFree(ph, 0, state_kb);
    HeapFree(ph, 0, prop_rng);
    HeapFree(ph, 0, odf_pad);
    HeapFree(ph, 0, df_pad);
    HeapFree(ph, 0, odf_kb);
    HeapFree(ph, 0, df_kb);
    HeapFree(ph, 0, out_ptr);
    HeapFree(ph, 0, guid_pad);
    HeapFree(ph, 0, guid_kb);
    HeapFree(ph, 0, iid_di8);

    printf("PASS dinput8 xbox360 controls\n");
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
    test_dxt_colour();
    test_heap();
    test_file_mapping();
    test_dinput8_xbox360_controls();

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
