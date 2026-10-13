/*
 * Heap on top of the guest arena. First-fit over an address-ordered list of
 * blocks, with coalescing on free. Block metadata lives on the host, not in
 * guest memory, so the guest cannot corrupt it.
 *
 * Known limits (TODO):
 *   - Linear search for allocations and frees. Replace with a hash or size classes.
 *   - HeapDestroy does not return the region to the arena (bump-allocated).
 *   - HEAP_NO_SERIALIZE is ignored. Every heap is locked.
 */
#include "shim_internal.h"
#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"
#include "win32/kernel32.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define HEAP_MIN_BLOCK     16u
#define HEAP_DEFAULT_BYTES (1u << 20)
#define HEAP_PAGE          4096u

typedef struct blk {
    vt3_gaddr addr;
    uint32_t size;        /* allocated or free span, rounded to 16 */
    uint32_t req;         /* bytes the caller asked for (HeapSize) */
    int used;
    struct blk *prev, *next;
} blk;

typedef struct vt3s_heap {
    pthread_mutex_t mu;
    blk *head;
    int is_process_heap;
} vt3s_heap;

/* Bump allocator for regions handed out to HeapCreate. */
static vt3_gaddr g_carve_next;
static vt3_gaddr g_carve_end;
static HANDLE g_process_heap;

static uint32_t align16(uint64_t n)
{
    return (uint32_t)((n + 15u) & ~(uint64_t)15u);
}

static blk *blk_new(vt3_gaddr addr, uint32_t size, int used)
{
    blk *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->addr = addr;
    b->size = size;
    b->used = used;
    return b;
}

static void unlink_blk(vt3s_heap *h, blk *b)
{
    if (b->prev) b->prev->next = b->next;
    else h->head = b->next;
    if (b->next) b->next->prev = b->prev;
    free(b);
}

static void coalesce(vt3s_heap *h, blk *b)
{
    while (b->next && !b->next->used) {
        blk *n = b->next;
        b->size += n->size;
        unlink_blk(h, n);
    }
    if (b->prev && !b->prev->used) {
        blk *p = b->prev;
        p->size += b->size;
        unlink_blk(h, b);
    }
}

/* Splits b so that it keeps `need` bytes and the rest becomes a free block. */
static void split(vt3s_heap *h, blk *b, uint32_t need)
{
    if (b->size - need < HEAP_MIN_BLOCK) return;
    blk *r = blk_new(b->addr + need, b->size - need, 0);
    if (!r) return;
    r->prev = b;
    r->next = b->next;
    if (b->next) b->next->prev = r;
    b->next = r;
    b->size = need;
    (void)h;
}

static vt3s_heap *heap_new(vt3_gaddr base, uint32_t size, int is_process)
{
    vt3s_heap *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    pthread_mutex_init(&h->mu, NULL);
    h->head = blk_new(base, size, 0);
    h->is_process_heap = is_process;
    if (!h->head) {
        free(h);
        return NULL;
    }
    return h;
}

static vt3s_heap *heap_of(HANDLE hh)
{
    return (vt3s_heap *)vt3s_handle_get(hh, VT3S_H_HEAP);
}

static blk *find_used(vt3s_heap *h, vt3_gaddr addr)
{
    for (blk *b = h->head; b; b = b->next) {
        if (b->used && b->addr == addr) return b;
    }
    return NULL;
}

void vt3s_heap_module_init(void)
{
    /* Process heap: lower half of the region above the image and below the top. */
    vt3_gaddr base = VT3_GUEST_HEAP_BASE;
    uint32_t total = (uint32_t)(vt3_guest_size() - base);
    uint32_t process_size = (total / 2) & ~(HEAP_PAGE - 1);
    vt3s_heap *ph = heap_new(base, process_size, 1);
    g_process_heap = vt3s_handle_new(VT3S_H_HEAP, ph);

    g_carve_next = base + process_size;
    g_carve_end = (vt3_gaddr)vt3_guest_size();
    VT3_INFO("heap: process heap %u MiB at 0x%08x", process_size >> 20, base);
}

void vt3s_heap_module_shutdown(void)
{
    vt3s_heap *ph = (vt3s_heap *)vt3s_handle_take(g_process_heap, VT3S_H_HEAP);
    if (!ph) return;
    for (blk *b = ph->head; b;) {
        blk *n = b->next;
        free(b);
        b = n;
    }
    pthread_mutex_destroy(&ph->mu);
    free(ph);
    g_process_heap = 0;
}

HANDLE WINAPI GetProcessHeap(void)
{
    return g_process_heap;
}

HANDLE WINAPI HeapCreate(DWORD options, SIZE_T initial_size, SIZE_T maximum_size)
{
    (void)options;
    (void)maximum_size;
    uint64_t want = initial_size ? initial_size : HEAP_DEFAULT_BYTES;
    uint64_t size = (want + HEAP_PAGE - 1) & ~(uint64_t)(HEAP_PAGE - 1);
    if (size == 0 || (uint64_t)g_carve_next + size > g_carve_end) {
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    vt3s_heap *h = heap_new(g_carve_next, (uint32_t)size, 0);
    if (!h) {
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    g_carve_next += (vt3_gaddr)size;
    HANDLE hh = vt3s_handle_new(VT3S_H_HEAP, h);
    if (!hh) {
        free(h->head);
        pthread_mutex_destroy(&h->mu);
        free(h);
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    }
    return hh;
}

BOOL WINAPI HeapDestroy(HANDLE hh)
{
    vt3s_heap *h = heap_of(hh);
    if (!h || h->is_process_heap) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    vt3s_handle_take(hh, VT3S_H_HEAP);
    for (blk *b = h->head; b;) {
        blk *n = b->next;
        free(b);
        b = n;
    }
    pthread_mutex_destroy(&h->mu);
    free(h);
    return TRUE;
}

GPTR WINAPI HeapAlloc(HANDLE hh, DWORD flags, SIZE_T bytes)
{
    vt3s_heap *h = heap_of(hh);
    if (!h) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (bytes > 0x7FFFFFFFu) {
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    uint32_t need = align16(bytes ? bytes : 1);
    vt3_gaddr addr = 0;

    pthread_mutex_lock(&h->mu);
    for (blk *b = h->head; b; b = b->next) {
        if (!b->used && b->size >= need) {
            split(h, b, need);
            b->used = 1;
            b->req = (uint32_t)bytes;
            addr = b->addr;
            break;
        }
    }
    pthread_mutex_unlock(&h->mu);

    if (!addr) {
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (flags & HEAP_ZERO_MEMORY) {
        memset(vt3_g2h(addr), 0, bytes);
    }
    vt3s_set_last_error(ERROR_SUCCESS);
    return addr;
}

BOOL WINAPI HeapFree(HANDLE hh, DWORD flags, GPTR mem)
{
    (void)flags;
    vt3s_heap *h = heap_of(hh);
    if (!h) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&h->mu);
    blk *b = find_used(h, mem);
    if (b) {
        b->used = 0;
        b->req = 0;
        coalesce(h, b);
    }
    pthread_mutex_unlock(&h->mu);
    if (!b) {
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

GPTR WINAPI HeapReAlloc(HANDLE hh, DWORD flags, GPTR mem, SIZE_T bytes)
{
    vt3s_heap *h = heap_of(hh);
    if (!h) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (mem == 0) return HeapAlloc(hh, flags, bytes);
    if (bytes > 0x7FFFFFFFu) {
        vt3s_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    uint32_t need = align16(bytes ? bytes : 1);
    pthread_mutex_lock(&h->mu);
    blk *b = find_used(h, mem);
    if (!b) {
        pthread_mutex_unlock(&h->mu);
        vt3s_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (need <= b->size) {
        /* Shrink or same size: keep the block, free any tail. */
        uint32_t old_req = b->req;
        split(h, b, need);
        if (b->next && !b->next->used) coalesce(h, b->next);
        b->req = (uint32_t)bytes;
        pthread_mutex_unlock(&h->mu);
        if ((flags & HEAP_ZERO_MEMORY) && bytes > old_req) {
            memset((uint8_t *)vt3_g2h(mem) + old_req, 0, bytes - old_req);
        }
        vt3s_set_last_error(ERROR_SUCCESS);
        return mem;
    }
    uint32_t old_req = b->req;
    pthread_mutex_unlock(&h->mu);

    GPTR fresh = HeapAlloc(hh, flags, bytes);
    if (!fresh) return 0;
    uint32_t copy = old_req < bytes ? old_req : (uint32_t)bytes;
    memcpy(vt3_g2h(fresh), vt3_g2h(mem), copy);
    HeapFree(hh, 0, mem);
    return fresh;
}

SIZE_T WINAPI HeapSize(HANDLE hh, DWORD flags, GPTR mem)
{
    (void)flags;
    vt3s_heap *h = heap_of(hh);
    if (!h) {
        vt3s_set_last_error(ERROR_INVALID_HANDLE);
        return (SIZE_T)-1;
    }
    pthread_mutex_lock(&h->mu);
    blk *b = find_used(h, mem);
    SIZE_T size = b ? b->req : (SIZE_T)-1;
    pthread_mutex_unlock(&h->mu);
    if (!b) vt3s_set_last_error(ERROR_INVALID_PARAMETER);
    return size;
}
