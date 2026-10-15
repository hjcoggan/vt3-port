/* GLSL for the built-in programs. Vulkan versions only: the GL renderer keeps its own strings. */
#include "vt3/vt3_builtin.h"

const char *const vt3_builtin_ff_fs_vk =
    "#version 450\n"
    "layout(location = 0) in vec4 vo_D0;\n"
    "layout(location = 2) in vec4 vo_T0;\n"
    "layout(set = 1, binding = 0) uniform sampler2D s0;\n"
    "layout(location = 0) out vec4 oC0;\n"
    "void main() { oC0 = vo_D0 * texture(s0, vo_T0.xy); }\n";

/* The push constants are the logical size (for the pixel-to-clip mapping) and the texture flag. */
const char *const vt3_builtin_tl_vs_vk =
    "#version 450\n"
    "layout(location = 0) in vec4 aPosRhw;\n"
    "layout(location = 1) in vec4 aColor;\n"        /* bytes arrive as B,G,R,A */
    "layout(location = 2) in vec2 aUV;\n"
    "layout(push_constant) uniform Push { vec2 size; int use_tex; } pc;\n"
    "layout(location = 0) out vec4 vColor;\n"
    "layout(location = 1) out vec2 vUV;\n"
    "void main() {\n"
    "    float invw = 1.0 / aPosRhw.w;\n"
    "    vec2 ndc = vec2(aPosRhw.x / pc.size.x * 2.0 - 1.0, aPosRhw.y / pc.size.y * 2.0 - 1.0);\n"
    "    gl_Position = vec4(ndc * invw, aPosRhw.z * invw, invw);\n"
    "    vColor = aColor.bgra;\n"
    "    vUV = aUV;\n"
    "}\n";

const char *const vt3_builtin_tl_fs_vk =
    "#version 450\n"
    "layout(location = 0) in vec4 vColor;\n"
    "layout(location = 1) in vec2 vUV;\n"
    "layout(push_constant) uniform Push { vec2 size; int use_tex; } pc;\n"
    "layout(set = 1, binding = 0) uniform sampler2D uTex;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main() {\n"
    "    vec4 c = vColor;\n"
    "    if (pc.use_tex != 0) c *= texture(uTex, vUV);\n"
    "    oColor = c;\n"
    "}\n";
