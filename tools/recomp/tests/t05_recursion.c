/* Recursion: fib, Ackermann, Hanoi move counts, mutual recursion, and recursion
 * through a struct parameter. Exercises call/ret and the guest stack depth.
 */
#include "tcommon.h"

typedef struct {
    int depth;
    int acc;
} state_t;

static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static int ack(int m, int n)
{
    if (m == 0) return n + 1;
    if (n == 0) return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}

static unsigned int hanoi_moves;

static void hanoi(int n, int from, int to, int via)
{
    if (n == 0) return;
    hanoi(n - 1, from, via, to);
    hanoi_moves++;
    hanoi(n - 1, via, to, from);
}

static int is_odd(int n);

static int is_even(int n)
{
    return n == 0 ? 1 : is_odd(n - 1);
}

static int is_odd(int n)
{
    return n == 0 ? 0 : is_even(n - 1);
}

static void walk(state_t *s, int levels)
{
    if (levels == 0) return;
    s->depth++;
    s->acc = s->acc * 7 + levels;
    walk(s, levels - 1);
    s->acc ^= s->depth;
}

int vt_main(void)
{
    kv("fib20", fib(20));
    kv("ack23", ack(2, 3));
    kv("ack31", ack(3, 1));
    hanoi(12, 1, 3, 2);
    kv("hanoi12", (int)hanoi_moves);
    kv("even_77", is_even(77));
    kv("odd_77", is_odd(77));
    state_t st = {0, 1};
    walk(&st, 40);
    kv("walk_depth", st.depth);
    kvx("walk_acc", (unsigned int)st.acc);
    return fib(10) & 0x7F;
}
