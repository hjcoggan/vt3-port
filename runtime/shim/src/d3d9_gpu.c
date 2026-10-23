/*
 * GPU glue for the D3D9 shim. All renderer and window access goes through here, so
 * the shim has one place that knows about vt3_platform_* and vt3_gfx_*. Without a
 * linked renderer the weak defaults (gfx_weak.c) make d3d9_gpu_init() fail, and the
 * device runs headless: it records state and skips drawing.
 */
#include "d3d9_internal.h"
#include "shim_internal.h"
#include "vt3/vt3_input.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "vt3/vt3_touch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* VT3_TRACE_SLOW: texture operations are the loader's bulk work and run with the guest lock
 * held, so the slow ones are what stalls every other guest thread (the audio writer among
 * them). slow_start returns 0 when the trace is off, making the pair free. */
struct slow_op { const char *what; struct timespec t0; };

static int slow_start(struct slow_op *s, const char *what)
{
    if (vt3s_slow_ms() <= 0) return 0;
    s->what = what;
    clock_gettime(CLOCK_MONOTONIC, &s->t0);
    return 1;
}

static void slow_end(struct slow_op *s, uint32_t w, uint32_t h, uint32_t fmt)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long ms = (long long)(t1.tv_sec - s->t0.tv_sec) * 1000 + (t1.tv_nsec - s->t0.tv_nsec) / 1000000;
    if (ms >= vt3s_slow_ms())
        VT3_INFO("slow tex %s: %ux%u fmt 0x%x took %lld ms (gil holder tid %u)",
                 s->what, (unsigned)w, (unsigned)h, (unsigned)fmt, ms, vt3s_gil_holder());
}

static int g_gpu_ok;
static int g_plat_ok;

/* The game has its own widescreen mode, named WIDESCREEN in its INI. With it on, the 3D camera
 * keeps its vertical field of view and widens horizontally (a true Hor+), and the HUD lays itself
 * out for the wider frame. Two guest globals drive it:
 *   - 0x6cf0e8, the float aspect the wide camera uses (16:9 on the disc),
 *   - 0x8599c2, the WIDESCREEN config byte, copied to the engine at +0x9c/+0x9d.
 * The byte is read once at startup, so vt3s_display_prepare() writes it (and the resolution) into
 * the game's INI before the game runs; widescreen_enable() then keeps the engine copy set in case
 * the engine is rebuilt.
 *
 * The camera aspect is not derived from the resolution: it is the fixed 16:9 constant, so a phone
 * panel (about 19.5:9) would still be pillarboxed. widescreen_aspect() points that constant at the
 * frame the game actually renders, which keeps the vertical FOV and widens only horizontally. */
#define VT3_GUEST_WIDE_ASPECT 0x006cf0e8u
#define VT3_GUEST_CONFIG_WIDE 0x008599c2u
#define VT3_GUEST_ENGINE      0x009abe88u
#define VT3_ENGINE_WIDE_OFF   0x9cu

/* Cap the back buffer height: a 1440p phone panel would otherwise cost about four times the pixels
 * of the game's 1024x768 frame for no visible gain at arm's length. */
#define VT3_WIDE_MAX_H 1080

static void widescreen_enable(void)
{
    if (getenv("VT3_NO_WIDESCREEN")) return;
    uint32_t c = vt3s_load_u32((GPTR)VT3_GUEST_CONFIG_WIDE);
    vt3s_store_u32((GPTR)VT3_GUEST_CONFIG_WIDE, (c & 0xffffff00u) | 0x01u);
    uint32_t eng = vt3s_load_u32((GPTR)VT3_GUEST_ENGINE);
    if (eng) {
        uint32_t f = vt3s_load_u32((GPTR)(eng + VT3_ENGINE_WIDE_OFF));
        vt3s_store_u32((GPTR)(eng + VT3_ENGINE_WIDE_OFF), (f & 0xffff0000u) | 0x0101u);
    }
}

static void widescreen_aspect(int w, int h)
{
    if (getenv("VT3_NO_WIDESCREEN")) return;
    if (w <= 0 || h <= 0) return;
    float aspect = (float)w / (float)h;
    uint32_t bits;
    memcpy(&bits, &aspect, 4);
    vt3s_store_u32((GPTR)VT3_GUEST_WIDE_ASPECT, bits);
}

/* A frame at the display's aspect and a reasonable height, so the renderer fills the screen. */
static void choose_target(int dw, int dh, uint32_t *out_w, uint32_t *out_h){
    if (dw <= 0 || dh <= 0) {
        *out_w = 1024;
        *out_h = 768;
        return;
    }
    const char *env = getenv("VT3_RENDER_HEIGHT");
    int max_h = env ? atoi(env) : VT3_WIDE_MAX_H;
    int h = dh > max_h ? max_h : dh;
    if (h < 2) h = 2;
    int w = (int)(((int64_t)h * dw + dh / 2) / dh);
    if (w < 2) w = 2;
    *out_w = (uint32_t)(w & ~1);
    *out_h = (uint32_t)(h & ~1);
}

/* Bring up the window and pick the frame size. Returns 0, or -1 when there is no window. */
int d3d9_gpu_prepare(uint32_t req_w, uint32_t req_h, uint32_t *out_w, uint32_t *out_h)
{
    if (!g_plat_ok) {
        vt3_config cfg;
        vt3_config_defaults(&cfg);
        cfg.title = "VT3";
        cfg.window_w = (int)(req_w ? req_w : 640);
        cfg.window_h = (int)(req_h ? req_h : 480);
        cfg.logical_w = cfg.window_w;
        cfg.logical_h = cfg.window_h;
        cfg.aspect_w = cfg.window_w;     /* display keeps the back buffer's own aspect */
        cfg.aspect_h = cfg.window_h;
        cfg.audio = 0;                   /* audio is not wired through the shim yet */
        if (vt3_platform_init(&cfg) != 0) {
            VT3_WARN("gpu: no window or GL context; the device runs headless");
            return -1;
        }
        g_plat_ok = 1;
    }
    int dw = 0, dh = 0;
    vt3_platform_drawable_size(&dw, &dh);
    choose_target(dw, dh, out_w, out_h);
    return 0;
}

/* Write (or refresh) the three keys we drive, keeping any other key the player put in the file. */
static int write_widescreen_ini(const char *path, uint32_t w, uint32_t h)
{
    char *body = NULL;
    size_t blen = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long n = ftell(f);
            if (n > 0 && n < (1L << 20)) {
                rewind(f);
                body = malloc((size_t)n + 1);
                if (body) {
                    blen = fread(body, 1, (size_t)n, f);
                    body[blen] = '\0';
                }
            }
        }
        fclose(f);
    }
    FILE *o = fopen(path, "wb");
    if (!o) {
        free(body);
        return -1;
    }
    fprintf(o, "DX/RES/WIDTH = %u\nDX/RES/HEIGHT = %u\nWIDESCREEN = 1\n", (unsigned)w, (unsigned)h);
    if (body) {
        char *save = NULL;
        for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *s = line;
            while (*s == ' ' || *s == '\t' || *s == '\r') s++;
            if (!*s) continue;
            if (strncmp(s, "DX/RES/WIDTH", 12) == 0 || strncmp(s, "DX/RES/HEIGHT", 13) == 0 ||
                strncmp(s, "WIDESCREEN", 10) == 0)
                continue;   /* a previous copy of ours: it is rewritten above */
            fputs(s, o);
            fputc('\n', o);
        }
        free(body);
    }
    return fclose(o);
}

/* Called once from the shim before the game runs: bring up the window, then hand the game a
 * WIDESCREEN INI for a frame that fills the display. The game reads this before it sets up the
 * renderer, so its viewport, HUD and camera all agree on the same widescreen frame; overriding the
 * back buffer behind its back would leave the picture in the top-left corner. */
void vt3s_display_prepare(void)
{
    uint32_t tw = 0, th = 0;
    if (d3d9_gpu_prepare(0, 0, &tw, &th) != 0) return;
    if (getenv("VT3_NO_WIDESCREEN")) return;

    char path[1200];
    snprintf(path, sizeof path, "%svt3.ini", vt3s_game_root());
    if (write_widescreen_ini(path, tw, th) != 0) {
        VT3_WARN("widescreen: cannot write %s; the game keeps its own resolution", path);
    } else {
        VT3_INFO("widescreen: %s = %ux%u", path, (unsigned)tw, (unsigned)th);
    }
    widescreen_aspect((int)tw, (int)th);
    widescreen_enable();
}

int d3d9_gpu_init(uint32_t width, uint32_t height)
{
    if (g_gpu_ok) return 0;
    if (width == 0 || height == 0) return -1;
    if (!g_plat_ok) {
        uint32_t tw = width, th = height;
        if (d3d9_gpu_prepare(width, height, &tw, &th) != 0) return -1;
    }
    vt3_gfx_config gc;
    gc.logical_w = (int)width;
    gc.logical_h = (int)height;
    gc.aspect_w = (int)width;
    gc.aspect_h = (int)height;
    gc.integer_scale = 0;
    if (vt3_gfx_init(vt3_gfx_default_ops(), &gc) != 0) {
        VT3_WARN("gpu: renderer did not start; the device runs headless");
        vt3_platform_shutdown();
        return -1;
    }
    g_gpu_ok = 1;
    widescreen_aspect((int)width, (int)height);   /* follow the INI if it set its own size */
    widescreen_enable();
    VT3_INFO("gpu: %ux%u window and renderer", (unsigned)width, (unsigned)height);
    return 0;
}

int d3d9_gpu_ok(void)
{
    return g_gpu_ok;
}

void d3d9_gpu_begin(void)
{
    if (!g_gpu_ok) return;
    widescreen_enable();   /* the engine can be rebuilt between frames; keep its widescreen flag on */
    vt3_gfx_begin_frame();
}

void d3d9_gpu_clear(uint32_t argb, int do_color, int do_depth, float depth)
{
    if (!g_gpu_ok) return;
    if (do_color) vt3_gfx_clear(argb);
    if (do_depth) vt3_gfx_clear_depth(depth);
}

void d3d9_gpu_set_scissor(int enable, int x, int y, int w, int h)
{
    if (!g_gpu_ok) return;
    vt3_rect r = { x, y, w, h };
    vt3_gfx_set_scissor(enable, r);
}

void d3d9_gpu_present(void)
{
    if (!g_gpu_ok) return;
    int w = 0, h = 0;
    vt3_platform_drawable_size(&w, &h);
    vt3_touch_frame(w, h);         /* on-screen controls, drawn over the game's frame */
    vt3_gfx_present(w, h);
    vt3_platform_swap();
    vt3_platform_pump_events();    /* keeps the window responsive; quit requests are ignored here */
}

vt3_tex d3d9_gpu_rt_create(uint32_t w, uint32_t h, uint32_t fmt)
{
    if (!g_gpu_ok || w == 0 || h == 0) return 0;
    vt3_texfmt tf = fmt == D3DFMT_CHK_G16R16F || fmt == D3DFMT_CHK_A16B16G16R16F ? VT3_TEXFMT_RGBA16F
                                                                                : VT3_TEXFMT_A8R8G8B8;
    vt3_tex t = 0;
    if (vt3_gfx_rt_create(&t, (int)w, (int)h, tf) != 0) return 0;
    return t;
}

void d3d9_gpu_rt_bind(vt3_tex t)
{
    if (g_gpu_ok) vt3_gfx_rt_bind(t);
}

void d3d9_gpu_blit(vt3_tex dst, vt3_rect dr, vt3_tex src, vt3_rect sr, int nearest)
{
    if (g_gpu_ok) vt3_gfx_blit(dst, dr, src, sr, nearest);
}

vt3_tex d3d9_gpu_tex_create(uint32_t w, uint32_t h, uint32_t fmt)
{
    if (!g_gpu_ok || w == 0 || h == 0) return 0;
    vt3_texfmt tf;
    switch (fmt) {
    case D3DFMT_CHK_X8R8G8B8: tf = VT3_TEXFMT_X8R8G8B8; break;
    case D3DFMT_CHK_A8: tf = VT3_TEXFMT_A8; break;
    default: tf = VT3_TEXFMT_A8R8G8B8; break;   /* everything else is converted to BGRA */
    }
    vt3_tex t = 0;
    vt3_input_note_load();   /* loading activity, for the input script's quiet waits */
    struct slow_op op;
    int timed = slow_start(&op, "create gl");
    int gfx_rc = vt3_gfx_tex_create(&t, (int)w, (int)h, tf);
    if (timed) slow_end(&op, w, h, fmt);
    if (gfx_rc != 0) {
        static int warned_create;
        if (!warned_create) {
            warned_create = 1;
            VT3_WARN("texture %ux%u fmt 0x%x: renderer could not create it (draws white)", (unsigned)w, (unsigned)h, (unsigned)fmt);
        }
        return 0;
    }
    return t;
}

void d3d9_gpu_tex_destroy(vt3_tex t)
{
    if (g_gpu_ok && t) vt3_gfx_tex_destroy(t);
}

void d3d9_gpu_tex_upload(vt3_tex t, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt)
{
    if (!g_gpu_ok || !t || !src || w == 0 || h == 0) return;
    if (fmt == D3DFMT_CHK_A8R8G8B8 || fmt == D3DFMT_CHK_X8R8G8B8 || fmt == D3DFMT_CHK_A8) {
        struct slow_op op;
        int timed = slow_start(&op, "upload gl");
        vt3_gfx_tex_upload(t, src, (int)pitch, (int)w, (int)h);
        if (timed) slow_end(&op, w, h, fmt);
        return;
    }
    uint8_t *bgra = malloc((size_t)w * h * 4);
    if (!bgra) return;
    struct slow_op conv;
    int conv_timed = slow_start(&conv, "convert");
    int conv_ok = d3d9_to_bgra(fmt, src, pitch, w, h, bgra) == 0;
    if (conv_timed) slow_end(&conv, w, h, fmt);
    if (conv_ok) {
        if (getenv("VT3_TRACE_TEX")) {   /* average of the level: a black or empty texture shows here */
            double sum[4] = {0, 0, 0, 0};
            size_t n = (size_t)w * h;
            for (size_t k = 0; k < n; k++)
                for (int c = 0; c < 4; c++) sum[c] += bgra[4 * k + c];
            VT3_INFO("tex upload gl %u %ux%u fmt 0x%x avg rgba %.0f %.0f %.0f %.0f", (unsigned)t, (unsigned)w,
                     (unsigned)h, (unsigned)fmt, sum[2] / n, sum[1] / n, sum[0] / n, sum[3] / n);
        }
        const char *dump_dir = getenv("VT3_DUMP_TEX");   /* each decoded upload as <dir>/tex_<gl>_<w>x<h>.tga */
        if (dump_dir) {
            char path[1024];
            snprintf(path, sizeof path, "%s/tex_%u_%ux%u.tga", dump_dir, (unsigned)t, (unsigned)w, (unsigned)h);
            FILE *f = fopen(path, "wb");
            if (f) {
                /* Uncompressed true-colour TGA, 32 bits BGRA, top-left origin: the bytes as they are. */
                uint8_t hdr[18] = { 0, 0, 2 };
                hdr[12] = (uint8_t)w; hdr[13] = (uint8_t)(w >> 8);
                hdr[14] = (uint8_t)h; hdr[15] = (uint8_t)(h >> 8);
                hdr[16] = 32; hdr[17] = 0x28;
                fwrite(hdr, 1, sizeof hdr, f);
                fwrite(bgra, 4, (size_t)w * h, f);
                fclose(f);
            }
        }
        struct slow_op up;
        int up_timed = slow_start(&up, "upload gl");
        vt3_gfx_tex_upload(t, bgra, (int)(w * 4), (int)w, (int)h);
        if (up_timed) slow_end(&up, w, h, fmt);
    }
    free(bgra);
}

/* Cube maps are colour only: the renderer has no alpha-only cube. */
vt3_tex d3d9_gpu_cube_create(uint32_t size, uint32_t fmt)
{
    if (!g_gpu_ok || size == 0 || fmt == D3DFMT_CHK_A8) return 0;
    vt3_texfmt tf = fmt == D3DFMT_CHK_X8R8G8B8 ? VT3_TEXFMT_X8R8G8B8 : VT3_TEXFMT_A8R8G8B8;
    vt3_tex t = 0;
    if (vt3_gfx_cube_create(&t, (int)size, tf) != 0) {
        static int warned_cube;
        if (!warned_cube) {
            warned_cube = 1;
            VT3_WARN("cube texture %u fmt 0x%x: renderer could not create it (reads as zero)", (unsigned)size,
                     (unsigned)fmt);
        }
        return 0;
    }
    return t;
}

void d3d9_gpu_cube_upload(vt3_tex t, uint32_t face, const uint8_t *src, uint32_t pitch, uint32_t size, uint32_t fmt)
{
    if (!g_gpu_ok || !t || !src || size == 0) return;
    if (fmt == D3DFMT_CHK_A8R8G8B8 || fmt == D3DFMT_CHK_X8R8G8B8) {
        vt3_gfx_cube_upload(t, (int)face, src, (int)pitch, (int)size);
        return;
    }
    uint8_t *bgra = malloc((size_t)size * size * 4);
    if (!bgra) return;
    if (d3d9_to_bgra(fmt, src, pitch, size, size, bgra) == 0)
        vt3_gfx_cube_upload(t, (int)face, bgra, (int)(size * 4), (int)size);
    free(bgra);
}
