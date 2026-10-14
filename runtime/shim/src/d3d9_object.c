/*
 * D3D9 COM objects: guest layout, host table, per-kind vtables, IUnknown helpers,
 * Direct3DCreate9. Every vtable slot is a guest address made by vt3shim_fn_token();
 * the recompiler maps it to the stdcall adapter, so a guest call through the slot
 * reaches the shim. Slot order is the SDK's (d3d9_com.h, generated).
 */
#include "d3d9_internal.h"
#include "vt3shim/thunk.h"
#include "vt3/vt3_log.h"

#include <stdlib.h>
#include <string.h>

d3d9_host g_d3d9_host[D3D9_MAX_HOST];

/* Without the recompiler there are no guest addresses for shim functions. The
 * recompiler's loader provides the real definition (tools/recomp/rt/vt3rt.c). */
__attribute__((weak)) uint32_t vt3shim_fn_token(const char *name)
{
    (void)name;
    return 0;
}

void *d3d9_gmem(GPTR p, DWORD bytes)
{
    if (!vt3s_guest_range_ok(p, bytes)) return NULL;
    return vt3_g2h(p);
}

uint32_t d3d9_rd(GPTR p, uint32_t off)
{
    return vt3s_load_u32(p + off);
}

void d3d9_wr(GPTR p, uint32_t off, uint32_t v)
{
    vt3s_store_u32(p + off, v);
}

static int host_index_of(GPTR obj, uint32_t kind)
{
    if (!vt3s_guest_range_ok(obj, D3D9_OBJ_SIZE)) return -1;
    if (d3d9_rd(obj, D3D9_OFF_KIND) != kind) return -1;
    uint32_t idx = d3d9_rd(obj, D3D9_OFF_HOST);
    if (idx >= D3D9_MAX_HOST || g_d3d9_host[idx].kind != kind) return -1;
    return (int)idx;
}

d3d9_host *d3d9_host_of(GPTR obj, uint32_t kind)
{
    int i = host_index_of(obj, kind);
    return i < 0 ? NULL : &g_d3d9_host[i];
}

/* ---- vtables -------------------------------------------------------------- */

#define NAME_IDirect3D9(n) "IDirect3D9_" #n,
#define NAME_IDirect3DDevice9(n) "IDirect3DDevice9_" #n,
#define NAME_IDirect3DSwapChain9(n) "IDirect3DSwapChain9_" #n,
#define NAME_IDirect3DTexture9(n) "IDirect3DTexture9_" #n,
#define NAME_IDirect3DCubeTexture9(n) "IDirect3DCubeTexture9_" #n,
#define NAME_IDirect3DVolumeTexture9(n) "IDirect3DVolumeTexture9_" #n,
#define NAME_IDirect3DVertexBuffer9(n) "IDirect3DVertexBuffer9_" #n,
#define NAME_IDirect3DIndexBuffer9(n) "IDirect3DIndexBuffer9_" #n,
#define NAME_IDirect3DSurface9(n) "IDirect3DSurface9_" #n,
#define NAME_IDirect3DVertexDeclaration9(n) "IDirect3DVertexDeclaration9_" #n,
#define NAME_IDirect3DVertexShader9(n) "IDirect3DVertexShader9_" #n,
#define NAME_IDirect3DPixelShader9(n) "IDirect3DPixelShader9_" #n,
#define NAME_IDirect3DStateBlock9(n) "IDirect3DStateBlock9_" #n,
#define NAME_IDirect3DQuery9(n) "IDirect3DQuery9_" #n,
#define NAME_IDirect3DVolume9(n) "IDirect3DVolume9_" #n,
#define NAME_IDirectSound(n) "IDirectSound_" #n,
#define NAME_IDirectSoundBuffer(n) "IDirectSoundBuffer_" #n,
#define NAME_ID3DXBuffer(n) "ID3DXBuffer_" #n,
#define NAME_ID3DXConstantTable(n) "ID3DXConstantTable_" #n,

static const char *const k_names_d3d9[] = { VT3_IDIRECT3D9_SLOTS(NAME_IDirect3D9) };
static const char *const k_names_dev[] = { VT3_IDIRECT3DDEVICE9_SLOTS(NAME_IDirect3DDevice9) };
static const char *const k_names_swap[] = { VT3_IDIRECT3DSWAPCHAIN9_SLOTS(NAME_IDirect3DSwapChain9) };
static const char *const k_names_tex[] = { VT3_IDIRECT3DTEXTURE9_SLOTS(NAME_IDirect3DTexture9) };
static const char *const k_names_cube[] = { VT3_IDIRECT3DCUBETEXTURE9_SLOTS(NAME_IDirect3DCubeTexture9) };
static const char *const k_names_voltex[] = { VT3_IDIRECT3DVOLUMETEXTURE9_SLOTS(NAME_IDirect3DVolumeTexture9) };
static const char *const k_names_vb[] = { VT3_IDIRECT3DVERTEXBUFFER9_SLOTS(NAME_IDirect3DVertexBuffer9) };
static const char *const k_names_ib[] = { VT3_IDIRECT3DINDEXBUFFER9_SLOTS(NAME_IDirect3DIndexBuffer9) };
static const char *const k_names_surf[] = { VT3_IDIRECT3DSURFACE9_SLOTS(NAME_IDirect3DSurface9) };
static const char *const k_names_vdecl[] = { VT3_IDIRECT3DVERTEXDECLARATION9_SLOTS(NAME_IDirect3DVertexDeclaration9) };
static const char *const k_names_vs[] = { VT3_IDIRECT3DVERTEXSHADER9_SLOTS(NAME_IDirect3DVertexShader9) };
static const char *const k_names_ps[] = { VT3_IDIRECT3DPIXELSHADER9_SLOTS(NAME_IDirect3DPixelShader9) };
static const char *const k_names_sb[] = { VT3_IDIRECT3DSTATEBLOCK9_SLOTS(NAME_IDirect3DStateBlock9) };
static const char *const k_names_query[] = { VT3_IDIRECT3DQUERY9_SLOTS(NAME_IDirect3DQuery9) };
static const char *const k_names_volume[] = { VT3_IDIRECT3DVOLUME9_SLOTS(NAME_IDirect3DVolume9) };
static const char *const k_names_dsound[] = { VT3_IDIRECTSOUND_SLOTS(NAME_IDirectSound) };
static const char *const k_names_dsbuffer[] = { VT3_IDIRECTSOUNDBUFFER_SLOTS(NAME_IDirectSoundBuffer) };
static const char *const k_names_d3dxbuf[] = { VT3_ID3DXBUFFER_SLOTS(NAME_ID3DXBuffer) };
static const char *const k_names_d3dxct[] = { VT3_ID3DXCONSTANTTABLE_SLOTS(NAME_ID3DXConstantTable) };

static GPTR build_vtbl(const char *const *names, size_t n)
{
    GPTR vt = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)(4 * n));
    if (!vt) return 0;
    for (size_t i = 0; i < n; i++) {
        d3d9_wr(vt, (uint32_t)(4 * i), vt3shim_fn_token(names[i]));
    }
    return vt;
}

static GPTR vtbl_for_kind(uint32_t kind)
{
    static GPTR built[D3D9_KIND_LAST + 1];
    if (kind == 0 || kind > D3D9_KIND_LAST) return 0;
    if (built[kind]) return built[kind];
    const char *const *names = NULL;
    size_t n = 0;
#define PICK(k, arr) case k: names = arr; n = sizeof arr / sizeof arr[0]; break;
    switch (kind) {
    PICK(D3D9_KIND_D3D9, k_names_d3d9)
    PICK(D3D9_KIND_DEVICE, k_names_dev)
    PICK(D3D9_KIND_SWAPCHAIN, k_names_swap)
    PICK(D3D9_KIND_TEXTURE, k_names_tex)
    PICK(D3D9_KIND_CUBETEXTURE, k_names_cube)
    PICK(D3D9_KIND_VOLUMETEXTURE, k_names_voltex)
    PICK(D3D9_KIND_VB, k_names_vb)
    PICK(D3D9_KIND_IB, k_names_ib)
    PICK(D3D9_KIND_SURFACE, k_names_surf)
    PICK(D3D9_KIND_VDECL, k_names_vdecl)
    PICK(D3D9_KIND_VSHADER, k_names_vs)
    PICK(D3D9_KIND_PSHADER, k_names_ps)
    PICK(D3D9_KIND_STATEBLOCK, k_names_sb)
    PICK(D3D9_KIND_QUERY, k_names_query)
    PICK(D3D9_KIND_VOLUME, k_names_volume)
    PICK(D3D9_KIND_DSOUND, k_names_dsound)
    PICK(D3D9_KIND_DSBUFFER, k_names_dsbuffer)
    PICK(D3D9_KIND_D3DXBUFFER, k_names_d3dxbuf)
    PICK(D3D9_KIND_D3DXCONSTTABLE, k_names_d3dxct)
    default:
        names = dinput_vtbl_names(kind, &n);
        if (!names) return 0;
        break;
    }
#undef PICK
    built[kind] = build_vtbl(names, n);
    return built[kind];
}

/* ---- objects -------------------------------------------------------------- */

static void free_data(uint32_t kind, void *data)
{
    if (!data) return;
    switch (kind) {
    case D3D9_KIND_DEVICE:
        d3d9_device_free((d3d9_device *)data);
        break;
    case D3D9_KIND_TEXTURE:
    case D3D9_KIND_CUBETEXTURE: {
        d3d9_texture *t = data;
        for (int f = 0; f < D3D9_MAX_FACES; f++)
            for (int l = 0; l < D3D9_MAX_LEVELS; l++)
                if (t->lv[f][l].data) HeapFree(GetProcessHeap(), 0, t->lv[f][l].data);
        d3d9_gpu_tex_destroy(t->gl);
        free(t);
        break;
    }
    case D3D9_KIND_SURFACE: {
        d3d9_surface *s = data;
        if (!s->owner && s->data) HeapFree(GetProcessHeap(), 0, s->data);
        free(s);
        break;
    }
    case D3D9_KIND_VB:
    case D3D9_KIND_IB: {
        d3d9_buffer *b = data;
        if (b->data) HeapFree(GetProcessHeap(), 0, b->data);
        free(b);
        break;
    }
    case D3D9_KIND_VDECL:
        free(data);
        break;
    case D3D9_KIND_DSOUND:
    case D3D9_KIND_DSBUFFER:
        dsound_free(kind, data);
        break;
    case D3D9_KIND_D3DXBUFFER:
    case D3D9_KIND_D3DXCONSTTABLE:
        d3dx_obj_free(kind, data);
        break;
    case D3D9_KIND_DINPUT:
    case D3D9_KIND_DIDEVICE:
        dinput_obj_free(kind, data);
        break;
    case D3D9_KIND_VSHADER:
    case D3D9_KIND_PSHADER: {
        d3d9_shader *s = data;
        vt3_sm2_free(&s->sm);
        free(s->tok);
        free(s);
        break;
    }
    default:
        break;
    }
}

GPTR d3d9_obj_new(uint32_t kind, void *data, GPTR parent)
{
    int slot = -1;
    for (int i = 0; i < D3D9_MAX_HOST; i++) {
        if (g_d3d9_host[i].kind == D3D9_KIND_NONE) { slot = i; break; }
    }
    if (slot < 0) return 0;
    GPTR vt = vtbl_for_kind(kind);
    if (!vt) return 0;
    GPTR obj = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, D3D9_OBJ_SIZE);
    if (!obj) return 0;
    d3d9_wr(obj, D3D9_OFF_VTBL, vt);
    d3d9_wr(obj, D3D9_OFF_REFS, 1);
    d3d9_wr(obj, D3D9_OFF_KIND, kind);
    d3d9_wr(obj, D3D9_OFF_HOST, (uint32_t)slot);
    g_d3d9_host[slot].kind = kind;
    g_d3d9_host[slot].refs = 1;
    g_d3d9_host[slot].guest = obj;
    g_d3d9_host[slot].data = data;
    g_d3d9_host[slot].parent = parent;
    return obj;
}

GPTR d3d9_new_stub(uint32_t kind, GPTR parent)
{
    return d3d9_obj_new(kind, NULL, parent);
}

void d3d9_obj_free(GPTR obj)
{
    int i = host_index_of(obj, d3d9_rd(obj, D3D9_OFF_KIND));
    if (i < 0) return;
    free_data(g_d3d9_host[i].kind, g_d3d9_host[i].data);
    memset(&g_d3d9_host[i], 0, sizeof g_d3d9_host[i]);
    HeapFree(GetProcessHeap(), 0, obj);
}

/* ---- IUnknown and the helpers the generated boilerplate calls ----------- */

static int iid_equals(GPTR riid, const uint8_t *want)
{
    const uint8_t *p = d3d9_gmem(riid, 16);
    return p && memcmp(p, want, 16) == 0;
}

static const uint8_t k_iid_unknown[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };

DWORD d3d9_qi_kind(GPTR This, GPTR riid, GPTR ppv, uint32_t kind)
{
    if (ppv) vt3s_store_u32(ppv, 0);
    if (!d3d9_host_of(This, kind)) return E_NOINTERFACE;
    const uint8_t *own = d3d9_iid_of_kind(kind);
    if (!iid_equals(riid, k_iid_unknown) && !(own && iid_equals(riid, own))) return E_NOINTERFACE;
    if (ppv) vt3s_store_u32(ppv, This);
    d3d9_add_ref(This);
    return S_OK;
}

DWORD d3d9_add_ref(GPTR This)
{
    int i = host_index_of(This, d3d9_rd(This, D3D9_OFF_KIND));
    if (i < 0) return 0;
    uint32_t refs = d3d9_rd(This, D3D9_OFF_REFS) + 1;
    d3d9_wr(This, D3D9_OFF_REFS, refs);
    g_d3d9_host[i].refs = refs;
    return refs;
}

DWORD d3d9_release(GPTR This)
{
    int i = host_index_of(This, d3d9_rd(This, D3D9_OFF_KIND));
    if (i < 0) return 0;
    uint32_t refs = d3d9_rd(This, D3D9_OFF_REFS);
    if (refs == 0) return 0;
    refs--;
    d3d9_wr(This, D3D9_OFF_REFS, refs);
    g_d3d9_host[i].refs = refs;
    if (refs == 0) d3d9_obj_free(This);
    return refs;
}

HRESULT d3d9_get_device(GPTR This, GPTR ppDevice)
{
    int i = host_index_of(This, d3d9_rd(This, D3D9_OFF_KIND));
    if (i < 0) return D3DERR_CHK_INVALIDCALL;
    GPTR dev = g_d3d9_host[i].parent;
    if (!dev) return D3DERR_CHK_INVALIDCALL;
    d3d9_return_obj(ppDevice, dev);
    return S_OK;
}

UINT d3d9_resource_type(uint32_t kind)
{
    switch (kind) {
    case D3D9_KIND_SURFACE: return D3DRTYPE_CHK_SURFACE;
    case D3D9_KIND_VOLUME: return D3DRTYPE_CHK_VOLUME;
    case D3D9_KIND_TEXTURE: return D3DRTYPE_CHK_TEXTURE;
    case D3D9_KIND_VOLUMETEXTURE: return D3DRTYPE_CHK_VOLUMETEXTURE;
    case D3D9_KIND_CUBETEXTURE: return D3DRTYPE_CHK_CUBETEXTURE;
    case D3D9_KIND_VB: return D3DRTYPE_CHK_VERTEXBUFFER;
    case D3D9_KIND_IB: return D3DRTYPE_CHK_INDEXBUFFER;
    default: return 0;
    }
}

void d3d9_return_obj(GPTR out, GPTR obj)
{
    if (!out) return;
    vt3s_store_u32(out, obj);
    if (obj) d3d9_add_ref(obj);
}

/* ---- Direct3DCreate9 ------------------------------------------------------ */

GPTR WINAPI Direct3DCreate9(UINT sdk_version)
{
    (void)sdk_version;   /* the game passes 32 (D3D_SDK_VERSION); other values are accepted */
    GPTR obj = d3d9_obj_new(D3D9_KIND_D3D9, NULL, 0);
    if (!obj) {
        VT3_ERROR("Direct3DCreate9: no free object slot or guest heap");
        return 0;
    }
    VT3_INFO("Direct3DCreate9: IDirect3D9 at 0x%08x", obj);
    return obj;
}

/* ---- d3d9.dll profiling exports (no profiler here) ------------------------ */

void WINAPI D3DPERF_SetOptions(DWORD options)
{
    VT3_DEBUG("D3DPERF_SetOptions 0x%x: ignored", (unsigned)options);
}
