/*
 * On-screen touch controls (see vt3/vt3_touch.h).
 *
 * The overlay is drawn as translucent vector shapes through the fixed-function (XYZRHW) path, so
 * it looks the same on Vulkan and OpenGL and scales with the frame. Geometry is built in the
 * renderer's logical pixels; touch points arrive in drawable pixels and are mapped back through the
 * present letterbox, so a control is hit where it is drawn even when the frame is letterboxed.
 *
 * Layout (design units, 1080 logical pixels tall; scaled to the frame height):
 *   - a thumb stick lower-left (movement),
 *   - the A/B/X/Y diamond lower-right (Top Spin / Slice / Lob),
 *   - LB/RB pills in the top corners (Instruct COM Partner / Tour Calendar),
 *   - SELECT / START pills bottom centre (Change Camera / Pause).
 *
 * The controls hide themselves while a physical keyboard or gamepad is in use (see physical_present),
 * so they never cover the game for a player on real hardware.
 *
 * Labels come from the public-domain 8x8 bitmap font in font8x8.h, drawn as solid pixel quads.
 */
#include "vt3/vt3_log.h"
#include "vt3/vt3_input.h"
#include "vt3/vt3_renderer.h"
#include "vt3/vt3_touch.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "font8x8.h"

enum {
    CTL_STICK = 0,
    CTL_A, CTL_B, CTL_X, CTL_Y,
    CTL_LB, CTL_RB,
    CTL_SELECT, CTL_START,
    CTL_COUNT
};

#define TOUCH_MAX_PTR 10

typedef struct {
    int   used;
    int   id;
    int   ctl;              /* CTL_* the pointer is driving, -1 while unassigned */
    float lx, ly;           /* last position, logical pixels */
} touch_ptr;

static struct {
    int   inited;
    int   enabled;          /* VT3_TOUCH */
    int   force;            /* VT3_TOUCH_FORCE: show even with a physical device */
    float user_scale;
    float alpha;
    int   color;
    int   hidden_logged;

    /* View: how logical pixels map into the drawable. */
    int      have_view;
    int      logical_w, logical_h;
    int      draw_w, draw_h;
    vt3_rect dst;

    /* Layout, logical pixels. */
    float s;                                    /* design scale: logical_h / 1080 * user_scale */
    float stick_cx, stick_cy, stick_r, knob_r;
    float ab_r;
    float ab_cx[4], ab_cy[4];                   /* A, B, X, Y */
    float sh_w, sh_h, lb_cx, lb_cy, rb_cx, rb_cy;
    float pill_h, sel_cx, sel_cy, sel_w, st_cx, st_cy, st_w;

    touch_ptr ptr[TOUCH_MAX_PTR];
    float knob_x, knob_y;
    uint32_t draw_mask;                         /* controls held, for the drawn pressed state */
    int stick_held;

    /* Last input actually pushed to the game, so only changes are emitted. */
    uint32_t applied_mask;
    float    applied_lx, applied_ly;

    /* Vertex batch for this frame. */
    vt3_tlvertex *v;
    uint32_t vn, vcap;
    int      shadow;
} g;

/* ---- small helpers -------------------------------------------------------- */

#define VT3_PI 3.14159265358979323846f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint32_t rgba(int r, int gr, int b, float a)
{
    a *= g.alpha;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return ((uint32_t)(a * 255.0f + 0.5f) << 24) | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | (uint32_t)b;
}

/* Per-button accent. Monochrome when VT3_TOUCH_COLOR=0. */
static uint32_t accent(int ctl, float a)
{
    if (!g.color) return rgba(220, 227, 240, a);
    switch (ctl) {
    case CTL_A: return rgba(112, 202, 112, a);
    case CTL_B: return rgba(236, 94, 100, a);
    case CTL_X: return rgba(98, 156, 242, a);
    case CTL_Y: return rgba(240, 196, 72, a);
    default:    return rgba(220, 227, 240, a);
    }
}

/* ---- vertex batch --------------------------------------------------------- */

static int ensure_verts(uint32_t add)
{
    if (g.vn + add <= g.vcap) return 1;
    uint32_t cap = g.vcap ? g.vcap : 1024;
    while (cap < g.vn + add) cap *= 2;
    vt3_tlvertex *nv = realloc(g.v, (size_t)cap * sizeof *nv);
    if (!nv) return 0;
    g.v = nv;
    g.vcap = cap;
    return 1;
}

static float g_sh_x, g_sh_y;

static void push_vtx(float x, float y, uint32_t c)
{
    if (g.vn >= g.vcap) return;
    if (g.shadow) {
        uint32_t a = ((c >> 24) & 0xffu) * 62u / 100u;
        c = (a << 24);
        x += g_sh_x; y += g_sh_y;
    }
    vt3_tlvertex *t = &g.v[g.vn++];
    t->x = x; t->y = y; t->z = 0.0f; t->rhw = 1.0f;
    t->color = c; t->u = 0.0f; t->v = 0.0f;
}

static void push_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
    if (!ensure_verts(3)) return;
    push_vtx(x0, y0, c); push_vtx(x1, y1, c); push_vtx(x2, y2, c);
}

static void push_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c)
{
    push_tri(x0, y0, x1, y1, x2, y2, c);
    push_tri(x0, y0, x2, y2, x3, y3, c);
}

static void emit_rect(float x, float y, float w, float h, uint32_t c)
{
    push_quad(x, y, x + w, y, x + w, y + h, x, y + h, c);
}

static void emit_line(float x0, float y0, float x1, float y1, float th, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    push_quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

static int arc_segments(float r)
{
    int seg = (int)(r * 0.9f);
    if (seg < 10) seg = 10;
    if (seg > 96) seg = 96;
    return seg;
}

/* A ring centred on radius r_out - th/2, from angle a0 to a1 (radians, 0 = +x, growing clockwise). */
static void emit_arc(float cx, float cy, float r_out, float th, float a0, float a1, uint32_t c)
{
    float r = r_out - th * 0.5f;
    if (r < 0.5f) return;
    int seg = arc_segments(r);
    if (fabsf(a1 - a0) < 3.0f) seg = seg / 4 + 1;      /* quarter arcs need fewer */
    if (seg < 3) seg = 3;
    float inner = r - th * 0.5f < 0.0f ? 0.0f : r - th * 0.5f;
    float outer = r + th * 0.5f;
    for (int i = 0; i < seg; i++) {
        float t0 = a0 + (a1 - a0) * (float)i / (float)seg;
        float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)seg;
        float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
        push_quad(cx + inner * c0, cy + inner * s0, cx + outer * c0, cy + outer * s0,
                  cx + outer * c1, cy + outer * s1, cx + inner * c1, cy + inner * s1, c);
    }
}

static void emit_ring(float cx, float cy, float r_out, float th, uint32_t c)
{
    emit_arc(cx, cy, r_out, th, 0.0f, 2.0f * VT3_PI, c);
}

static void emit_disc(float cx, float cy, float r, uint32_t c)
{
    if (r < 0.5f) return;
    int seg = arc_segments(r) + 8;
    for (int i = 0; i < seg; i++) {
        float t0 = 2.0f * VT3_PI * (float)i / (float)seg;
        float t1 = 2.0f * VT3_PI * (float)(i + 1) / (float)seg;
        push_tri(cx, cy, cx + r * cosf(t0), cy + r * sinf(t0), cx + r * cosf(t1), cy + r * sinf(t1), c);
    }
}

/* Outline (th small) or solid fill (th == h) of a rounded rectangle. */
static void emit_round_rect(float x, float y, float w, float h, float rad, float th, uint32_t c)
{
    if (rad > w * 0.5f) rad = w * 0.5f;
    if (rad > h * 0.5f) rad = h * 0.5f;
    if (rad < 0.5f) {
        emit_rect(x, y, w, th, c);
        emit_rect(x, y + h - th, w, th, c);
        emit_rect(x, y, th, h, c);
        emit_rect(x + w - th, y, th, h, c);
        return;
    }
    emit_arc(x + rad,     y + rad,     rad + th * 0.5f, th, VT3_PI,         VT3_PI * 1.5f, c);
    emit_arc(x + w - rad, y + rad,     rad + th * 0.5f, th, VT3_PI * 1.5f,  VT3_PI * 2.0f, c);
    emit_arc(x + w - rad, y + h - rad, rad + th * 0.5f, th, 0.0f,           VT3_PI * 0.5f, c);
    emit_arc(x + rad,     y + h - rad, rad + th * 0.5f, th, VT3_PI * 0.5f,  VT3_PI,         c);
    emit_line(x + rad, y + th * 0.5f, x + w - rad, y + th * 0.5f, th, c);
    emit_line(x + rad, y + h - th * 0.5f, x + w - rad, y + h - th * 0.5f, th, c);
    emit_line(x + th * 0.5f, y + rad, x + th * 0.5f, y + h - rad, th, c);
    emit_line(x + w - th * 0.5f, y + rad, x + w - th * 0.5f, y + h - rad, th, c);
}

static void emit_glyph(char ch, float x, float y, float px, uint32_t c)
{
    unsigned u = (unsigned char)ch;
    if (u < VT3_FONT_FIRST || u > VT3_FONT_LAST) return;
    const uint8_t *gl = vt3_font8x8[u - VT3_FONT_FIRST];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = gl[row];
        int col = 0;
        while (col < 8) {
            if (bits & (1u << col)) {
                int start = col;
                while (col < 8 && (bits & (1u << col))) col++;
                emit_rect(x + (float)start * px, y + (float)row * px, (float)(col - start) * px, px, c);
            } else {
                col++;
            }
        }
    }
}

static void emit_text(const char *s, float x, float y, float px, uint32_t c, int center)
{
    if (center) x -= (float)strlen(s) * 8.0f * px * 0.5f;
    for (; *s; s++, x += 8.0f * px) emit_glyph(*s, x, y, px, c);
}

/* ---- environment and visibility ------------------------------------------- */

static void read_env(void)
{
    if (g.inited) return;
    g.inited = 1;
    g.user_scale = 1.0f;
    g.alpha = 1.0f;
    g.color = 1;
    const char *e = getenv("VT3_TOUCH");
#ifdef __ANDROID__
    g.enabled = e ? atoi(e) != 0 : 1;
#else
    g.enabled = e ? atoi(e) != 0 : 0;
#endif
    g.force = getenv("VT3_TOUCH_FORCE") != NULL;
    if ((e = getenv("VT3_TOUCH_SCALE")) != NULL && *e) g.user_scale = clampf((float)atof(e), 0.4f, 2.5f);
    if ((e = getenv("VT3_TOUCH_ALPHA")) != NULL && *e) g.alpha = clampf((float)atof(e), 0.1f, 1.0f);
    if ((e = getenv("VT3_TOUCH_COLOR")) != NULL && *e) g.color = atoi(e) != 0;
    if (g.enabled) VT3_INFO("touch: on-screen controls enabled (scale %.2f, alpha %.2f, colour %d)",
                            g.user_scale, g.alpha, g.color);
}

/* A physical keyboard or gamepad is in use: the controls stay out of the way. */
static int physical_present(void)
{
    if (g.force) return 0;
    return vt3_input_pad_connected() || vt3_input_keyboard_connected();
}

static int active(void)
{
    read_env();
    return g.enabled && !physical_present();
}

int vt3_touch_enabled(void)
{
    return active();
}

/* ---- layout and hit testing ---------------------------------------------- */

static void layout(void)
{
    float W = (float)g.logical_w, H = (float)g.logical_h;
    float s = H / 1080.0f * g.user_scale;
    g.s = s;

    g.stick_r = 168.0f * s;
    g.knob_r = 70.0f * s;
    g.stick_cx = 200.0f * s;
    g.stick_cy = H - 200.0f * s;

    g.ab_r = 74.0f * s;
    float d = 152.0f * s;
    float acx = W - 275.0f * s, acy = H - 250.0f * s;
    g.ab_cx[0] = acx;        g.ab_cy[0] = acy + d;   /* A, bottom */
    g.ab_cx[1] = acx + d;    g.ab_cy[1] = acy;       /* B, right */
    g.ab_cx[2] = acx - d;    g.ab_cy[2] = acy;       /* X, left */
    g.ab_cx[3] = acx;        g.ab_cy[3] = acy - d;   /* Y, top */

    g.sh_w = 150.0f * s;
    g.sh_h = 62.0f * s;
    g.lb_cx = 40.0f * s + g.sh_w * 0.5f;
    g.rb_cx = W - 40.0f * s - g.sh_w * 0.5f;
    g.lb_cy = g.rb_cy = 34.0f * s + g.sh_h * 0.5f;

    g.pill_h = 66.0f * s;
    float px = 4.2f * s;
    g.sel_w = 6.0f * 8.0f * px + 48.0f * s;          /* "SELECT" */
    g.st_w  = 5.0f * 8.0f * px + 48.0f * s;          /* "START"  */
    g.sel_cx = W * 0.5f - 140.0f * s;
    g.st_cx  = W * 0.5f + 140.0f * s;
    g.sel_cy = g.st_cy = H - 78.0f * s;
}

static int in_rect(float px, float py, float x, float y, float w, float h, float pad)
{
    return px >= x - pad && px <= x + w + pad && py >= y - pad && py <= y + h + pad;
}

/* Which control a fresh touch at (x, y) logical grabs, or -1. Order: stick, buttons, pills. */
static int hit_test(float x, float y)
{
    if (g.s <= 0.0f) return -1;
    float rr = g.stick_r * 1.9f;
    if ((x - g.stick_cx) * (x - g.stick_cx) + (y - g.stick_cy) * (y - g.stick_cy) <= rr * rr) return CTL_STICK;
    for (int i = 0; i < 4; i++) {
        float r = g.ab_r * 1.35f;
        if ((x - g.ab_cx[i]) * (x - g.ab_cx[i]) + (y - g.ab_cy[i]) * (y - g.ab_cy[i]) <= r * r) return CTL_A + i;
    }
    float pad = 14.0f * g.s;
    if (in_rect(x, y, g.lb_cx - g.sh_w * 0.5f, g.lb_cy - g.sh_h * 0.5f, g.sh_w, g.sh_h, pad)) return CTL_LB;
    if (in_rect(x, y, g.rb_cx - g.sh_w * 0.5f, g.rb_cy - g.sh_h * 0.5f, g.sh_w, g.sh_h, pad)) return CTL_RB;
    if (in_rect(x, y, g.sel_cx - g.sel_w * 0.5f, g.sel_cy - g.pill_h * 0.5f, g.sel_w, g.pill_h, pad)) return CTL_SELECT;
    if (in_rect(x, y, g.st_cx - g.st_w * 0.5f, g.st_cy - g.pill_h * 0.5f, g.st_w, g.pill_h, pad)) return CTL_START;
    return -1;
}

static uint32_t bit_for(int ctl)
{
    switch (ctl) {
    case CTL_A: return VT3_BTN_A;
    case CTL_B: return VT3_BTN_B;
    case CTL_X: return VT3_BTN_X;
    case CTL_Y: return VT3_BTN_Y;
    case CTL_LB: return VT3_BTN_LB;
    case CTL_RB: return VT3_BTN_RB;
    case CTL_SELECT: return VT3_BTN_BACK;
    case CTL_START: return VT3_BTN_START;
    default: return 0;
    }
}

/* ---- input -> game state -------------------------------------------------- */

/* VT3_TOUCH_DEBUG=1 traces every change of what the controls are holding. */
static void log_state(uint32_t mask, float lx, float ly)
{
    static int dbg = -1;
    static uint32_t last_mask;
    static float last_lx, last_ly;
    if (dbg < 0) dbg = getenv("VT3_TOUCH_DEBUG") != NULL;
    if (!dbg) return;
    if (mask == last_mask && lx == last_lx && ly == last_ly) return;
    last_mask = mask; last_lx = lx; last_ly = ly;
    int held = 0;
    for (int i = 0; i < TOUCH_MAX_PTR; i++) if (g.ptr[i].used) held++;
    VT3_INFO("touch: buttons %08x stick %+.2f,%+.2f (%d pointers)", mask, lx, ly, held);
}

/* Push whatever the controls currently hold into the game, changing only what differs. */
static void apply_state(void)
{
    uint32_t mask = 0;
    float lx = 0.0f, ly = 0.0f;
    int stick = 0;
    float kx = g.stick_cx, ky = g.stick_cy;

    for (int i = 0; i < TOUCH_MAX_PTR; i++) {
        touch_ptr *p = &g.ptr[i];
        if (!p->used || p->ctl < 0) continue;
        if (p->ctl == CTL_STICK) {
            stick = 1;
            float dx = p->lx - g.stick_cx, dy = p->ly - g.stick_cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > g.stick_r && d > 0.0f) { dx = dx * g.stick_r / d; dy = dy * g.stick_r / d; d = g.stick_r; }
            kx = g.stick_cx + dx;
            ky = g.stick_cy + dy;
            float ax = g.stick_r > 0.0f ? dx / g.stick_r : 0.0f;
            float ay = g.stick_r > 0.0f ? dy / g.stick_r : 0.0f;
            const float dz = 0.14f;
            float mag = sqrtf(ax * ax + ay * ay);
            if (mag <= dz) { ax = 0.0f; ay = 0.0f; }
            else { float k = (mag - dz) / (1.0f - dz) / mag; ax *= k; ay *= k; }
            lx = clampf(ax, -1.0f, 1.0f);
            ly = clampf(ay, -1.0f, 1.0f);
        } else {
            mask |= bit_for(p->ctl);
        }
    }
    g.knob_x = kx;
    g.knob_y = ky;
    g.stick_held = stick;

    /* The stick also drives the d-pad, so menus (which read the hat) navigate. */
    uint32_t dp = 0;
    if (lx < -0.45f) dp |= VT3_BTN_LEFT;
    if (lx >  0.45f) dp |= VT3_BTN_RIGHT;
    if (ly < -0.45f) dp |= VT3_BTN_UP;
    if (ly >  0.45f) dp |= VT3_BTN_DOWN;
    g.draw_mask = mask | dp;
    mask |= dp;

    log_state(mask, lx, ly);

    uint32_t on = mask & ~g.applied_mask;
    uint32_t off = g.applied_mask & ~mask;
    if (on)  { vt3_input_set_button(on, 1);  vt3_input_set_pad_button(on, 1); }
    if (off) { vt3_input_set_button(off, 0); vt3_input_set_pad_button(off, 0); }
    g.applied_mask = mask;

    if (lx != g.applied_lx || ly != g.applied_ly) {
        vt3_input_set_axis(VT3_AXIS_LX, lx);
        vt3_input_set_axis(VT3_AXIS_LY, ly);
        g.applied_lx = lx;
        g.applied_ly = ly;
    }
}

/* Drop everything the controls were holding (hidden, focus lost, window gone). */
static void release_state(void)
{
    for (int i = 0; i < TOUCH_MAX_PTR; i++) g.ptr[i].used = 0;
    g.draw_mask = 0;
    g.stick_held = 0;
    if (g.applied_mask) {
        vt3_input_set_button(g.applied_mask, 0);
        vt3_input_set_pad_button(g.applied_mask, 0);
        g.applied_mask = 0;
    }
    if (g.applied_lx != 0.0f || g.applied_ly != 0.0f) {
        vt3_input_set_axis(VT3_AXIS_LX, 0.0f);
        vt3_input_set_axis(VT3_AXIS_LY, 0.0f);
        g.applied_lx = g.applied_ly = 0.0f;
    }
}

/* Map a drawable-space point into logical pixels through the present letterbox. */
static void to_logical(float dx, float dy, float *lx, float *ly)
{
    if (!g.have_view || g.dst.w <= 0 || g.dst.h <= 0 || g.logical_w <= 0 || g.logical_h <= 0) {
        *lx = dx; *ly = dy;
        return;
    }
    *lx = (dx - (float)g.dst.x) * (float)g.logical_w / (float)g.dst.w;
    *ly = (dy - (float)g.dst.y) * (float)g.logical_h / (float)g.dst.h;
}

void vt3_touch_pointer(int id, int down, float x, float y)
{
    if (!vt3_touch_enabled()) return;
    float lx, ly;
    to_logical(x, y, &lx, &ly);

    touch_ptr *p = NULL;
    for (int i = 0; i < TOUCH_MAX_PTR; i++) {
        if (g.ptr[i].used && g.ptr[i].id == id) { p = &g.ptr[i]; break; }
    }
    if (!p) {
        if (!down) return;
        for (int i = 0; i < TOUCH_MAX_PTR; i++) {
            if (!g.ptr[i].used) { p = &g.ptr[i]; break; }
        }
        if (!p) return;
        p->used = 1;
        p->id = id;
        p->ctl = hit_test(lx, ly);
        if (p->ctl < 0) { p->used = 0; return; }   /* a touch outside the controls is ignored */
    }
    if (down) {
        p->lx = lx;
        p->ly = ly;
    } else {
        p->used = 0;
    }
    apply_state();
}

void vt3_touch_release_all(void)
{
    release_state();
}

/* ---- drawing -------------------------------------------------------------- */

static void draw_disc_shape(float cx, float cy, float r, uint32_t fill, uint32_t stroke, float th)
{
    for (int pass = 0; pass < 2; pass++) {
        g.shadow = pass == 0;
        emit_disc(cx, cy, r, fill);
        emit_ring(cx, cy, r, th, stroke);
    }
    g.shadow = 0;
}

static void draw_pill(float cx, float cy, float w, float h, const char *label, float px,
                      uint32_t fill, uint32_t stroke, uint32_t text)
{
    float x = cx - w * 0.5f, y = cy - h * 0.5f;
    float rad = h * 0.5f;
    float th = 3.0f * g.s;
    if (th < 1.5f) th = 1.5f;
    for (int pass = 0; pass < 2; pass++) {
        g.shadow = pass == 0;
        emit_round_rect(x, y, w, h, rad, h, fill);            /* solid rounded rect */
        emit_round_rect(x, y, w, h, rad, th, stroke);         /* its outline */
        emit_text(label, cx, cy - 4.0f * px, px, text, 1);
    }
    g.shadow = 0;
}

static void draw_controls(void)
{
    float s = g.s;
    float th = 3.2f * s;
    if (th < 1.6f) th = 1.6f;
    g_sh_x = 0.0f;
    g_sh_y = 1.5f * s;

    uint32_t stroke = rgba(233, 238, 247, 0.72f);
    uint32_t fill   = rgba(255, 255, 255, 0.10f);
    uint32_t fill_p = rgba(255, 255, 255, 0.30f);
    uint32_t text   = rgba(238, 242, 250, 0.94f);

    /* Thumb stick: well + ring, then the knob. */
    int sp = g.stick_held;
    for (int pass = 0; pass < 2; pass++) {
        g.shadow = pass == 0;
        emit_disc(g.stick_cx, g.stick_cy, g.stick_r - th * 0.5f, sp ? fill_p : fill);
        emit_ring(g.stick_cx, g.stick_cy, g.stick_r, th, stroke);
    }
    g.shadow = 0;
    for (int pass = 0; pass < 2; pass++) {
        g.shadow = pass == 0;
        emit_disc(g.knob_x, g.knob_y, g.knob_r, rgba(255, 255, 255, sp ? 0.30f : 0.16f));
        emit_ring(g.knob_x, g.knob_y, g.knob_r, th * 0.85f, stroke);
    }
    g.shadow = 0;

    /* A/B/X/Y diamond. */
    static const char *ab_label = "ABXY";
    float bpx = g.ab_r * 0.155f;
    for (int i = 0; i < 4; i++) {
        int ctl = CTL_A + i;
        int pressed = (g.draw_mask & bit_for(ctl)) != 0;
        float r = g.ab_r * (pressed ? 1.06f : 1.0f);
        draw_disc_shape(g.ab_cx[i], g.ab_cy[i], r, accent(ctl, pressed ? 0.44f : 0.16f),
                        accent(ctl, pressed ? 1.0f : 0.88f), th);
        float cx = g.ab_cx[i], cy = g.ab_cy[i];
        for (int pass = 0; pass < 2; pass++) {
            g.shadow = pass == 0;
            emit_glyph(ab_label[i], cx - 4.0f * bpx, cy - 4.0f * bpx, bpx, text);
        }
        g.shadow = 0;
    }

    /* LB / RB shoulders. */
    float spx = 5.0f * s;
    draw_pill(g.lb_cx, g.lb_cy, g.sh_w, g.sh_h, "LB", spx,
              (g.draw_mask & VT3_BTN_LB) ? fill_p : fill, stroke, text);
    draw_pill(g.rb_cx, g.rb_cy, g.sh_w, g.sh_h, "RB", spx,
              (g.draw_mask & VT3_BTN_RB) ? fill_p : fill, stroke, text);

    /* SELECT / START pills. */
    float tpx = 4.2f * s;
    draw_pill(g.sel_cx, g.sel_cy, g.sel_w, g.pill_h, "SELECT", tpx,
              (g.draw_mask & VT3_BTN_BACK) ? fill_p : fill, stroke, text);
    draw_pill(g.st_cx, g.st_cy, g.st_w, g.pill_h, "START", tpx,
              (g.draw_mask & VT3_BTN_START) ? fill_p : fill, stroke, text);
}

void vt3_touch_frame(int drawable_w, int drawable_h)
{
    if (!active()) {
        if (g.enabled && !g.hidden_logged && physical_present()) {
            g.hidden_logged = 1;
            VT3_INFO("touch: physical keyboard or gamepad in use; on-screen controls hidden");
        }
        release_state();
        return;
    }

    g.draw_w = drawable_w;
    g.draw_h = drawable_h;
    int lw = 0, lh = 0;
    vt3_gfx_logical_size(&lw, &lh);
    if (lw <= 0 || lh <= 0) return;
    g.logical_w = lw;
    g.logical_h = lh;
    vt3_gfx_view_rect(drawable_w, drawable_h, &g.dst);
    g.have_view = 1;

    layout();
    apply_state();

    /* Draw on the back buffer, over the game's frame, with no scissor. */
    vt3_gfx_rt_bind(0);
    vt3_rect none = { 0, 0, 0, 0 };
    vt3_gfx_set_scissor(0, none);

    g.vn = 0;
    g.shadow = 0;
    draw_controls();
    if (g.vn) vt3_gfx_draw(VT3_PRIM_TRIANGLELIST, g.v, g.vn, 0);
}
