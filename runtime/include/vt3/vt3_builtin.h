/*
 * GLSL the runtime writes itself, besides the game's shaders: the fixed-function pixel shader and
 * the transformed-quad (XYZRHW) pair. The Vulkan versions are compiled to SPIR-V into the shader pack
 * (tools/formats/shaders/gen_spirv_pack.py), which looks them up by the hash of this text.
 */
#ifndef VT3_BUILTIN_H
#define VT3_BUILTIN_H

extern const char *const vt3_builtin_ff_fs_vk;   /* fixed function: colour times texture (set 1, binding 0) */
extern const char *const vt3_builtin_tl_vs_vk;   /* XYZRHW vertex: pixel coordinates to clip space */
extern const char *const vt3_builtin_tl_fs_vk;   /* XYZRHW fragment: colour, optionally times the texture */

#endif /* VT3_BUILTIN_H */
