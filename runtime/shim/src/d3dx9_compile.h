/* D3DX shader compilation for the HLSL subset the game's debug shaders use (d3dx9_compile.c). */
#ifndef VT3_SHIM_D3DX9_COMPILE_H
#define VT3_SHIM_D3DX9_COMPILE_H

#include <stddef.h>
#include <stdint.h>

/* Compiles HLSL source to SM2 tokens for the named entry point and profile (vs_2_0 or
 * ps_2_0). Returns 0 with *out (malloc'd, *nout tokens) set, or -1 with msg holding the
 * reason and the line number. Host-only: no guest memory is touched. */
int d3dx_compile_hlsl(const char *src, size_t len, const char *entry, const char *profile,
                      uint32_t **out, uint32_t *nout, char *msg, size_t msgcap);

/* A guest ID3DXBuffer holding a copy of nbytes at bytes. Returns 0 when out of memory. */
uint32_t d3dx_buffer_new(const void *bytes, uint32_t nbytes);

/* Frees the data of an ID3DXConstantTable object (d3dx9_ctable.c). */
void d3dx_ctab_free(void *data);

#endif /* VT3_SHIM_D3DX9_COMPILE_H */
