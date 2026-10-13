/* Internal to the D3D9 shim: COM objects, guest memory helpers, device state. */
#ifndef VT3_SHIM_D3D9_INTERNAL_H
#define VT3_SHIM_D3D9_INTERNAL_H

#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "win32/d3d9.h"
#include "win32/d3d9_com.h"
#include "win32/kernel32.h"

#include <stdint.h>

/* Guest object layout. A COM object is 16 bytes of guest memory:
 *   +0  lpVtbl (guest address of the vtable)
 *   +4  reference count
 *   +8  kind (D3D9_KIND_*)
 *   +12 index of the host object in d3d9_host[] */
#define D3D9_OBJ_SIZE 16
#define D3D9_OFF_VTBL 0
#define D3D9_OFF_REFS 4
#define D3D9_OFF_KIND 8
#define D3D9_OFF_HOST 12
enum { D3D9_KIND_NONE = 0, D3D9_KIND_D3D9 = 1, D3D9_KIND_DEVICE = 2 };

#define D3D9_MAX_HOST 64

/* Host-side state for one object. */
typedef struct d3d9_host {
    uint32_t kind;
    uint32_t refs;        /* host copy of the guest count, for the shared Release path */
    GPTR guest;           /* the guest object that owns this entry */
    void *data;           /* D3D9_KIND_DEVICE: d3d9_device; D3D9_KIND_D3D9: NULL */
    GPTR parent;          /* device: the IDirect3D9 that created it */
} d3d9_host;

extern d3d9_host g_d3d9_host[D3D9_MAX_HOST];

/* Guest memory helpers. Pointers are checked against the arena. */
void *d3d9_gmem(GPTR p, DWORD bytes);                 /* NULL when out of range */
uint32_t d3d9_rd(GPTR p, uint32_t off);               /* 0 when out of range */
void d3d9_wr(GPTR p, uint32_t off, uint32_t v);       /* no-op when out of range */

/* Object creation and lookup. */
GPTR d3d9_obj_new(uint32_t kind, GPTR vtbl, void *data, GPTR parent);
d3d9_host *d3d9_host_of(GPTR obj, uint32_t kind);     /* NULL unless kind matches */
void d3d9_obj_free(GPTR obj);

/* Vtables, built once from the thunk tokens for each SDK slot. */
GPTR d3d9_vtbl_d3d9(void);
GPTR d3d9_vtbl_device(void);

/* Device state. Filled by d3d9_device.c. */
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
    uint32_t textures[16];
    uint32_t rt[4];
    uint32_t depth;
    uint32_t streams[16][3];            /* buffer, offset, stride */
    uint32_t indices, indices_format;
    float vsc[256][4];
    float psc[224][4];
    uint32_t scene_depth;
} d3d9_device;

void d3d9_device_free(d3d9_device *d);

/* Shared COM helpers (d3d9_object.c). */
extern const uint8_t d3d9_iid_unknown[16];
extern const uint8_t d3d9_iid_d3d9[16];
extern const uint8_t d3d9_iid_device9[16];
DWORD d3d9_query_interface(GPTR This, GPTR riid, GPTR ppv, const uint8_t *iface_iid);
DWORD d3d9_add_ref(GPTR This);
DWORD d3d9_release(GPTR This);

/* Caps for IDirect3D9::GetDeviceCaps and IDirect3DDevice9::GetDeviceCaps (d3d9_iface.c). */
void d3d9_fill_caps(GPTR caps);

/* D3DFORMAT, D3DERR and HRESULT values used here (SDK values, see tests/sdk_layout_check.c). */
#define D3DFMT_CHK_X8R8G8B8 22u
#define D3DFMT_CHK_A8R8G8B8 21u
#define D3DFMT_CHK_R5G6B5   23u
#define D3DFMT_CHK_A1R5G5B5 25u
#define D3DFMT_CHK_A8       28u
#define D3DFMT_CHK_D24S8    75u
#define D3DFMT_CHK_INDEX16  101u
#define D3DFMT_CHK_G16R16F  112u
#define D3DFMT_CHK_A16B16G16R16F 113u
#define D3DFMT_CHK_DXT1     0x31545844u
#define D3DFMT_CHK_DXT3     0x33545844u
#define D3DFMT_CHK_DXT5     0x35545844u
#ifndef E_NOINTERFACE
#define E_NOINTERFACE 0x80004002u          /* SDK: _HRESULT_TYPEDEF_(0x80004002) */
#endif
#define E_OUTOFMEMORY_D3D9 0x8007000Eu          /* SDK: _HRESULT_TYPEDEF_(0x8007000E) */
#define D3DERR_CHK_NOTAVAILABLE 0x8876086Au
#define D3DERR_CHK_INVALIDCALL  0x8876086Cu
#define D3DDEVTYPE_CHK_HAL      1u

#endif /* VT3_SHIM_D3D9_INTERNAL_H */
