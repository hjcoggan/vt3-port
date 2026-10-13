/* Byte and block operations. At -Os and with -mstringop-strategy=rep_byte, gcc emits
 * the string instructions directly: rep movsb, rep stosb, and repnz scasb for strlen.
 * Also checks the cmps path through a manual comparison loop.
 */
#include "tcommon.h"

static char buf_a[300];
static char buf_b[300];

static unsigned long my_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static int my_memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *x = a, *y = b;
    for (unsigned long i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

int vt_main(void)
{
    for (int i = 0; i < 300; i++) buf_a[i] = (char)(i * 7 + 1);
    memcpy(buf_b, buf_a, 300);
    kv("cmp_equal", my_memcmp(buf_a, buf_b, 300));
    buf_b[250] = 'Z';
    kv("cmp_diff", my_memcmp(buf_a, buf_b, 300) != 0);

    memset(buf_b, 0, 300);
    unsigned int sum = 0;
    for (int i = 0; i < 300; i++) sum += (unsigned char)buf_b[i];
    kv("zeroed_sum", (int)sum);

    const char *text = "the quick brown fox jumps over the lazy dog";
    kv("strlen", (int)my_strlen(text));

    char out[64];
    for (int i = 0; i < 64; i++) out[i] = 0;
    unsigned long n = my_strlen(text);
    for (unsigned long i = 0; i < n && i < 63; i++) out[i] = text[n - 1 - i];
    out_str(out);
    out_nl();

    /* A struct-sized copy. At -Os this goes through rep movs. */
    struct {
        int a[12];
    } s1, s2;
    for (int i = 0; i < 12; i++) s1.a[i] = i * 11;
    s2 = s1;
    kv("struct_copy", s2.a[7]);
    return (int)(sum & 0x7F);
}
