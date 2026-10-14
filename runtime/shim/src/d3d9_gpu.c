/*
 * GPU glue for the D3D9 shim. All renderer and window access goes through here, so
 * the shim has one place that knows about vt3_platform_* and vt3_gfx_*. Without a
 * linked renderer the weak defaults (gfx_weak.c) make d3d9_gpu_init() fail, and the
 * device runs headless: it records state and skips drawing.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"

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
    if (vt3_gfx_init(&vt3_gfx_gl33, &gc) != 0) {
        VT3_WARN("gpu: renderer did not start; the device runs headless");
        vt3_platform_shutdown();
        return -1;
    }
    g_gpu_ok = 1;
    VT3_INFO("gpu: %ux%u window and OpenGL 3.3 renderer", (unsigned)width, (unsigned)height);
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
    if (d3d9_to_bgra(fmt, src, pitch, w, h, bgra) == 0)
        vt3_gfx_tex_upload(t, bgra, (int)(w * 4), (int)w, (int)h);
    free(bgra);
}
