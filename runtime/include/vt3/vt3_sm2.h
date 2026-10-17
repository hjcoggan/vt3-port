/*
 * Shader Model 2 (vs_2_0, ps_2_0) bytecode to GLSL 330 translator.
 *
 * Input: the D3D9 token stream (version token first, end token 0x0000FFFF),
 * as little-endian 32-bit words. Output: GLSL source plus the facts the device
 * needs to bind it: input locations, sampler types, constant defaults, and
 * the constant range used.
 *
 * Conventions in the generated code (see runtime/docs in d3d9-scope.md):
 *   - float constants: uniform vec4 vsc[256] / psc[256]
 *   - int constants:   uniform ivec4 vsi[16] / psi[16] (loop counts)
 *   - bool constants:  uniform bool vsb[16] / psb[16]
 *   - VS -> PS varyings: vo_D0, vo_D1 (colour), vo_T0..vo_T7 (texcoord)
 *   - VS position: gl_Position, with z converted from D3D [0,1] to GL [-1,1]
 *   - PS output: oC0
 *
 * Meanings of the constant registers are not checked here. The translator
 * only reproduces what the bytecode says.
 */
#ifndef VT3_SM2_H
#define VT3_SM2_H

#include <stddef.h>
#include <stdint.h>

#define VT3_SM2_MAX_CONST    256
#define VT3_SM2_MAX_INT      16
#define VT3_SM2_MAX_BOOL     16
#define VT3_SM2_MAX_SAMPLER  16
#define VT3_SM2_MAX_ATTR     16
#define VT3_SM2_MAX_TEMP     32
#define VT3_SM2_MAX_TEXCOORD 8

typedef enum vt3_sm_kind {
    VT3_SM_VS = 0,
    VT3_SM_PS = 1
} vt3_sm_kind;

typedef struct vt3_sm2_shader {
    vt3_sm_kind kind;
    int major, minor;
    int ninsn;                          /* instructions translated (excludes dcl/def) */
    char *glsl;                         /* malloc'd; free with vt3_sm2_free() */
    char error[160];                    /* set when translation fails */

    uint32_t attr_mask;                 /* VS: bit N set when vertex attribute location N is read */
    uint8_t  sampler_type[VT3_SM2_MAX_SAMPLER];   /* 0 unused, 2 = 2D, 3 = cube */
    uint16_t sampler_mask;              /* bit N for sampler sN */
    int      const_max;                 /* one past the highest float constant addressed */
    uint16_t bool_used;                 /* bit N for bool register bN */

    float    cdef[VT3_SM2_MAX_CONST][4];    /* def c#: defaults, valid where cdef_set */
    uint8_t  cdef_set[VT3_SM2_MAX_CONST];
    uint8_t  cdef_n;                        /* entries in cdef_idx; 0 for most shaders */
    uint16_t cdef_idx[VT3_SM2_MAX_CONST];   /* the c# that have a def, so a draw need not scan 256 */
    int32_t  idef[VT3_SM2_MAX_INT][4];      /* defi i#: defaults, valid where idef_set */
    uint8_t  idef_set[VT3_SM2_MAX_INT];
} vt3_sm2_shader;

/* Translate one shader. Returns 0 on success. On failure, out->error holds why. */
int  vt3_sm2_translate(const uint32_t *tokens, size_t ntokens, vt3_sm2_shader *out);

/* The same, with flags. VT3_SM2_FLAG_VULKAN writes GLSL 450 for SPIR-V (see the note above):
 * constants in uniform blocks (binding 0 for vs, 1 for ps), samplers in set 1 at their index,
 * varyings at fixed locations (D0 = 0, D1 = 1, T# = 2 + #), clip space as Vulkan's (z kept,
 * y negated so that the image is D3D's orientation). */
#define VT3_SM2_FLAG_VULKAN 1u
int  vt3_sm2_translate_ex(const uint32_t *tokens, size_t ntokens, vt3_sm2_shader *out, unsigned flags);
void vt3_sm2_free(vt3_sm2_shader *out);

/*
 * Vertex input location for a D3D usage (D3DDECLUSAGE_*) and usage index.
 * Returns -1 when the usage has no location in this layout.
 * The shim uses the same mapping when it binds a vertex declaration.
 */
int vt3_sm2_attr_location(uint32_t usage, uint32_t usage_index);

#endif /* VT3_SM2_H */
