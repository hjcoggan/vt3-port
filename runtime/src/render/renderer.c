/* Renderer front end: holds the active backend and the letterbox settings. */
#include "vt3/vt3_log.h"
#include "vt3/vt3_renderer.h"
#include "vt3/vt3_sm2.h"

#include <stddef.h>

static const vt3_gfx_ops *g_ops;
static vt3_gfx_config g_cfg;

int vt3_gfx_init(const vt3_gfx_ops *ops, const vt3_gfx_config *cfg)
{
    if (!ops || !cfg || cfg->logical_w <= 0 || cfg->logical_h <= 0) return -1;
    if (ops->init(cfg->logical_w, cfg->logical_h) != 0) {
        VT3_ERROR("renderer backend '%s' failed to initialise", ops->name);
        return -1;
    }
    g_ops = ops;
    g_cfg = *cfg;
    VT3_INFO("renderer: %s, logical %dx%d, display %d:%d%s", ops->name,
             cfg->logical_w, cfg->logical_h, cfg->aspect_w, cfg->aspect_h,
             cfg->integer_scale ? ", integer scale" : "");
    return 0;
}

void vt3_gfx_shutdown(void)
{
    if (!g_ops) return;
    g_ops->shutdown();
    g_ops = NULL;
}

int vt3_gfx_tex_create(vt3_tex *out, int w, int h, vt3_texfmt fmt)
{
    return g_ops ? g_ops->tex_create(out, w, h, fmt) : -1;
}

void vt3_gfx_tex_upload(vt3_tex t, const void *pixels, int pitch, int w, int h)
{
    if (g_ops) g_ops->tex_upload(t, pixels, pitch, w, h);
}

unsigned vt3_gfx_glsl_flags(void)
{
    return g_ops && g_ops->glsl_vulkan ? VT3_SM2_FLAG_VULKAN : 0u;
}

int vt3_gfx_cube_create(vt3_tex *out, int size, vt3_texfmt fmt)
{
    return g_ops ? g_ops->cube_create(out, size, fmt) : -1;
}

void vt3_gfx_cube_upload(vt3_tex t, int face, const void *pixels, int pitch, int size)
{
    if (g_ops) g_ops->cube_upload(t, face, pixels, pitch, size);
}

void vt3_gfx_tex_destroy(vt3_tex t)
{
    if (g_ops) g_ops->tex_destroy(t);
}

int vt3_gfx_rt_create(vt3_tex *out, int w, int h, vt3_texfmt fmt)
{
    return (g_ops && g_ops->rt_create) ? g_ops->rt_create(out, w, h, fmt) : -1;
}

void vt3_gfx_rt_bind(vt3_tex t)
{
    if (g_ops && g_ops->rt_bind) g_ops->rt_bind(t);
}

void vt3_gfx_blit(vt3_tex dst, vt3_rect dr, vt3_tex src, vt3_rect sr, int nearest)
{
    if (g_ops && g_ops->blit) g_ops->blit(dst, dr, src, sr, nearest);
}

void vt3_gfx_begin_frame(void)
{
    if (g_ops) g_ops->begin_frame();
}

void vt3_gfx_clear(uint32_t argb)
{
    if (g_ops) g_ops->clear(argb);
}

void vt3_gfx_draw(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex)
{
    if (g_ops && v && count) g_ops->draw(prim, v, count, tex);
}

void vt3_gfx_present(int drawable_w, int drawable_h)
{
    if (!g_ops) return;
    vt3_rect dst = vt3_letterbox(drawable_w, drawable_h, g_cfg.logical_w, g_cfg.logical_h,
                                 g_cfg.aspect_w, g_cfg.aspect_h, g_cfg.integer_scale);
    /* Integer scaling uses nearest-neighbour. Fractional uses bilinear, which
     * is less harsh on the sprites and text. */
    int nearest = g_cfg.integer_scale && dst.w % g_cfg.logical_w == 0 && dst.h % g_cfg.logical_h == 0;
    g_ops->present(&dst, nearest, drawable_w, drawable_h);
}

int vt3_gfx_prog_create(vt3_prog *out, const char *vs_src, const char *ps_src, char *err, size_t errcap)
{
    if (!g_ops || !g_ops->prog_create) return -1;
    return g_ops->prog_create(out, vs_src, ps_src, err, errcap);
}

void vt3_gfx_prog_destroy(vt3_prog p)
{
    if (g_ops && g_ops->prog_destroy) g_ops->prog_destroy(p);
}

void vt3_gfx_draw_program(const vt3_draw *d)
{
    if (g_ops && g_ops->draw_program && d && d->count) g_ops->draw_program(d);
}

void vt3_gfx_readback(uint8_t *rgba)
{
    if (g_ops && g_ops->readback) g_ops->readback(rgba);
}

void vt3_gfx_clear_depth(float depth)
{
    if (g_ops && g_ops->clear_depth) g_ops->clear_depth(depth);
}

void vt3_gfx_set_scissor(int enable, vt3_rect r)
{
    if (g_ops && g_ops->set_scissor) g_ops->set_scissor(enable, r);
}

void vt3_gfx_logical_size(int *w, int *h)
{
    if (w) *w = g_cfg.logical_w;
    if (h) *h = g_cfg.logical_h;
}

void vt3_gfx_view_rect(int drawable_w, int drawable_h, vt3_rect *out)
{
    if (!out) return;
    *out = vt3_letterbox(drawable_w, drawable_h, g_cfg.logical_w, g_cfg.logical_h,
                         g_cfg.aspect_w, g_cfg.aspect_h, g_cfg.integer_scale);
}

