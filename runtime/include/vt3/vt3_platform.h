/*
 * Platform layer: window, GL context, events, and gamepad/keyboard input.
 * Implemented on SDL3 (src/platform/platform_sdl.c). An Android version would
 * sit behind the same functions, using SDL's EGL path.
 */
#ifndef VT3_PLATFORM_H
#define VT3_PLATFORM_H

typedef struct vt3_config {
    const char *title;
    int window_w, window_h;      /* default 640x480 */
    int logical_w, logical_h;    /* default 640x480 */
    int aspect_w, aspect_h;      /* default 4:3 */
    int integer_scale;           /* default 0 */
    int vsync;                   /* default 1 */
    int audio;                   /* default 1 (failure to open audio is not fatal) */
    int frames;                  /* 0 = run until quit; N = stop after N frames */
    const char *game_root;       /* NULL = no game data (no game-root mount) */
} vt3_config;

#include <stdint.h>

void vt3_config_defaults(vt3_config *c);

int  vt3_platform_init(const vt3_config *c);   /* window, GL 3.3 core context */
void vt3_platform_shutdown(void);
int  vt3_platform_pump_events(void);           /* returns 0 once quit is requested */

/* Window events, in platform-neutral form, queued by the pump. The shim turns them into
 * WM_ messages. RESIZE carries the new client size in points. */
typedef enum vt3_event_kind {
    VT3_EV_NONE = 0, VT3_EV_CLOSE, VT3_EV_RESIZE, VT3_EV_FOCUS, VT3_EV_BLUR
} vt3_event_kind;
typedef struct vt3_event {
    vt3_event_kind kind;
    int w, h;
} vt3_event;
int  vt3_platform_next_event(vt3_event *ev);   /* 1 with *ev filled when one is queued, else 0 */
void vt3_platform_drawable_size(int *w, int *h);
void vt3_platform_swap(void);
void *vt3_platform_gl_proc(const char *name);
/* Android: the ANativeWindow the Vulkan renderer presents to, or NULL before the window exists. */
void *vt3_platform_native_window(void);
/* Vulkan window support on the SDL platform (VT3_RENDERER=vulkan): the instance extensions the
 * window needs (SDL_Vulkan_GetInstanceExtensions), and a VkSurfaceKHR for the window. NULL/-1
 * before the window exists or when the platform is not Vulkan-ready. Android uses
 * vt3_platform_native_window instead. Passing handles as void* keeps vulkan.h out of this header. */
const char *const *vt3_platform_vk_instance_extensions(uint32_t *count);
int vt3_platform_vk_create_surface(void *instance, void *surface);
/* 1 when the window is a Vulkan surface window. Set by vt3_platform_init. */
int vt3_platform_is_vulkan(void);
/* Desktop: 1 when a Vulkan instance can be created here (the loader and an ICD are present). */
int vt3_vk_probe(void);

#endif /* VT3_PLATFORM_H */
