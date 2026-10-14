/*
 * D3DFORMAT helpers: level layout, bytes per pixel, and conversion to 32-bit BGRA
 * (D3DFMT_A8R8G8B8 byte order). DXT1/3/5 are decoded on the CPU, so every texture
 * the renderer holds is a plain 8-bit BGRA or alpha texture.
 */
#include "d3d9_internal.h"

#include <math.h>
#include <string.h>

/* One 16-bit IEEE half at p, as an 8-bit colour channel: clamped to 0..1, rounded. */
static uint8_t half_to_u8(const uint8_t *p)
{
    uint32_t h = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 31u, man = h & 1023u;
    if (exp == 31u) return (sign || man) ? 0 : 255;          /* infinity clamps, NaN is 0 */
    float f = exp == 0 ? ldexpf((float)man, -24) : ldexpf((float)(man | 1024u), (int)exp - 25);
    if (sign) return 0;
    if (f >= 1.0f) return 255;
    return (uint8_t)(f * 255.0f + 0.5f);
}

int d3d9_fmt_is_dxt(uint32_t fmt)
{
    return fmt == D3DFMT_CHK_DXT1 || fmt == D3DFMT_CHK_DXT3 || fmt == D3DFMT_CHK_DXT5;
}

uint32_t d3d9_fmt_bpp(uint32_t fmt)
{
    switch (fmt) {
    case D3DFMT_CHK_X8R8G8B8: case D3DFMT_CHK_A8R8G8B8: case D3DFMT_CHK_D24S8:
    case D3DFMT_CHK_G16R16F:
        return 4;
    case D3DFMT_CHK_R5G6B5: case D3DFMT_CHK_A1R5G5B5: case D3DFMT_CHK_INDEX16:
        return 2;
    case D3DFMT_CHK_A8:
        return 1;
    case D3DFMT_CHK_A16B16G16R16F:
        return 8;
    case D3DFMT_CHK_INDEX32:
        return 4;
    default:
        return 0;
    }
}

uint32_t d3d9_level_layout(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *pitch)
{
    if (w == 0 || h == 0) return 0;
    if (d3d9_fmt_is_dxt(fmt)) {
        uint32_t bs = fmt == D3DFMT_CHK_DXT1 ? 8u : 16u;
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        *pitch = bw * bs;
        return *pitch * bh;
    }
    uint32_t bpp = d3d9_fmt_bpp(fmt);
    if (!bpp) return 0;
    *pitch = w * bpp;
    return *pitch * h;
}

/* ---- DXT block decoding -------------------------------------------------- */

static void rgb565(uint16_t c, uint8_t *b, uint8_t *g, uint8_t *r)
{
    uint32_t r5 = (c >> 11) & 31u, g6 = (c >> 5) & 63u, b5 = c & 31u;
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/* Colour block of a BC1 block (8 bytes at p). force4 selects the 4-colour mode (BC2/BC3). */
static void decode_color(const uint8_t *p, int force4, uint8_t col[4][4], uint8_t *idx)
{
    uint16_t c0 = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t c1 = (uint16_t)(p[2] | (p[3] << 8));
    /* rgb565 writes its outputs in B, G, R order (the BGRA layout). col is R, G, B. */
    rgb565(c0, &col[0][2], &col[0][1], &col[0][0]);
    rgb565(c1, &col[1][2], &col[1][1], &col[1][0]);
    col[0][3] = col[1][3] = 255;
    if (force4 || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            col[2][k] = (uint8_t)((2 * col[0][k] + col[1][k] + 1) / 3);
            col[3][k] = (uint8_t)((col[0][k] + 2 * col[1][k] + 1) / 3);
        }
        col[2][3] = col[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) col[2][k] = (uint8_t)((col[0][k] + col[1][k] + 1) / 2);
        col[2][3] = 255;
        col[3][0] = col[3][1] = col[3][2] = 0;
        col[3][3] = 0;
    }
    uint32_t bits = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    for (int i = 0; i < 16; i++) idx[i] = (uint8_t)((bits >> (2 * i)) & 3u);
}

static void put_pixel(uint8_t *dst, uint32_t pitch_px, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      const uint8_t rgba[4])
{
    if (x >= w || y >= h) return;
    uint8_t *d = dst + (size_t)(y * pitch_px + x) * 4;
    d[0] = rgba[2];   /* B */
    d[1] = rgba[1];   /* G */
    d[2] = rgba[0];   /* R */
    d[3] = rgba[3];   /* A */
}

static void decode_dxt(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst)
{
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint32_t bs = fmt == D3DFMT_CHK_DXT1 ? 8u : 16u;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *blk = src + by * pitch + bx * bs;
            uint8_t col[4][4], idx[16], alpha[16];
            if (fmt == D3DFMT_CHK_DXT1) {
                decode_color(blk, 0, col, idx);
                for (int i = 0; i < 16; i++) alpha[i] = col[idx[i]][3];
            } else {
                /* Explicit alpha (DXT3: 4 bits per pixel) or interpolated (DXT5: 3-bit indices). */
                if (fmt == D3DFMT_CHK_DXT3) {
                    uint64_t a = 0;
                    for (int k = 0; k < 8; k++) a |= (uint64_t)blk[k] << (8 * k);
                    for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)(((a >> (4 * i)) & 15u) * 17u);
                } else {
                    uint8_t a0 = blk[0], a1 = blk[1];
                    uint8_t ap[8];
                    ap[0] = a0;
                    ap[1] = a1;
                    if (a0 > a1) {
                        for (int k = 1; k <= 6; k++)
                            ap[k + 1] = (uint8_t)(((7 - k) * a0 + k * a1 + 3) / 7);
                    } else {
                        for (int k = 1; k <= 4; k++)
                            ap[k + 1] = (uint8_t)(((5 - k) * a0 + k * a1 + 2) / 5);
                        ap[6] = 0;
                        ap[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int k = 0; k < 6; k++) bits |= (uint64_t)blk[2 + k] << (8 * k);
                    for (int i = 0; i < 16; i++) alpha[i] = ap[(bits >> (3 * i)) & 7u];
                }
                decode_color(blk + 8, 1, col, idx);
            }
            for (int i = 0; i < 16; i++) {
                uint8_t rgba[4] = { col[idx[i]][0], col[idx[i]][1], col[idx[i]][2], alpha[i] };
                put_pixel(dst, w, bx * 4 + (uint32_t)(i & 3), by * 4 + (uint32_t)(i >> 2), w, h, rgba);
            }
        }
    }
}

int d3d9_to_bgra(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst)
{
    if (!src || !dst || w == 0 || h == 0) return -1;
    if (d3d9_fmt_is_dxt(fmt)) {
        decode_dxt(fmt, src, pitch, w, h, dst);
        return 0;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint8_t *out = dst + (size_t)y * w * 4;
        switch (fmt) {
        case D3DFMT_CHK_A8R8G8B8:
        case D3DFMT_CHK_X8R8G8B8:
            memcpy(out, row, (size_t)w * 4);
            if (fmt == D3DFMT_CHK_X8R8G8B8)
                for (uint32_t x = 0; x < w; x++) out[x * 4 + 3] = 255;
            break;
        case D3DFMT_CHK_A8:
            for (uint32_t x = 0; x < w; x++) {
                out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = 255;
                out[x * 4 + 3] = row[x];
            }
            break;
        case D3DFMT_CHK_R5G6B5:
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = (uint16_t)(row[2 * x] | (row[2 * x + 1] << 8));
                rgb565(c, &out[x * 4 + 0], &out[x * 4 + 1], &out[x * 4 + 2]);
                out[x * 4 + 3] = 255;
            }
            break;
        case D3DFMT_CHK_A1R5G5B5:
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = (uint16_t)(row[2 * x] | (row[2 * x + 1] << 8));
                uint32_t r5 = (c >> 10) & 31u, g5 = (c >> 5) & 31u, b5 = c & 31u;
                out[x * 4 + 0] = (uint8_t)((b5 << 3) | (b5 >> 2));
                out[x * 4 + 1] = (uint8_t)((g5 << 3) | (g5 >> 2));
                out[x * 4 + 2] = (uint8_t)((r5 << 3) | (r5 >> 2));
                out[x * 4 + 3] = (c & 0x8000u) ? 255 : 0;
            }
            break;
        case D3DFMT_CHK_A16B16G16R16F:
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = row + 8 * (size_t)x;   /* R, G, B, A as 16-bit halves */
                out[x * 4 + 0] = half_to_u8(p + 4);
                out[x * 4 + 1] = half_to_u8(p + 2);
                out[x * 4 + 2] = half_to_u8(p + 0);
                out[x * 4 + 3] = half_to_u8(p + 6);
            }
            break;
        default:
            return -1;   /* depth and other float formats are not uploaded */
        }
    }
    return 0;
}
