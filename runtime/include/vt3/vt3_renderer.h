/*
 * Renderer front end. The D3D9 shim calls these functions; a backend fills in
 * a vt3_gfx_ops table. Backends:
 *   - gl33   OpenGL 3.3 core through an SDL GL context (macOS). Implemented.
 *   - gles30 OpenGL ES 3.0 (Android). Not written yet.
 *
 * Coordinates use the D3D9 convention for pre-transformed vertices (XYZRHW):
 * logical pixels, origin top-left. The game draws into a logical_w x logical_h
 * buffer; present() scales that buffer into the window, letterboxed.
 */
#ifndef VT3_RENDERER_H
#define VT3_RENDERER_H

#include <stdint.h>

typedef struct vt3_rect {
    int x, y, w, h;
} vt3_rect;

/*
 * Fit a src_w x src_h logical image into a dst_w x dst_h pixel area, keeping
 * the display aspect aspect_w:aspect_h, centred (black bars elsewhere).
 * integer_only picks the largest whole-number multiple of the source size
 * that fits, and falls back to fractional when no integer scale fits.
 */
vt3_rect vt3_letterbox(int dst_w, int dst_h, int src_w, int src_h,
                       int aspect_w, int aspect_h, int integer_only);

/* Values match D3DPRIMITIVETYPE so shim code can pass them through. */
typedef enum vt3_prim {
    VT3_PRIM_TRIANGLELIST  = 4,
    VT3_PRIM_TRIANGLESTRIP = 5
} vt3_prim;

/* D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1. Exactly 28 bytes. */
typedef struct vt3_tlvertex {
    float    x, y, z, rhw;
    uint32_t color;          /* D3DCOLOR, 0xAARRGGBB */
    float    u, v;
} vt3_tlvertex;

/* Texture formats, named after the D3DFMT values they correspond to. */
typedef enum vt3_texfmt {
    VT3_TEXFMT_A8R8G8B8 = 0,
    VT3_TEXFMT_X8R8G8B8,
    VT3_TEXFMT_A8
} vt3_texfmt;

typedef uint32_t vt3_tex;    /* handle; 0 = no texture */

typedef struct vt3_gfx_config {
    int logical_w, logical_h;  /* game resolution, default 640x480 */
    int aspect_w, aspect_h;    /* display aspect, default 4:3 */
    int integer_scale;         /* nonzero: whole-number scaling where it fits */
} vt3_gfx_config;

typedef struct vt3_gfx_ops {
    const char *name;
    int  (*init)(int logical_w, int logical_h);
    void (*shutdown)(void);
    int  (*tex_create)(vt3_tex *out, int w, int h, vt3_texfmt fmt);
    void (*tex_upload)(vt3_tex t, const void *pixels, int pitch, int w, int h);
    void (*tex_destroy)(vt3_tex t);
    void (*begin_frame)(void);
    void (*clear)(uint32_t argb);
    void (*draw)(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex);
    void (*present)(const vt3_rect *dst, int nearest, int drawable_w, int drawable_h);
} vt3_gfx_ops;

int  vt3_gfx_init(const vt3_gfx_ops *ops, const vt3_gfx_config *cfg);
void vt3_gfx_shutdown(void);
int  vt3_gfx_tex_create(vt3_tex *out, int w, int h, vt3_texfmt fmt);
void vt3_gfx_tex_upload(vt3_tex t, const void *pixels, int pitch, int w, int h);
void vt3_gfx_tex_destroy(vt3_tex t);
void vt3_gfx_begin_frame(void);
void vt3_gfx_clear(uint32_t argb);
void vt3_gfx_draw(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex);
/* Letterbox the logical buffer into the drawable and show it. Does not swap. */
void vt3_gfx_present(int drawable_w, int drawable_h);

extern const vt3_gfx_ops vt3_gfx_gl33;

#endif /* VT3_RENDERER_H */
