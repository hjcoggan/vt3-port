/*
 * Win32 shim: public entry points for the runtime.
 *
 * Call order: vt3_guest_init(), then vt3shim_init(), then the game. Set the
 * game root first if the game will open files.
 */
#ifndef VT3SHIM_H
#define VT3SHIM_H

/* Runs a guest function on a guest stack, stdcall style: args are pushed right to left, and
 * the function pops them. The translator provides this (rt/vt3rt.c), because only it knows how
 * to call guest code. Returns the function's EAX. */
typedef uint32_t (*vt3shim_guest_call_fn)(uint32_t stack_top, uint32_t teb, uint32_t fn,
                                          const uint32_t *args, uint32_t nargs);
void vt3shim_set_guest_call(vt3shim_guest_call_fn fn);

/* The calling host thread becomes the guest main thread and takes the guest lock. Call once,
 * before the guest entry point runs. */
void vt3shim_thread_begin_main(void);

/* Creates the process heap inside the guest arena. Returns 0 on success. */
int  vt3shim_init(void);
void vt3shim_shutdown(void);

/* Directory that Win32 paths resolve against. Trailing slash optional. */
void vt3shim_set_game_root(const char *path);

#endif /* VT3SHIM_H */
