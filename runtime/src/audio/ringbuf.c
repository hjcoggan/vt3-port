/*
 * Single-producer, single-consumer byte ring. head and tail are monotonic
 * byte counters. Only the writer stores head, and only the reader stores
 * tail, so the acquire/release pairs are enough without a lock.
 */
#include "vt3/vt3_audio.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct vt3_ringbuf {
    uint8_t *buf;
    size_t cap;
    _Atomic size_t head;   /* bytes ever written */
    _Atomic size_t tail;   /* bytes ever read */
};

static size_t min_sz(size_t a, size_t b) { return a < b ? a : b; }

vt3_ringbuf *vt3_rb_create(size_t capacity)
{
    if (capacity == 0) return NULL;
    vt3_ringbuf *rb = calloc(1, sizeof *rb);
    if (!rb) return NULL;
    rb->buf = malloc(capacity);
    if (!rb->buf) {
        free(rb);
        return NULL;
    }
    rb->cap = capacity;
    atomic_init(&rb->head, 0);
    atomic_init(&rb->tail, 0);
    return rb;
}

void vt3_rb_destroy(vt3_ringbuf *rb)
{
    if (!rb) return;
    free(rb->buf);
    free(rb);
}

size_t vt3_rb_used(const vt3_ringbuf *rb)
{
    size_t h = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t t = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return h - t;
}

size_t vt3_rb_space(const vt3_ringbuf *rb)
{
    return rb->cap - vt3_rb_used(rb);
}

size_t vt3_rb_capacity(const vt3_ringbuf *rb)
{
    return rb->cap;
}

size_t vt3_rb_write(vt3_ringbuf *rb, const void *src, size_t n)
{
    size_t h = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&rb->tail, memory_order_acquire);
    size_t n_ok = min_sz(n, rb->cap - (h - t));
    if (n_ok == 0) return 0;

    size_t off = h % rb->cap;
    size_t first = min_sz(n_ok, rb->cap - off);
    memcpy(rb->buf + off, src, first);
    memcpy(rb->buf, (const uint8_t *)src + first, n_ok - first);

    atomic_store_explicit(&rb->head, h + n_ok, memory_order_release);
    return n_ok;
}

size_t vt3_rb_read(vt3_ringbuf *rb, void *dst, size_t n)
{
    size_t h = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t t = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t n_ok = min_sz(n, h - t);
    if (n_ok == 0) return 0;

    size_t off = t % rb->cap;
    size_t first = min_sz(n_ok, rb->cap - off);
    memcpy(dst, rb->buf + off, first);
    memcpy((uint8_t *)dst + first, rb->buf, n_ok - first);

    atomic_store_explicit(&rb->tail, t + n_ok, memory_order_release);
    return n_ok;
}
