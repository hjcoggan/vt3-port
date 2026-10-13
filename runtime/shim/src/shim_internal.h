/* Internal interfaces shared by the shim's .c files. Not for the game. */
#ifndef VT3_SHIM_INTERNAL_H
#define VT3_SHIM_INTERNAL_H

#include "win32/types.h"

/* Handle table. HANDLE values are (slot + 1) * 4, never 0 or -1. */
typedef enum vt3s_hkind {
    VT3S_H_NONE = 0,
    VT3S_H_HEAP,
    VT3S_H_FILE
} vt3s_hkind;

HANDLE vt3s_handle_new(vt3s_hkind kind, void *obj);
void  *vt3s_handle_get(HANDLE h, vt3s_hkind kind);
void  *vt3s_handle_take(HANDLE h, vt3s_hkind kind);   /* removes the entry */

/* Errors and logging. */
void vt3s_set_last_error(DWORD err);
void vt3s_unimpl(const char *api);   /* logs once per API name */

/* Guest memory access helpers. Return 0 / do nothing for NULL or out of range. */
int      vt3s_guest_range_ok(GPTR p, DWORD bytes);
void     vt3s_store_u32(GPTR p, uint32_t v);
void     vt3s_store_u64(GPTR p, uint64_t v);
uint32_t vt3s_load_u32(GPTR p);

/* Game root for file paths. Never NULL: "." if unset. */
const char *vt3s_game_root(void);

/* Heap module. */
void vt3s_heap_module_init(void);
void vt3s_heap_module_shutdown(void);

#endif /* VT3_SHIM_INTERNAL_H */
