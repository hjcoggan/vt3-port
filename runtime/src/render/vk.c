/*
 * Vulkan renderer for Android: the vt3_gfx_ops backend the device uses.
 *
 * It follows gl33.c: one offscreen target at the logical resolution, with D3D's orientation (row 0
 * at the top, so textures and vertices mean what they mean in D3D); draws recorded in order; present
 * blits the target to a letterboxed rectangle of the swapchain.
 *
 * Orientation: the vertex epilogue negates clip-space y (sm2_glsl.c), and Vulkan's framebuffer has
 * row 0 at the top, so every image a draw lands in has D3D's row order: the logical buffer and every
 * render target alike. Unlike GL (whose back buffer is bottom-up and whose targets need vt3_flip),
 * nothing here flips again: blits copy rows as they are and the cull winding is the same everywhere.
 *
 * Render targets (rt_create, rt_bind, blit): each one is a colour image the game can sample, plus a
 * depth image and a framebuffer. A render pass is begun lazily, for the target the game has bound, and
 * ended when the target changes, when a draw samples a target that is still an attachment, and for
 * blits and present; layout barriers go between those. Shaders are GLSL 450 from the SM2
 * translator (VT3_SM2_FLAG_VULKAN), compiled offline to SPIR-V and looked up in the shader pack
 * (/sdcard/vt3/shaders.pack, from tools/formats/shaders/gen_spirv_pack.py) by the hash of their text.
 *
 * Frames: NFRAMES slots, each with a fence, an acquire semaphore, a descriptor pool and two command
 * buffers (uploads, then rendering). Rings hold per-frame vertices, indices and the constant blocks.
 * Submissions form a chain on one semaphore, so the GPU runs frames in order and a texture uploaded
 * for a later frame is never written while an earlier frame reads it.
 */
/* Vulkan's structure initialisers leave pNext and the rest zero on purpose. */
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#ifdef __ANDROID__
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <android/native_window.h>
#endif
#include <vulkan/vulkan.h>

#include "vt3/vt3_builtin.h"
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"
#include "vt3/vt3_renderer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NFRAMES 2
#define VT3_CONST_REGS 256
#define PROG_MAX 512
#define PIPE_MAX 1024
#define MODULE_MAX 1024
#define PACK_MAX 2048
#define DESC_SETS_PER_FRAME 8192
#define VB_RING (32u << 20)            /* per frame: a busy frame redraws characters for the shadow passes */
#define IB_RING (16u << 20)            /* per frame: the index ring filled and dropped draws at 2 MB */
#define UBO_RING (16u << 20)           /* per frame */
#define VS_BLOCK_BYTES 4416u           /* vsc[256] + vsi[16] + vsb4[4], std140 */
#define PS_BLOCK_BYTES 4432u           /* psc[256] + psi[16] + psb4[4] + at_func + at_ref */
#define UBO_SLOT 4608u                 /* each block aligned to 256 bytes, the usual minUniformBufferOffsetAlignment */
#define TEX_WHITE (VT3_MAX_TEX + 0)
#define TEX_ZERO (VT3_MAX_TEX + 1)
#define TEX_ZERO_CUBE (VT3_MAX_TEX + 2)
#define TEX_SLOTS (VT3_MAX_TEX + 3)
#define TL_VERTEX_BYTES 28u
#define RPK_RGBA8 0                    /* the logical buffer and A8R8G8B8 targets */
#define RPK_RGBA16F 1                  /* G16R16F targets (the game's variance shadow maps), stored as RGBA16F */
#define RPK_COUNT 2
#ifndef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#define VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME "VK_KHR_portability_subset"
#endif

#define VKT(expr, what)                                                  \
    do {                                                                 \
        VkResult r_ = (expr);                                            \
        if (r_ != VK_SUCCESS) {                                          \
            VT3_ERROR("vulkan: %s failed (%d)", (what), (int)r_);        \
            return -1;                                                   \
        }                                                                \
    } while (0)

typedef struct vk_tex {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageLayout layout;
    int w, h;
    int cube;
    int used;
    VkFormat format;
    VkImageViewType view_type;
    int bpp;                       /* bytes per texel as uploaded: 4, or 1 for A8 */
    int mips;                      /* mip levels in the image (1 for cube/render targets) */
    int rt;                        /* a render target: the fields below are set */
    int rpk;                       /* render pass kind: RPK_RGBA8 or RPK_RGBA16F */
    VkImage dep;
    VkDeviceMemory dep_mem;
    VkImageView dep_view;
    VkFramebuffer fb;
} vk_tex;

typedef struct vk_prog {
    VkShaderModule vs, ps;
    uint8_t cube_mask;             /* sampler n is a samplerCube in this program */
    uint16_t sampler_mask;         /* samplers the program declares (sampler2D or samplerCube) */
    int used;
} vk_prog;

typedef struct vk_pipe_key {
    uint32_t prog;                 /* 1-based program handle, 0 = the built-in TL pair */
    uint32_t layout_hash;
    uint32_t state_hash;
    uint32_t topology;
    uint32_t rpk;                  /* render pass kind: the colour format of the target */
} vk_pipe_key;

typedef struct vk_pipe {
    vk_pipe_key key;
    VkPipeline pipe;
    int used;
} vk_pipe;

typedef struct vk_module {
    uint64_t hash;
    VkShaderModule mod;
    int used;
} vk_module;

typedef struct vk_pack_entry {
    uint64_t hash;
    uint32_t stage;                /* 0 vertex, 1 fragment */
    uint32_t offset, size;         /* into the pack file buffer */
} vk_pack_entry;

#define SAMPLER_CACHE 128
typedef struct vk_sampler_entry {
    uint8_t minf, magf, u, v, w;
    VkSampler sampler;
} vk_sampler_entry;

/* A resource to free once the frame that used it has finished. */
typedef struct vk_dead {
    int is_image;                  /* 0: a buffer and its memory; 1: an image, its view and its memory */
    VkBuffer buf;
    VkImage img;
    VkImageView view;
    VkDeviceMemory mem;
    VkImage img2;                  /* a render target's depth image, view and memory, and its framebuffer */
    VkImageView view2;
    VkDeviceMemory mem2;
    VkFramebuffer fb;
} vk_dead;

typedef struct vk_frame {
    VkCommandBuffer cmd_up, cmd;
    VkFence fence;
    VkSemaphore acquire;
    VkDescriptorPool pool;
    VkDescriptorSet set0;
    uint32_t vb_base, ib_base, ubo_base;   /* this frame's ring regions */
    uint32_t vb_used, ib_used, ubo_used;
    int recording;
    int timed;                     /* its two timestamps were written and the frame submitted */
    vk_dead *dead;                 /* freed when this slot is next used: the GPU may read them until then */
    uint32_t ndead, capdead;
} vk_frame;

static struct {
    int logical_w, logical_h;
#ifdef __ANDROID__
    ANativeWindow *window;
#else
    void *window;                  /* unused on the desktop */
#endif
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkSwapchainKHR sc;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t nsc;
    VkImage sc_img[8];
    VkSemaphore sc_done[8];
    VkPresentModeKHR present_mode;
    VkImage col, dep;
    VkDeviceMemory col_mem, dep_mem;
    VkImageView col_view, dep_view;
    VkImageLayout col_layout;
    VkRenderPass rps[RPK_COUNT];   /* one per colour format; the depth format is the same for all */
    VkFramebuffer fb;
    vt3_tex tgt;                   /* the render target draws and clears go to; 0 = the logical buffer */
    int pass_open;                 /* a render pass is recording in the current frame's command buffer */
    vt3_tex pass_tgt;              /* the target of that pass */
    int linear_f16;                /* RGBA16F can be blitted with a linear filter */
    VkQueryPool qp;                /* two timestamps per frame slot: the GPU time of each frame, for the trace */
    uint64_t ts_mask;
    double ts_period_ns;
    VkCommandPool pool;
    VkSemaphore chain;
    int chain_armed;
    vk_frame fr[NFRAMES];
    uint32_t cur;
    VkDescriptorSetLayout dsl0, dsl1;
    VkPipelineLayout pl;
    VkSampler samp;                /* default clamp/linear sampler, also used by TL */
    vk_sampler_entry samplers[SAMPLER_CACHE];
    uint32_t nsamplers;
    float max_aniso;
    int aniso_enabled;
    VkBuffer ubo, vb, ib;
    VkDeviceMemory ubo_mem, vb_mem, ib_mem;
    uint8_t *ubo_ptr, *vb_ptr, *ib_ptr;
    vk_tex tex[TEX_SLOTS];
    vk_prog progs[PROG_MAX];
    vk_pipe pipes[PIPE_MAX];
    vk_module modules[MODULE_MAX];
    uint8_t *pack;
    size_t pack_size;
    vk_pack_entry pack_entries[PACK_MAX];
    uint32_t npack;
    VkShaderModule tl_vs, tl_fs;
    int scissor_on;
    vt3_rect scissor;
    int open;                      /* a frame is being recorded */
} g;

/* ---- helpers ------------------------------------------------------------- */

static uint64_t fnv1a64(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static int find_memory(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out)
{
    for (uint32_t i = 0; i < g.mem_props.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        if ((g.mem_props.memoryTypes[i].propertyFlags & want) == want) {
            *out = i;
            return 0;
        }
    }
    return -1;
}

static int alloc_bind(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory *mem)
{
    uint32_t type;
    if (find_memory(req.memoryTypeBits, want, &type) != 0) return -1;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    return vkAllocateMemory(g.dev, &ai, NULL, mem) == VK_SUCCESS ? 0 : -1;
}

static void defer(vk_frame *f, vk_dead d)
{
    if (f->ndead == f->capdead) {
        uint32_t nc = f->capdead ? f->capdead * 2 : 64;
        vk_dead *p = realloc(f->dead, nc * sizeof *p);
        if (!p) {
            VT3_ERROR("vulkan: out of memory tracking freed resources");
            return;
        }
        f->dead = p;
        f->capdead = nc;
    }
    f->dead[f->ndead++] = d;
}

static void destroy_dead(const vk_dead *d)
{
    if (d->fb) vkDestroyFramebuffer(g.dev, d->fb, NULL);
    if (d->is_image) {
        if (d->view) vkDestroyImageView(g.dev, d->view, NULL);
        if (d->img) vkDestroyImage(g.dev, d->img, NULL);
    } else {
        if (d->buf) vkDestroyBuffer(g.dev, d->buf, NULL);
    }
    if (d->view2) vkDestroyImageView(g.dev, d->view2, NULL);
    if (d->img2) vkDestroyImage(g.dev, d->img2, NULL);
    if (d->mem) vkFreeMemory(g.dev, d->mem, NULL);
    if (d->mem2) vkFreeMemory(g.dev, d->mem2, NULL);
}

/* Frees what a frame slot kept alive. Called only after the slot's fence has signalled. */
static void free_dead(vk_frame *f)
{
    for (uint32_t i = 0; i < f->ndead; i++) destroy_dead(&f->dead[i]);
    f->ndead = 0;
}

/* A host-visible staging buffer, mapped. It is freed when the frame slot that uses it is reused. */
static int stage_alloc(vk_frame *f, VkDeviceSize size, VkBuffer *buf_out, void **ptr)
{
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g.dev, &bi, NULL, &buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, buf, &req);
    if (alloc_bind(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mem) != 0) {
        vkDestroyBuffer(g.dev, buf, NULL);
        return -1;
    }
    vkBindBufferMemory(g.dev, buf, mem, 0);
    if (vkMapMemory(g.dev, mem, 0, size, 0, ptr) != VK_SUCCESS) {
        vkDestroyBuffer(g.dev, buf, NULL);
        vkFreeMemory(g.dev, mem, NULL);
        return -1;
    }
    vk_dead d = { 0, buf, VK_NULL_HANDLE, VK_NULL_HANDLE, mem };
    defer(f, d);
    *buf_out = buf;
    return 0;
}

/* ---- format and state mapping ---------------------------------------------- */

static VkBlendFactor map_blend(int d3d)
{
    switch (d3d) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp map_blendop(int d3d)
{
    switch (d3d) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp map_cmp(int d3d)
{
    switch (d3d) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static uint32_t state_hash(const vt3_rstate *s)
{
    uint32_t h = 2166136261u;
    const uint32_t v[] = { (uint32_t)s->alpha_blend, (uint32_t)s->src_blend, (uint32_t)s->dst_blend,
                           (uint32_t)s->depth_test, (uint32_t)s->depth_write, (uint32_t)s->depth_func,
                           (uint32_t)s->cull, (uint32_t)s->blend_op, (uint32_t)s->color_write };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        h ^= v[i];
        h *= 16777619u;
    }
    return h;
}

/* Vertex layouts and their formats. */
static VkFormat attr_format(const vt3_vattr *a)
{
    static const VkFormat f_float[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                         VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    static const VkFormat f_ubyte[4] = { VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED,
                                         VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED };
    static const VkFormat f_ubyte_n[4] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                           VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    static const VkFormat f_short[4] = { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                         VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
    static const VkFormat f_short_n[4] = { VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM,
                                           VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
    int c = a->components >= 1 && a->components <= 4 ? a->components - 1 : 3;
    switch (a->type) {
    case VT3_VT_UBYTE_NORM: return f_ubyte_n[c];
    case VT3_VT_UBYTE: return f_ubyte[c];
    case VT3_VT_SHORT: return f_short[c];
    case VT3_VT_SHORT_NORM: return f_short_n[c];
    default: return f_float[c];
    }
}

static uint32_t layout_hash(const vt3_vlayout *l)
{
    uint32_t h = 2166136261u ^ (uint32_t)l->stride;
    for (int i = 0; i < l->count; i++) {
        const uint32_t v[] = { (uint32_t)l->attr[i].location, (uint32_t)l->attr[i].components,
                               (uint32_t)l->attr[i].type, (uint32_t)l->attr[i].offset };
        for (size_t k = 0; k < 4; k++) {
            h ^= v[k];
            h *= 16777619u;
        }
    }
    return h;
}

/* ---- the shader pack --------------------------------------------------------- */

static int pack_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        VT3_ERROR("vulkan: no shader pack at %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 16) {
        fclose(f);
        return -1;
    }
    g.pack = malloc((size_t)n);
    if (!g.pack || fread(g.pack, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    g.pack_size = (size_t)n;
    if (memcmp(g.pack, "VT3PACK1", 8) != 0) {
        VT3_ERROR("vulkan: %s is not a shader pack", path);
        return -1;
    }
    uint32_t count;
    memcpy(&count, g.pack + 8, 4);
    size_t at = 12;
    for (uint32_t i = 0; i < count && i < PACK_MAX; i++) {
        if (at + 16 > g.pack_size) break;
        vk_pack_entry *e = &g.pack_entries[g.npack++];
        memcpy(&e->hash, g.pack + at, 8);
        memcpy(&e->stage, g.pack + at + 8, 4);
        memcpy(&e->size, g.pack + at + 12, 4);
        e->offset = (uint32_t)(at + 16);
        at += 16 + e->size;
    }
    VT3_INFO("vulkan: shader pack %s: %u programs", path, (unsigned)g.npack);
    return 0;
}

static VkShaderModule module_for_text(const char *text, uint32_t stage_want, char *err, size_t cap)
{
    uint64_t h = fnv1a64(text, strlen(text));
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g.modules[i].used && g.modules[i].hash == h) return g.modules[i].mod;
    }
    for (uint32_t i = 0; i < g.npack; i++) {
        vk_pack_entry *e = &g.pack_entries[i];
        if (e->hash != h) continue;
        if (e->stage != stage_want) {
            snprintf(err, cap, "shader %016llx is the wrong stage", (unsigned long long)h);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = e->size;
        ci.pCode = (const uint32_t *)(g.pack + e->offset);
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(g.dev, &ci, NULL, &mod) != VK_SUCCESS) {
            snprintf(err, cap, "vkCreateShaderModule failed for %016llx", (unsigned long long)h);
            return VK_NULL_HANDLE;
        }
        for (int k = 0; k < MODULE_MAX; k++) {
            if (!g.modules[k].used) {
                g.modules[k].used = 1;
                g.modules[k].hash = h;
                g.modules[k].mod = mod;
                break;
            }
        }
        return mod;
    }
    snprintf(err, cap, "no SPIR-V in the pack for shader %016llx (rebuild the pack)", (unsigned long long)h);
    return VK_NULL_HANDLE;
}

/* Which samplers the program declares as samplerCube (the text is the translator's output). */
static uint8_t cube_samplers(const char *text)
{
    uint8_t mask = 0;
    for (int n = 0; n < 16; n++) {
        char pat[32];
        snprintf(pat, sizeof pat, "samplerCube s%d;", n);
        if (strstr(text, pat)) mask |= (uint8_t)(1u << n);
    }
    return mask;
}

/* Which samplers the program declares at all, 2D or cube. Only those are bound to real textures. */
static uint16_t declared_samplers(const char *text)
{
    uint16_t mask = 0;
    for (int n = 0; n < 16; n++) {
        char pat[32];
        snprintf(pat, sizeof pat, "sampler2D s%d;", n);
        if (strstr(text, pat)) mask |= (uint16_t)(1u << n);
        snprintf(pat, sizeof pat, "samplerCube s%d;", n);
        if (strstr(text, pat)) mask |= (uint16_t)(1u << n);
    }
    return mask;
}

/* ---- instance, device, swapchain --------------------------------------------- */

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VKT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surf, &caps), "surface capabilities");
    VkExtent2D ext = caps.currentExtent;
#ifdef __ANDROID__
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = (uint32_t)ANativeWindow_getWidth(g.window);
        ext.height = (uint32_t)ANativeWindow_getHeight(g.window);
    }
#endif
    if (ext.width == 0 || ext.height == 0) return -1;

    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surf, &nfmt, NULL);
    VkSurfaceFormatKHR fmts[16];
    if (nfmt > 16) nfmt = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surf, &nfmt, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nfmt; i++) {
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            pick = fmts[i];
            if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) break;
        }
    }

    uint32_t nmodes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surf, &nmodes, NULL);
    VkPresentModeKHR modes[8];
    if (nmodes > 8) nmodes = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surf, &nmodes, modes);
    /* FIFO only: the game advances once per presented frame, so presents must wait for vsync (as on
     * the desktop, where SDL's swap interval does). Mailbox would let it run several times too fast. */
    g.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    (void)modes;

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = g.surf;
    sci.minImageCount = want;
    sci.imageFormat = pick.format;
    sci.imageColorSpace = pick.colorSpace;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = g.present_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = g.sc;
    VKT(vkCreateSwapchainKHR(g.dev, &sci, NULL, &g.sc), "vkCreateSwapchainKHR");
    if (sci.oldSwapchain) vkDestroySwapchainKHR(g.dev, sci.oldSwapchain, NULL);
    g.sc_format = pick.format;
    g.sc_extent = ext;
    vkGetSwapchainImagesKHR(g.dev, g.sc, &g.nsc, NULL);
    if (g.nsc > 8) g.nsc = 8;
    vkGetSwapchainImagesKHR(g.dev, g.sc, &g.nsc, g.sc_img);
    VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < g.nsc; i++) {
        if (!g.sc_done[i]) VKT(vkCreateSemaphore(g.dev, &sem, NULL, &g.sc_done[i]), "semaphore");
    }
    VT3_INFO("vulkan: swapchain %ux%u, format %d, %u images, present mode %d", ext.width, ext.height,
             (int)pick.format, (unsigned)g.nsc, (int)g.present_mode);
    return 0;
}

/* Total mip levels of a full pyramid for a w x h texture. */
static uint32_t full_mip_count(uint32_t w, uint32_t h)
{
    uint32_t m = 0, mx = w > h ? w : h;
    while (mx) { mx >>= 1; m++; }
    return m;
}

static int create_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageCreateFlags flags,
                        uint32_t layers, uint32_t mips, VkImage *img, VkDeviceMemory *mem)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.flags = flags;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = mips;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKT(vkCreateImage(g.dev, &ii, NULL, img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, *img, &req);
    if (alloc_bind(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem) != 0) return -1;
    VKT(vkBindImageMemory(g.dev, *img, *mem, 0), "vkBindImageMemory");
    return 0;
}

static VkImageView create_view(VkImage img, VkFormat fmt, VkImageViewType type, uint32_t layers, uint32_t mips,
                               VkComponentMapping swz, VkImageAspectFlags aspect)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img;
    vi.viewType = type;
    vi.format = fmt;
    vi.components = swz;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.layerCount = layers;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(g.dev, &vi, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

/* A render pass for one colour format, with a D32 depth attachment. Both attachments are loaded and
 * stored, so the game's contents persist across passes (it clears what it wants cleared), and both
 * stay in their attachment layouts at the pass boundaries: the colour layouts change by barriers
 * outside the pass (layout_to), the depth image never leaves DEPTH_STENCIL_ATTACHMENT_OPTIMAL. */
static int create_render_pass(VkFormat fmt, VkRenderPass *out)
{
    VkAttachmentDescription at[2] = {0};
    at[0].format = fmt;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[1].format = VK_FORMAT_D32_SFLOAT;
    at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colour_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colour_ref;
    sub.pDepthStencilAttachment = &depth_ref;
    /* Earlier writes to either attachment (a previous pass on the same target, or a clear) come before
     * this pass's reads and writes. The colour layout barriers carry the transfer and shader hazards. */
    VkSubpassDependency dep = {0};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = dep.srcStageMask;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 2;
    rpi.pAttachments = at;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VKT(vkCreateRenderPass(g.dev, &rpi, NULL, out), "vkCreateRenderPass");
    return 0;
}

static int create_offscreen(void)
{
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    if (create_image((uint32_t)g.logical_w, (uint32_t)g.logical_h, fmt,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     0, 1, 1, &g.col, &g.col_mem) != 0) return -1;
    if (create_image((uint32_t)g.logical_w, (uint32_t)g.logical_h, VK_FORMAT_D32_SFLOAT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, 1, 1, &g.dep, &g.dep_mem) != 0) return -1;
    VkComponentMapping id = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    g.col_view = create_view(g.col, fmt, VK_IMAGE_VIEW_TYPE_2D, 1, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    g.dep_view = create_view(g.dep, VK_FORMAT_D32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, 1, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!g.col_view || !g.dep_view) return -1;

    if (create_render_pass(VK_FORMAT_R8G8B8A8_UNORM, &g.rps[RPK_RGBA8]) != 0) return -1;
    if (create_render_pass(VK_FORMAT_R16G16B16A16_SFLOAT, &g.rps[RPK_RGBA16F]) != 0) return -1;

    VkImageView views[2] = { g.col_view, g.dep_view };
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = g.rps[RPK_RGBA8];
    fci.attachmentCount = 2;
    fci.pAttachments = views;
    fci.width = (uint32_t)g.logical_w;
    fci.height = (uint32_t)g.logical_h;
    fci.layers = 1;
    VKT(vkCreateFramebuffer(g.dev, &fci, NULL, &g.fb), "vkCreateFramebuffer");
    g.col_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, VK_FORMAT_R16G16B16A16_SFLOAT, &fp);
    g.linear_f16 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    return 0;
}

/* ---- command helpers ----------------------------------------------------------- */

static int ensure_frame(void);
static int create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, void **ptr);

static VkCommandPool g_once_pool;

static int once_submit(void (*record)(VkCommandBuffer, void *), void *arg)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = g_once_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VKT(vkAllocateCommandBuffers(g.dev, &ai, &cb), "one-time command buffer");
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    record(cb, arg);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult r = vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) vkQueueWaitIdle(g.queue);
    vkFreeCommandBuffers(g.dev, g_once_pool, 1, &cb);
    return r == VK_SUCCESS ? 0 : -1;
}

static void barrier_image(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                          VkPipelineStageFlags dst_stage, VkAccessFlags dst_access,
                          uint32_t first_layer, uint32_t layers, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = first_layer;
    b.subresourceRange.layerCount = layers;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Stages and accesses that make or consume an image in each of the layouts a colour image moves
 * between. "src" is the scope of what happened in that layout (writes only: reads need only an
 * execution dependency), "dst" what will happen in it. */
static void layout_scope(VkImageLayout l, int as_src, VkPipelineStageFlags *stage, VkAccessFlags *access)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = as_src ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        *access = as_src ? 0 : VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = as_src ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    default:   /* UNDEFINED: nothing to wait for */
        *stage = as_src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        *access = 0;
        break;
    }
}

/* Moves a colour image from the layout the recording last left it in to `want`, with the barrier for
 * the hazards between the two uses. `cur` is the tracked layout. Recording is linear and frames run
 * in order, so the tracked layout is the layout at this point of the GPU timeline. */
static void layout_to(VkCommandBuffer cb, VkImage img, VkImageLayout *cur, VkImageLayout want)
{
    if (*cur == want) return;
    VkPipelineStageFlags ss, ds;
    VkAccessFlags sa, da;
    layout_scope(*cur, 1, &ss, &sa);
    layout_scope(want, 0, &ds, &da);
    barrier_image(cb, img, *cur, want, ss, sa, ds, da, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    *cur = want;
}

/* ---- textures ------------------------------------------------------------------- */

typedef struct upload_arg {
    VkBuffer src;
    VkImage img;
    uint32_t layer;
    uint32_t w, h;
    uint32_t row_pixels;
    int cube;
    uint32_t mips;
} upload_arg;

/* A barrier for one mip level of one layer (used while generating the mip chain). */
static void barrier_mip(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                        VkPipelineStageFlags dst_stage, VkAccessFlags dst_access,
                        uint32_t layer, uint32_t mip, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = mip;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = layer;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void record_upload(VkCommandBuffer cb, void *p)
{
    upload_arg *u = p;
    barrier_image(cb, u->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, u->layer, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy c = {0};
    c.bufferRowLength = u->row_pixels;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.baseArrayLayer = u->layer;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = u->w;
    c.imageExtent.height = u->h;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, u->src, u->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    /* Generate the mip chain from level 0. The game loads one level, but a single-mip fine grid
     * (the net) aliases badly at distance; pre-filtering removes the shimmer. */
    uint32_t pw = u->w, ph = u->h;
    for (uint32_t l = 1; l < u->mips; l++) {
        uint32_t mw = pw >> 1 ? pw >> 1 : 1u;
        uint32_t mh = ph >> 1 ? ph >> 1 : 1u;
        barrier_mip(cb, u->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    u->layer, l - 1, VK_IMAGE_ASPECT_COLOR_BIT);
        barrier_mip(cb, u->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, u->layer, l, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit blit = {0};
        blit.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l - 1, u->layer, 1 };
        blit.srcOffsets[1] = (VkOffset3D){ (int)pw, (int)ph, 1 };
        blit.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l, u->layer, 1 };
        blit.dstOffsets[1] = (VkOffset3D){ (int)mw, (int)mh, 1 };
        vkCmdBlitImage(cb, u->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u->img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        pw = mw;
        ph = mh;
    }
    for (uint32_t l = 0; l < u->mips; l++) {
        int was_src = l + 1 < u->mips;
        barrier_mip(cb, u->img,
                    was_src ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    was_src ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    u->layer, l, VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

/* Formats: the device gets BGRA bytes (D3DFMT_A8R8G8B8 order) or A8 as one byte per texel. */
static VkFormat tex_format(vt3_texfmt fmt)
{
    return fmt == VT3_TEXFMT_A8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
}

static VkComponentMapping tex_swizzle(vt3_texfmt fmt)
{
    VkComponentMapping m = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    if (fmt == VT3_TEXFMT_A8) {                       /* white, alpha from the byte */
        m.r = VK_COMPONENT_SWIZZLE_ONE;
        m.g = VK_COMPONENT_SWIZZLE_ONE;
        m.b = VK_COMPONENT_SWIZZLE_ONE;
        m.a = VK_COMPONENT_SWIZZLE_R;
    } else if (fmt == VT3_TEXFMT_X8R8G8B8) {
        m.a = VK_COMPONENT_SWIZZLE_ONE;
    }
    return m;
}

static int tex_build(vk_tex *t, int w, int h, int cube, vt3_texfmt fmt)
{
    VkImageCreateFlags flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    uint32_t layers = cube ? 6 : 1;
    /* A 2-D texture gets a full mip chain even though the game loads one level: the net's
     * single-mip grid aliases at distance, so pre-filter the pyramid on upload. Cube textures and
     * render targets keep one level (their sampling paths never select a mip). */
    uint32_t mips = cube ? 1u : full_mip_count((uint32_t)w, (uint32_t)h);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (mips > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    if (create_image((uint32_t)w, (uint32_t)h, tex_format(fmt), usage,
                     flags, layers, mips, &t->image, &t->mem) != 0) return -1;
    t->view = create_view(t->image, tex_format(fmt), cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, layers, mips,
                          tex_swizzle(fmt), VK_IMAGE_ASPECT_COLOR_BIT);
    if (!t->view) return -1;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->w = w;
    t->h = h;
    t->cube = cube;
    t->format = tex_format(fmt);
    t->view_type = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    t->mips = mips;
    t->bpp = fmt == VT3_TEXFMT_A8 ? 1 : 4;
    t->used = 1;
    return 0;
}

/* A texture the game released: the GPU may still read it in the frame being recorded, so it is freed
 * with that frame's slot. Outside a frame nothing is in flight that the texture can be part of. */
static void end_pass(void);

static void tex_release(vk_tex *t)
{
    vk_dead d = { 1, VK_NULL_HANDLE, t->image, t->view, t->mem, t->dep, t->dep_view, t->dep_mem, t->fb };
    if (t->rt) {
        vt3_tex h = (vt3_tex)(t - g.tex + 1);
        if (g.pass_open && g.pass_tgt == h) end_pass();   /* the pass recorded so far keeps the images alive */
        if (g.tgt == h) g.tgt = 0;
    }
    if (g.open) {
        defer(&g.fr[g.cur], d);
    } else {
        vkQueueWaitIdle(g.queue);
        destroy_dead(&d);
    }
    memset(t, 0, sizeof *t);
}

static int vk_tex_create(vt3_tex *out, int w, int h, vt3_texfmt fmt)
{
    if (!out || w <= 0 || h <= 0) return -1;
    uint32_t slot = 0;
    while (slot < VT3_MAX_TEX && g.tex[slot].used) slot++;
    if (slot == VT3_MAX_TEX) {
        static int warned_full;
        if (!warned_full) {
            warned_full = 1;
            VT3_WARN("vulkan: texture table full (%d): new textures have no handle", VT3_MAX_TEX);
        }
        return -1;
    }
    if (tex_build(&g.tex[slot], w, h, 0, fmt) != 0) {
        VT3_WARN("vulkan: could not create a %dx%d texture (format %d)", w, h, (int)fmt);
        return -1;
    }
    *out = (vt3_tex)(slot + 1);
    return 0;
}

static void upload_rows(vk_tex *t, uint32_t layer, const void *pixels, int pitch, int w, int h)
{
    if (ensure_frame() != 0) return;
    vk_frame *f = &g.fr[g.cur];
    size_t bytes = (size_t)pitch * (size_t)h;
    VkBuffer sb = VK_NULL_HANDLE;
    void *ptr = NULL;
    if (stage_alloc(f, bytes, &sb, &ptr) != 0) {
        VT3_WARN("vulkan: staging buffer of %zu bytes failed", bytes);
        return;
    }
    memcpy(ptr, pixels, bytes);
    upload_arg u = { sb, t->image, layer, (uint32_t)w, (uint32_t)h, (uint32_t)(pitch / t->bpp), t->cube, t->mips };
    record_upload(f->cmd_up, &u);
}

static vk_tex *tex_of(vt3_tex t)
{
    if (t == 0 || t > VT3_MAX_TEX || !g.tex[t - 1].used) return NULL;
    return &g.tex[t - 1];
}

static void vk_tex_upload(vt3_tex t, const void *pixels, int pitch, int w, int h)
{
    vk_tex *tx = tex_of(t);
    if (!tx || tx->cube || w != tx->w || h != tx->h || !pixels) return;   /* sub-rect uploads: TODO, as in gl33 */
    if (tx->rt) {
        static int warned;
        if (!warned) {
            warned = 1;
            VT3_WARN("vulkan: CPU upload to a render target %dx%d is ignored", w, h);
        }
        return;
    }
    upload_rows(tx, 0, pixels, pitch, w, h);
}

static void vk_tex_destroy(vt3_tex t)
{
    vk_tex *tx = tex_of(t);
    if (tx) tex_release(tx);
}

static int vk_cube_create(vt3_tex *out, int size, vt3_texfmt fmt)
{
    if (!out || size <= 0 || fmt == VT3_TEXFMT_A8) return -1;
    uint32_t slot = 0;
    while (slot < VT3_MAX_TEX && g.tex[slot].used) slot++;
    if (slot == VT3_MAX_TEX) return -1;
    if (tex_build(&g.tex[slot], size, size, 1, fmt) != 0) {
        VT3_WARN("vulkan: could not create a %d cube texture", size);
        return -1;
    }
    *out = (vt3_tex)(slot + 1);
    return 0;
}

static void vk_cube_upload(vt3_tex t, int face, const void *pixels, int pitch, int size)
{
    vk_tex *tx = tex_of(t);
    if (!tx || !tx->cube || face < 0 || face >= 6 || size != tx->w || !pixels) return;
    upload_rows(tx, (uint32_t)face, pixels, pitch, size, size);
}

/* A one-texel texture, every layer set to px. */
typedef struct dummy_arg {
    VkBuffer src;
    VkImage img;
    uint32_t layers;
} dummy_arg;

static void record_dummy(VkCommandBuffer cb, void *p)
{
    dummy_arg *a = p;
    barrier_image(cb, a->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, a->layers, VK_IMAGE_ASPECT_COLOR_BIT);
    for (uint32_t l = 0; l < a->layers; l++) {
        VkBufferImageCopy c = {0};
        c.bufferOffset = 4u * l;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.baseArrayLayer = l;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = 1;
        c.imageExtent.height = 1;
        c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, a->src, a->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    barrier_image(cb, a->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, a->layers,
                  VK_IMAGE_ASPECT_COLOR_BIT);
}

static int upload_dummy(vk_tex *t, uint32_t px, uint32_t layers)
{
    VkBuffer sb = VK_NULL_HANDLE;
    VkDeviceMemory sm = VK_NULL_HANDLE;
    void *ptr = NULL;
    if (create_buffer(4 * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &ptr) != 0) return -1;
    for (uint32_t l = 0; l < layers; l++) memcpy((uint8_t *)ptr + 4 * l, &px, 4);
    dummy_arg a = { sb, t->image, layers };
    int rc = once_submit(record_dummy, &a);
    vkDestroyBuffer(g.dev, sb, NULL);
    vkFreeMemory(g.dev, sm, NULL);
    return rc;
}


/* ---- frames ------------------------------------------------------------------ */

/* Per-second diagnostic counters, enabled only by VT3_TRACE_FPS: where frame time goes. */
static struct {
    uint64_t t0, fence_ns, acquire_ns, limit_ns, frames, draws, passes, blits;
    uint64_t asked_ns, slow_frames, max_ns, last_present;
    uint64_t pipes, pipe_ns, submit_ns, record_ns, gpu_ns, gpu_frames;
} g_stats;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int stats_enabled(void)
{
    static int on = -1;
    if (on < 0) on = getenv("VT3_TRACE_FPS") != NULL;
    return on;
}

static void stats_log(void)
{
    if (!stats_enabled()) return;
    uint64_t now = now_ns();
    g_stats.frames++;
    if (g_stats.last_present) {
        uint64_t dt = now - g_stats.last_present;
        if (dt > 20000000ull) g_stats.slow_frames++;
        if (dt > g_stats.max_ns) g_stats.max_ns = dt;
    }
    g_stats.last_present = now;
    if (!g_stats.t0) g_stats.t0 = now;
    if (now - g_stats.t0 < 1000000000ull) return;
    double n = (double)g_stats.frames;
    VT3_INFO("vulkan: frames over 20 ms: %u, longest %.1f ms, limiter asked %.2f ms per frame; "
             "pipelines created %u in %.1f ms; submit %.2f ms per frame; draw recording %.2f ms per frame; "
             "GPU %.2f ms per frame",
             (unsigned)g_stats.slow_frames, (double)g_stats.max_ns / 1e6, (double)g_stats.asked_ns / n / 1e6,
             (unsigned)g_stats.pipes, (double)g_stats.pipe_ns / 1e6, (double)g_stats.submit_ns / n / 1e6,
             (double)g_stats.record_ns / n / 1e6,
             g_stats.gpu_frames ? (double)g_stats.gpu_ns / (double)g_stats.gpu_frames / 1e6 : 0.0);
    VT3_INFO("vulkan: per frame: %.0f draws, %.1f passes, %.1f blits; waits: fence %.2f ms, acquire %.2f ms, "
             "frame limit %.2f ms",
             (double)g_stats.draws / n, (double)g_stats.passes / n, (double)g_stats.blits / n,
             (double)g_stats.fence_ns / n / 1e6, (double)g_stats.acquire_ns / n / 1e6,
             (double)g_stats.limit_ns / n / 1e6);
    uint64_t keep = g_stats.last_present;
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.last_present = keep;
    g_stats.t0 = now;
}

static int ensure_frame(void)
{
    if (g.open) return 0;
    vk_frame *f = &g.fr[g.cur];
    uint64_t wt0 = stats_enabled() ? now_ns() : 0;
    VkResult w = vkWaitForFences(g.dev, 1, &f->fence, VK_TRUE, 5000000000ull);
    if (stats_enabled()) g_stats.fence_ns += now_ns() - wt0;
    if (w != VK_SUCCESS) {
        VT3_ERROR("vulkan: frame %u did not finish (fence %d)", (unsigned)g.cur, (int)w);
        return -1;
    }
    vkResetFences(g.dev, 1, &f->fence);
    if (f->timed && g.qp) {
        uint64_t t[2];
        if (vkGetQueryPoolResults(g.dev, g.qp, 2 * g.cur, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS) {
            g_stats.gpu_ns += (uint64_t)((double)((t[1] - t[0]) & g.ts_mask) * g.ts_period_ns);
            g_stats.gpu_frames++;
        }
    }
    f->timed = 0;
    free_dead(f);
    vkResetDescriptorPool(g.dev, f->pool, 0);
    f->vb_used = f->ib_used = f->ubo_used = 0;

    VkDescriptorSetAllocateInfo sai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    sai.descriptorPool = f->pool;
    sai.descriptorSetCount = 1;
    sai.pSetLayouts = &g.dsl0;
    if (vkAllocateDescriptorSets(g.dev, &sai, &f->set0) != VK_SUCCESS) {
        VT3_ERROR("vulkan: descriptor set for the frame failed");
        return -1;
    }
    /* Both constant blocks are dynamic: the offsets are absolute positions in the ring buffer. */
    VkDescriptorBufferInfo bi = { g.ubo, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet ws[2] = {0};
    for (uint32_t b = 0; b < 2; b++) {
        ws[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[b].dstSet = f->set0;
        ws[b].dstBinding = b;
        ws[b].descriptorCount = 1;
        ws[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ws[b].pBufferInfo = &bi;
    }
    vkUpdateDescriptorSets(g.dev, 2, ws, 0, NULL);

    VkCommandBufferBeginInfo bgi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(f->cmd_up, 0);
    vkResetCommandBuffer(f->cmd, 0);
    vkBeginCommandBuffer(f->cmd_up, &bgi);
    vkBeginCommandBuffer(f->cmd, &bgi);
    if (g.qp) {
        vkCmdResetQueryPool(f->cmd_up, g.qp, 2 * g.cur, 2);
        vkCmdWriteTimestamp(f->cmd_up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.qp, 2 * g.cur);
    }

    f->recording = 1;
    g.open = 1;
    g.pass_open = 0;   /* a render pass is begun by the first draw or clear, for the target bound then */
    return 0;
}

/* ---- render passes and targets ---------------------------------------------------- */

static vk_tex *rt_of(vt3_tex t)
{
    if (t == 0 || t > VT3_MAX_TEX || !g.tex[t - 1].used || !g.tex[t - 1].rt) return NULL;
    return &g.tex[t - 1];
}

/* Size of a target: 0 (or a handle that is not a render target) is the logical buffer. */
static void target_size(vt3_tex t, int *w, int *h)
{
    vk_tex *r = rt_of(t);
    *w = r ? r->w : g.logical_w;
    *h = r ? r->h : g.logical_h;
}

static void end_pass(void)
{
    if (!g.pass_open) return;
    vkCmdEndRenderPass(g.fr[g.cur].cmd);
    g.pass_open = 0;
}

/* Makes sure a render pass is recording on the bound target, ending a pass on another target first. */
static int ensure_pass(void)
{
    if (ensure_frame() != 0) return -1;
    vk_tex *r = rt_of(g.tgt);
    vt3_tex want = r ? g.tgt : 0;
    if (g.pass_open) {
        if (g.pass_tgt == want) return 0;
        end_pass();
    }
    VkCommandBuffer cb = g.fr[g.cur].cmd;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    if (r) {
        layout_to(cb, r->image, &r->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rbi.renderPass = g.rps[r->rpk];
        rbi.framebuffer = r->fb;
    } else {
        layout_to(cb, g.col, &g.col_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rbi.renderPass = g.rps[RPK_RGBA8];
        rbi.framebuffer = g.fb;
    }
    int w, h;
    target_size(want, &w, &h);
    rbi.renderArea.extent.width = (uint32_t)w;
    rbi.renderArea.extent.height = (uint32_t)h;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    g.pass_open = 1;
    g.pass_tgt = want;
    if (stats_enabled()) g_stats.passes++;
    return 0;
}

/* A draw is about to sample t. A render target must be in its shader-read layout for that, which
 * cannot change inside a render pass: the open pass ends, and the next draw or clear begins another.
 * A target the pass is rendering to cannot be sampled at all (see sampled_view). */
static void prepare_sampled(vt3_tex t)
{
    vk_tex *r = rt_of(t);
    if (!r || t == g.tgt || r->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
    end_pass();
    layout_to(g.fr[g.cur].cmd, r->image, &r->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* Ends and submits the current frame. Frames run in order: each submission waits on the chain
 * semaphore that the previous one signalled. */
static int submit_frame(uint32_t sc_index, int use_sc)
{
    vk_frame *f = &g.fr[g.cur];
    end_pass();
    if (g.qp) vkCmdWriteTimestamp(f->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qp, 2 * g.cur + 1);
    VKT(vkEndCommandBuffer(f->cmd_up), "end upload command buffer");
    VKT(vkEndCommandBuffer(f->cmd), "end render command buffer");
    VkSemaphore waits[2];
    VkPipelineStageFlags stages[2];
    uint32_t nw = 0;
    if (use_sc) {
        waits[nw] = f->acquire;
        stages[nw++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (g.chain_armed) {
        waits[nw] = g.chain;
        stages[nw++] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkSemaphore signals[2];
    uint32_t ns = 0;
    signals[ns++] = g.chain;
    if (use_sc) signals[ns++] = g.sc_done[sc_index];
    VkCommandBuffer cbs[2] = { f->cmd_up, f->cmd };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = nw;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 2;
    si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = ns;
    si.pSignalSemaphores = signals;
    uint64_t st0 = stats_enabled() ? now_ns() : 0;
    VKT(vkQueueSubmit(g.queue, 1, &si, f->fence), "vkQueueSubmit");
    if (stats_enabled()) g_stats.submit_ns += now_ns() - st0;
    g.chain_armed = 1;
    f->timed = g.qp != VK_NULL_HANDLE;
    f->recording = 0;
    g.open = 0;
    g.cur = (g.cur + 1) % NFRAMES;
    return 0;
}

/* ---- rendering ----------------------------------------------------------------- */

/* The scissor (D3D: top-left origin, in target pixels), clipped to the bound target. */
static VkRect2D current_scissor(void)
{
    int tw, th;
    target_size(g.tgt, &tw, &th);
    VkRect2D r = { { 0, 0 }, { (uint32_t)tw, (uint32_t)th } };
    if (g.scissor_on) {
        int x0 = g.scissor.x < 0 ? 0 : g.scissor.x;
        int y0 = g.scissor.y < 0 ? 0 : g.scissor.y;
        int x1 = g.scissor.x + g.scissor.w, y1 = g.scissor.y + g.scissor.h;
        if (x1 > tw) x1 = tw;
        if (y1 > th) y1 = th;
        r.offset.x = x0;
        r.offset.y = y0;
        r.extent.width = x1 > x0 ? (uint32_t)(x1 - x0) : 0;
        r.extent.height = y1 > y0 ? (uint32_t)(y1 - y0) : 0;
    }
    return r;
}

/* The D3D viewport (vt3_rstate vp_*: logical pixels, top-left origin; none = the whole bound target)
 * and its depth range. Its height is positive and its rows are D3D's own: the vertex epilogue already
 * negates y, so row 0 is the top of every target (see the header comment). gl33.c converts the rows
 * for its bottom-up back buffer; here there is nothing to convert. */
static VkViewport current_viewport(const vt3_rstate *st)
{
    int tw, th;
    target_size(g.tgt, &tw, &th);
    VkViewport vp = { 0.0f, 0.0f, (float)tw, (float)th, 0.0f, 1.0f };
    if (st && st->vp_w > 0 && st->vp_h > 0) {
        vp.x = (float)st->vp_x;
        vp.y = (float)st->vp_y;
        vp.width = (float)st->vp_w;
        vp.height = (float)st->vp_h;
        vp.minDepth = st->vp_min_z < 0.0f ? 0.0f : (st->vp_min_z > 1.0f ? 1.0f : st->vp_min_z);
        vp.maxDepth = st->vp_max_z < 0.0f ? 0.0f : (st->vp_max_z > 1.0f ? 1.0f : st->vp_max_z);
    }
    return vp;
}

static void vk_set_scissor(int enable, vt3_rect r)
{
    g.scissor_on = enable;
    g.scissor = r;
}

static void vk_begin_frame(void)
{
    ensure_frame();
}

static void vk_clear(uint32_t argb)
{
    if (ensure_pass() != 0) return;
    VkClearAttachment ca = {0};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.clearValue.color.float32[0] = ((argb >> 16) & 0xFF) / 255.0f;
    ca.clearValue.color.float32[1] = ((argb >> 8) & 0xFF) / 255.0f;
    ca.clearValue.color.float32[2] = (argb & 0xFF) / 255.0f;
    ca.clearValue.color.float32[3] = ((argb >> 24) & 0xFF) / 255.0f;
    VkClearRect cr = { current_scissor(), 0, 1 };
    if (cr.rect.extent.width && cr.rect.extent.height)
        vkCmdClearAttachments(g.fr[g.cur].cmd, 1, &ca, 1, &cr);
}

static void vk_clear_depth(float depth)
{
    if (ensure_pass() != 0) return;
    VkClearAttachment ca = {0};
    ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ca.clearValue.depthStencil.depth = depth;
    VkClearRect cr = { current_scissor(), 0, 1 };
    if (cr.rect.extent.width && cr.rect.extent.height)
        vkCmdClearAttachments(g.fr[g.cur].cmd, 1, &ca, 1, &cr);
}

/* Resolves a texture handle for a sampler: the texture when its kind matches the program's
 * declaration, else the zero (or white) texel of the declared kind. A render target the pass is
 * drawing into reads as white: sampling it would be a feedback loop, and its layout is the
 * attachment's. */
static VkImageView resolve_view(vt3_tex t, int want_cube)
{
    uint32_t idx;
    if (t == 0) idx = want_cube ? TEX_ZERO_CUBE : TEX_WHITE;
    else if (t == VT3_TEX_ZERO) idx = want_cube ? TEX_ZERO_CUBE : TEX_ZERO;
    else if (t <= VT3_MAX_TEX && g.tex[t - 1].used) {
        if (g.tex[t - 1].rt && t == g.tgt) idx = TEX_WHITE;
        else if (!!g.tex[t - 1].cube == !!want_cube) return g.tex[t - 1].view;
        else idx = want_cube ? TEX_ZERO_CUBE : TEX_ZERO;
    } else idx = want_cube ? TEX_ZERO_CUBE : TEX_WHITE;
    return g.tex[idx].view;
}

/* D3DTADDRESS_*: wrap, mirror, clamp, border, mirror-once. */
static VkSamplerAddressMode sampler_address(uint8_t mode)
{
    switch (mode) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; /* mirror-once is optional in Vulkan */
    case 3:
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkSampler sampler_for(uint8_t minf, uint8_t magf, uint8_t u, uint8_t v, uint8_t w)
{
    /* The non-programmable TL path passes NULL state and keeps its historical clamp/linear sampler. */
    if (minf == 0 && magf == 0 && u == 0 && v == 0 && w == 0) return g.samp;
    for (uint32_t i = 0; i < g.nsamplers; i++) {
        vk_sampler_entry *e = &g.samplers[i];
        if (e->minf == minf && e->magf == magf && e->u == u && e->v == v && e->w == w)
            return e->sampler;
    }
    if (g.nsamplers >= SAMPLER_CACHE) return g.samp;

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = magf == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.minFilter = minf == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    /* Trilinear mip selection with an unbounded LOD: without this the sampler defaults to
     * mipmapMode NEAREST and lodMax 0, hard-clamping every sample to mip 0. A fine grid tiled
     * many times (the net) then aliases into a scrambled pattern because the smaller mips are
     * never chosen no matter how far away it is drawn. */
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.minLod = 0.0f;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.addressModeU = sampler_address(u);
    sci.addressModeV = sampler_address(v);
    sci.addressModeW = sampler_address(w);
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (minf == 3 && g.aniso_enabled) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = g.max_aniso;
    }
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(g.dev, &sci, NULL, &sampler) != VK_SUCCESS) {
        VT3_WARN_ONCE("vulkan: custom sampler creation failed; using clamp/linear fallback");
        return g.samp;
    }
    vk_sampler_entry *e = &g.samplers[g.nsamplers++];
    e->minf = minf; e->magf = magf; e->u = u; e->v = v; e->w = w; e->sampler = sampler;
    return sampler;
}

/* Samplers the program does not declare get the white texel: the game leaves stale textures bound on
 * stages a shader never reads, and those should neither cost a layout change nor be read. */
static VkDescriptorSet alloc_set1(vk_frame *f, uint8_t cube_mask, uint16_t declared, const vt3_tex *samplers,
                                  const uint8_t *minf, const uint8_t *magf, const uint8_t *addr_u,
                                  const uint8_t *addr_v, const uint8_t *addr_w)
{
    VkDescriptorSetAllocateInfo sai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    sai.descriptorPool = f->pool;
    sai.descriptorSetCount = 1;
    sai.pSetLayouts = &g.dsl1;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g.dev, &sai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo info[VT3_MAX_SAMPLERS];
    VkWriteDescriptorSet ws[VT3_MAX_SAMPLERS];
    for (int n = 0; n < VT3_MAX_SAMPLERS; n++) {
        int want_cube = (cube_mask >> n) & 1;
        uint8_t mn = minf ? minf[n] : 0u;
        uint8_t mg = magf ? magf[n] : 0u;
        uint8_t au = addr_u ? addr_u[n] : 0u;
        uint8_t av = addr_v ? addr_v[n] : 0u;
        uint8_t aw = addr_w ? addr_w[n] : 0u;
        info[n].sampler = sampler_for(mn, mg, au, av, aw);
        vt3_tex st = samplers && ((declared >> n) & 1) ? samplers[n] : 0;
        /* A declared sampler that reads white is a texture the game expects and we do not have. */
        if ((declared >> n) & 1) {
            int missing = st == 0 || (st <= VT3_MAX_TEX && !g.tex[st - 1].used);
            int self = st != 0 && st <= VT3_MAX_TEX && g.tex[st - 1].rt && st == g.tgt;
            if (missing) VT3_WARN_ONCE("vk: sampler %d has no texture (white), handle %u", n, (unsigned)st);
            if (self) VT3_WARN_ONCE("vk: sampler %d samples the target it draws into (white), handle %u", n, (unsigned)st);
        }
        info[n].imageView = resolve_view(st, want_cube);
        info[n].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&ws[n], 0, sizeof ws[n]);
        ws[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[n].dstSet = set;
        ws[n].dstBinding = (uint32_t)n;
        ws[n].descriptorCount = 1;
        ws[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws[n].pImageInfo = &info[n];
    }
    vkUpdateDescriptorSets(g.dev, VT3_MAX_SAMPLERS, ws, 0, NULL);
    return set;
}

static uint32_t tl_layout_hash(void)
{
    return 0x7A1Eu;   /* the XYZRHW layout is fixed */
}

static VkPipeline get_pipeline(uint32_t prog, VkShaderModule vs, VkShaderModule fs, const vt3_vlayout *lay,
                               int tl, const vt3_rstate *st, int strip, uint32_t rpk)
{
    uint32_t lh = tl ? tl_layout_hash() : layout_hash(lay);
    uint32_t sh = state_hash(st);
    for (int i = 0; i < PIPE_MAX; i++) {
        vk_pipe *p = &g.pipes[i];
        if (p->used && p->key.prog == prog && p->key.layout_hash == lh && p->key.state_hash == sh &&
            p->key.topology == (uint32_t)strip && p->key.rpk == rpk)
            return p->pipe;
    }
    int slot = -1;
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g.pipes[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        VT3_WARN("vulkan: pipeline table full");
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vb = {0};
    VkVertexInputAttributeDescription va[VT3_MAX_VATTR];
    uint32_t nva = 0;
    if (tl) {
        vb.binding = 0;
        vb.stride = TL_VERTEX_BYTES;
        vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        va[0] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
        va[1] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 16 };
        va[2] = (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_R32G32_SFLOAT, 20 };
        nva = 3;
    } else {
        vb.binding = 0;
        vb.stride = (uint32_t)lay->stride;
        vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        for (int i = 0; i < lay->count && nva < VT3_MAX_VATTR; i++) {
            va[nva].location = (uint32_t)lay->attr[i].location;
            va[nva].binding = 0;
            va[nva].format = attr_format(&lay->attr[i]);
            va[nva].offset = (uint32_t)lay->attr[i].offset;
            nva++;
        }
    }
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vis.vertexBindingDescriptionCount = 1;
    vis.pVertexBindingDescriptions = &vb;
    vis.vertexAttributeDescriptionCount = nva;
    vis.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ias.topology = strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* Viewport and scissor are dynamic: they follow the bound target's size (set per draw). */
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    /* D3DCULL_NONE, CW, CCW. The y of the vertex shader is flipped, so windings match D3D's, for the
     * logical buffer and for render targets alike (gl33.c swaps them for its flipped targets). */
    if (st->cull == 1) rs.cullMode = VK_CULL_MODE_NONE;
    else {
        rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = st->cull == 2 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    }

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = st->depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = st->depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = st->depth_test ? map_cmp(st->depth_func) : VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable = st->alpha_blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = map_blend(st->src_blend);
    cba.dstColorBlendFactor = map_blend(st->dst_blend);
    cba.colorBlendOp = map_blendop(st->blend_op);
    cba.srcAlphaBlendFactor = map_blend(st->src_blend);
    cba.dstAlphaBlendFactor = map_blend(st->dst_blend);
    cba.alphaBlendOp = map_blendop(st->blend_op);
    /* D3DRS_COLORWRITEENABLE: bit 0 red, 1 green, 2 blue, 3 alpha. */
    cba.colorWriteMask = (st->color_write & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) |
                         (st->color_write & 2 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                         (st->color_write & 4 ? VK_COLOR_COMPONENT_B_BIT : 0) |
                         (st->color_write & 8 ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vis;
    gci.pInputAssemblyState = &ias;
    gci.pViewportState = &vps;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cbs;
    gci.pDynamicState = &dsi;
    gci.layout = g.pl;
    gci.renderPass = g.rps[rpk];
    gci.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    uint64_t pt0 = stats_enabled() ? now_ns() : 0;
    VkResult pcr = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    if (stats_enabled()) {
        g_stats.pipes++;
        g_stats.pipe_ns += now_ns() - pt0;
    }
    if (pcr != VK_SUCCESS) {
        VT3_ERROR("vulkan: pipeline creation failed (program %u)", (unsigned)prog);
        return VK_NULL_HANDLE;
    }
    g.pipes[slot].used = 1;
    g.pipes[slot].key.prog = prog;
    g.pipes[slot].key.layout_hash = lh;
    g.pipes[slot].key.state_hash = sh;
    g.pipes[slot].key.topology = (uint32_t)strip;
    g.pipes[slot].key.rpk = rpk;
    g.pipes[slot].pipe = pipe;
    return pipe;
}

/* Fixed-function draws (XYZRHW): alpha blend, no depth, no culling, as gl33.c. */
static void vk_draw(vt3_prim prim, const vt3_tlvertex *v, uint32_t count, vt3_tex tex)
{
    if (count == 0 || !g.tl_vs || !g.tl_fs || ensure_frame() != 0) return;
    prepare_sampled(tex);
    if (ensure_pass() != 0) return;
    vk_frame *f = &g.fr[g.cur];
    uint32_t bytes = count * TL_VERTEX_BYTES;
    if (f->vb_used + bytes > VB_RING) {
        static int warned;
        if (!warned) {
            warned = 1;
            VT3_WARN("vulkan: vertex ring full in a frame; the rest of the frame is dropped");
        }
        return;
    }
    uint32_t off = g.fr[g.cur].vb_base + f->vb_used;
    memcpy(g.vb_ptr + off, v, bytes);
    f->vb_used = (f->vb_used + bytes + 15u) & ~15u;

    static const vt3_rstate tl_state = { 1, 5, 6, 0, 0, 8, 1, 1, 0, 0, 0, 0xF };
    vk_tex *tgt = rt_of(g.tgt);
    VkPipeline pipe = get_pipeline(0, g.tl_vs, g.tl_fs, NULL, 1, &tl_state, prim == VT3_PRIM_TRIANGLESTRIP,
                                   tgt ? (uint32_t)tgt->rpk : RPK_RGBA8);
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    vt3_tex samplers[VT3_MAX_SAMPLERS] = {0};
    samplers[0] = tex;
    set1 = alloc_set1(f, 0, 1u, samplers, NULL, NULL, NULL, NULL, NULL);
    if (!pipe || !set1) return;

    /* The push constants carry the bound target's size: pixel coordinates map to its whole area. */
    struct { float size[2]; int32_t use_tex; } pc;
    int tw, th;
    target_size(g.tgt, &tw, &th);
    pc.size[0] = (float)tw;
    pc.size[1] = (float)th;
    pc.use_tex = (tex != 0 && tex <= VT3_MAX_TEX && g.tex[tex - 1].used) ? 1 : 0;
    VkCommandBuffer cb = f->cmd;
    VkRect2D sc = current_scissor();
    VkViewport vp = current_viewport(&tl_state);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 1, 1, &set1, 0, NULL);
    vkCmdPushConstants(cb, g.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkDeviceSize vo = off;
    vkCmdBindVertexBuffers(cb, 0, 1, &g.vb, &vo);
    vkCmdDraw(cb, count, 1, 0, 0);
}

static void vk_draw_program_impl(const vt3_draw *d);

static void vk_draw_program(const vt3_draw *d)
{
    if (!stats_enabled()) {
        vk_draw_program_impl(d);
        return;
    }
    uint64_t t0 = now_ns();
    vk_draw_program_impl(d);
    g_stats.record_ns += now_ns() - t0;
}

static void vk_draw_program_impl(const vt3_draw *d)
{
    if (!d->prog || d->prog > PROG_MAX || !d->layout || !d->vertices || ensure_frame() != 0) return;
    vk_prog *p = &g.progs[d->prog - 1];
    if (!p->used) return;
    for (int n = 0; n < VT3_MAX_SAMPLERS; n++) {
        if (((p->sampler_mask >> n) & 1) && !((p->cube_mask >> n) & 1)) prepare_sampled(d->samplers[n]);
    }
    if (ensure_pass() != 0) return;
    vk_frame *f = &g.fr[g.cur];

    /* Vertices, then indices, in the frame's rings. */
    uint32_t vbytes = (d->vertex_bytes + 15u) & ~15u;
    if (f->vb_used + vbytes > VB_RING) { VT3_WARN_ONCE("vk: vertex ring full, draw dropped"); return; }
    uint32_t voff = f->vb_base + f->vb_used;
    memcpy(g.vb_ptr + voff, d->vertices, d->vertex_bytes);
    f->vb_used += vbytes;
    uint32_t ioff = 0;
    if (d->indices) {
        uint32_t ibytes = (d->count * 2u + 15u) & ~15u;
        if (f->ib_used + ibytes > IB_RING) { VT3_WARN_ONCE("vk: index ring full, draw dropped"); return; }
        ioff = f->ib_base + f->ib_used;
        memcpy(g.ib_ptr + ioff, d->indices, d->count * 2u);
        f->ib_used += ibytes;
    }

    /* The constant blocks, std140 as the translator declares them. Only copy the range each
     * shader reads directly (vt3_sm2 sets const_max at translate time; relative addressing
     * forces it to the full range, so this is always safe). Registers the game never wrote
     * read as zero, and this ring slot is fresh per draw - previously we always memcpy'd the
     * full 4 KB vertex + 4 KB pixel blocks, ~1.8 MB per frame at 210 draws. */
    if (f->ubo_used + 2u * UBO_SLOT > UBO_RING) { VT3_WARN_ONCE("vk: uniform ring full, draw dropped"); return; }
    uint32_t vs_off = f->ubo_base + f->ubo_used;
    uint32_t ps_off = vs_off + UBO_SLOT;
    f->ubo_used += 2u * UBO_SLOT;
    uint8_t *vsb = g.ubo_ptr + vs_off, *psb = g.ubo_ptr + ps_off;
    uint32_t vs_n = d->vs_const_max * 16u;   /* d->vs_const_max ≤ 256 */
    uint32_t ps_n = d->ps_const_max * 16u;   /* d->ps_const_max ≤ 224 */
    if (d->vsc) memcpy(vsb, d->vsc, vs_n);
    else memset(vsb, 0, vs_n);
    memset(vsb + vs_n, 0, VS_BLOCK_BYTES - vs_n);
    if (d->psc) memcpy(psb, d->psc, ps_n);
    else memset(psb, 0, ps_n);
    memset(psb + ps_n, 0, PS_BLOCK_BYTES - ps_n);
    /* vsi[i] holds the bool constants b0..b15 as ints, and vsb4[i >> 2][i & 3] (ivec4 vsb4[4])
     * lives after vsi[16], at offset 4096 + 256; the two runs are the same 16 values. Stores,
     * not the 32 four-byte memcpys this used to be. */
    int32_t *vsi = (int32_t *)(vsb + 4096u), *vsb4 = (int32_t *)(vsb + 4096u + 256u);
    for (int i = 0; i < 16; i++) { int32_t v = d->vsb[i] != 0; vsi[i] = v; vsb4[i] = v; }
    int32_t *psi = (int32_t *)(psb + 4096u), *psb4 = (int32_t *)(psb + 4096u + 256u);
    for (int i = 0; i < 16; i++) { int32_t v = d->psb[i] != 0; psi[i] = v; psb4[i] = v; }
    *(int32_t *)(psb + 4416u) = d->state.alpha_test ? d->state.alpha_func : 0;
    *(float *)(psb + 4420u) = (float)d->state.alpha_ref;

    VkDescriptorSet set1 = alloc_set1(f, p->cube_mask, p->sampler_mask, d->samplers,
                                     d->samp_min, d->samp_mag, d->samp_addr_u, d->samp_addr_v, d->samp_addr_w);
    vk_tex *tgt = rt_of(g.tgt);
    VkPipeline pipe = get_pipeline(d->prog, p->vs, p->ps, d->layout, 0, &d->state,
                                   d->prim == VT3_PRIM_TRIANGLESTRIP, tgt ? (uint32_t)tgt->rpk : RPK_RGBA8);
    if (!set1 || !pipe) { VT3_WARN_ONCE("vk: no descriptor set or pipeline, draw dropped"); return; }

    VkCommandBuffer cb = f->cmd;
    VkRect2D sc = current_scissor();
    VkViewport vp = current_viewport(&d->state);
    if (stats_enabled()) g_stats.draws++;
    uint32_t dyn[2] = { vs_off, ps_off };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &f->set0, 2, dyn);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 1, 1, &set1, 0, NULL);
    VkDeviceSize vo = voff;
    vkCmdBindVertexBuffers(cb, 0, 1, &g.vb, &vo);
    if (d->indices) {
        vkCmdBindIndexBuffer(cb, g.ib, ioff, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, d->count, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cb, d->count, 1, 0, 0);
    }
}

/* ---- present, readback ----------------------------------------------------------- */

/* The game advances once per presented frame and was built for 60 of them a second, as on the desktop
 * where SDL's swap interval paces it. FIFO follows the panel, which may run at 120 Hz, so the present
 * path waits out the rest of each 1/60 s. VT3_FPS_CAP sets the rate; 0 turns the limit off. */
static void frame_limit(void)
{
    static long long period_ns = -1;
    static uint64_t last_ns;
    if (period_ns < 0) {
        const char *cap = getenv("VT3_FPS_CAP");
        /* The default follows the display's rate. The Mac's is 60 Hz, where pacing the present is
         * free and matches what the game was written for. The Retroid's panel runs at 120 Hz, and
         * holding the game to 60 on it would show each frame twice over while the frame times still
         * land unevenly - that is judder, not smoothness. At 120 the budget is 8.3 ms, below what a
         * frame costs today, so nothing sleeps and the game runs flat out; if it ever gets fast
         * enough to reach 120 the same code starts holding it there. VT3_FPS_CAP overrides, and 0
         * turns the limit off.
         *
         * Be careful attributing frame rate to the cap on the Retroid: every run picks a different
         * opponent and venue, so two runs do not measure the same work. A cap whose budget sits
         * near a frame's own cost does sleep, and that idle lets the big cores drop from 2.0 GHz to
         * 0.85 so the next frame costs more than the sleep saved - but isolating that needs two
         * runs of the same match, which the game does not offer. */
        int hz = cap ? atoi(cap)
#ifdef __ANDROID__
                     : 120;
#else
                     : 60;
#endif
        period_ns = hz > 0 ? 1000000000ll / hz : 0;
    }
    if (!period_ns) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (last_ns) {
        uint64_t elapsed = now - last_ns;
        if (elapsed < (uint64_t)period_ns) {
            uint64_t wait = (uint64_t)period_ns - elapsed;
            if (stats_enabled()) g_stats.asked_ns += wait;
            ts.tv_sec = (time_t)(wait / 1000000000ull);
            ts.tv_nsec = (long)(wait % 1000000000ull);
            nanosleep(&ts, NULL);
            now += wait;
        }
    }
    last_ns = now;
}

static int recreate_swapchain(void)
{
    vkDeviceWaitIdle(g.dev);
    return create_swapchain();
}

#ifdef __ANDROID__
/* Replace the surface only after retiring its swapchain. A swapchain must be destroyed
 * before its surface, and oldSwapchain passed to vkCreateSwapchainKHR must belong to
 * the same surface as the new swapchain. */
static int replace_android_surface(ANativeWindow *win)
{
    vkDeviceWaitIdle(g.dev);
    if (g.sc) {
        vkDestroySwapchainKHR(g.dev, g.sc, NULL);
        g.sc = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < 8; i++) {
        if (g.sc_done[i]) vkDestroySemaphore(g.dev, g.sc_done[i], NULL);
        g.sc_done[i] = VK_NULL_HANDLE;
        g.sc_img[i] = VK_NULL_HANDLE;
    }
    g.nsc = 0;
    if (g.surf) {
        vkDestroySurfaceKHR(g.inst, g.surf, NULL);
        g.surf = VK_NULL_HANDLE;
    }

    VkAndroidSurfaceCreateInfoKHR asci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    asci.window = win;
    if (vkCreateAndroidSurfaceKHR(g.inst, &asci, NULL, &g.surf) != VK_SUCCESS) {
        VT3_ERROR("vulkan: could not recreate the Android surface");
        return -1;
    }
    g.window = win;
    if (create_swapchain() != 0) {
        g.window = NULL; /* leave recovery eligible for the next frame */
        return -1;
    }
    return 0;
}
#endif

static void vk_present(const vt3_rect *dst, int nearest, int drawable_w, int drawable_h)
{
    (void)drawable_w;
    (void)drawable_h;
    if (ensure_frame() != 0) return;
#ifdef __ANDROID__
    /* The Android window surface is destroyed when the device sleeps and recreated on wake. If the
     * window pointer changed, rebuild the Vulkan surface and swapchain so we present to the new one;
     * otherwise we keep presenting to the dead surface and the screen stays black while the game
     * (and its audio) carries on. */
    ANativeWindow *win = vt3_platform_native_window();
    if (!win) {
        submit_frame(0, 0);
        return;
    }
    if (!g.sc || win != g.window) {
        VT3_INFO("vulkan: android window changed (%p -> %p), recreating surface and swapchain",
                 (void *)g.window, (void *)win);
        end_pass();
        if (replace_android_surface(win) != 0) {
            /* ensure_frame() reset this slot's fence before surface recovery. Submit its
             * offscreen work so the next frame can wait on a fence that will be signalled. */
            submit_frame(0, 0);
            return;
        }
    }
#else
    if (!g.sc) {
        submit_frame(0, 0);
        return;
    }
#endif
    uint32_t cur = g.cur;
    vk_frame *f = &g.fr[cur];
    uint32_t idx = 0;
    uint64_t at0 = stats_enabled() ? now_ns() : 0;
    VkResult ar = vkAcquireNextImageKHR(g.dev, g.sc, 2000000000ull, f->acquire, VK_NULL_HANDLE, &idx);
    if (stats_enabled()) g_stats.acquire_ns += now_ns() - at0;
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        /* The window changed under us. Drop this frame; the next one uses the new swapchain. */
        submit_frame(0, 0);
        recreate_swapchain();
        return;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        VT3_ERROR("vulkan: acquire failed (%d)", (int)ar);
        submit_frame(0, 0);
        return;
    }
    VkCommandBuffer cb = f->cmd;
    end_pass();
    layout_to(cb, g.col, &g.col_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImage sc = g.sc_img[idx];
    barrier_image(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue black = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    VkImageSubresourceRange whole = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);

    /* The letterbox rectangle, in drawable pixels, top-left origin: the same as D3D's rows. */
    int32_t x0 = dst->x, y0 = dst->y, x1 = dst->x + dst->w, y1 = dst->y + dst->h;
    if (x1 > (int32_t)g.sc_extent.width) x1 = (int32_t)g.sc_extent.width;
    if (y1 > (int32_t)g.sc_extent.height) y1 = (int32_t)g.sc_extent.height;
    if (x1 > x0 && y1 > y0) {
        VkImageBlit blit = {0};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = g.logical_w;
        blit.srcOffsets[1].y = g.logical_h;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0].x = x0;
        blit.dstOffsets[0].y = y0;
        blit.dstOffsets[1].x = x1;
        blit.dstOffsets[1].y = y1;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, g.col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    barrier_image(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    /* The logical buffer stays in the transfer layout: the next pass or blit moves it where it needs. */
    submit_frame(idx, 1);

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.sc_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.sc;
    pi.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(g.queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    else if (pr != VK_SUCCESS) VT3_ERROR("vulkan: present failed (%d)", (int)pr);
    uint64_t lt0 = stats_enabled() ? now_ns() : 0;
    frame_limit();
    if (stats_enabled()) g_stats.limit_ns += now_ns() - lt0;
    stats_log();
}

/* RGBA8 rows, top row first, the logical size. The target already has D3D's row order. */
static void vk_readback(uint8_t *rgba)
{
    if (ensure_frame() != 0) return;
    uint32_t cur = g.cur;
    vk_frame *f = &g.fr[cur];
    VkCommandBuffer cb = f->cmd;
    end_pass();
    size_t bytes = (size_t)g.logical_w * (size_t)g.logical_h * 4;
    VkBuffer sb = VK_NULL_HANDLE;
    void *ptr = NULL;
    if (stage_alloc(f, bytes, &sb, &ptr) != 0) return;
    layout_to(cb, g.col, &g.col_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)g.logical_w;
    c.imageExtent.height = (uint32_t)g.logical_h;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, g.col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &c);
    submit_frame(0, 0);
    vkWaitForFences(g.dev, 1, &f->fence, VK_TRUE, 5000000000ull);
    memcpy(rgba, ptr, bytes);
}

/* ---- setup and teardown -------------------------------------------------------- */

static void one_time_clear(VkCommandBuffer cb, void *arg)
{
    (void)arg;
    barrier_image(cb, g.col, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue black = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    VkImageSubresourceRange whole = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, g.col, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    barrier_image(cb, g.col, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  0, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    barrier_image(cb, g.dep, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
}

static int create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, void **ptr)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKT(vkCreateBuffer(g.dev, &bi, NULL, buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, *buf, &req);
    if (alloc_bind(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mem) != 0) return -1;
    VKT(vkBindBufferMemory(g.dev, *buf, *mem, 0), "vkBindBufferMemory");
    VKT(vkMapMemory(g.dev, *mem, 0, size, 0, ptr), "vkMapMemory");
    return 0;
}

#ifndef __ANDROID__
/* VT3_VK_DEBUG: validation messages (with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation) go to the log. */
static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT *d, void *user)
{
    (void)sev;
    (void)type;
    (void)user;
    VT3_WARN("vk validation: %s", d->pMessage);
    return VK_FALSE;
}
#endif

static int vk_init(int logical_w, int logical_h)
{
    g.logical_w = logical_w;
    g.logical_h = logical_h;
#ifdef __ANDROID__
    g.window = vt3_platform_native_window();
    if (!g.window) {
        VT3_ERROR("vulkan: no window yet");
        return -1;
    }
#endif


    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vt3";
    app.apiVersion = VK_API_VERSION_1_1;
#ifdef __ANDROID__
    const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    uint32_t n_ext = 2;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = n_ext;
#else
    /* SDL names the surface extensions the window needs (VK_KHR_surface + VK_EXT_metal_surface on
     * macOS). MoltenVK's devices are portability devices: without the enumeration opt-in they are
     * hidden and vkEnumeratePhysicalDevices finds nothing. */
    const char *iext[8];
    uint32_t n_ext;
    uint32_t n_sdl = 0;
    const char *const *sdl_ext = vt3_platform_vk_instance_extensions(&n_sdl);
    if (!sdl_ext || n_sdl == 0 || n_sdl > 6) {
        VT3_ERROR("vulkan: no window or no surface extensions (SDL_WINDOW_VULKAN missing?)");
        return -1;
    }
    n_ext = 0;
    for (uint32_t i = 0; i < n_sdl; i++) iext[n_ext++] = sdl_ext[i];
    iext[n_ext++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    if (getenv("VT3_VK_DEBUG")) iext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = n_ext;
    ici.ppEnabledExtensionNames = iext;
    VKT(vkCreateInstance(&ici, NULL, &g.inst), "vkCreateInstance");
#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR asci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    asci.window = g.window;
    VKT(vkCreateAndroidSurfaceKHR(g.inst, &asci, NULL, &g.surf), "vkCreateAndroidSurfaceKHR");
#else
    if (vt3_platform_vk_create_surface(g.inst, &g.surf) != 0) {
        VT3_ERROR("vulkan: SDL_Vulkan_CreateSurface failed (see the platform layer's log)");
        return -1;
    }
    if (getenv("VT3_VK_DEBUG")) {   /* route the validation layer's messages into the log */
        PFN_vkCreateDebugUtilsMessengerEXT mk =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = dbg_cb;
        VkDebugUtilsMessengerEXT m;
        if (mk) mk(g.inst, &mi, NULL, &m);
    }
#endif

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(g.inst, &ndev, NULL);
    if (!ndev) {
        VT3_ERROR("vulkan: no device");
        return -1;
    }
    VkPhysicalDevice devs[4];
    if (ndev > 4) ndev = 4;
    vkEnumeratePhysicalDevices(g.inst, &ndev, devs);
    g.phys = devs[0];
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem_props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nq, qf);
    int found = -1;
    for (uint32_t i = 0; i < nq && found < 0; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, i, g.surf, &present);
        if (present && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) found = (int)i;
    }
    if (found < 0) {
        VT3_ERROR("vulkan: no queue that draws and presents");
        return -1;
    }
    g.qfam = (uint32_t)found;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g.qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *dext[4] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    uint32_t ndext = 1;
#ifndef __ANDROID__
    /* MoltenVK advertises VK_KHR_portability_subset only on devices that need it; enabling it where
     * it does not exist would make vkCreateDevice fail, so it is enabled when the device lists it. */
    char subset_name[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t n_dev_ext = 0;
    vkEnumerateDeviceExtensionProperties(g.phys, NULL, &n_dev_ext, NULL);
    if (n_dev_ext) {
        VkExtensionProperties *ex = malloc(n_dev_ext * sizeof *ex);
        if (ex && vkEnumerateDeviceExtensionProperties(g.phys, NULL, &n_dev_ext, ex) == VK_SUCCESS) {
            for (uint32_t i = 0; i < n_dev_ext; i++) {
                if (strcmp(ex[i].extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) == 0) {
                    snprintf(subset_name, sizeof subset_name, "%s", ex[i].extensionName);
                    dext[ndext++] = subset_name;
                    break;
                }
            }
        }
        free(ex);
    }
#endif
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ndext;
    dci.ppEnabledExtensionNames = dext;
    VkPhysicalDeviceFeatures supported_features, enabled_features;
    memset(&enabled_features, 0, sizeof enabled_features);
    vkGetPhysicalDeviceFeatures(g.phys, &supported_features);
    g.aniso_enabled = supported_features.samplerAnisotropy == VK_TRUE;
    enabled_features.samplerAnisotropy = g.aniso_enabled ? VK_TRUE : VK_FALSE;
    dci.pEnabledFeatures = &enabled_features;
    VKT(vkCreateDevice(g.phys, &dci, NULL, &g.dev), "vkCreateDevice");
    vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.phys, &props);
    VkPhysicalDeviceLimits lim = props.limits;
    g.max_aniso = g.aniso_enabled && lim.maxSamplerAnisotropy > 0.0f ? lim.maxSamplerAnisotropy : 1.0f;
    if (lim.minUniformBufferOffsetAlignment > 256) {
        VT3_ERROR("vulkan: uniform buffer alignment %llu is larger than the 256 bytes assumed",
                  (unsigned long long)lim.minUniformBufferOffsetAlignment);
        return -1;
    }
    VT3_INFO("vulkan: %s, API %u.%u", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
             VK_VERSION_MINOR(props.apiVersion));
    if (stats_enabled() && qf[found].timestampValidBits && props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qci2 = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci2.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci2.queryCount = 2 * NFRAMES;
        if (vkCreateQueryPool(g.dev, &qci2, NULL, &g.qp) == VK_SUCCESS) {
            g.ts_mask = qf[found].timestampValidBits >= 64 ? ~0ull : ((1ull << qf[found].timestampValidBits) - 1);
            g.ts_period_ns = (double)props.limits.timestampPeriod;
        }
    }

    if (create_swapchain() != 0) return -1;
    if (create_offscreen() != 0) return -1;

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g.qfam;
    VKT(vkCreateCommandPool(g.dev, &cpi, NULL, &g.pool), "vkCreateCommandPool");
    VkCommandPoolCreateInfo opi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    opi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    opi.queueFamilyIndex = g.qfam;
    VKT(vkCreateCommandPool(g.dev, &opi, NULL, &g_once_pool), "one-time pool");

    VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VKT(vkCreateSemaphore(g.dev, &sem, NULL, &g.chain), "chain semaphore");
    for (int i = 0; i < NFRAMES; i++) {
        vk_frame *f = &g.fr[i];
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = g.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VKT(vkAllocateCommandBuffers(g.dev, &ai, &f->cmd_up), "frame command buffer");
        VKT(vkAllocateCommandBuffers(g.dev, &ai, &f->cmd), "frame command buffer");
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKT(vkCreateFence(g.dev, &fci, NULL, &f->fence), "fence");
        VKT(vkCreateSemaphore(g.dev, &sem, NULL, &f->acquire), "acquire semaphore");
        VkDescriptorPoolSize sizes[2] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VT3_MAX_SAMPLERS * DESC_SETS_PER_FRAME },
        };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = DESC_SETS_PER_FRAME + 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        VKT(vkCreateDescriptorPool(g.dev, &dpi, NULL, &f->pool), "descriptor pool");
        f->vb_base = (uint32_t)i * VB_RING;
        f->ib_base = (uint32_t)i * IB_RING;
        f->ubo_base = (uint32_t)i * UBO_RING;
    }

    if (create_buffer((VkDeviceSize)NFRAMES * VB_RING, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g.vb, &g.vb_mem,
                      (void **)&g.vb_ptr) != 0) return -1;
    if (create_buffer((VkDeviceSize)NFRAMES * IB_RING, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &g.ib, &g.ib_mem,
                      (void **)&g.ib_ptr) != 0) return -1;
    if (create_buffer((VkDeviceSize)NFRAMES * UBO_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g.ubo, &g.ubo_mem,
                      (void **)&g.ubo_ptr) != 0) return -1;

    VkDescriptorSetLayoutBinding b0[2] = {0};
    b0[0].binding = 0;
    b0[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b0[0].descriptorCount = 1;
    b0[0].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    b0[1].binding = 1;
    b0[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b0[1].descriptorCount = 1;
    b0[1].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    VkDescriptorSetLayoutCreateInfo dsli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dsli.bindingCount = 2;
    dsli.pBindings = b0;
    VKT(vkCreateDescriptorSetLayout(g.dev, &dsli, NULL, &g.dsl0), "set 0 layout");

    VkDescriptorSetLayoutBinding b1[VT3_MAX_SAMPLERS];
    for (int n = 0; n < VT3_MAX_SAMPLERS; n++) {
        b1[n].binding = (uint32_t)n;
        b1[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b1[n].descriptorCount = 1;
        b1[n].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        b1[n].pImmutableSamplers = NULL;
    }
    dsli.bindingCount = VT3_MAX_SAMPLERS;
    dsli.pBindings = b1;
    VKT(vkCreateDescriptorSetLayout(g.dev, &dsli, NULL, &g.dsl1), "set 1 layout");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 12 };
    VkDescriptorSetLayout sets[2] = { g.dsl0, g.dsl1 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VKT(vkCreatePipelineLayout(g.dev, &pli, NULL, &g.pl), "pipeline layout");

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    /* Same unbounded-LOD mip selection as sampler_for: the fallback sampler is used by every draw
     * that passes no per-draw sampler state, so it must be able to reach the smaller mip levels. */
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.minLod = 0.0f;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKT(vkCreateSampler(g.dev, &sci, NULL, &g.samp), "sampler");

    /* Initial layouts for the offscreen target, then the dummy textures every sampler can fall back to. */
    if (once_submit(one_time_clear, NULL) != 0) return -1;
    g.col_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    if (tex_build(&g.tex[TEX_WHITE], 1, 1, 0, VT3_TEXFMT_A8R8G8B8) != 0 ||
        tex_build(&g.tex[TEX_ZERO], 1, 1, 0, VT3_TEXFMT_A8R8G8B8) != 0 ||
        tex_build(&g.tex[TEX_ZERO_CUBE], 1, 1, 1, VT3_TEXFMT_A8R8G8B8) != 0) {
        VT3_ERROR("vulkan: dummy textures failed");
        return -1;
    }
    if (upload_dummy(&g.tex[TEX_WHITE], 0xFFFFFFFFu, 1) != 0 || upload_dummy(&g.tex[TEX_ZERO], 0u, 1) != 0 ||
        upload_dummy(&g.tex[TEX_ZERO_CUBE], 0u, 6) != 0) {
        VT3_ERROR("vulkan: dummy texture upload failed");
        return -1;
    }

    const char *pack = getenv("VT3_SHADER_PACK");
    if (pack_load(pack ? pack : "/sdcard/vt3/shaders.pack") != 0) return -1;
    char err[256] = "";
    g.tl_vs = module_for_text(vt3_builtin_tl_vs_vk, 0, err, sizeof err);
    if (g.tl_vs == VK_NULL_HANDLE) {
        VT3_ERROR("vulkan: %s", err);
        return -1;
    }
    g.tl_fs = module_for_text(vt3_builtin_tl_fs_vk, 1, err, sizeof err);
    if (g.tl_fs == VK_NULL_HANDLE) {
        VT3_ERROR("vulkan: %s", err);
        return -1;
    }
    VT3_INFO("vulkan: renderer ready, logical %dx%d", logical_w, logical_h);
    return 0;
}

static void vk_shutdown(void)
{
    if (!g.dev) return;
    vkDeviceWaitIdle(g.dev);
    for (uint32_t i = 0; i < NFRAMES; i++) {
        vk_frame *f = &g.fr[i];
        free_dead(f);
        free(f->dead);
        f->dead = NULL;
        if (f->fence) vkDestroyFence(g.dev, f->fence, NULL);
        if (f->acquire) vkDestroySemaphore(g.dev, f->acquire, NULL);
        if (f->pool) vkDestroyDescriptorPool(g.dev, f->pool, NULL);
    }
    for (int i = 0; i < TEX_SLOTS; i++) {
        vk_tex *t = &g.tex[i];
        vk_dead d = { 1, VK_NULL_HANDLE, t->image, t->view, t->mem, t->dep, t->dep_view, t->dep_mem, t->fb };
        destroy_dead(&d);
        memset(t, 0, sizeof *t);
    }
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g.modules[i].used) vkDestroyShaderModule(g.dev, g.modules[i].mod, NULL);
    }
    for (int i = 0; i < PIPE_MAX; i++) {
        if (g.pipes[i].used) vkDestroyPipeline(g.dev, g.pipes[i].pipe, NULL);
    }
    if (g.fb) vkDestroyFramebuffer(g.dev, g.fb, NULL);
    for (int i = 0; i < RPK_COUNT; i++) {
        if (g.rps[i]) vkDestroyRenderPass(g.dev, g.rps[i], NULL);
    }
    if (g.col_view) vkDestroyImageView(g.dev, g.col_view, NULL);
    if (g.dep_view) vkDestroyImageView(g.dev, g.dep_view, NULL);
    if (g.col) vkDestroyImage(g.dev, g.col, NULL);
    if (g.dep) vkDestroyImage(g.dev, g.dep, NULL);
    if (g.col_mem) vkFreeMemory(g.dev, g.col_mem, NULL);
    if (g.dep_mem) vkFreeMemory(g.dev, g.dep_mem, NULL);
    if (g.qp) vkDestroyQueryPool(g.dev, g.qp, NULL);
    if (g.pl) vkDestroyPipelineLayout(g.dev, g.pl, NULL);
    if (g.dsl0) vkDestroyDescriptorSetLayout(g.dev, g.dsl0, NULL);
    if (g.dsl1) vkDestroyDescriptorSetLayout(g.dev, g.dsl1, NULL);
    if (g.samp) vkDestroySampler(g.dev, g.samp, NULL);
    for (uint32_t i = 0; i < g.nsamplers; i++)
        if (g.samplers[i].sampler) vkDestroySampler(g.dev, g.samplers[i].sampler, NULL);
    if (g.ubo) vkDestroyBuffer(g.dev, g.ubo, NULL);
    if (g.vb) vkDestroyBuffer(g.dev, g.vb, NULL);
    if (g.ib) vkDestroyBuffer(g.dev, g.ib, NULL);
    if (g.ubo_mem) vkFreeMemory(g.dev, g.ubo_mem, NULL);
    if (g.vb_mem) vkFreeMemory(g.dev, g.vb_mem, NULL);
    if (g.ib_mem) vkFreeMemory(g.dev, g.ib_mem, NULL);
    if (g.chain) vkDestroySemaphore(g.dev, g.chain, NULL);
    for (uint32_t i = 0; i < g.nsc; i++) {
        if (g.sc_done[i]) vkDestroySemaphore(g.dev, g.sc_done[i], NULL);
    }
    if (g.sc) vkDestroySwapchainKHR(g.dev, g.sc, NULL);
    if (g_once_pool) vkDestroyCommandPool(g.dev, g_once_pool, NULL);
    if (g.pool) vkDestroyCommandPool(g.dev, g.pool, NULL);
    vkDestroyDevice(g.dev, NULL);
    if (g.surf) vkDestroySurfaceKHR(g.inst, g.surf, NULL);
    if (g.inst) vkDestroyInstance(g.inst, NULL);
    free(g.pack);
    memset(&g, 0, sizeof g);
    VT3_INFO("vulkan: shut down");
}

/* ---- programs ------------------------------------------------------------------ */

static int vk_prog_create(vt3_prog *out, const char *vs_src, const char *ps_src, char *err, size_t errcap)
{
    if (!vs_src || !ps_src) {
        snprintf(err, errcap, "no shader text");
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < PROG_MAX; i++) {
        if (!g.progs[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        snprintf(err, errcap, "program table full");
        return -1;
    }
    VkShaderModule vs = module_for_text(vs_src, 0, err, errcap);
    if (vs == VK_NULL_HANDLE) return -1;
    VkShaderModule ps = module_for_text(ps_src, 1, err, errcap);
    if (ps == VK_NULL_HANDLE) return -1;
    g.progs[slot].vs = vs;
    g.progs[slot].ps = ps;
    g.progs[slot].cube_mask = cube_samplers(ps_src);
    g.progs[slot].sampler_mask = (uint16_t)(declared_samplers(ps_src) | declared_samplers(vs_src));
    g.progs[slot].used = 1;
    *out = (vt3_prog)(slot + 1);
    return 0;
}

static void vk_prog_destroy(vt3_prog h)
{
    if (h == 0 || h > PROG_MAX) return;
    g.progs[h - 1].used = 0;   /* the modules stay cached; pipelines keyed by the handle are dropped on reuse */
    for (int i = 0; i < PIPE_MAX; i++) {
        if (g.pipes[i].used && g.pipes[i].key.prog == h) {
            vkDeviceWaitIdle(g.dev);
            vkDestroyPipeline(g.dev, g.pipes[i].pipe, NULL);
            g.pipes[i].used = 0;
        }
    }
}

/* ---- render targets -------------------------------------------------------------- */

typedef struct rt_init_arg {
    vk_tex *t;
} rt_init_arg;

/* A new target starts cleared (black, depth 1) and ready to be sampled. The clear runs inside a pass
 * of the target's own kind, so no transfer usage is needed on the depth image. */
static void record_rt_init(VkCommandBuffer cb, void *p)
{
    vk_tex *t = ((rt_init_arg *)p)->t;
    VkImageLayout cl = VK_IMAGE_LAYOUT_UNDEFINED;
    layout_to(cb, t->image, &cl, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier_image(cb, t->dep, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, 1,
                  VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = g.rps[t->rpk];
    rbi.framebuffer = t->fb;
    rbi.renderArea.extent.width = (uint32_t)t->w;
    rbi.renderArea.extent.height = (uint32_t)t->h;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca[2] = {0};
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ca[1].clearValue.depthStencil.depth = 1.0f;
    VkClearRect cr = { { { 0, 0 }, { (uint32_t)t->w, (uint32_t)t->h } }, 0, 1 };
    vkCmdClearAttachments(cb, 2, ca, 1, &cr);
    vkCmdEndRenderPass(cb);
    layout_to(cb, t->image, &cl, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->layout = cl;
}

/* A render target: a colour texture the game can sample later, with a depth image of its own and a
 * framebuffer. A8R8G8B8 and X8R8G8B8 targets are RGBA8; G16R16F (and A16B16G16R16F) is RGBA16F. */
static int vk_rt_create(vt3_tex *out, int w, int h, vt3_texfmt fmt)
{
    if (!out || w <= 0 || h <= 0) return -1;
    uint32_t slot = 0;
    while (slot < VT3_MAX_TEX && g.tex[slot].used) slot++;
    if (slot == VT3_MAX_TEX) {
        VT3_WARN("vulkan: texture table full (%d): no render target handle", VT3_MAX_TEX);
        return -1;
    }
    vk_tex *t = &g.tex[slot];
    memset(t, 0, sizeof *t);
    int f16 = fmt == VT3_TEXFMT_RGBA16F;
    VkFormat vf = f16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    VkComponentMapping id = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    if (create_image((uint32_t)w, (uint32_t)h, vf,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     0, 1, 1, &t->image, &t->mem) != 0 ||
        create_image((uint32_t)w, (uint32_t)h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, 1,
                     1, &t->dep, &t->dep_mem) != 0)
        goto fail;
    t->view = create_view(t->image, vf, VK_IMAGE_VIEW_TYPE_2D, 1, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    t->dep_view = create_view(t->dep, VK_FORMAT_D32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, 1, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!t->view || !t->dep_view) goto fail;
    t->rpk = f16 ? RPK_RGBA16F : RPK_RGBA8;
    VkImageView views[2] = { t->view, t->dep_view };
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = g.rps[t->rpk];
    fci.attachmentCount = 2;
    fci.pAttachments = views;
    fci.width = (uint32_t)w;
    fci.height = (uint32_t)h;
    fci.layers = 1;
    if (vkCreateFramebuffer(g.dev, &fci, NULL, &t->fb) != VK_SUCCESS) goto fail;
    t->w = w;
    t->h = h;
    t->format = vf;
    t->view_type = VK_IMAGE_VIEW_TYPE_2D;
    t->bpp = f16 ? 8 : 4;
    t->rt = 1;
    t->used = 1;
    rt_init_arg a = { t };
    if (once_submit(record_rt_init, &a) != 0) {
        VT3_WARN("vulkan: could not initialise a %dx%d render target", w, h);
        t->rt = 0;
        goto fail;
    }
    *out = (vt3_tex)(slot + 1);
    VT3_INFO("vulkan: render target %u: %dx%d %s", (unsigned)(slot + 1), w, h, f16 ? "RGBA16F" : "RGBA8");
    return 0;
fail:
    t->used = 0;
    vk_dead d = { 1, VK_NULL_HANDLE, t->image, t->view, t->mem, t->dep, t->dep_view, t->dep_mem, t->fb };
    destroy_dead(&d);
    memset(t, 0, sizeof *t);
    VT3_WARN("vulkan: could not create a %dx%d render target (format %d)", w, h, (int)fmt);
    return -1;
}

/* Draws and clears go to t from now on. 0, or a handle that is not a render target: the logical
 * buffer. The render pass changes lazily, at the next draw or clear. */
static void vk_rt_bind(vt3_tex t)
{
    g.tgt = rt_of(t) ? t : 0;
}

/* Clips a source and a destination rectangle to their images, keeping the scale between them. */
static int clip_blit(vt3_rect *sr, int sw, int sh, vt3_rect *dr, int dw, int dh)
{
    if (sr->w <= 0 || sr->h <= 0 || dr->w <= 0 || dr->h <= 0) return 0;
    double sx0 = sr->x, sy0 = sr->y, sx1 = sr->x + sr->w, sy1 = sr->y + sr->h;
    double dx0 = dr->x, dy0 = dr->y, dx1 = dr->x + dr->w, dy1 = dr->y + dr->h;
    double kx = (sx1 - sx0) / (dx1 - dx0), ky = (sy1 - sy0) / (dy1 - dy0);
    if (dx0 < 0) { sx0 += -dx0 * kx; dx0 = 0; }
    if (dy0 < 0) { sy0 += -dy0 * ky; dy0 = 0; }
    if (dx1 > dw) { sx1 -= (dx1 - dw) * kx; dx1 = dw; }
    if (dy1 > dh) { sy1 -= (dy1 - dh) * ky; dy1 = dh; }
    if (sx0 < 0) { dx0 += -sx0 / kx; sx0 = 0; }
    if (sy0 < 0) { dy0 += -sy0 / ky; sy0 = 0; }
    if (sx1 > sw) { dx1 -= (sx1 - sw) / kx; sx1 = sw; }
    if (sy1 > sh) { dy1 -= (sy1 - sh) / ky; sy1 = sh; }
    sr->x = (int)(sx0 + 0.5); sr->y = (int)(sy0 + 0.5);
    sr->w = (int)(sx1 + 0.5) - sr->x; sr->h = (int)(sy1 + 0.5) - sr->y;
    dr->x = (int)(dx0 + 0.5); dr->y = (int)(dy0 + 0.5);
    dr->w = (int)(dx1 + 0.5) - dr->x; dr->h = (int)(dy1 + 0.5) - dr->y;
    return sr->w > 0 && sr->h > 0 && dr->w > 0 && dr->h > 0;
}

/* Copies sr of src onto dr of dst, scaling as needed. 0 is the logical buffer. Every image here has
 * D3D's row order, so the rows need no conversion (gl33.c converts because its back buffer is
 * bottom-up). The scissor applies to draws, not to copies. */
static void vk_blit(vt3_tex dst, vt3_rect dr, vt3_tex src, vt3_rect sr, int nearest)
{
    vk_tex *rs = rt_of(src), *rd = rt_of(dst);
    if (rs == rd) return;   /* a surface onto itself, or the logical buffer onto itself */
    int sw, sh, dw, dh;
    target_size(rs ? src : 0, &sw, &sh);
    target_size(rd ? dst : 0, &dw, &dh);
    if (!clip_blit(&sr, sw, sh, &dr, dw, dh)) return;
    if (ensure_frame() != 0) return;
    end_pass();
    VkCommandBuffer cb = g.fr[g.cur].cmd;
    VkImage simg = rs ? rs->image : g.col, dimg = rd ? rd->image : g.col;
    VkImageLayout *sl = rs ? &rs->layout : &g.col_layout, *dl = rd ? &rd->layout : &g.col_layout;
    layout_to(cb, simg, sl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    layout_to(cb, dimg, dl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b = {0};
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = 1;
    b.srcOffsets[0].x = sr.x;
    b.srcOffsets[0].y = sr.y;
    b.srcOffsets[1].x = sr.x + sr.w;
    b.srcOffsets[1].y = sr.y + sr.h;
    b.srcOffsets[1].z = 1;
    b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.dstSubresource.layerCount = 1;
    b.dstOffsets[0].x = dr.x;
    b.dstOffsets[0].y = dr.y;
    b.dstOffsets[1].x = dr.x + dr.w;
    b.dstOffsets[1].y = dr.y + dr.h;
    b.dstOffsets[1].z = 1;
    int linear = !nearest && (!rs || rs->rpk != RPK_RGBA16F || g.linear_f16);
    if (stats_enabled()) g_stats.blits++;
    vkCmdBlitImage(cb, simg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

/* ---- the ops table ------------------------------------------------------------------ */

#ifndef __ANDROID__
/* Can a Vulkan instance be made here? The window is created before the renderer, so the platform
 * asks this first: MoltenVK answers on macOS, and a machine with no ICD falls back to OpenGL. */
int vt3_vk_probe(void)
{
    const char *ext[] = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = ext;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, NULL);
    vkDestroyInstance(inst, NULL);
    return r == VK_SUCCESS && n > 0;
}
#endif

const vt3_gfx_ops vt3_gfx_vk = {
    .name = "Vulkan 1.1",
    .init = vk_init,
    .shutdown = vk_shutdown,
    .tex_create = vk_tex_create,
    .tex_upload = vk_tex_upload,
    .tex_destroy = vk_tex_destroy,
    .cube_create = vk_cube_create,
    .cube_upload = vk_cube_upload,
    .begin_frame = vk_begin_frame,
    .clear = vk_clear,
    .draw = vk_draw,
    .present = vk_present,
    .prog_create = vk_prog_create,
    .prog_destroy = vk_prog_destroy,
    .draw_program = vk_draw_program,
    .readback = vk_readback,
    .clear_depth = vk_clear_depth,
    .set_scissor = vk_set_scissor,
    .glsl_vulkan = 1,
    .rt_create = vk_rt_create,
    .rt_bind = vk_rt_bind,
    .blit = vk_blit,
};

#ifdef __ANDROID__
/* On Android there is no GL backend and no environment variable: Vulkan is the only renderer. */
const vt3_gfx_ops *vt3_gfx_default_ops(void)
{
    return &vt3_gfx_vk;
}
#endif
