/*
 * D3DX 9 math. Same conventions as the SDK: row vectors (v * M), matrices stored
 * row-major (_11 first), left-handed unless the name says RH. Matrices and vectors
 * are read from and written to guest memory. FLOAT arguments arrive as DWORD bits.
 */
#include "d3d9_internal.h"
#include "win32/d3dx9.h"

#include <math.h>
#include <string.h>

static float f32(DWORD bits)
{
    union { uint32_t u; float f; } v;
    v.u = bits;
    return v.f;
}

static int rd_f(GPTR p, float *dst, uint32_t n)
{
    const float *q = d3d9_gmem(p, n * 4u);
    if (!q) return 0;
    memcpy(dst, q, n * 4u);
    return 1;
}

static int wr_f(GPTR p, const float *src, uint32_t n)
{
    float *q = d3d9_gmem(p, n * 4u);
    if (!q) return 0;
    memcpy(q, src, n * 4u);
    return 1;
}

static void ident(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    memcpy(out, r, sizeof r);
}

/* v4 = (x, y, z, w) * m */
static void vec_mul(float out[4], const float v[4], const float m[16])
{
    float r[4];
    for (int c = 0; c < 4; c++)
        r[c] = v[0] * m[0 * 4 + c] + v[1] * m[1 * 4 + c] + v[2] * m[2 * 4 + c] + v[3] * m[3 * 4 + c];
    memcpy(out, r, sizeof r);
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(float out[3], const float a[3], const float b[3])
{
    float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(out, r, sizeof r);
}

static void norm3(float v[3])
{
    float l = sqrtf(dot3(v, v));
    if (l > 0.0f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    } else {
        v[0] = v[1] = v[2] = 0.0f;
    }
}

static void sub3(float out[3], const float a[3], const float b[3])
{
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static GPTR store_mat(GPTR out, const float m[16])
{
    return wr_f(out, m, 16) ? out : 0;
}

/* ---- matrices ----------------------------------------------------------------- */

GPTR WINAPI D3DXMatrixMultiply(GPTR out, GPTR m1, GPTR m2)
{
    float a[16], b[16], r[16];
    if (!rd_f(m1, a, 16) || !rd_f(m2, b, 16)) return 0;
    mat_mul(r, a, b);
    return store_mat(out, r);
}

GPTR WINAPI D3DXMatrixTranspose(GPTR out, GPTR m)
{
    float a[16], r[16];
    if (!rd_f(m, a, 16)) return 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i * 4 + j] = a[j * 4 + i];
    return store_mat(out, r);
}

GPTR WINAPI D3DXMatrixScaling(GPTR out, DWORD sx, DWORD sy, DWORD sz)
{
    float m[16];
    ident(m);
    m[0] = f32(sx);
    m[5] = f32(sy);
    m[10] = f32(sz);
    return store_mat(out, m);
}

GPTR WINAPI D3DXMatrixTranslation(GPTR out, DWORD x, DWORD y, DWORD z)
{
    float m[16];
    ident(m);
    m[12] = f32(x);
    m[13] = f32(y);
    m[14] = f32(z);
    return store_mat(out, m);
}

GPTR WINAPI D3DXMatrixRotationZ(GPTR out, DWORD angle)
{
    float a = f32(angle), m[16];
    ident(m);
    m[0] = cosf(a);
    m[1] = sinf(a);
    m[4] = -sinf(a);
    m[5] = cosf(a);
    return store_mat(out, m);
}

GPTR WINAPI D3DXMatrixLookAtRH(GPTR out, GPTR eye, GPTR at, GPTR up)
{
    float e[3], t[3], u[3], z[3], x[3], y[3], m[16];
    if (!rd_f(eye, e, 3) || !rd_f(at, t, 3) || !rd_f(up, u, 3)) return 0;
    sub3(z, e, t);
    norm3(z);
    cross3(x, u, z);
    norm3(x);
    cross3(y, z, x);
    memset(m, 0, sizeof m);
    m[0] = x[0]; m[1] = y[0]; m[2] = z[0];
    m[4] = x[1]; m[5] = y[1]; m[6] = z[1];
    m[8] = x[2]; m[9] = y[2]; m[10] = z[2];
    m[12] = -dot3(x, e);
    m[13] = -dot3(y, e);
    m[14] = -dot3(z, e);
    m[15] = 1.0f;
    return store_mat(out, m);
}

static void ortho_offcenter(float m[16], float l, float r, float b, float t, float zn, float zf, int rh)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (r - l);
    m[5] = 2.0f / (t - b);
    m[10] = rh ? 1.0f / (zn - zf) : 1.0f / (zf - zn);
    m[12] = (l + r) / (l - r);
    m[13] = (t + b) / (b - t);
    m[14] = rh ? zn / (zn - zf) : zn / (zn - zf);
    m[15] = 1.0f;
}

GPTR WINAPI D3DXMatrixOrthoOffCenterRH(GPTR out, DWORD l, DWORD r, DWORD b, DWORD t, DWORD zn, DWORD zf)
{
    float m[16];
    ortho_offcenter(m, f32(l), f32(r), f32(b), f32(t), f32(zn), f32(zf), 1);
    return store_mat(out, m);
}

GPTR WINAPI D3DXMatrixOrthoOffCenterLH(GPTR out, DWORD l, DWORD r, DWORD b, DWORD t, DWORD zn, DWORD zf)
{
    float m[16];
    ortho_offcenter(m, f32(l), f32(r), f32(b), f32(t), f32(zn), f32(zf), 0);
    return store_mat(out, m);
}

GPTR WINAPI D3DXMatrixPerspectiveFovRH(GPTR out, DWORD fovy, DWORD aspect, DWORD zn, DWORD zf)
{
    float fy = f32(fovy), asp = f32(aspect), n = f32(zn), f = f32(zf), m[16];
    float ys = 1.0f / tanf(fy * 0.5f), xs = ys / asp;
    memset(m, 0, sizeof m);
    m[0] = xs;
    m[5] = ys;
    m[10] = f / (n - f);
    m[11] = -1.0f;
    m[14] = n * f / (n - f);
    return store_mat(out, m);
}

GPTR WINAPI D3DXMatrixPerspectiveOffCenterRH(GPTR out, DWORD l, DWORD r, DWORD b, DWORD t, DWORD zn, DWORD zf)
{
    float L = f32(l), R = f32(r), B = f32(b), T = f32(t), n = f32(zn), f = f32(zf), m[16];
    memset(m, 0, sizeof m);
    m[0] = 2.0f * n / (R - L);
    m[5] = 2.0f * n / (T - B);
    m[8] = (L + R) / (L - R);
    m[9] = (T + B) / (B - T);
    m[10] = f / (n - f);
    m[11] = -1.0f;
    m[14] = n * f / (n - f);
    return store_mat(out, m);
}

/* ---- vectors ------------------------------------------------------------------ */

GPTR WINAPI D3DXVec3Normalize(GPTR out, GPTR v)
{
    float a[3];
    if (!rd_f(v, a, 3)) return 0;
    norm3(a);
    return wr_f(out, a, 3) ? out : 0;
}

GPTR WINAPI D3DXVec3Transform(GPTR out, GPTR v, GPTR m)
{
    float a[3], mm[16], r[4];
    if (!rd_f(v, a, 3) || !rd_f(m, mm, 16)) return 0;
    float in[4] = { a[0], a[1], a[2], 1.0f };
    vec_mul(r, in, mm);
    return wr_f(out, r, 4) ? out : 0;
}

GPTR WINAPI D3DXVec4TransformArray(GPTR out, UINT outstride, GPTR v, UINT vstride, GPTR m, UINT n)
{
    float mm[16];
    if (!rd_f(m, mm, 16)) return 0;
    for (UINT i = 0; i < n; i++) {
        float in[4], r[4];
        if (!rd_f(v + i * vstride, in, 4)) return 0;
        vec_mul(r, in, mm);
        if (!wr_f(out + i * outstride, r, 4)) return 0;
    }
    return out;
}

GPTR WINAPI D3DXVec3Project(GPTR out, GPTR v, GPTR viewport, GPTR projection, GPTR view, GPTR world)
{
    float a[3], pm[16], vm[16], wm[16], mm[16], tmp[16], p[4];
    if (!rd_f(v, a, 3)) return 0;
    if (projection) { if (!rd_f(projection, pm, 16)) return 0; } else ident(pm);
    if (view) { if (!rd_f(view, vm, 16)) return 0; } else ident(vm);
    if (world) { if (!rd_f(world, wm, 16)) return 0; } else ident(wm);
    mat_mul(tmp, wm, vm);
    mat_mul(mm, tmp, pm);
    float in[4] = { a[0], a[1], a[2], 1.0f };
    vec_mul(p, in, mm);
    uint32_t vx = d3d9_rd(viewport, 0), vy = d3d9_rd(viewport, 4);
    uint32_t vw = d3d9_rd(viewport, 8), vh = d3d9_rd(viewport, 12);
    float minz = f32(d3d9_rd(viewport, 16)), maxz = f32(d3d9_rd(viewport, 20));
    float ndc[3] = { p[0] / p[3], p[1] / p[3], p[2] / p[3] };
    float s[3] = {
        (float)vx + (ndc[0] + 1.0f) * 0.5f * (float)vw,
        (float)vy + (1.0f - ndc[1]) * 0.5f * (float)vh,
        minz + ndc[2] * (maxz - minz),
    };
    return wr_f(out, s, 3) ? out : 0;
}

/* ---- planes ------------------------------------------------------------------- */

GPTR WINAPI D3DXPlaneFromPoints(GPTR out, GPTR p1, GPTR p2, GPTR p3)
{
    float a[3], b[3], c[3], u[3], v[3], n[3], pl[4];
    if (!rd_f(p1, a, 3) || !rd_f(p2, b, 3) || !rd_f(p3, c, 3)) return 0;
    sub3(u, b, a);
    sub3(v, c, a);
    cross3(n, u, v);
    norm3(n);
    pl[0] = n[0];
    pl[1] = n[1];
    pl[2] = n[2];
    pl[3] = -dot3(n, a);
    return wr_f(out, pl, 4) ? out : 0;
}
