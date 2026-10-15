/*
 * GPU glue for the D3D9 shim. All renderer and window access goes through here, so
 * the shim has one place that knows about vt3_platform_* and vt3_gfx_*. Without a
 * linked renderer the weak defaults (gfx_weak.c) make d3d9_gpu_init() fail, and the
 * device runs headless: it records state and skips drawing.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_input.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_gpu_ok;

int d3d9_gpu_init(uint32_t width, uint32_t height)
{
    if (g_gpu_ok) return 0;
    if (width == 0 || height == 0) return -1;
    vt3_config cfg;
    vt3_config_defaults(&cfg);
    cfg.title = "VT3";
    cfg.window_w = (int)width;
    cfg.window_h = (int)height;
    cfg.logical_w = (int)width;
    cfg.logical_h = (int)height;
    cfg.aspect_w = (int)width;     /* display keeps the back buffer's own aspect */
    cfg.aspect_h = (int)height;
    cfg.audio = 0;                 /* audio is not wired through the shim yet */
    if (vt3_platform_init(&cfg) != 0) {
        VT3_WARN("gpu: no window or GL context; the device runs headless");
        return -1;
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
    VT3_INFO("gpu: %ux%u window and renderer", (unsigned)width, (unsigned)height);
    return 0;
}

int d3d9_gpu_ok(void)
{
    return g_gpu_ok;
}

void d3d9_gpu_begin(void)
{
    if (g_gpu_ok) vt3_gfx_begin_frame();
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
    if (vt3_gfx_tex_create(&t, (int)w, (int)h, tf) != 0) {
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
        vt3_gfx_tex_upload(t, src, (int)pitch, (int)w, (int)h);
        return;
    }
    uint8_t *bgra = malloc((size_t)w * h * 4);
    if (!bgra) return;
    if (d3d9_to_bgra(fmt, src, pitch, w, h, bgra) == 0) {
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
        vt3_gfx_tex_upload(t, bgra, (int)(w * 4), (int)w, (int)h);
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
