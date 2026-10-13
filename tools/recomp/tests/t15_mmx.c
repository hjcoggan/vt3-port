/* MMX: the packed-integer forms on 64-bit registers. Built with -mmmx. Inputs come from
 * volatile globals so gcc does not fold the operations. Every result is printed as two
 * 32-bit halves, so the comparison with Wine is exact.
 */
#include "tcommon.h"

#include <mmintrin.h>
#include <xmmintrin.h>   /* the MMX extensions: pextrw, pinsrw, pshufw, pmovmskb, pavgb, pmaxsw */

static volatile unsigned long long va = 0x7F80FF01C8B03E22ull;
static volatile unsigned long long vb = 0x8001807FF0C2D7A5ull;
static volatile int shift_count = 3;

static void show(const char *k, __m64 v)
{
    unsigned long long u = (unsigned long long)v;
    kvx(k, (unsigned int)(u >> 32));
    kvx(k, (unsigned int)u);
}

int vt_main(void)
{
    __m64 a = (__m64)va;
    __m64 b = (__m64)vb;

    /* wrap-around add and subtract at each lane width */
    show("paddb", _mm_add_pi8(a, b));
    show("paddw", _mm_add_pi16(a, b));
    show("paddd", _mm_add_pi32(a, b));
    show("psubb", _mm_sub_pi8(a, b));
    show("psubw", _mm_sub_pi16(a, b));

    /* saturating add and subtract */
    show("paddsw", _mm_adds_pi16(a, b));
    show("paddsb", _mm_adds_pi8(a, b));
    show("paddusb", _mm_adds_pu8(a, b));
    show("psubusb", _mm_subs_pu8(a, b));
    show("psubsw", _mm_subs_pi16(a, b));

    /* multiplies */
    show("pmullw", _mm_mullo_pi16(a, b));
    show("pmulhw", _mm_mulhi_pi16(a, b));
    show("pmaddwd", _mm_madd_pi16(a, b));

    /* averages and min/max */
    show("pavgb", _mm_avg_pu8(a, b));
    show("pmaxsw", _mm_max_pi16(a, b));
    show("pminub", _mm_min_pu8(a, b));

    /* compares: all-ones or zero per lane */
    show("pcmpeqb", _mm_cmpeq_pi8(a, b));
    show("pcmpeqw", _mm_cmpeq_pi16(a, a));
    show("pcmpgtw", _mm_cmpgt_pi16(a, b));
    show("pcmpgtd", _mm_cmpgt_pi32(a, b));

    /* pack with saturation */
    show("packsswb", _mm_packs_pi16(a, b));
    show("packuswb", _mm_packs_pu16(a, b));
    show("packssdw", _mm_packs_pi32(a, b));

    /* unpack */
    show("punpcklbw", _mm_unpacklo_pi8(a, b));
    show("punpckhbw", _mm_unpackhi_pi8(a, b));
    show("punpcklwd", _mm_unpacklo_pi16(a, b));
    show("punpckhdq", _mm_unpackhi_pi32(a, b));

    /* shifts by immediate and by register count */
    show("psllw", _mm_slli_pi16(a, 5));
    show("pslld", _mm_slli_pi32(a, 17));
    show("psrlw", _mm_srli_pi16(a, 3));
    show("psrlq", _mm_srli_si64(a, 44));
    show("psraw", _mm_srai_pi16(a, 4));
    show("psrad", _mm_srai_pi32(a, shift_count + 20));
    show("psllq_reg", _mm_sll_si64(a, _mm_cvtsi32_si64(shift_count)));
    show("psraw_big", _mm_srai_pi16(a, 20));

    /* shuffle, extract, insert, and sign mask */
    show("pshufw", _mm_shuffle_pi16(a, 0x1B));
    kv("pextrw", _mm_extract_pi16(a, 2));
    show("pinsrw", (__m64)_mm_insert_pi16(a, 0x1234, 1));
    kv("pmovmskb", _mm_movemask_pi8(a));
    kv("pmovmskb_b", _mm_movemask_pi8(b));

    /* logical */
    show("pand", _mm_and_si64(a, b));
    show("pandn", _mm_andnot_si64(a, b));
    show("por", _mm_or_si64(a, b));
    show("pxor", _mm_xor_si64(a, b));

    /* a short loop that keeps values in MMX registers across iterations */
    __m64 acc = _mm_setzero_si64();
    for (int i = 0; i < 64; i++) {
        __m64 x = _mm_set1_pi16((short)(i * 37 - 900));
        acc = _mm_add_pi16(acc, _mm_mullo_pi16(x, b));
        acc = _mm_xor_si64(acc, _mm_slli_si64(a, i & 7));
    }
    show("loop_acc", acc);

    _mm_empty();
    unsigned int checksum = (unsigned int)((unsigned long long)acc & 0xFF);
    return (int)(checksum & 0x7F);
}
