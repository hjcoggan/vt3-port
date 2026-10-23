#include "vt3/vt3_guest.h"
#include "vt3/vt3_log.h"

#include <stdint.h>
#include <sys/mman.h>

uint8_t *vt3_guest_base_;   /* exported for the shim's inline accessors (vt3_guest.h) */
size_t vt3_guest_size_;

int vt3_guest_init(size_t bytes)
{
    if (vt3_guest_base_) return 0;
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
    vt3_guest_base_ = (uint8_t *)p;
    vt3_guest_size_ = bytes;
    VT3_INFO("guest arena: %zu MiB at host %p", bytes >> 20, (void *)vt3_guest_base_);
    return 0;
}

void vt3_guest_shutdown(void)
{
    if (!vt3_guest_base_) return;
    munmap(vt3_guest_base_, vt3_guest_size_);
    vt3_guest_base_ = NULL;
    vt3_guest_size_ = 0;
}

size_t vt3_guest_size(void)
{
    return vt3_guest_size_;
}

void *vt3_g2h(vt3_gaddr a)
{
    if (!vt3_guest_base_ || a == 0 || (size_t)a >= vt3_guest_size_) return NULL;
    return vt3_guest_base_ + a;
}

vt3_gaddr vt3_h2g(const void *p)
{
    if (!vt3_guest_base_ || !p) return 0;
    uintptr_t off = (uintptr_t)p - (uintptr_t)vt3_guest_base_;
    if (off >= vt3_guest_size_) return 0;
    return (vt3_gaddr)off;
}
