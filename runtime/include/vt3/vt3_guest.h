/*
 * Guest address space.
 *
 * The recompiled x86 code addresses memory with 32-bit guest addresses. The
 * runtime reserves one contiguous host region and maps guest address A to
 * host pointer (base + A). The PE image at 0x00400000 and the shim heap both
 * live inside this region, so the translation is a single add.
 *
 * Guest address 0 is NULL. vt3_g2h(0) returns NULL.
 */
#ifndef VT3_GUEST_H
#define VT3_GUEST_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t vt3_gaddr;

#define VT3_GUEST_DEFAULT_BYTES  ((size_t)1 << 30)   /* 1 GiB reservation */
#define VT3_GUEST_IMAGE_BASE     0x00400000u         /* PE default ImageBase for VT3.exe */
#define VT3_GUEST_HEAP_BASE      0x20000000u         /* shim heap starts here */

/* Reserve the arena. Idempotent. Returns 0 on success. */
int   vt3_guest_init(size_t bytes);
void  vt3_guest_shutdown(void);
size_t vt3_guest_size(void);

/* Translate. Returns NULL / 0 for addresses outside the arena. */
void     *vt3_g2h(vt3_gaddr a);
vt3_gaddr vt3_h2g(const void *p);

#endif /* VT3_GUEST_H */
