/*
 * D3D9 COM objects: guest layout, host table, vtables, Direct3DCreate9.
 *
 * Every vtable slot is a guest address made by vt3shim_fn_token(). The recompiler
 * maps that address to the stdcall adapter for the method, so a guest call through
 * the slot reaches the shim. The slot order is the SDK's (d3d9_com.h, generated).
 */
#include "d3d9_internal.h"
#include "win32/kernel32.h"
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

GPTR d3d9_obj_new(uint32_t kind, GPTR vtbl, void *data, GPTR parent)
{
    int slot = -1;
    for (int i = 0; i < D3D9_MAX_HOST; i++) {
        if (g_d3d9_host[i].kind == D3D9_KIND_NONE) { slot = i; break; }
    }
    if (slot < 0) return 0;
    GPTR obj = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, D3D9_OBJ_SIZE);
    if (!obj) return 0;
    d3d9_wr(obj, D3D9_OFF_VTBL, vtbl);
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

void d3d9_device_free(d3d9_device *d)
{
    free(d);     /* host allocation, made with calloc */
}

void d3d9_obj_free(GPTR obj)
{
    int i = host_index_of(obj, d3d9_rd(obj, D3D9_OFF_KIND));
    if (i < 0) return;
    if (g_d3d9_host[i].kind == D3D9_KIND_DEVICE && g_d3d9_host[i].data) {
        d3d9_device_free((d3d9_device *)g_d3d9_host[i].data);
    }
    memset(&g_d3d9_host[i], 0, sizeof g_d3d9_host[i]);
    HeapFree(GetProcessHeap(), 0, obj);   /* guest allocation */
}

/* ---- vtables -------------------------------------------------------------- */

#define D3D9_NAME_D3D9(n) "IDirect3D9_" #n,
#define D3D9_NAME_DEV(n) "IDirect3DDevice9_" #n,
static const char *const k_names_d3d9[] = { VT3_IDIRECT3D9_SLOTS(D3D9_NAME_D3D9) };
static const char *const k_names_dev[] = { VT3_IDIRECT3DDEVICE9_SLOTS(D3D9_NAME_DEV) };

static GPTR build_vtbl(const char *const *names, size_t n)
{
    GPTR vt = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)(4 * n));
    if (!vt) return 0;
    for (size_t i = 0; i < n; i++) {
        d3d9_wr(vt, (uint32_t)(4 * i), vt3shim_fn_token(names[i]));
    }
    return vt;
}

static GPTR g_vtbl_d3d9;
static GPTR g_vtbl_dev;

GPTR d3d9_vtbl_d3d9(void)
{
    if (!g_vtbl_d3d9) g_vtbl_d3d9 = build_vtbl(k_names_d3d9, sizeof k_names_d3d9 / sizeof k_names_d3d9[0]);
    return g_vtbl_d3d9;
}

GPTR d3d9_vtbl_device(void)
{
    if (!g_vtbl_dev) g_vtbl_dev = build_vtbl(k_names_dev, sizeof k_names_dev / sizeof k_names_dev[0]);
    return g_vtbl_dev;
}

/* ---- IUnknown ------------------------------------------------------------- */

/* GUID bytes as they sit in memory (little-endian fields). */
const uint8_t d3d9_iid_unknown[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };
const uint8_t d3d9_iid_d3d9[16] = {          /* {81BDCBCA-64D4-426D-AE8D-AD0147F4275C} */
    0xCA, 0xCB, 0xBD, 0x81, 0xD4, 0x64, 0x6D, 0x42, 0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C };
const uint8_t d3d9_iid_device9[16] = {       /* {D0223B96-BF7A-43FD-92BD-A43B0D82B9EB} */
    0x96, 0x3B, 0x22, 0xD0, 0x7A, 0xBF, 0xFD, 0x43, 0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB };

static int iid_equals(GPTR riid, const uint8_t *want)
{
    const uint8_t *p = d3d9_gmem(riid, 16);
    return p && memcmp(p, want, 16) == 0;
}

DWORD d3d9_query_interface(GPTR This, GPTR riid, GPTR ppv, const uint8_t *iface_iid)
{
    if (ppv) vt3s_store_u32(ppv, 0);
    if (!iid_equals(riid, d3d9_iid_unknown) && !iid_equals(riid, iface_iid)) {
        return E_NOINTERFACE;
    }
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

/* ---- Direct3DCreate9 ------------------------------------------------------ */

GPTR WINAPI Direct3DCreate9(UINT sdk_version)
{
    (void)sdk_version;   /* the game passes 32 (D3D_SDK_VERSION); other values are accepted */
    GPTR vt = d3d9_vtbl_d3d9();
    if (!vt) return 0;
    GPTR obj = d3d9_obj_new(D3D9_KIND_D3D9, vt, NULL, 0);
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
