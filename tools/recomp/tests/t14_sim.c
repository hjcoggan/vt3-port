/* A small deterministic simulation in the style of game code: a pseudo-random
 * generator, a particle array updated with integer and float math, a spatial
 * bucket count, a sort by key, and a checksum over the final state. Integer and
 * single-precision results must match Wine exactly. Built for both SSE and x87.
 */
#include "tcommon.h"

typedef struct {
    float x, y, vx, vy;
    int life;
    unsigned short tag;
} particle_t;

static particle_t parts[64];
static unsigned int rng_state = 0x2545F491u;

static unsigned int rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static float rndf(void)
{
    return (float)(rnd() & 0xFFFF) / 65536.0f;
}

static void spawn(particle_t *p, int i)
{
    p->x = rndf() * 640.0f;
    p->y = rndf() * 480.0f;
    p->vx = (rndf() - 0.5f) * 8.0f;
    p->vy = (rndf() - 0.5f) * 8.0f - 1.0f;
    p->life = 30 + (int)(rnd() % 90u);
    p->tag = (unsigned short)i;
}

static void update(particle_t *p)
{
    p->vy += 0.15f;
    p->x += p->vx;
    p->y += p->vy;
    if (p->x < 0.0f) { p->x = -p->x; p->vx = -p->vx * 0.8f; }
    if (p->x > 640.0f) { p->x = 1280.0f - p->x; p->vx = -p->vx * 0.8f; }
    if (p->y > 480.0f) { p->y = 960.0f - p->y; p->vy = -p->vy * 0.6f; }
    p->life--;
}

static int bucket_count(void)
{
    int buckets[8] = {0};
    for (int i = 0; i < 64; i++) {
        int bx = (int)(parts[i].x / 80.0f);
        if (bx < 0) bx = 0;
        if (bx > 7) bx = 7;
        buckets[bx]++;
    }
    int best = 0;
    for (int i = 1; i < 8; i++) if (buckets[i] > buckets[best]) best = i;
    return best * 100 + buckets[best];
}

static void sort_by_life(void)
{
    for (int i = 1; i < 64; i++) {
        particle_t t = parts[i];
        int j = i - 1;
        while (j >= 0 && parts[j].life > t.life) {
            parts[j + 1] = parts[j];
            j--;
        }
        parts[j + 1] = t;
    }
}

int vt_main(void)
{
    for (int i = 0; i < 64; i++) spawn(&parts[i], i);

    unsigned int checksum = 0;
    for (int frame = 0; frame < 240; frame++) {
        for (int i = 0; i < 64; i++) {
            if (parts[i].life > 0) update(&parts[i]);
            else spawn(&parts[i], i);
        }
        if (frame % 60 == 59) {
            kv("frame", frame);
            kv("bucket", bucket_count());
        }
        checksum = checksum * 1664525u + (unsigned int)parts[frame & 63].life;
    }
    sort_by_life();
    for (int i = 0; i < 4; i++) {
        kv("life", parts[i].life);
        kv("tag", parts[i].tag);
    }
    kvx("checksum", checksum);

    /* Final positions as scaled integers (floats are exact only in SSE builds). */
    int sx = 0, sy = 0;
    for (int i = 0; i < 64; i++) {
        sx += (int)parts[i].x;
        sy += (int)parts[i].y;
    }
    kv("sum_x", sx);
    kv("sum_y", sy);
    return (int)(checksum & 0x7F);
}
