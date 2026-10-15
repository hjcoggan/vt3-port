/*
 * ID3DXConstantTable: the named constants of a shader. The table is read from the CTAB comment
 * that every D3DX-compiled shader carries (constant name, register set, first register, count).
 * SetFloatArray writes the values into the device's constant registers, so a game that sets a
 * shader constant by name reaches the same registers as one that uses SetVertexShaderConstantF.
 */
#include "d3d9_internal.h"
#include "d3dx9_compile.h"
#include "vt3/vt3_log.h"

#include <stdlib.h>
#include <string.h>

#define CTAB_RS_FLOAT4 2u
#define CTAB_RS_SAMPLER 3u
#define CTAB_FOURCC 0x42415443u                  /* "CTAB", little-endian */
#define CTAB_NAME_CAP 64
#define BYTECODE_CAP (1u << 20)

typedef struct ctab_const {
    char name[CTAB_NAME_CAP];
    uint32_t regset, regindex, regcount;
} ctab_const;

typedef struct ctab_data {
    int is_vs;
    uint32_t n;
    ctab_const *c;
} ctab_data;

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

void d3dx_ctab_free(void *data)
{
    ctab_data *t = data;
    if (!t) return;
    free(t->c);
    free(t);
}

/* Reads the constant list of a CTAB payload. p starts at the "CTAB" fourcc. */
static int ctab_read(const uint8_t *p, uint32_t len, ctab_data *t)
{
    const uint32_t base = 4;                         /* offsets are from the struct after the fourcc */
    if (len < base + 28) return -1;
    uint32_t count = rd32(p + base + 12), info = rd32(p + base + 16);
    if (count > 256) count = 256;
    t->c = calloc(count ? count : 1, sizeof *t->c);
    if (!t->c) return -1;
    t->n = 0;
    for (uint32_t k = 0; k < count; k++) {
        uint64_t q = (uint64_t)base + info + 20u * k;
        if (q + 20 > len) break;
        uint32_t name_off = rd32(p + q);
        ctab_const *c = &t->c[t->n++];
        c->regset = rd16(p + q + 4);
        c->regindex = rd16(p + q + 6);
        c->regcount = rd16(p + q + 8);
        uint64_t a = (uint64_t)base + name_off;
        size_t i = 0;
        for (; i + 1 < CTAB_NAME_CAP && a + i < len && p[a + i] != 0; i++) c->name[i] = (char)p[a + i];
        c->name[i] = 0;
    }
    return 0;
}

/* Finds the CTAB comment in shader bytecode (tokens, host copy). A comment token is 0xFFFE in
 * the low word with its payload length in tokens in bits 16..30, followed by the fourcc. */
static ctab_data *ctab_from_code(const uint8_t *code, uint32_t bytes)
{
    ctab_data *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    if (bytes >= 4) t->is_vs = (rd32(code) >> 16) == 0xFFFEu;   /* vs_x_y version tokens are 0xFFFE____ */
    for (uint32_t off = 0; off + 8 <= bytes; off += 4) {
        uint32_t tok = rd32(code + off);
        if ((tok & 0xFFFFu) != 0xFFFEu || rd32(code + off + 4) != CTAB_FOURCC) continue;
        uint32_t words = (tok >> 16) & 0x7FFFu;
        uint32_t avail = bytes - off - 4;
        if (words * 4u > avail || words < 7) break;
        if (ctab_read(code + off + 4, words * 4u, t) != 0) break;
        return t;
    }
    t->n = 0;
    return t;
}

/* Copies the bytecode at a guest pointer, up to its end token, into a host buffer. */
static uint8_t *copy_bytecode(GPTR code, uint32_t *bytes)
{
    uint32_t n = 0;
    while (n + 4 <= BYTECODE_CAP) {
        const uint8_t *w = d3d9_gmem(code + n, 4);
        if (!w) return NULL;
        n += 4;
        if (rd32(w) == 0x0000FFFFu) break;
    }
    uint8_t *buf = malloc(n ? n : 1);
    if (!buf) return NULL;
    memcpy(buf, d3d9_gmem(code, n), n);
    *bytes = n;
    return buf;
}

uint32_t d3dx_ctable_object(const void *code, uint32_t bytes)
{
    ctab_data *t = ctab_from_code((const uint8_t *)code, bytes);
    if (!t) return 0;
    GPTR obj = d3d9_obj_new(D3D9_KIND_D3DXCONSTTABLE, t, 0);
    if (!obj) d3dx_ctab_free(t);
    return obj;
}

HRESULT WINAPI D3DXGetShaderConstantTable(GPTR byte_code, GPTR constant_table)
{
    if (!constant_table) return D3DERR_CHK_INVALIDCALL;
    vt3s_store_u32(constant_table, 0);
    if (!byte_code) return D3DERR_CHK_INVALIDCALL;
    uint32_t bytes = 0;
    uint8_t *code = copy_bytecode(byte_code, &bytes);
    if (!code) return D3DERR_CHK_INVALIDCALL;
    uint32_t obj = d3dx_ctable_object(code, bytes);
    free(code);
    if (!obj) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(constant_table, obj);
    return S_OK;
}

static ctab_data *ctab_of(GPTR This)
{
    d3d9_host *h = d3d9_host_of(This, D3D9_KIND_D3DXCONSTTABLE);
    return h ? h->data : NULL;
}

/* Reads a NUL-terminated guest string into out. Returns 0 when it is unreadable or too long. */
static int guest_name(GPTR p, char *out, size_t cap)
{
    for (size_t i = 0; i < cap; i++) {
        const uint8_t *b = d3d9_gmem(p + (GPTR)i, 1);
        if (!b) return 0;
        out[i] = (char)b[0];
        if (b[0] == 0) return 1;
    }
    return 0;
}

UINT WINAPI ID3DXConstantTable_GetConstantByName(GPTR This, UINT constant, GPTR name)
{
    (void)constant;
    ctab_data *t = ctab_of(This);
    char want[CTAB_NAME_CAP];
    if (!t || !name || !guest_name(name, want, sizeof want)) return 0;
    for (uint32_t i = 0; i < t->n; i++) {
        if (strcmp(t->c[i].name, want) == 0) return i + 1;      /* handles are index + 1; 0 = none */
    }
    return 0;
}

UINT WINAPI ID3DXConstantTable_GetSamplerIndex(GPTR This, UINT hConstant)
{
    ctab_data *t = ctab_of(This);
    if (!t || hConstant == 0 || hConstant > t->n) return 0xFFFFFFFFu;
    const ctab_const *c = &t->c[hConstant - 1];
    return c->regset == CTAB_RS_SAMPLER ? c->regindex : 0xFFFFFFFFu;
}

HRESULT WINAPI ID3DXConstantTable_SetFloatArray(GPTR This, GPTR device, UINT constant, GPTR values, UINT value_count)
{
    ctab_data *t = ctab_of(This);
    d3d9_device *d = d3d9_device_of(device);
    if (!t || !d || constant == 0 || constant > t->n || !values) return D3DERR_CHK_INVALIDCALL;
    const ctab_const *c = &t->c[constant - 1];
    if (c->regset != CTAB_RS_FLOAT4) return D3DERR_CHK_INVALIDCALL;

    /* Floats go to consecutive registers from regindex: a float array is packed four to a register. */
    uint32_t floats = value_count < c->regcount * 4u ? value_count : c->regcount * 4u;
    uint32_t bank_floats = (t->is_vs ? 256u : 224u) * 4u;
    uint32_t first = c->regindex * 4u;
    if (floats == 0 || first + floats > bank_floats) return D3DERR_CHK_INVALIDCALL;
    const float *src = d3d9_gmem(values, floats * 4u);
    if (!src) return D3DERR_CHK_INVALIDCALL;
    float *dst = t->is_vs ? &d->vsc[0][0] : &d->psc[0][0];
    memcpy(dst + first, src, floats * 4u);
    return S_OK;
}
