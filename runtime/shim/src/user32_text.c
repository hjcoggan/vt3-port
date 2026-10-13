/*
 * wsprintfA and wvsprintfA: Win32 formatting into a guest buffer.
 *
 * Conversions: %d %i %u %o %x %X %c %s %%. Flags: - 0 + space #. Width and precision
 * are digits or * (taken from the arguments). The length prefixes h and l are accepted
 * and ignored, because the guest's int and long are both 32 bits. I64 takes an 8-byte
 * argument. Arguments are 4-byte stack slots in guest memory, read in order.
 *
 * Output is capped at 1023 characters plus NUL, the limit Win32 puts on wsprintf.
 * Return value: characters written, not counting the NUL.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "win32/user32_text.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#define WSPRINTF_MAX   1023u   /* output characters, not counting the NUL */
#define WSPRINTF_LIMIT 1024    /* cap on width and precision values */

typedef struct wspec {
    int left, zero, plus, space, alt;
    int width;   /* -1 when absent */
    int prec;    /* -1 when absent */
} wspec;

typedef struct wout {
    char buf[WSPRINTF_MAX + 1];
    size_t len;
    uint32_t va;   /* guest address of the next argument */
} wout;

/* One byte of guest memory, or 0 when the address is out of range. */
static int gbyte(uint32_t a)
{
    return vt3s_guest_range_ok(a, 1) ? *(const uint8_t *)vt3_g2h(a) : 0;
}

static void put(wout *o, char ch)
{
    if (o->len < WSPRINTF_MAX) o->buf[o->len++] = ch;
}

static void pad(wout *o, char ch, size_t n)
{
    while (n--) put(o, ch);
}

static void put_n(wout *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) put(o, s[i]);
}

static uint32_t next_arg(wout *o)
{
    uint32_t v = vt3s_load_u32(o->va);
    o->va += 4;
    return v;
}

static uint64_t next_arg64(wout *o)
{
    uint64_t lo = next_arg(o);
    uint64_t hi = next_arg(o);
    return (hi << 32) | lo;
}

/* Width and precision are capped so a huge value cannot make the padding loops long. */
static int clamp_size(int64_t v)
{
    if (v < 0) v = -v;
    return v > WSPRINTF_LIMIT ? WSPRINTF_LIMIT : (int)v;
}

/* Integer field: sign or prefix, zero-filled digits, then width. C printf rules. */
static void put_int(wout *o, const wspec *sp, uint64_t mag, int neg, int is_signed,
                    unsigned base, int upper)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;
    char dig[24];
    size_t nd = 0;
    int nonzero = mag != 0;

    /* A precision of 0 with a value of 0 prints no digits, as in C. */
    if (!(mag == 0 && sp->prec == 0)) {
        do {
            dig[nd++] = digits[mag % base];
            mag /= base;
        } while (mag);
    }

    char pre[4];
    size_t np = 0;
    if (is_signed && neg) pre[np++] = '-';
    else if (is_signed && sp->plus) pre[np++] = '+';
    else if (is_signed && sp->space) pre[np++] = ' ';
    if (sp->alt && base == 16 && nonzero) {
        pre[np++] = '0';
        pre[np++] = upper ? 'X' : 'x';
    }

    size_t zeros = sp->prec > (int)nd ? (size_t)(sp->prec - (int)nd) : 0;
    size_t body = np + zeros + nd;
    size_t fill = sp->width > (int)body ? (size_t)(sp->width - (int)body) : 0;

    if (sp->left) {
        put_n(o, pre, np);
        pad(o, '0', zeros);
    } else if (sp->zero && sp->prec < 0) {
        /* The 0 flag is ignored when a precision is given, as in C. */
        put_n(o, pre, np);
        pad(o, '0', fill + zeros);
        fill = 0;
    } else {
        pad(o, ' ', fill);
        put_n(o, pre, np);
        pad(o, '0', zeros);
        fill = 0;
    }
    for (size_t i = nd; i-- > 0;) put(o, dig[i]);
    if (sp->left) pad(o, ' ', fill);
}

/* String field. A precision limits the characters taken; width pads with spaces. */
static void put_str(wout *o, const wspec *sp, const char *s, size_t n)
{
    if (sp->prec >= 0 && (size_t)sp->prec < n) n = (size_t)sp->prec;
    size_t fill = sp->width > (int)n ? (size_t)(sp->width - (int)n) : 0;
    if (!sp->left) pad(o, ' ', fill);
    put_n(o, s, n);
    if (sp->left) pad(o, ' ', fill);
}

/*
 * Length of the NUL-terminated guest string at a, read at most limit bytes. Returns -1
 * when a is NULL or out of range. The scan stops at the first address that is not in
 * guest memory, so a string with no terminator cannot run past the arena.
 */
static long gstr_len(uint32_t a, size_t limit)
{
    if (!a || !vt3s_guest_range_ok(a, 1)) return -1;
    size_t n = 0;
    while (n < limit && gbyte(a + (uint32_t)n)) n++;
    return (long)n;
}

static void format_into(wout *o, GPTR fmt, GPTR va)
{
    o->len = 0;
    o->va = va;
    uint32_t p = fmt;

    for (;;) {
        int ch = gbyte(p++);
        if (ch == 0) break;
        if (ch != '%') {
            put(o, (char)ch);
            continue;
        }

        uint32_t spec_start = p - 1;
        wspec sp = { 0, 0, 0, 0, 0, -1, -1 };

        for (;;) {
            int f = gbyte(p);
            if (f == '-') sp.left = 1;
            else if (f == '0') sp.zero = 1;
            else if (f == '+') sp.plus = 1;
            else if (f == ' ') sp.space = 1;
            else if (f == '#') sp.alt = 1;
            else break;
            p++;
        }

        if (gbyte(p) == '*') {
            p++;
            int64_t w = (int32_t)next_arg(o);
            if (w < 0) sp.left = 1;
            sp.width = clamp_size(w);
        } else if (isdigit(gbyte(p))) {
            int64_t w = 0;
            while (isdigit(gbyte(p))) {
                if (w < 100000) w = w * 10 + (gbyte(p) - '0');
                p++;
            }
            sp.width = clamp_size(w);
        }

        if (gbyte(p) == '.') {
            p++;
            if (gbyte(p) == '*') {
                p++;
                int64_t pr = (int32_t)next_arg(o);
                sp.prec = pr < 0 ? -1 : clamp_size(pr);
            } else {
                int64_t pr = 0;
                while (isdigit(gbyte(p))) {
                    if (pr < 100000) pr = pr * 10 + (gbyte(p) - '0');
                    p++;
                }
                sp.prec = clamp_size(pr);
            }
        }

        int wide64 = 0;
        if (gbyte(p) == 'I' && gbyte(p + 1) == '6' && gbyte(p + 2) == '4') {
            wide64 = 1;
            p += 3;
        } else {
            while (gbyte(p) == 'h' || gbyte(p) == 'l') p++;
        }

        int conv = gbyte(p);
        if (conv == 0) break;   /* the format ends in the middle of a spec */
        p++;

        switch (conv) {
        case '%':
            put(o, '%');
            break;
        case 'd':
        case 'i': {
            int64_t v = wide64 ? (int64_t)next_arg64(o) : (int64_t)(int32_t)next_arg(o);
            uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            put_int(o, &sp, mag, v < 0, 1, 10, 0);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            uint64_t v = wide64 ? next_arg64(o) : next_arg(o);
            unsigned base = conv == 'u' ? 10u : conv == 'o' ? 8u : 16u;
            put_int(o, &sp, v, 0, 0, base, conv == 'X');
            break;
        }
        case 'c': {
            char c = (char)next_arg(o);
            wspec one = sp;
            one.prec = -1;
            put_str(o, &one, &c, 1);
            break;
        }
        case 's': {
            uint32_t a = next_arg(o);
            long n = gstr_len(a, WSPRINTF_MAX);
            if (n < 0) put_str(o, &sp, "(null)", 6);
            else put_str(o, &sp, (const char *)vt3_g2h(a), (size_t)n);
            break;
        }
        default:
            /* Not a conversion this file handles. Copy the spec text through unchanged. */
            vt3s_unimpl("wsprintfA conversion");
            for (uint32_t q = spec_start; q < p; q++) put(o, (char)gbyte(q));
            break;
        }
    }
    o->buf[o->len] = '\0';
}

/* Copy the formatted text into guest memory. Returns the characters written. */
static int store_out(GPTR buf, const wout *o)
{
    if (!buf || !vt3s_guest_range_ok(buf, (DWORD)o->len + 1)) return 0;
    memcpy(vt3_g2h(buf), o->buf, o->len + 1);
    return (int)o->len;
}

int WINAPI wvsprintfA(LPSTR buf, LPCSTR fmt, GPTR arglist)
{
    wout o;
    format_into(&o, fmt, arglist);
    int n = store_out(buf, &o);
    VT3_DEBUG("wvsprintfA: \"%s\" (%d chars)", o.buf, n);
    return n;
}

/* cdecl: args is the guest address of the first variadic argument (see user32_text.h). */
int WINAPI wsprintfA(LPSTR buf, LPCSTR fmt, GPTR args)
{
    wout o;
    format_into(&o, fmt, args);
    int n = store_out(buf, &o);
    VT3_DEBUG("wsprintfA: \"%s\" (%d chars)", o.buf, n);
    return n;
}
