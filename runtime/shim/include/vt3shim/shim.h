/*
 * Win32 shim: public entry points for the runtime.
 *
 * Call order: vt3_guest_init(), then vt3shim_init(), then the game. Set the
 * game root first if the game will open files.
 */
#ifndef VT3SHIM_H
#define VT3SHIM_H

/* Creates the process heap inside the guest arena. Returns 0 on success. */
int  vt3shim_init(void);
void vt3shim_shutdown(void);

/* Directory that Win32 paths resolve against. Trailing slash optional. */
void vt3shim_set_game_root(const char *path);

#endif /* VT3SHIM_H */
