#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"

#include <stdint.h>
#include <sys/mman.h>

static uint8_t *g_base;
static size_t g_size;

int vt3_guest_init(size_t bytes)
{
    if (g_base) return 0;
    if (bytes == 0 || bytes > 0xFFFFFFFFu) {
        VT3_ERROR("guest arena: size %zu is outside the 32-bit guest space", bytes);
        return -1;
    }
    /* NORESERVE: pages are committed only when the guest touches them. */
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        VT3_ERROR("guest arena: mmap of %zu bytes failed", bytes);
        return -1;
    }
    g_base = (uint8_t *)p;
    g_size = bytes;
    VT3_INFO("guest arena: %zu MiB at host %p", bytes >> 20, (void *)g_base);
    return 0;
}

void vt3_guest_shutdown(void)
{
    if (!g_base) return;
    munmap(g_base, g_size);
    g_base = NULL;
    g_size = 0;
}

size_t vt3_guest_size(void)
{
    return g_size;
}

void *vt3_g2h(vt3_gaddr a)
{
    if (!g_base || a == 0 || (size_t)a >= g_size) return NULL;
    return g_base + a;
}

vt3_gaddr vt3_h2g(const void *p)
{
    if (!g_base || !p) return 0;
    uintptr_t off = (uintptr_t)p - (uintptr_t)g_base;
    if (off >= g_size) return 0;
    return (vt3_gaddr)off;
}
