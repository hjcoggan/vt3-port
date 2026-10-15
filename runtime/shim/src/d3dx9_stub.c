/*
 * D3DX 9 entry points that are not math. Shader compilation is done by d3dx9_compile.c
 * for the HLSL subset the game uses; a source outside that subset fails with the reason
 * in ppErrorMsgs. File-based compilation and the constant table are not implemented, and
 * their calls fail. D3DXCreateTexture makes a real texture through the device's own path.
 * Every creation call writes an object (a stub when the real one is not available), so
 * the caller never gets NULL from a creation output.
 */
#include "d3d9_internal.h"
#include "d3dx9_compile.h"
#include "vt3/vt3_log.h"
#include "win32/d3dx9.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define E_FAIL_D3DX 0x80004005u

static void note_unavailable(const char *what)
{
    static const char *seen[8];
    static int n;
    for (int i = 0; i < n; i++)
        if (strcmp(seen[i], what) == 0) return;
    if (n < 8) seen[n++] = what;
    VT3_WARN("%s: not available in the native runtime (returns E_FAIL)", what);
}

static void put32(GPTR p, uint32_t v)
{
    if (p) vt3s_store_u32(p, v);
}

/* Copies guest text (up to max bytes, stopping at NUL) for logs. Controls become '.'. */
static size_t guest_text(GPTR p, uint32_t max, char *dst, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap && n < max) {
        const uint8_t *q = d3d9_gmem(p + (GPTR)n, 1);
        if (!q || *q == 0) break;
        uint8_t c = *q;
        dst[n++] = (c == '\n' || c == '\r') ? '|' : (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    dst[n] = 0;
    return n;
}

/* With VT3_HLSL_DUMP set, writes each source passed to D3DXCompileShader to that
 * directory as d3dx_src_NN.hlsl, for analysis. */
static void dump_source(GPTR src, UINT len)
{
    static unsigned index;
    const char *dir = getenv("VT3_HLSL_DUMP");
    const uint8_t *s = d3d9_gmem(src, len);
    if (!dir || !s) return;
    char path[512];
    snprintf(path, sizeof path, "%s/d3dx_src_%02u.hlsl", dir, index++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(s, 1, len, f);
    fclose(f);
}

/* Each HLSL source the game compiles has a precompiled blob shipped in VT3.exe. These entries
 * map a source (its address in the game's image, its length and its entry point) to the blob
 * the shipped build uses, so the call returns that blob instead of compiling. Only the
 * addresses are kept here; the bytes are read from the game's own image at run time.
 * Matched by constant names and instruction shape, not by hash. Sources with no entry here
 * have no shipped blob: their constant names appear in no blob of any of the three executables. */
typedef struct vt3_shipped {
    uint32_t src, src_len;   /* the HLSL text in the image */
    const char *entry;
    char kind;               /* 'v' or 'p', from the profile */
    uint32_t blob, blob_len; /* the compiled blob in the image */
} vt3_shipped;

static const vt3_shipped k_shipped[] = {
    { 0x0075e948u, 1019u, "debug_ps_main", 'p', 0x006d9130u, 112u },
    { 0x006d01a8u, 207u, "main", 'v', 0x0072f0f8u, 228u },
};

static const vt3_shipped *shipped_for(GPTR src, UINT len, const char *entry, const char *profile)
{
    for (size_t i = 0; i < sizeof k_shipped / sizeof k_shipped[0]; i++) {
        const vt3_shipped *s = &k_shipped[i];
        if (s->src == src && s->src_len == len && !strcmp(s->entry, entry) && profile[0] == s->kind)
            return s;
    }
    return NULL;
}

HRESULT WINAPI D3DXCompileShader(GPTR src_data, UINT data_len, GPTR defines, GPTR include, GPTR function_name,
                                 GPTR profile, DWORD flags, GPTR ppShader, GPTR ppErrorMsgs, GPTR ppConstantTable)
{
    char entry[64], target[32], msg[256];
    guest_text(function_name, sizeof entry - 1, entry, sizeof entry);
    guest_text(profile, sizeof target - 1, target, sizeof target);
    VT3_INFO("D3DXCompileShader: source 0x%08x, %u bytes, entry \"%s\", profile \"%s\", flags 0x%x, defines %s, include %s",
             src_data, (unsigned)data_len, entry, target, (unsigned)flags, defines ? "yes" : "no", include ? "yes" : "no");
    dump_source(src_data, data_len);

    uint32_t rb_n = 0;
    const uint32_t *rb = d3dx_rebuilt_lookup(src_data, data_len, entry, target[0], &rb_n);
    if (rb) {
        uint32_t blob = d3dx_buffer_new(rb, rb_n * 4u);
        VT3_INFO("D3DXCompileShader: %s returns the rebuilt blob (%u bytes)", entry, (unsigned)(rb_n * 4u));
        put32(ppShader, blob ? blob : d3d9_new_stub(D3D9_KIND_D3DXBUFFER, 0));
        put32(ppErrorMsgs, 0);
        if (ppConstantTable) {
            uint32_t table = d3dx_ctable_object(rb, rb_n * 4u);
            put32(ppConstantTable, table ? table : d3d9_new_stub(D3D9_KIND_D3DXCONSTTABLE, 0));
        }
        return S_OK;
    }

    const vt3_shipped *sb = shipped_for(src_data, data_len, entry, target);
    if (sb) {
        const void *bytes = d3d9_gmem(sb->blob, sb->blob_len);
        if (bytes) {
            uint32_t blob = d3dx_buffer_new(bytes, sb->blob_len);
            VT3_INFO("D3DXCompileShader: %s returns the shipped blob at 0x%08x (%u bytes)", entry, sb->blob, sb->blob_len);
            put32(ppShader, blob ? blob : d3d9_new_stub(D3D9_KIND_D3DXBUFFER, 0));
            put32(ppErrorMsgs, 0);
            if (ppConstantTable) put32(ppConstantTable, d3d9_new_stub(D3D9_KIND_D3DXCONSTTABLE, 0));
            return S_OK;
        }
    }

    uint32_t *tok = NULL, ntok = 0;
    int rc;
    const char *src = data_len ? d3d9_gmem(src_data, data_len) : NULL;
    if (!src) {
        snprintf(msg, sizeof msg, "the source is not in guest memory");
        rc = -1;
    } else {
        rc = d3dx_compile_hlsl(src, data_len, entry, target, &tok, &ntok, msg, sizeof msg);
    }
    uint32_t blob = 0;
    if (rc == 0) {
        blob = d3dx_buffer_new(tok, ntok * 4u);
        VT3_INFO("D3DXCompileShader: %s compiled to %u bytes of %s", entry, (unsigned)(ntok * 4u), target);
    } else {
        VT3_WARN("D3DXCompileShader: %s (%s) failed: %s", entry, target, msg);
    }
    free(tok);
    if (!blob) blob = d3d9_new_stub(D3D9_KIND_D3DXBUFFER, 0);
    put32(ppShader, blob);
    put32(ppErrorMsgs, rc == 0 ? 0 : d3dx_buffer_new(msg, (uint32_t)strlen(msg) + 1u));
    if (ppConstantTable) put32(ppConstantTable, d3d9_new_stub(D3D9_KIND_D3DXCONSTTABLE, 0));
    return rc == 0 ? S_OK : E_FAIL_D3DX;
}

HRESULT WINAPI D3DXCompileShaderFromFileA(GPTR filename, GPTR defines, GPTR include, GPTR function_name,
                                          GPTR profile, DWORD flags, GPTR ppShader, GPTR ppErrorMsgs,
                                          GPTR ppConstantTable)
{
    (void)filename; (void)defines; (void)include; (void)function_name; (void)profile; (void)flags;
    note_unavailable("D3DXCompileShaderFromFileA");
    static const char msg[] = "file-based shader compilation is not available in the native runtime";
    put32(ppShader, d3d9_new_stub(D3D9_KIND_D3DXBUFFER, 0));
    put32(ppErrorMsgs, d3dx_buffer_new(msg, sizeof msg));
    put32(ppConstantTable, d3d9_new_stub(D3D9_KIND_D3DXCONSTTABLE, 0));
    return E_FAIL_D3DX;
}

HRESULT WINAPI D3DXCreateTexture(GPTR device, UINT width, UINT height, UINT miplevels, DWORD usage, UINT format,
                                 UINT pool, GPTR texture)
{
    if (!d3d9_device_of(device) || !texture) {
        vt3s_store_u32(texture, 0);
        return D3DERR_CHK_INVALIDCALL;
    }
    GPTR t = d3d9_create_texture_obj(device, 0, width, height, miplevels, format, usage, pool);
    if (!t) t = d3d9_new_stub(D3D9_KIND_TEXTURE, device);
    if (!t) return E_OUTOFMEMORY_D3D9;
    vt3s_store_u32(texture, t);
    return S_OK;
}

/* ---- DDS textures ---------------------------------------------------------------
 * The game's textures are DDS files in guest memory. The header gives the size, the mip count,
 * the format and the face count; the data follows the 128-byte header (4-byte magic plus the
 * 124-byte DDS_HEADER). Mips are stored largest first, and a cube map stores all mips of each
 * face in turn. The loader takes the face count from the data, not from the cube flag: the
 * game's files set the cube flag on single 2-D textures. */
#define D3DX_DEFAULT_U 0xFFFFFFFFu
#define D3DX_FROM_FILE_U 0xFFFFFFFDu
#define DDS_MAGIC 0x20534444u          /* "DDS " */
#define DDS_TOTAL_HEADER 128u
#define DDS_MAX_MIPS 32u
#define DDSD_MIPMAPCOUNT 0x00020000u
#define DDPF_ALPHAPIXELS 0x00000001u
#define DDPF_ALPHA 0x00000002u
#define DDPF_FOURCC 0x00000004u
#define DDPF_RGB 0x00000040u
#define DDSCAPS2_CUBEMAP 0x00000200u
#define DDS_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DDS_FCC_DXT1 DDS_FOURCC('D', 'X', 'T', '1')
#define DDS_FCC_DXT3 DDS_FOURCC('D', 'X', 'T', '3')
#define DDS_FCC_DXT5 DDS_FOURCC('D', 'X', 'T', '5')
#define DDS_FCC_DX10 DDS_FOURCC('D', 'X', '1', '0')
#define DDS_FCC_D3DFMT_A16B16G16R16F 113u   /* a D3DFORMAT value used as the FourCC */

typedef struct dds_info {
    uint32_t w, h, mips, faces, fmt;
} dds_info;

typedef struct dds_want {   /* the caller's request; D3DX_DEFAULT and 0 mean "from the file" */
    uint32_t w, h, mips, fmt;
} dds_want;

static uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint32_t mip_dim(uint32_t d, uint32_t level)
{
    uint32_t v = d >> level;
    return v ? v : 1u;
}

/* Reads the header and checks the data. Returns 0, or -1 with *why set. */
static int dds_read(const uint8_t *d, uint32_t n, dds_info *o, const char **why)
{
    if (n < DDS_TOTAL_HEADER || le32(d) != DDS_MAGIC || le32(d + 4) != 124u) {
        *why = "not a DDS file (other image formats are not supported)";
        return -1;
    }
    uint32_t flags = le32(d + 8);
    o->h = le32(d + 12);
    o->w = le32(d + 16);
    o->mips = (flags & DDSD_MIPMAPCOUNT) ? le32(d + 28) : 1u;
    if (o->mips == 0) o->mips = 1;
    if (o->w == 0 || o->h == 0) { *why = "DDS size is zero"; return -1; }
    if (o->mips > DDS_MAX_MIPS) { *why = "DDS mip count is out of range"; return -1; }
    if (le32(d + 76) != 32u) { *why = "DDS pixel format header has the wrong size"; return -1; }
    uint32_t pflags = le32(d + 80), fcc = le32(d + 84), bits = le32(d + 88);
    uint32_t rmask = le32(d + 92), gmask = le32(d + 96), bmask = le32(d + 100), amask = le32(d + 104);
    if (pflags & DDPF_FOURCC) {
        switch (fcc) {
        case DDS_FCC_DXT1: o->fmt = D3DFMT_CHK_DXT1; break;
        case DDS_FCC_DXT3: o->fmt = D3DFMT_CHK_DXT3; break;
        case DDS_FCC_DXT5: o->fmt = D3DFMT_CHK_DXT5; break;
        case DDS_FCC_D3DFMT_A16B16G16R16F: o->fmt = D3DFMT_CHK_A16B16G16R16F; break;
        case DDS_FCC_DX10: *why = "DX10 DDS headers are not supported"; return -1;
        default: *why = "unsupported DDS FourCC"; return -1;
        }
    } else if (pflags & DDPF_RGB) {
        if (bits != 32u || rmask != 0x00ff0000u || gmask != 0x0000ff00u || bmask != 0x000000ffu) {
            *why = "only 32-bit ARGB and XRGB DDS files are supported";
            return -1;
        }
        if ((pflags & DDPF_ALPHAPIXELS) && amask == 0xff000000u) o->fmt = D3DFMT_CHK_A8R8G8B8;
        else if (amask == 0) o->fmt = D3DFMT_CHK_X8R8G8B8;
        else { *why = "unsupported DDS alpha mask"; return -1; }
    } else if ((pflags & DDPF_ALPHA) && bits == 8u && amask == 0xffu) {
        o->fmt = D3DFMT_CHK_A8;
    } else {
        *why = "unsupported DDS pixel format";
        return -1;
    }
    /* Faces: all six if the data holds six, one if it holds one. */
    uint64_t one_face = 0;
    for (uint32_t l = 0; l < o->mips; l++) {
        uint32_t pitch = 0;
        uint32_t bytes = d3d9_level_layout(o->fmt, mip_dim(o->w, l), mip_dim(o->h, l), &pitch);
        if (bytes == 0) { *why = "DDS format has no layout"; return -1; }
        one_face += bytes;
    }
    uint64_t have = (uint64_t)n - DDS_TOTAL_HEADER;
    uint32_t cube_flag = (le32(d + 112) & DDSCAPS2_CUBEMAP) ? 6u : 1u;
    if (cube_flag == 6u && have >= 6u * one_face) o->faces = 6u;
    else if (have >= one_face) o->faces = 1u;
    else { *why = "DDS data is shorter than the header says"; return -1; }
    return 0;
}

/* Creates a texture (a cube texture when cube is set) from DDS data in guest memory, copies its
 * faces and mips into the texture's storage, and uploads the first level to the renderer. The
 * caller gets the object with one reference. Returns 0, or -1 with *why set. */
static int dds_texture(GPTR device, GPTR src, UINT size, int cube, const dds_want *want, UINT usage, UINT pool,
                       GPTR *out, const char **why)
{
    const uint8_t *d = size ? d3d9_gmem(src, size) : NULL;
    if (!d) { *why = "the source data is not in guest memory"; return -1; }
    dds_info f;
    if (dds_read(d, size, &f, why) < 0) return -1;
    if (cube && f.faces != 6) { *why = "the file holds one face, not a cube map of six"; return -1; }
    if (!cube && f.faces != 1) { *why = "the file holds a cube map: use D3DXCreateCubeTextureFromFileInMemory"; return -1; }
    uint32_t levels = f.mips;
    if (want) {
        if (want->w != 0 && want->w != D3DX_DEFAULT_U && want->w != f.w) {
            *why = "the requested width differs from the file (resizing is not implemented)";
            return -1;
        }
        if (want->h != 0 && want->h != D3DX_DEFAULT_U && want->h != f.h) {
            *why = "the requested height differs from the file (resizing is not implemented)";
            return -1;
        }
        if (want->mips != 0 && want->mips != D3DX_DEFAULT_U && want->mips != D3DX_FROM_FILE_U && want->mips < levels)
            levels = want->mips;
        if (want->fmt != 0 && want->fmt != D3DX_FROM_FILE_U && want->fmt != f.fmt)
            VT3_INFO("D3DX: requested format %u is not converted; the texture keeps the file's format %u",
                     (unsigned)want->fmt, (unsigned)f.fmt);
    }
    GPTR obj = d3d9_create_texture_obj(device, cube ? 1u : 0u, f.w, f.h, levels, f.fmt, usage, pool);
    if (!obj) { *why = "no memory for the texture"; return -1; }
    d3d9_texture *t = d3d9_texture_of(obj);
    uint32_t off = DDS_TOTAL_HEADER;
    for (uint32_t face = 0; face < f.faces; face++) {
        for (uint32_t l = 0; l < f.mips; l++) {
            uint32_t pitch = 0;
            uint32_t bytes = d3d9_level_layout(f.fmt, mip_dim(f.w, l), mip_dim(f.h, l), &pitch);
            if (l < t->levels) {
                d3d9_level *lv = &t->lv[face][l];
                void *dst = d3d9_gmem(lv->data, lv->bytes);
                if (dst && bytes <= lv->bytes) memcpy(dst, d + off, bytes);
            }
            off += bytes;
        }
    }
    if (t->gl) {
        for (uint32_t face = 0; face < t->faces; face++) {
            d3d9_level *lv = &t->lv[face][0];
            const uint8_t *src = d3d9_gmem(lv->data, lv->bytes);
            if (t->faces == 6) d3d9_gpu_cube_upload(t->gl, face, src, lv->pitch, lv->w, t->fmt);
            else d3d9_gpu_tex_upload(t->gl, src, lv->pitch, lv->w, lv->h, t->fmt);
        }
    }
    *out = obj;
    return 0;
}

HRESULT WINAPI D3DXCreateTextureFromFileInMemoryEx(GPTR device, GPTR srcdata, UINT srcdatasize, UINT width,
                                                   UINT height, UINT miplevels, DWORD usage, UINT format,
                                                   UINT pool, DWORD filter, DWORD mipfilter, DWORD colorkey,
                                                   GPTR srcinfo, GPTR palette, GPTR texture)
{
    (void)filter; (void)mipfilter; (void)colorkey; (void)srcinfo; (void)palette;
    VT3_INFO("D3DXCreateTextureFromFileInMemoryEx: %u bytes, request %ux%u, %u mips, format %u, usage 0x%x, pool %u",
             (unsigned)srcdatasize, (unsigned)width, (unsigned)height, (unsigned)miplevels, (unsigned)format,
             (unsigned)usage, (unsigned)pool);
    dds_want want = { width, height, miplevels, format };
    GPTR obj = 0;
    const char *why = NULL;
    if (dds_texture(device, srcdata, srcdatasize, 0, &want, usage, pool, &obj, &why) == 0) {
        put32(texture, obj);
        const char *dump_dir = getenv("VT3_DUMP_TEX");   /* the source file as given: <dir>/dds_<gl>.dds */
        d3d9_texture *t = dump_dir ? d3d9_texture_of(obj) : NULL;
        const void *bytes = t ? d3d9_gmem(srcdata, srcdatasize) : NULL;
        if (bytes) {
            char path[1024];
            snprintf(path, sizeof path, "%s/dds_%u.dds", dump_dir, (unsigned)t->gl);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(bytes, 1, srcdatasize, f); fclose(f); }
        }
        return S_OK;
    }
    VT3_WARN("D3DXCreateTextureFromFileInMemoryEx: %s; returning a stub texture", why);
    put32(texture, d3d9_new_stub(D3D9_KIND_TEXTURE, device));
    return E_FAIL_D3DX;
}

HRESULT WINAPI D3DXCreateCubeTextureFromFileInMemory(GPTR device, GPTR srcdata, UINT srcdatasize, GPTR texture)
{
    GPTR obj = 0;
    const char *why = NULL;
    if (dds_texture(device, srcdata, srcdatasize, 1, NULL, 0, 0, &obj, &why) == 0) {
        put32(texture, obj);
        return S_OK;
    }
    VT3_WARN("D3DXCreateCubeTextureFromFileInMemory: %s; returning a stub cube texture", why);
    put32(texture, d3d9_new_stub(D3D9_KIND_CUBETEXTURE, device));
    return E_FAIL_D3DX;
}
