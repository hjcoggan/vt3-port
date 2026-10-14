/*
 * Weak defaults for the renderer, platform and SM2 translator entry points that the
 * D3D9 shim calls. A build that links the real implementations (runtime/src/render,
 * runtime/src/platform, runtime/src/render/sm2_glsl.c) overrides every one of these.
 * A build without them, such as the recompiler's harness, runs the device headless:
 * no window, no textures on the GPU, draws skipped.
 */
#include "vt3/vt3_platform.h"
#include "vt3/vt3_renderer.h"
#include "vt3/vt3_sm2.h"

#include <stddef.h>
#include <stdint.h>

__attribute__((weak)) void vt3_config_defaults(vt3_config *c)
{
    c->title = "VT3";
    c->window_w = 640;
    c->window_h = 480;
    c->logical_w = 640;
    c->logical_h = 480;
    c->aspect_w = 4;
    c->aspect_h = 3;
    c->integer_scale = 0;
    c->vsync = 1;
    c->audio = 0;
    c->frames = 0;
    c->game_root = NULL;
}

__attribute__((weak)) int vt3_platform_init(const vt3_config *c)
{
    (void)c;
    return -1;
}

__attribute__((weak)) void vt3_platform_shutdown(void) {}

__attribute__((weak)) int vt3_platform_pump_events(void)
{
    return 1;
}

__attribute__((weak)) int vt3_platform_next_event(vt3_event *ev)
{
    (void)ev;
    return 0;
}

__attribute__((weak)) void vt3_platform_drawable_size(int *w, int *h)
{
    if (w) *w = 0;
    if (h) *h = 0;
}

__attribute__((weak)) void vt3_platform_swap(void) {}

__attribute__((weak)) const vt3_gfx_ops vt3_gfx_gl33 = { .name = "none (no renderer linked)" };

__attribute__((weak)) int vt3_gfx_init(const vt3_gfx_ops *ops, const vt3_gfx_config *cfg)
{
    (void)ops;
    (void)cfg;
    return -1;
}

__attribute__((weak)) void vt3_gfx_shutdown(void) {}

__attribute__((weak)) int vt3_gfx_tex_create(vt3_tex *out, int w, int h, vt3_texfmt fmt)
{
    (void)out;
    (void)w;
    (void)h;
    (void)fmt;
    return -1;
}

__attribute__((weak)) void vt3_gfx_tex_upload(vt3_tex t, const void *pixels, int pitch, int w, int h)
{
    (void)t;
    (void)pixels;
    (void)pitch;
    (void)w;
    (void)h;
}

__attribute__((weak)) void vt3_gfx_tex_destroy(vt3_tex t)
{
    (void)t;
}

__attribute__((weak)) void vt3_gfx_begin_frame(void) {}

__attribute__((weak)) void vt3_gfx_clear(uint32_t argb)
{
    (void)argb;
}

__attribute__((weak)) void vt3_gfx_clear_depth(float depth)
{
    (void)depth;
}

__attribute__((weak)) void vt3_gfx_draw(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex)
{
    (void)prim;
    (void)v;
    (void)count;
    (void)tex;
}

__attribute__((weak)) void vt3_gfx_draw_program(const vt3_draw *d)
{
    (void)d;
}

__attribute__((weak)) int vt3_gfx_prog_create(vt3_prog *out, const char *vs_src, const char *ps_src,
                                              char *err, size_t errcap)
{
    (void)out;
    (void)vs_src;
    (void)ps_src;
    if (err && errcap) err[0] = '\0';
    return -1;
}

__attribute__((weak)) void vt3_gfx_prog_destroy(vt3_prog p)
{
    (void)p;
}

__attribute__((weak)) void vt3_gfx_present(int drawable_w, int drawable_h)
{
    (void)drawable_w;
    (void)drawable_h;
}

__attribute__((weak)) int vt3_sm2_translate(const uint32_t *tokens, size_t ntokens, vt3_sm2_shader *out)
{
    (void)tokens;
    (void)ntokens;
    if (out) {
        __builtin_memset(out, 0, sizeof *out);
        __builtin_strcpy(out->error, "no SM2 translator linked");
    }
    return -1;
}

__attribute__((weak)) void vt3_sm2_free(vt3_sm2_shader *out)
{
    if (out) out->glsl = NULL;
}

/* Same mapping as sm2_glsl.c. Identical so both builds agree on attribute locations. */
__attribute__((weak)) int vt3_sm2_attr_location(uint32_t usage, uint32_t usage_index)
{
    switch (usage) {
    case 0:  return usage_index == 0 ? 0 : -1;
    case 1:  return usage_index == 0 ? 1 : -1;
    case 2:  return usage_index == 0 ? 2 : -1;
    case 3:  return usage_index == 0 ? 3 : -1;
    case 4:  return usage_index == 0 ? 4 : -1;
    case 5:  return usage_index < 8 ? (int)(5 + usage_index) : -1;
    case 6:  return usage_index == 0 ? 13 : -1;
    case 7:  return usage_index == 0 ? 14 : -1;
    case 10: return usage_index == 0 ? 15 : -1;
    default: return -1;
    }
}
