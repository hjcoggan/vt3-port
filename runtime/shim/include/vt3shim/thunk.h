/*
 * Guest addresses for shim functions. A shim function that a guest vtable (or any
 * guest code reached through a pointer) calls needs one of these addresses.
 *
 * The recompiler's loader defines vt3shim_fn_token(). It maps a name from the shim's
 * adapter table to a token address, and vt_icall dispatches a call to that token
 * through the same stdcall adapter that an import uses. The native test runtime
 * has no guest code, so the weak default in shim/src/d3d9_object.c returns 0.
 */
#ifndef VT3SHIM_THUNK_H
#define VT3SHIM_THUNK_H

#include <stdint.h>

/* Token address for the shim function `name`, or 0 when there is none. */
uint32_t vt3shim_fn_token(const char *name);

#endif /* VT3SHIM_THUNK_H */
