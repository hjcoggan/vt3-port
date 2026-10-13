/* Internal interfaces shared by the shim's .c files. Not for the game. */
#ifndef VT3_SHIM_INTERNAL_H
#define VT3_SHIM_INTERNAL_H

#include "win32/types.h"

#include <pthread.h>

/* File object behind a VT3S_H_FILE handle. std is set for the console handles,
 * which CloseHandle must not close. */
typedef struct vfile {
    int fd;
    int64_t size;
    int std;
} vfile;

/* Handle table. HANDLE values are (slot + 1) * 4, never 0 or -1. */
typedef enum vt3s_hkind {
    VT3S_H_NONE = 0,
    VT3S_H_HEAP,
    VT3S_H_FILE,
    VT3S_H_SYNC,    /* event or mutex: a struct vt3s_sync */
    VT3S_H_THREAD,  /* a guest thread: a vt3s_thread */
    VT3S_H_FIND     /* a directory search: a vfind (kernel32_file.c) */
} vt3s_hkind;

typedef struct vt3s_thread vt3s_thread;

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

/* System DLL handles for GetModuleHandleA and LoadLibraryA. 0 when not a system DLL. */
HANDLE vt3s_system_module(LPCSTR name);

/* Heap module. */
void vt3s_heap_module_init(void);
void vt3s_heap_module_shutdown(void);

/* Guest threads and the guest lock (kernel32_thread.c). Guest code runs with gil held. */
pthread_mutex_t *vt3s_gil(void);
pthread_cond_t *vt3s_gcond(void);
uint32_t vt3s_current_tid(void);
uint32_t vt3s_now_ms(void);
vt3s_thread *vt3s_thread_attach_host(void);
DWORD vt3s_call_guest(uint32_t stack_top, uint32_t teb, uint32_t fn, const uint32_t *args, uint32_t nargs);
DWORD vt3s_call_guest_current(uint32_t fn, const uint32_t *args, uint32_t nargs);

/* Polling APIs call this. Every 64th call releases the guest lock for a moment, so a
 * thread that spins cannot starve the others (kernel32_thread.c). Caller holds gil. */
void vt3s_spin_point(void);

/* A VirtualAlloc-style page range from the arena. Returns 0 when out of space. */
uint32_t vt3s_vm_alloc(uint32_t bytes);

#endif /* VT3_SHIM_INTERNAL_H */
