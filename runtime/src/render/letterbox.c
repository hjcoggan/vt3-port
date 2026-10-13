#include "vt3/vt3_renderer.h"

#include <stdint.h>

vt3_rect vt3_letterbox(int dst_w, int dst_h, int src_w, int src_h,
                       int aspect_w, int aspect_h, int integer_only)
{
    vt3_rect r = { 0, 0, 0, 0 };
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0 ||
        aspect_w <= 0 || aspect_h <= 0)
        return r;

    /* Largest box with the display aspect that fits in the destination. */
    int box_w, box_h;
    if ((int64_t)dst_w * aspect_h > (int64_t)dst_h * aspect_w) {
        box_h = dst_h;
        box_w = (int)(((int64_t)dst_h * aspect_w) / aspect_h);
    } else {
        box_w = dst_w;
        box_h = (int)(((int64_t)dst_w * aspect_h) / aspect_w);
    }

    int w = box_w, h = box_h;
    if (integer_only) {
        int sx = box_w / src_w;
        int sy = box_h / src_h;
        int s = sx < sy ? sx : sy;
        if (s >= 1) {
            w = src_w * s;
            h = src_h * s;
        }
        /* else: window too small for an integer scale, keep the fractional box */
    }

    r.w = w;
    r.h = h;
    r.x = (dst_w - w) / 2;
    r.y = (dst_h - h) / 2;
    return r;
}
