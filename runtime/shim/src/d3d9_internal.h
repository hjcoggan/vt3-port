/* Internal to the D3D9 shim: COM objects, host data per kind, guest memory helpers. */
#ifndef VT3_SHIM_D3D9_INTERNAL_H
#define VT3_SHIM_D3D9_INTERNAL_H

#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_renderer.h"
#include "vt3/vt3_sm2.h"
#include "win32/d3d9.h"
#include "win32/d3d9_com.h"
#include "win32/kernel32.h"

#include <stddef.h>
#include <stdint.h>

/* Guest object layout. A COM object is 16 bytes of guest memory:
 *   +0  lpVtbl (guest address of the vtable)
 *   +4  reference count
 *   +8  kind (D3D9_KIND_*)
 *   +12 index of the host object in g_d3d9_host[] */
#define D3D9_OBJ_SIZE 16
#define D3D9_OFF_VTBL 0
#define D3D9_OFF_REFS 4
#define D3D9_OFF_KIND 8
#define D3D9_OFF_HOST 12

enum {
    D3D9_KIND_NONE = 0,
    D3D9_KIND_D3D9, D3D9_KIND_DEVICE, D3D9_KIND_SWAPCHAIN, D3D9_KIND_TEXTURE,
    D3D9_KIND_CUBETEXTURE, D3D9_KIND_VOLUMETEXTURE, D3D9_KIND_VB, D3D9_KIND_IB,
    D3D9_KIND_SURFACE, D3D9_KIND_VDECL, D3D9_KIND_VSHADER, D3D9_KIND_PSHADER,
    D3D9_KIND_STATEBLOCK, D3D9_KIND_QUERY, D3D9_KIND_VOLUME,
    D3D9_KIND_DSOUND, D3D9_KIND_DSBUFFER,    /* DirectSound objects (dsound_stub.c) */
    D3D9_KIND_D3DXBUFFER, D3D9_KIND_D3DXCONSTTABLE,  /* D3DX objects (d3dx9_compile.c, d3dx9_stub.c) */
    D3D9_KIND_DINPUT, D3D9_KIND_DIDEVICE             /* DirectInput objects (dinput8.c) */
};
#define D3D9_KIND_LAST D3D9_KIND_DIDEVICE

#define D3D9_MAX_HOST 4096

#define D3DUSAGE_RENDERTARGET_CHK 0x1u
#define D3DUSAGE_DEPTHSTENCIL_CHK 0x2u

typedef struct d3d9_host {
    uint32_t kind;
    uint32_t refs;        /* host copy of the guest count */
    GPTR guest;           /* the guest object that owns this entry */
    void *data;           /* per-kind host data; NULL for stub objects */
    GPTR parent;          /* the device for resources; the IDirect3D9 for devices */
} d3d9_host;

extern d3d9_host g_d3d9_host[D3D9_MAX_HOST];

/* Guest memory helpers. Pointers are checked against the arena. Inline: every D3D9 method goes
 * through them, with d3d9_host_of (about 2% of Retroid match CPU out of line). */
static inline void *d3d9_gmem(GPTR p, DWORD bytes)   /* NULL when out of range */
{
    return vt3s_guest_range_ok(p, bytes) ? vt3_guest_base_ + p : NULL;
}
static inline uint32_t d3d9_rd(GPTR p, uint32_t off) { return vt3s_load_u32(p + off); }   /* 0 when out of range */
static inline void d3d9_wr(GPTR p, uint32_t off, uint32_t v) { vt3s_store_u32(p + off, v); }   /* no-op when out of range */

/* Objects. */
GPTR d3d9_obj_new(uint32_t kind, void *data, GPTR parent);     /* picks the vtable for kind */
GPTR d3d9_new_stub(uint32_t kind, GPTR parent);                /* E_NOTIMPL methods, no data */
static inline d3d9_host *d3d9_host_of(GPTR obj, uint32_t kind)   /* NULL unless kind matches */
{
    if (!vt3s_guest_range_ok(obj, D3D9_OBJ_SIZE) || d3d9_rd(obj, D3D9_OFF_KIND) != kind) return NULL;
    uint32_t idx = d3d9_rd(obj, D3D9_OFF_HOST);
    return idx < D3D9_MAX_HOST && g_d3d9_host[idx].kind == kind ? &g_d3d9_host[idx] : NULL;
}
void d3d9_obj_free(GPTR obj);
void dsound_free(uint32_t kind, void *data);                   /* host data of a DirectSound object (dsound_stub.c) */
void d3dx_obj_free(uint32_t kind, void *data);                 /* host data of an ID3DX object (d3dx9_compile.c) */
/* DirectInput objects (dinput8.c): vtable method names for a kind (NULL for other kinds), and frees. */
const char *const *dinput_vtbl_names(uint32_t kind, size_t *count);
void dinput_obj_free(uint32_t kind, void *data);
DWORD d3d9_qi_kind(GPTR This, GPTR riid, GPTR ppv, uint32_t kind);
DWORD d3d9_add_ref(GPTR This);
DWORD d3d9_release(GPTR This);
HRESULT d3d9_get_device(GPTR This, GPTR ppDevice);
UINT d3d9_resource_type(uint32_t kind);
const uint8_t *d3d9_iid_of_kind(uint32_t kind);               /* generated (d3d9_com_generic.c) */

/* Caps for IDirect3D9::GetDeviceCaps and IDirect3DDevice9::GetDeviceCaps (d3d9_iface.c). */
void d3d9_fill_caps(GPTR caps);

/* Store an object pointer through an output parameter, taking a reference first. */
void d3d9_return_obj(GPTR out, GPTR obj);

/* ---- format helpers ------------------------------------------------------ */

int d3d9_fmt_is_dxt(uint32_t fmt);
uint32_t d3d9_fmt_bpp(uint32_t fmt);                  /* bytes per pixel, 0 when unknown */
/* Pitch in bytes and total bytes for one level. Returns 0 for an unknown format. */
uint32_t d3d9_level_layout(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *pitch);
/* Converts one level to 32-bit BGRA (D3DFMT_A8R8G8B8 byte order). dst is w*h*4. */
int d3d9_to_bgra(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst);

/* ---- host data per kind -------------------------------------------------- */

#define D3D9_MAX_LEVELS 16
#define D3D9_MAX_FACES 6

typedef struct d3d9_level {
    GPTR data;            /* guest heap copy of the level */
    uint32_t w, h, pitch, bytes;
} d3d9_level;

typedef struct d3d9_texture {
    uint32_t fmt, usage, pool, w, h, levels, faces;
    d3d9_level lv[D3D9_MAX_FACES][D3D9_MAX_LEVELS];
    vt3_tex gl;           /* renderer texture for level 0 of face 0 (0 = none) */
    int locked[D3D9_MAX_FACES][D3D9_MAX_LEVELS];
} d3d9_texture;

typedef struct d3d9_surface {
    GPTR owner;           /* texture the surface belongs to, or 0 */
    uint32_t face, level;
    uint32_t w, h, fmt, usage, pool;
    GPTR data;            /* storage (guest), or 0 when the surface has none */
    uint32_t pitch;
    int backbuffer;
    int locked;
    vt3_tex rt;           /* renderer target for a surface with no texture owner (made on first use) */
} d3d9_surface;

typedef struct d3d9_buffer {
    uint32_t length, usage, fvf_or_fmt, pool;
    GPTR data;            /* guest heap storage */
    int locked;
} d3d9_buffer;

/* D3DVERTEXELEMENT9 array as the game wrote it: 8 bytes per element (Stream u16,
 * Offset u16, Type u8, Method u8, Usage u8, UsageIndex u8), ending with D3DDECL_END. */
typedef struct d3d9_decl {
    uint32_t n;                         /* elements before D3DDECL_END, including nothing else */
    uint8_t el[65][8];
    uint32_t stride[16];                /* bytes per vertex, per stream */
} d3d9_decl;

typedef struct d3d9_shader {
    int is_ps;
    uint32_t ntok;
    uint32_t *tok;                      /* host copy of the bytecode */
    vt3_sm2_shader sm;
    int translated;                     /* sm holds valid GLSL */
    int failed_logged;
    int skinned;                        /* declares blend weights or indices (usage 1 or 2) */
} d3d9_shader;

/* ---- device state ------------------------------------------------------- */

typedef struct d3d9_device {
    GPTR parent;
    uint32_t adapter, devtype, hwnd, behavior;
    uint32_t pp[14];                    /* D3DPRESENT_PARAMETERS, guest word order */
    uint32_t fvf;
    uint32_t rs[256];                   /* D3DRENDERSTATETYPE */
    uint32_t tss[8][33];                /* D3DTEXTURESTAGESTATETYPE per stage */
    uint32_t samp[16][14];              /* D3DSAMPLERSTATETYPE per sampler */
    uint32_t viewport[6];               /* X, Y, Width, Height, MinZ bits, MaxZ bits */
    uint32_t vs, ps;                    /* shader objects, guest addresses */
    uint32_t decl;                      /* vertex declaration object */
    uint32_t textures[16];
    uint32_t rt[4];
    uint32_t depth;
    uint32_t backbuffer;                /* surface object for the back buffer */
    uint32_t streams[16][3];            /* buffer, offset, stride */
    uint32_t indices, indices_format;
    float vsc[256][4];
    float psc[224][4];
    int vsb[16], psb[16];               /* bool constants, set by SetVertexShaderConstantB */
    uint32_t scene_depth;
    int gpu;                            /* a renderer context exists for this device */
    int32_t scissor_rc[4];              /* RECT from SetScissorRect: left, top, right, bottom */
    int scissor_set;                    /* 0: the rect is the whole target */
} d3d9_device;

extern int d3d9_frame_draws;   /* draws since the last Present (debug bisect) */
extern int d3d9_pixel_watch;   /* nonzero during the frame VT3_PIXEL_AFTER picks (d3d9_draw.c) */

void d3d9_device_free(d3d9_device *d);

/* ---- GPU glue (d3d9_gpu.c) ----------------------------------------------- */

int d3d9_gpu_init(uint32_t width, uint32_t height);   /* 0 on success, -1 when no renderer */
/* Bring up the window and choose a back buffer at the display's aspect. 0 on success. */
int d3d9_gpu_prepare(uint32_t req_w, uint32_t req_h, uint32_t *out_w, uint32_t *out_h);
int d3d9_gpu_ok(void);
void d3d9_gpu_begin(void);
void d3d9_gpu_clear(uint32_t argb, int do_color, int do_depth, float depth);
void d3d9_gpu_set_scissor(int enable, int x, int y, int w, int h);
void d3d9_gpu_present(void);
vt3_tex d3d9_gpu_tex_create(uint32_t w, uint32_t h, uint32_t fmt);
vt3_tex d3d9_gpu_rt_create(uint32_t w, uint32_t h, uint32_t fmt);                   /* a render target texture; 0 if refused */
void d3d9_gpu_rt_bind(vt3_tex t);                                     /* draws and clears go to t; 0: back buffer */
void d3d9_gpu_blit(vt3_tex dst, vt3_rect dr, vt3_tex src, vt3_rect sr, int nearest);
vt3_tex d3d9_surface_target(GPTR surf);                               /* renderer target of a surface; 0: back buffer */
int d3d9_target_missing(GPTR surf);                                    /* a render target that the renderer cannot make */
void d3d9_gpu_tex_destroy(vt3_tex t);
void d3d9_gpu_tex_upload(vt3_tex t, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt);
vt3_tex d3d9_gpu_cube_create(uint32_t size, uint32_t fmt);           /* 0 when the renderer refuses */
void d3d9_gpu_cube_upload(vt3_tex t, uint32_t face, const uint8_t *src, uint32_t pitch, uint32_t size,
                          uint32_t fmt);
/* One draw call, as the device methods describe it. */
typedef struct d3d9_draw_args {
    uint32_t prim;                      /* D3DPRIMITIVETYPE */
    uint32_t prim_count;
    int indexed;                        /* 1: indices from the bound index buffer or up_indices */
    uint32_t start;                     /* start vertex (plain) or start index (indexed) */
    int32_t base_vertex;                /* indexed only */
    uint32_t min_vertex, num_vertices;  /* indexed only */
    const void *up_vertices;            /* DrawPrimitiveUP: user vertex data, or NULL */
    uint32_t up_stride;
    const void *up_indices;             /* DrawIndexedPrimitiveUP: user indices, or NULL */
    uint32_t up_index_format;           /* D3DFMT_INDEX16 or _INDEX32 */
} d3d9_draw_args;

/* Issues one draw. Returns an HRESULT. Unsupported draws are skipped with a log line. */
HRESULT d3d9_draw(d3d9_device *d, const d3d9_draw_args *a);

/* Device lookup for host objects. NULL unless the guest pointer is a device. */
d3d9_device *d3d9_device_of(GPTR dev);

/* Creation helpers (d3d9_resource.c). Each returns a guest object, or 0 on failure. */
GPTR d3d9_create_texture_obj(GPTR dev, uint32_t cube, uint32_t w, uint32_t h, uint32_t levels,
                             uint32_t fmt, uint32_t usage, uint32_t pool);
GPTR d3d9_create_surface_obj(GPTR dev, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage,
                             uint32_t pool, int storage, int backbuffer);
GPTR d3d9_create_buffer_obj(GPTR dev, uint32_t kind, uint32_t length, uint32_t usage, uint32_t fvf_or_fmt,
                            uint32_t pool);
GPTR d3d9_create_decl_obj(GPTR dev, GPTR elements);
GPTR d3d9_create_shader_obj(GPTR dev, int is_ps, GPTR byte_code);

/* Shared with d3d9_resource.c and d3d9_draw.c. */
d3d9_surface *d3d9_surface_of(GPTR obj);
d3d9_texture *d3d9_texture_of(GPTR obj);
d3d9_buffer *d3d9_buffer_of(GPTR obj);

/* D3DFORMAT, D3DERR, HRESULT and enum values used here. SDK values; see tests/sdk_layout_check.c. */
#ifndef E_NOINTERFACE
#define E_NOINTERFACE 0x80004002u
#endif
#define E_OUTOFMEMORY_D3D9 0x8007000Eu
#define D3DFMT_CHK_X8R8G8B8 22u
#define D3DFMT_CHK_A8R8G8B8 21u
#define D3DFMT_CHK_R5G6B5   23u
#define D3DFMT_CHK_A1R5G5B5 25u
#define D3DFMT_CHK_A8       28u
#define D3DFMT_CHK_D24S8    75u
#define D3DFMT_CHK_INDEX16  101u
#define D3DFMT_CHK_INDEX32  102u
#define D3DFMT_CHK_G16R16F  112u
#define D3DFMT_CHK_A16B16G16R16F 113u
#define D3DFMT_CHK_DXT1     0x31545844u
#define D3DFMT_CHK_DXT3     0x33545844u
#define D3DFMT_CHK_DXT5     0x35545844u
#define D3DERR_CHK_NOTAVAILABLE 0x8876086Au
#define D3DERR_CHK_INVALIDCALL  0x8876086Cu
#define D3DDEVTYPE_CHK_HAL      1u
#define D3DRTYPE_CHK_SURFACE 1u
#define D3DRTYPE_CHK_VOLUME 2u
#define D3DRTYPE_CHK_TEXTURE 3u
#define D3DRTYPE_CHK_VOLUMETEXTURE 4u
#define D3DRTYPE_CHK_CUBETEXTURE 5u
#define D3DRTYPE_CHK_VERTEXBUFFER 6u
#define D3DRTYPE_CHK_INDEXBUFFER 7u
#define D3DRS_CHK_SCISSORTESTENABLE 174u

#endif /* VT3_SHIM_D3D9_INTERNAL_H */
