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

#include <stddef.h>
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
    VT3_TEXFMT_A8,
    VT3_TEXFMT_RGBA16F          /* render targets only: the game's variance shadow maps */
} vt3_texfmt;

typedef uint32_t vt3_tex;    /* handle; 0 = no texture */
#define VT3_MAX_TEX 4096      /* live texture handles (the GL and Vulkan backends agree on it) */
/* A texture the sampler cannot use (for example a cube map on a 2D sampler): the renderer binds its zero texel. */
#define VT3_TEX_ZERO ((vt3_tex)0xFFFFFFFFu)

typedef struct vt3_gfx_config {
    int logical_w, logical_h;  /* game resolution, default 640x480 */
    int aspect_w, aspect_h;    /* display aspect, default 4:3 */
    int integer_scale;         /* nonzero: whole-number scaling where it fits */
} vt3_gfx_config;

/* ---- programmable path (SM2 translator output) ---------------------- */

#define VT3_MAX_VATTR    16
#define VT3_MAX_SAMPLERS 16

typedef uint32_t vt3_prog;      /* 0 = none */

typedef enum vt3_vtype {
    VT3_VT_FLOAT = 0,           /* 1-4 floats */
    VT3_VT_UBYTE_NORM = 1,      /* 4 unsigned bytes, normalised, stored R,G,B,A */
    VT3_VT_UBYTE = 2,           /* 1-4 unsigned bytes, as values (D3DDECLTYPE_UBYTE4: blend indices) */
    VT3_VT_SHORT = 3,           /* 2 or 4 signed 16-bit values, as values (SHORT2, SHORT4) */
    VT3_VT_SHORT_NORM = 4,      /* 2 or 4 signed 16-bit values, normalised (SHORT2N, SHORT4N) */
    VT3_VT_UBYTE_NORM_BGRA = 5  /* 4 unsigned bytes, normalised, stored B,G,R,A (D3DCOLOR) */
} vt3_vtype;

typedef struct vt3_vattr {
    int location;               /* GLSL input location */
    int components;             /* 1-4 */
    vt3_vtype type;
    int offset;                 /* bytes from the vertex start */
} vt3_vattr;

typedef struct vt3_vlayout {
    int stride;                 /* bytes per vertex */
    int count;
    vt3_vattr attr[VT3_MAX_VATTR];
} vt3_vlayout;

/* Render state in D3D9 terms (D3DBLEND_*, D3DCMP_*, D3DCULL_* values); the backend maps them. */
typedef struct vt3_rstate {
    int alpha_blend;            /* D3DRS_ALPHABLENDENABLE */
    int src_blend, dst_blend;   /* D3DBLEND_* */
    int depth_test;             /* D3DRS_ZENABLE */
    int depth_write;            /* D3DRS_ZWRITEENABLE */
    int depth_func;             /* D3DCMP_* */
    int cull;                   /* D3DCULL_NONE / CW / CCW */
    int blend_op;               /* D3DRS_BLENDOP: D3DBLENDOP_* (0 or 1 = ADD) */
    int alpha_test;             /* D3DRS_ALPHATESTENABLE */
    int alpha_func;             /* D3DRS_ALPHAFUNC: D3DCMP_* */
    int alpha_ref;              /* D3DRS_ALPHAREF, 0..255 */
    int color_write;            /* D3DRS_COLORWRITEENABLE: bit 0 red, 1 green, 2 blue, 3 alpha */
    int vp_x, vp_y, vp_w, vp_h; /* D3D viewport, logical pixels, top-left origin; vp_w 0 = the whole target */
    float vp_min_z, vp_max_z;   /* its depth range */
} vt3_rstate;

typedef struct vt3_draw {
    vt3_prog prog;
    const vt3_vlayout *layout;
    const void *vertices;       /* vertex buffer contents, or the ring slot when vertex_ring */
    uint32_t vertex_bytes;
    uint32_t vertex_offset;     /* buffer offset of the vertices when vertex_ring is set */
    int vertex_ring;            /* vertices were reserved and written in the frame's ring */
    const uint16_t *indices;    /* NULL = non-indexed, or the ring slot when index_ring */
    uint32_t index_offset;      /* buffer offset of the indices when index_ring is set */
    int index_ring;             /* indices were reserved and written in the frame's index ring */
    uint32_t count;             /* vertices, or indices when indexed */
    vt3_prim prim;
    const float *vsc;           /* 256 x vec4, NULL = zeros */
    const float *psc;           /* 256 x vec4, NULL = zeros */
    uint32_t vs_const_max;      /* one past the highest vertex constant the vs reads directly */
    uint32_t ps_const_max;      /* one past the highest pixel constant the ps reads directly */
    int vsb[16];                /* vertex bool constants b0..b15 (D3D bool: non-zero = true) */
    int psb[16];                /* pixel bool constants */
    vt3_tex samplers[VT3_MAX_SAMPLERS];
    uint8_t samp_min[VT3_MAX_SAMPLERS];  /* D3DTEXF_* min filter (1 POINT, 2 LINEAR, 3 ANISOTROPIC) */
    uint8_t samp_mag[VT3_MAX_SAMPLERS];  /* D3DTEXF_* mag filter */
    uint8_t samp_addr_u[VT3_MAX_SAMPLERS]; /* D3DTADDRESS_* */
    uint8_t samp_addr_v[VT3_MAX_SAMPLERS];
    uint8_t samp_addr_w[VT3_MAX_SAMPLERS];
    vt3_rstate state;
} vt3_draw;

typedef struct vt3_gfx_ops {
    const char *name;
    int  (*init)(int logical_w, int logical_h);
    void (*shutdown)(void);
    int  (*tex_create)(vt3_tex *out, int w, int h, vt3_texfmt fmt);
    void (*tex_upload)(vt3_tex t, const void *pixels, int pitch, int w, int h);
    void (*tex_destroy)(vt3_tex t);
    int  (*cube_create)(vt3_tex *out, int size, vt3_texfmt fmt);   /* colour formats only */
    void (*cube_upload)(vt3_tex t, int face, const void *pixels, int pitch, int size);
    void (*begin_frame)(void);
    void (*clear)(uint32_t argb);
    void (*draw)(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex);
    void (*present)(const vt3_rect *dst, int nearest, int drawable_w, int drawable_h);
    int  (*prog_create)(vt3_prog *out, const char *vs_src, const char *ps_src, char *err, size_t errcap);
    void (*prog_destroy)(vt3_prog p);
    void (*draw_program)(const vt3_draw *d);
    /* Reserve n bytes in the frame's vertex/index ring and return a writable pointer; *offset is
     * the buffer offset to pass back in vt3_draw. NULL when the backend copies submitted bytes
     * itself (GL) or the ring is full, in which case the caller keeps its own scratch copy. */
    void *(*reserve_vertices)(size_t n, uint32_t *offset);
    void *(*reserve_indices)(size_t n, uint32_t *offset);
    void (*readback)(uint8_t *rgba);        /* logical size, RGBA8, top row first */
    void (*clear_depth)(float depth);
    void (*set_scissor)(int enable, vt3_rect r);   /* logical pixels, top-left origin */
    int  glsl_vulkan;           /* 1: the renderer wants translated shaders as GLSL 450 (SPIR-V) */
    /* Render targets. A target is a texture that draws and clears can go to. NULL: not supported. */
    int  (*rt_create)(vt3_tex *out, int w, int h, vt3_texfmt fmt);
    void (*rt_bind)(vt3_tex t);                       /* draws and clears go to t; 0 = the back buffer */
    void (*blit)(vt3_tex dst, vt3_rect dr, vt3_tex src, vt3_rect sr, int nearest);   /* 0 = back buffer */
} vt3_gfx_ops;

int  vt3_gfx_init(const vt3_gfx_ops *ops, const vt3_gfx_config *cfg);
void vt3_gfx_shutdown(void);
int  vt3_gfx_tex_create(vt3_tex *out, int w, int h, vt3_texfmt fmt);
void vt3_gfx_tex_upload(vt3_tex t, const void *pixels, int pitch, int w, int h);
void vt3_gfx_tex_destroy(vt3_tex t);
/* A cube map, size x size per face, level 0 only. Face order is D3D's: +X, -X, +Y, -Y, +Z, -Z. */
int  vt3_gfx_cube_create(vt3_tex *out, int size, vt3_texfmt fmt);
void vt3_gfx_cube_upload(vt3_tex t, int face, const void *pixels, int pitch, int size);
void vt3_gfx_begin_frame(void);
void vt3_gfx_clear(uint32_t argb);
void vt3_gfx_draw(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex);
/* Letterbox the logical buffer into the drawable and show it. Does not swap. */
void vt3_gfx_present(int drawable_w, int drawable_h);

int  vt3_gfx_prog_create(vt3_prog *out, const char *vs_src, const char *ps_src, char *err, size_t errcap);
void vt3_gfx_prog_destroy(vt3_prog p);
void vt3_gfx_draw_program(const vt3_draw *d);
void *vt3_gfx_reserve_vertices(size_t n, uint32_t *offset);
void *vt3_gfx_reserve_indices(size_t n, uint32_t *offset);
void vt3_gfx_readback(uint8_t *rgba);
void vt3_gfx_clear_depth(float depth);
/* Render targets: -1 when the renderer has none. Handles of targets come from vt3_gfx_rt_create. */
int  vt3_gfx_rt_create(vt3_tex *out, int w, int h, vt3_texfmt fmt);
void vt3_gfx_rt_bind(vt3_tex t);
void vt3_gfx_blit(vt3_tex dst, vt3_rect dr, vt3_tex src, vt3_rect sr, int nearest);
void vt3_gfx_set_scissor(int enable, vt3_rect r);
/* The game's logical frame size, and where it lands in a drawable of this size (the same rect
 * present() uses). For 2D overlays that must hit-test against what is on screen. */
void vt3_gfx_logical_size(int *w, int *h);
void vt3_gfx_view_rect(int drawable_w, int drawable_h, vt3_rect *out);

extern const vt3_gfx_ops vt3_gfx_gl33;
extern const vt3_gfx_ops vt3_gfx_vk;   /* compile-time present: Vulkan on Android, MoltenVK on macOS */

/* The renderer the shim uses: GL 3.3 on the desktop by default (VT3_RENDERER=vulkan selects the
 * Vulkan backend), Vulkan on Android. Each platform defines it. */
const vt3_gfx_ops *vt3_gfx_default_ops(void);
/* Translator flags for the active renderer: VT3_SM2_FLAG_VULKAN when it runs SPIR-V, else 0. */
unsigned vt3_gfx_glsl_flags(void);

#endif /* VT3_RENDERER_H */
