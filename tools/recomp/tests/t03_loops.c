/* Loops: counted, while, do-while, break and continue, nested loops, a bubble sort
 * over an array, a sieve, and Collatz lengths.
 */
#include "tcommon.h"

static int data[24];

static void fill(void)
{
    unsigned int x = 12345u;
    for (int i = 0; i < 24; i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (int)((x >> 16) & 0x7FFF) - 16000;
    }
}

static void bubble(int *a, int n)
{
    for (int i = 0; i < n - 1; i++) {
        int swapped = 0;
        for (int j = 0; j < n - 1 - i; j++) {
            if (a[j] > a[j + 1]) {
                int t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
                swapped = 1;
            }
        }
        if (!swapped) break;
    }
}

static int sieve(int n)
{
    static unsigned char composite[1000];
    int count = 0;
    for (int i = 0; i < n; i++) composite[i] = 0;
    for (int i = 2; i < n; i++) {
        if (composite[i]) continue;
        count++;
        for (int j = i * i; j < n; j += i) composite[j] = 1;
    }
    return count;
}

static unsigned int collatz(unsigned int n)
{
    unsigned int steps = 0;
    while (n != 1) {
        n = (n & 1) ? 3 * n + 1 : n >> 1;
        steps++;
    }
    return steps;
}

int vt_main(void)
{
    fill();
    bubble(data, 24);
    for (int i = 0; i < 24; i += 4) kv("sorted", data[i]);

    kv("primes_under_1000", sieve(1000));

    unsigned int best = 0, arg = 0;
    for (unsigned int n = 1; n < 2000; n++) {
        unsigned int s = collatz(n);
        if (s > best) {
            best = s;
            arg = n;
        }
    }
    kv("collatz_steps", (int)best);
    kv("collatz_arg", (int)arg);

    int i = 0, acc = 0;
    do {
        acc += i * i;
        i += 3;
    } while (i < 50);
    kv("do_acc", acc);

    int found = -1;
    for (int r = 0; r < 20; r++) {
        for (int c = 0; c < 20; c++) {
            if (c == r) continue;
            if (r * c == 221) {
                found = r * 100 + c;
                goto done;
            }
        }
    }
done:
    kv("found", found);

    int w = 1000, steps = 0;
    while (w > 1) {
        w = w / 2 + (w & 1);
        if (++steps > 100) break;
    }
    kv("halve_steps", steps);
    return (int)(best & 0x7F);
}
