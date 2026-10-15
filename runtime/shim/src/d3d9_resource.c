/*
 * D3D9 resources: textures (2D and cube), surfaces, vertex and index buffers,
 * vertex declarations and shaders. Storage lives in the guest heap, because Lock
 * hands the game a guest pointer it writes into. Textures keep a GL copy of level 0
 * through the GPU glue, when a renderer is present.
 */
#include "d3d9_internal.h"
#include "vt3/vt3_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SURF_DESC_SIZE 32      /* D3DSURFACE_DESC */
#define LOCKED_RECT_SIZE 8     /* D3DLOCKED_RECT */
#define VB_DESC_SIZE 24        /* D3DVERTEXBUFFER_DESC */
#define IB_DESC_SIZE 20        /* D3DINDEXBUFFER_DESC */
#define D3DFMT_CHK_VERTEXDATA 100u
#define D3DPOOL_CHK_DEFAULT 0u

d3d9_texture *d3d9_texture_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_TEXTURE);
    if (!h) h = d3d9_host_of(obj, D3D9_KIND_CUBETEXTURE);
    return h ? (d3d9_texture *)h->data : NULL;
}

d3d9_surface *d3d9_surface_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_SURFACE);
    return h ? (d3d9_surface *)h->data : NULL;
}

d3d9_buffer *d3d9_buffer_of(GPTR obj)
{
    d3d9_host *h = d3d9_host_of(obj, D3D9_KIND_VB);
    if (!h) h = d3d9_host_of(obj, D3D9_KIND_IB);
    return h ? (d3d9_buffer *)h->data : NULL;
}

static uint32_t mip_count(uint32_t w, uint32_t h)
{
    uint32_t m = w > h ? w : h, n = 1;
    while (m > 1) { m >>= 1; n++; }
    return n;
}

static void write_surface_desc(GPTR out, uint32_t fmt, uint32_t type, uint32_t usage, uint32_t pool,
                               uint32_t w, uint32_t h)
{
    uint8_t *p = d3d9_gmem(out, SURF_DESC_SIZE);
    if (!p) return;
    memset(p, 0, SURF_DESC_SIZE);
    d3d9_wr(out, 0, fmt);
    d3d9_wr(out, 4, type);
    d3d9_wr(out, 8, usage);
    d3d9_wr(out, 12, pool);
    d3d9_wr(out, 16, 0);     /* multisample type */
    d3d9_wr(out, 20, 0);     /* multisample quality */
    d3d9_wr(out, 24, w);
    d3d9_wr(out, 28, h);
}

/* ---- creation (used by d3d9_device.c) ------------------------------------ */

/* A texture with storage for every face and level. Returns 0 when storage cannot be made. */
GPTR d3d9_create_texture_obj(GPTR dev, uint32_t cube, uint32_t w, uint32_t h, uint32_t levels,
                             uint32_t fmt, uint32_t usage, uint32_t pool)
{
    if (w == 0 || h == 0) return 0;
    uint32_t faces = cube ? 6u : 1u;
    uint32_t maxl = mip_count(w, h);
    if (levels == 0 || levels > maxl) levels = maxl;
    if (levels > D3D9_MAX_LEVELS) levels = D3D9_MAX_LEVELS;
    d3d9_texture *t = calloc(1, sizeof *t);
    if (!t) return 0;
    t->fmt = fmt;
    t->usage = usage;
    t->pool = pool;
    t->w = w;
    t->h = h;
    t->levels = levels;
    t->faces = faces;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t lw = w >> l, lh = h >> l;
            if (lw == 0) lw = 1;
            if (lh == 0) lh = 1;
            uint32_t pitch = 0;
            uint32_t bytes = d3d9_level_layout(fmt, lw, lh, &pitch);
            if (bytes == 0) {
                VT3_WARN("CreateTexture: format 0x%x has no layout (%ux%u); texture not created", (unsigned)fmt,
                         (unsigned)w, (unsigned)h);
                goto fail;
            }
            GPTR data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
            if (!data) goto fail;
            t->lv[f][l].data = data;
            t->lv[f][l].w = lw;
            t->lv[f][l].h = lh;
            t->lv[f][l].pitch = pitch;
            t->lv[f][l].bytes = bytes;
        }
    }
    if (!cube) t->gl = (usage & D3DUSAGE_RENDERTARGET_CHK) ? d3d9_gpu_rt_create(w, h, fmt) : d3d9_gpu_tex_create(w, h, fmt);
    else t->gl = d3d9_gpu_cube_create(w, fmt);
    GPTR obj = d3d9_obj_new(cube ? D3D9_KIND_CUBETEXTURE : D3D9_KIND_TEXTURE, t, dev);
    if (!obj) goto fail;
    return obj;
fail:
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < levels; l++)
            if (t->lv[f][l].data) HeapFree(GetProcessHeap(), 0, t->lv[f][l].data);
    free(t);
    return 0;
}

GPTR d3d9_create_surface_obj(GPTR dev, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage,
                             uint32_t pool, int storage, int backbuffer)
{
    d3d9_surface *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    s->usage = usage;
    s->pool = pool;
    s->backbuffer = backbuffer;
    if (storage) {
        uint32_t pitch = 0;
        uint32_t bytes = d3d9_level_layout(fmt, w, h, &pitch);
        if (bytes == 0) { free(s); return 0; }
        s->data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
        if (!s->data) { free(s); return 0; }
        s->pitch = pitch;
    }
    GPTR obj = d3d9_obj_new(D3D9_KIND_SURFACE, s, dev);
    if (!obj) {
        if (s->data) HeapFree(GetProcessHeap(), 0, s->data);
        free(s);
        return 0;
    }
    return obj;
}

GPTR d3d9_create_buffer_obj(GPTR dev, uint32_t kind, uint32_t length, uint32_t usage, uint32_t fvf_or_fmt,
                            uint32_t pool)
{
    if (length == 0) return 0;
    d3d9_buffer *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->length = length;
    b->usage = usage;
    b->fvf_or_fmt = fvf_or_fmt;
    b->pool = pool;
    b->data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, length);
    if (!b->data) { free(b); return 0; }
    GPTR obj = d3d9_obj_new(kind, b, dev);
    if (!obj) {
        HeapFree(GetProcessHeap(), 0, b->data);
        free(b);
        return 0;
    }
    return obj;
}

static uint32_t elem_size(uint32_t type)
{
    switch (type) {
    case 0: return 4;    /* FLOAT1 */
    case 1: return 8;    /* FLOAT2 */
    case 2: return 12;   /* FLOAT3 */
    case 3: return 16;   /* FLOAT4 */
    case 4: return 4;    /* D3DCOLOR */
    case 5: return 4;    /* UBYTE4 */
    case 6: return 4;    /* SHORT2 */
    case 7: return 8;    /* SHORT4 */
    case 8: return 4;    /* UBYTE4N */
    case 9: return 4;    /* SHORT2N */
    case 10: return 8;   /* SHORT4N */
    case 11: return 4;   /* USHORT2N */
    case 12: return 8;   /* USHORT4N */
    case 13: return 4;   /* UDEC3 */
    case 14: return 4;   /* DEC3N */
    case 15: return 4;   /* FLOAT16_2 */
    case 16: return 8;   /* FLOAT16_4 */
    default: return 0;
    }
}

GPTR d3d9_create_decl_obj(GPTR dev, GPTR elements)
{
    d3d9_decl *dc = calloc(1, sizeof *dc);
    if (!dc) return 0;
    for (uint32_t i = 0; i < 65; i++) {
        const uint8_t *e = d3d9_gmem(elements + 8u * i, 8);
        if (!e) { free(dc); return 0; }
        memcpy(dc->el[i], e, 8);
        uint32_t stream = e[0] | (e[1] << 8), offset = e[2] | (e[3] << 8), type = e[4];
        if (stream == 0xFF) {   /* D3DDECL_END */
            dc->n = i;
            break;
        }
        if (stream >= 16) { free(dc); return 0; }
        uint32_t end = offset + elem_size(type);
        if (end > dc->stride[stream]) dc->stride[stream] = end;
        if (i == 64) { free(dc); return 0; }
    }
    GPTR obj = d3d9_obj_new(D3D9_KIND_VDECL, dc, dev);
    if (!obj) free(dc);
    return obj;
}

GPTR d3d9_create_shader_obj(GPTR dev, int is_ps, GPTR byte_code)
{
    uint32_t n = 0, cap = 256;
    uint32_t *tok = malloc(cap * sizeof *tok);
    if (!tok) return 0;
    for (;;) {
        uint32_t t = d3d9_rd(byte_code, 4 * n);
        if (!d3d9_gmem(byte_code + 4 * n, 4) || n > 65536) { free(tok); return 0; }
        if (n == cap) {
            cap *= 2;
            uint32_t *nt = realloc(tok, cap * sizeof *tok);
            if (!nt) { free(tok); return 0; }
            tok = nt;
        }
        tok[n++] = t;
        if (t == 0x0000FFFFu) break;
    }
    d3d9_shader *sh = calloc(1, sizeof *sh);
    if (!sh) { free(tok); return 0; }
    sh->is_ps = is_ps;
    sh->ntok = n;
    sh->tok = tok;
    for (uint32_t i = 1; i < n && tok[i] != 0x0000FFFFu; ) {   /* dcl instructions: usage in the first operand */
        uint32_t op = tok[i] & 0xFFFFu;
        if (op == 31 && i + 1 < n && ((tok[i + 1] & 0x1Fu) == 1 || (tok[i + 1] & 0x1Fu) == 2)) sh->skinned = 1;
        i += ((tok[i] >> 24) & 0xFu) + 1;
    }
    if (getenv("VT3_TRACE_SHADERS")) VT3_INFO("shader created: %s, %u tokens, skinned %d", is_ps ? "ps" : "vs", (unsigned)n, sh->skinned);
    if (vt3_sm2_translate_ex(tok, n, &sh->sm, vt3_gfx_glsl_flags()) == 0) {
        sh->translated = 1;
    } else {
        VT3_WARN("%s shader not translated: %s", is_ps ? "pixel" : "vertex", sh->sm.error);
        sh->failed_logged = 1;
    }
    GPTR obj = d3d9_obj_new(is_ps ? D3D9_KIND_PSHADER : D3D9_KIND_VSHADER, sh, dev);
    if (!obj) {
        vt3_sm2_free(&sh->sm);
        free(tok);
        free(sh);
    }
    if (obj && getenv("VT3_DUMP_SHADERS")) {   /* debug: each bytecode, named by the object address the game sees */
        char path[512];
        snprintf(path, sizeof path, "%s/%s_%08x.bin", getenv("VT3_DUMP_SHADERS"), is_ps ? "ps" : "vs", (unsigned)obj);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(tok, 4, n, f); fclose(f); }
    }
    return obj;
}

/* ---- swap chain ----------------------------------------------------------- */

HRESULT WINAPI IDirect3DSwapChain9_Present(GPTR This, GPTR src_rect, GPTR dst_rect, GPTR dst_window_override,
                                           GPTR dirty_region, DWORD flags)
{
    (void)src_rect; (void)dst_rect; (void)dst_window_override; (void)dirty_region; (void)flags;
    if (!d3d9_host_of(This, D3D9_KIND_SWAPCHAIN)) return D3DERR_CHK_INVALIDCALL;
    d3d9_gpu_present();
    return S_OK;
}

HRESULT WINAPI IDirect3DSwapChain9_GetBackBuffer(GPTR This, UINT iBackBuffer, UINT Type, GPTR ppBackBuffer)
{
    (void)Type;
    d3d9_host *h = d3d9_host_of(This, D3D9_KIND_SWAPCHAIN);
    if (!h || iBackBuffer != 0) return D3DERR_CHK_INVALIDCALL;
    d3d9_device *d = d3d9_device_of(h->parent);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    d3d9_return_obj(ppBackBuffer, d->backbuffer);
    return S_OK;
}

HRESULT WINAPI IDirect3DSwapChain9_GetDisplayMode(GPTR This, GPTR pMode)
{
    if (!d3d9_host_of(This, D3D9_KIND_SWAPCHAIN) || !d3d9_gmem(pMode, 16)) return D3DERR_CHK_INVALIDCALL;
    d3d9_wr(pMode, 0, 640);
    d3d9_wr(pMode, 4, 480);
    d3d9_wr(pMode, 8, 60);
    d3d9_wr(pMode, 12, D3DFMT_CHK_X8R8G8B8);
    return S_OK;
}

HRESULT WINAPI IDirect3DSwapChain9_GetPresentParameters(GPTR This, GPTR pPresentationParameters)
{
    d3d9_host *h = d3d9_host_of(This, D3D9_KIND_SWAPCHAIN);
    if (!h || !d3d9_gmem(pPresentationParameters, 56)) return D3DERR_CHK_INVALIDCALL;
    d3d9_device *d = d3d9_device_of(h->parent);
    if (!d) return D3DERR_CHK_INVALIDCALL;
    for (int i = 0; i < 14; i++) d3d9_wr(pPresentationParameters, (uint32_t)(4 * i), d->pp[i]);
    return S_OK;
}

/* ---- textures -------------------------------------------------------------- */

static HRESULT level_desc(d3d9_texture *t, UINT level, GPTR pDesc)
{
    if (!t || level >= t->levels || !d3d9_gmem(pDesc, SURF_DESC_SIZE)) return D3DERR_CHK_INVALIDCALL;
    d3d9_level *lv = &t->lv[0][level];
    write_surface_desc(pDesc, t->fmt, D3DRTYPE_CHK_TEXTURE, t->usage, t->pool, lv->w, lv->h);
    return S_OK;
}

static HRESULT lock_level(d3d9_texture *t, UINT face, UINT level, GPTR locked, DWORD flags)
{
    (void)flags;
    if (!t || face >= t->faces || level >= t->levels || !d3d9_gmem(locked, LOCKED_RECT_SIZE))
        return D3DERR_CHK_INVALIDCALL;
    d3d9_level *lv = &t->lv[face][level];
    d3d9_wr(locked, 0, lv->pitch);
    d3d9_wr(locked, 4, lv->data);
    return S_OK;
}

static void unlock_level(d3d9_texture *t, UINT face, UINT level)
{
    if (!t || face >= t->faces || level >= t->levels) return;
    if (level != 0 || !t->gl) return;
    d3d9_level *lv = &t->lv[face][0];
    const uint8_t *src = d3d9_gmem(lv->data, lv->bytes);
    static int trace_lock = -1;   /* VT3_TRACE_LOCK=1: each level 0 upload, with the guest code that filled it */
    if (trace_lock < 0) trace_lock = getenv("VT3_TRACE_LOCK") != NULL;
    if (trace_lock) {
        char bt[128];
        vt3s_guest_backtrace(bt, sizeof bt);
        VT3_INFO("unlock gl %u %ux%u fmt 0x%x face %u data 0x%x caller 0x%08x stack %s", (unsigned)t->gl,
                 (unsigned)lv->w, (unsigned)lv->h, (unsigned)t->fmt, (unsigned)face, (unsigned)lv->data,
                 (unsigned)vt3s_guest_caller, bt);
    }
    if (t->faces == 6) d3d9_gpu_cube_upload(t->gl, face, src, lv->pitch, lv->w, t->fmt);
    else if (face == 0) d3d9_gpu_tex_upload(t->gl, src, lv->pitch, lv->w, lv->h, t->fmt);
}

static GPTR surface_for_level(GPTR owner, d3d9_texture *t, UINT face, UINT level, GPTR dev)
{
    d3d9_surface *s = calloc(1, sizeof *s);
    if (!s) return 0;
    d3d9_level *lv = &t->lv[face][level];
    s->owner = owner;
    s->face = face;
    s->level = level;
    s->w = lv->w;
    s->h = lv->h;
    s->fmt = t->fmt;
    s->usage = t->usage;
    s->pool = t->pool;
    s->data = lv->data;
    s->pitch = lv->pitch;
    GPTR obj = d3d9_obj_new(D3D9_KIND_SURFACE, s, dev);
    if (!obj) free(s);
    return obj;
}

DWORD WINAPI IDirect3DTexture9_GetLevelCount(GPTR This)
{
    d3d9_texture *t = d3d9_texture_of(This);
    return t ? t->levels : 0;
}

HRESULT WINAPI IDirect3DTexture9_GetLevelDesc(GPTR This, UINT Level, GPTR pDesc)
{
    return level_desc(d3d9_texture_of(This), Level, pDesc);
}

HRESULT WINAPI IDirect3DTexture9_GetSurfaceLevel(GPTR This, UINT Level, GPTR ppSurfaceLevel)
{
    d3d9_texture *t = d3d9_texture_of(This);
    d3d9_host *h = d3d9_host_of(This, D3D9_KIND_TEXTURE);
    if (!t || !h || Level >= t->levels) {
        vt3s_store_u32(ppSurfaceLevel, 0);
        return D3DERR_CHK_INVALIDCALL;
    }
    GPTR s = surface_for_level(This, t, 0, Level, h->parent);
    if (!s) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppSurfaceLevel, s);
    return S_OK;
}

HRESULT WINAPI IDirect3DTexture9_LockRect(GPTR This, UINT level, GPTR locked_rect, GPTR rect, DWORD flags)
{
    (void)rect;
    return lock_level(d3d9_texture_of(This), 0, level, locked_rect, flags);
}

HRESULT WINAPI IDirect3DTexture9_UnlockRect(GPTR This, UINT Level)
{
    unlock_level(d3d9_texture_of(This), 0, Level);
    return S_OK;
}

DWORD WINAPI IDirect3DCubeTexture9_GetLevelCount(GPTR This)
{
    d3d9_texture *t = d3d9_texture_of(This);
    return t ? t->levels : 0;
}

HRESULT WINAPI IDirect3DCubeTexture9_GetLevelDesc(GPTR This, UINT Level, GPTR pDesc)
{
    return level_desc(d3d9_texture_of(This), Level, pDesc);
}

HRESULT WINAPI IDirect3DCubeTexture9_GetCubeMapSurface(GPTR This, UINT FaceType, UINT Level, GPTR ppCubeMapSurface)
{
    d3d9_texture *t = d3d9_texture_of(This);
    d3d9_host *h = d3d9_host_of(This, D3D9_KIND_CUBETEXTURE);
    if (!t || !h || FaceType >= 6 || Level >= t->levels) {
        vt3s_store_u32(ppCubeMapSurface, 0);
        return D3DERR_CHK_INVALIDCALL;
    }
    GPTR s = surface_for_level(This, t, FaceType, Level, h->parent);
    if (!s) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(ppCubeMapSurface, s);
    return S_OK;
}

HRESULT WINAPI IDirect3DCubeTexture9_LockRect(GPTR This, UINT face, UINT level, GPTR locked_rect, GPTR rect,
                                              DWORD flags)
{
    (void)rect;
    return lock_level(d3d9_texture_of(This), face, level, locked_rect, flags);
}

HRESULT WINAPI IDirect3DCubeTexture9_UnlockRect(GPTR This, UINT FaceType, UINT Level)
{
    unlock_level(d3d9_texture_of(This), FaceType, Level);
    return S_OK;
}

DWORD WINAPI IDirect3DVolumeTexture9_GetLevelCount(GPTR This)
{
    (void)This;
    return 0;
}

HRESULT WINAPI IDirect3DVolumeTexture9_GetVolumeLevel(GPTR This, UINT Level, GPTR ppVolumeLevel)
{
    (void)This; (void)Level;
    vt3s_store_u32(ppVolumeLevel, 0);
    return E_NOTIMPL;   /* volume textures are not implemented; the game does not use them */
}

/* ---- buffers ---------------------------------------------------------------- */

static HRESULT buffer_lock(d3d9_buffer *b, UINT offset, UINT size, GPTR ppbData)
{
    (void)size;
    if (!b || offset > b->length) {
        vt3s_store_u32(ppbData, 0);
        return D3DERR_CHK_INVALIDCALL;
    }
    vt3s_store_u32(ppbData, b->data + offset);
    b->locked = 1;
    return S_OK;
}

static HRESULT buffer_desc(d3d9_buffer *b, GPTR pDesc, uint32_t fmt, uint32_t type, uint32_t size_at)
{
    if (!b || !d3d9_gmem(pDesc, size_at + 4)) return D3DERR_CHK_INVALIDCALL;
    d3d9_wr(pDesc, 0, fmt);
    d3d9_wr(pDesc, 4, type);
    d3d9_wr(pDesc, 8, b->usage);
    d3d9_wr(pDesc, 12, b->pool);
    d3d9_wr(pDesc, size_at, b->length);
    return S_OK;
}

HRESULT WINAPI IDirect3DVertexBuffer9_Lock(GPTR This, UINT OffsetToLock, UINT SizeToLock, GPTR ppbData, DWORD Flags)
{
    (void)Flags;
    return buffer_lock(d3d9_buffer_of(This), OffsetToLock, SizeToLock, ppbData);
}

HRESULT WINAPI IDirect3DVertexBuffer9_Unlock(GPTR This)
{
    d3d9_buffer *b = d3d9_buffer_of(This);
    if (b) b->locked = 0;
    return S_OK;
}

HRESULT WINAPI IDirect3DVertexBuffer9_GetDesc(GPTR This, GPTR pDesc)
{
    d3d9_buffer *b = d3d9_buffer_of(This);
    /* D3DVERTEXBUFFER_DESC: Format, Type, Usage, Pool, Size, FVF */
    if (!b || !d3d9_gmem(pDesc, VB_DESC_SIZE)) return D3DERR_CHK_INVALIDCALL;
    d3d9_wr(pDesc, 16, b->length);
    d3d9_wr(pDesc, 20, b->fvf_or_fmt);
    return buffer_desc(b, pDesc, D3DFMT_CHK_VERTEXDATA, D3DRTYPE_CHK_VERTEXBUFFER, 16);
}

HRESULT WINAPI IDirect3DIndexBuffer9_Lock(GPTR This, UINT OffsetToLock, UINT SizeToLock, GPTR ppbData, DWORD Flags)
{
    (void)Flags;
    return buffer_lock(d3d9_buffer_of(This), OffsetToLock, SizeToLock, ppbData);
}

HRESULT WINAPI IDirect3DIndexBuffer9_Unlock(GPTR This)
{
    d3d9_buffer *b = d3d9_buffer_of(This);
    if (b) b->locked = 0;
    return S_OK;
}

HRESULT WINAPI IDirect3DIndexBuffer9_GetDesc(GPTR This, GPTR pDesc)
{
    d3d9_buffer *b = d3d9_buffer_of(This);
    if (!b) return D3DERR_CHK_INVALIDCALL;
    /* D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size */
    return buffer_desc(b, pDesc, b->fvf_or_fmt, D3DRTYPE_CHK_INDEXBUFFER, 16);
}

/* ---- surfaces ---------------------------------------------------------------- */

HRESULT WINAPI IDirect3DSurface9_GetContainer(GPTR This, GPTR riid, GPTR ppContainer)
{
    (void)riid;
    d3d9_surface *s = d3d9_surface_of(This);
    if (!s) return D3DERR_CHK_INVALIDCALL;
    if (s->owner) {
        d3d9_return_obj(ppContainer, s->owner);
        return S_OK;
    }
    /* A surface that is not part of a texture: its container is the device. */
    d3d9_host *h = d3d9_host_of(This, D3D9_KIND_SURFACE);
    if (!h || !h->parent) return D3DERR_CHK_INVALIDCALL;
    d3d9_return_obj(ppContainer, h->parent);
    return S_OK;
}

HRESULT WINAPI IDirect3DSurface9_GetDesc(GPTR This, GPTR pDesc)
{
    d3d9_surface *s = d3d9_surface_of(This);
    if (!s || !d3d9_gmem(pDesc, SURF_DESC_SIZE)) return D3DERR_CHK_INVALIDCALL;
    write_surface_desc(pDesc, s->fmt, D3DRTYPE_CHK_SURFACE, s->usage, s->pool, s->w, s->h);
    return S_OK;
}

HRESULT WINAPI IDirect3DSurface9_LockRect(GPTR This, GPTR locked_rect, GPTR rect, DWORD flags)
{
    (void)rect; (void)flags;
    d3d9_surface *s = d3d9_surface_of(This);
    if (!s || !d3d9_gmem(locked_rect, LOCKED_RECT_SIZE)) return D3DERR_CHK_INVALIDCALL;
    if (!s->data) {
        d3d9_wr(locked_rect, 0, 0);
        d3d9_wr(locked_rect, 4, 0);
        return D3DERR_CHK_INVALIDCALL;     /* the back buffer has no CPU storage */
    }
    d3d9_wr(locked_rect, 0, s->pitch);
    d3d9_wr(locked_rect, 4, s->data);
    s->locked = 1;
    return S_OK;
}

HRESULT WINAPI IDirect3DSurface9_UnlockRect(GPTR This)
{
    d3d9_surface *s = d3d9_surface_of(This);
    if (!s) return D3DERR_CHK_INVALIDCALL;
    if (s->locked && s->owner) {
        d3d9_texture *t = d3d9_texture_of(s->owner);
        if (t) unlock_level(t, s->face, s->level);
    }
    s->locked = 0;
    return S_OK;
}

HRESULT WINAPI IDirect3DVolume9_GetContainer(GPTR This, GPTR riid, GPTR ppContainer)
{
    (void)This; (void)riid;
    vt3s_store_u32(ppContainer, 0);
    return E_NOTIMPL;
}

/* ---- declarations, shaders, state blocks, queries --------------------------- */

HRESULT WINAPI IDirect3DVertexDeclaration9_GetDeclaration(GPTR This, GPTR arg0, GPTR pNumElements)
{
    d3d9_host *h = d3d9_host_of(This, D3D9_KIND_VDECL);
    if (!h) return D3DERR_CHK_INVALIDCALL;
    d3d9_decl *dc = h->data;
    vt3s_store_u32(pNumElements, dc->n + 1);
    if (arg0) {
        uint8_t *p = d3d9_gmem(arg0, (dc->n + 1) * 8);
        if (!p) return D3DERR_CHK_INVALIDCALL;
        memcpy(p, dc->el, (dc->n + 1) * 8);
    }
    return S_OK;
}

static HRESULT shader_function(GPTR This, uint32_t kind, GPTR pData, GPTR pSizeOfData)
{
    d3d9_host *h = d3d9_host_of(This, kind);
    if (!h) return D3DERR_CHK_INVALIDCALL;
    d3d9_shader *sh = h->data;
    uint32_t bytes = sh->ntok * 4;
    vt3s_store_u32(pSizeOfData, bytes);
    if (pData) {
        uint8_t *p = d3d9_gmem(pData, bytes);
        if (!p) return D3DERR_CHK_INVALIDCALL;
        memcpy(p, sh->tok, bytes);
    }
    return S_OK;
}

HRESULT WINAPI IDirect3DVertexShader9_GetFunction(GPTR This, GPTR arg0, GPTR pSizeOfData)
{
    return shader_function(This, D3D9_KIND_VSHADER, arg0, pSizeOfData);
}

HRESULT WINAPI IDirect3DPixelShader9_GetFunction(GPTR This, GPTR arg0, GPTR pSizeOfData)
{
    return shader_function(This, D3D9_KIND_PSHADER, arg0, pSizeOfData);
}

HRESULT WINAPI IDirect3DStateBlock9_Capture(GPTR This)
{
    return d3d9_host_of(This, D3D9_KIND_STATEBLOCK) ? S_OK : D3DERR_CHK_INVALIDCALL;
}

HRESULT WINAPI IDirect3DStateBlock9_Apply(GPTR This)
{
    return d3d9_host_of(This, D3D9_KIND_STATEBLOCK) ? S_OK : D3DERR_CHK_INVALIDCALL;
}

UINT WINAPI IDirect3DQuery9_GetType(GPTR This)
{
    (void)This;
    return 8;   /* D3DQUERYTYPE_EVENT */
}

DWORD WINAPI IDirect3DQuery9_GetDataSize(GPTR This)
{
    (void)This;
    return 4;
}

HRESULT WINAPI IDirect3DQuery9_Issue(GPTR This, DWORD dwIssueFlags)
{
    (void)dwIssueFlags;
    return d3d9_host_of(This, D3D9_KIND_QUERY) ? S_OK : D3DERR_CHK_INVALIDCALL;
}

HRESULT WINAPI IDirect3DQuery9_GetData(GPTR This, GPTR pData, DWORD dwSize, DWORD dwGetDataFlags)
{
    (void)dwGetDataFlags;
    if (!d3d9_host_of(This, D3D9_KIND_QUERY)) return D3DERR_CHK_INVALIDCALL;
    if (pData && dwSize >= 4) d3d9_wr(pData, 0, 1);   /* events report done at once */
    return S_OK;
}
