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

void vt3_config_defaults(vt3_config *c);

int  vt3_platform_init(const vt3_config *c);   /* window, GL 3.3 core context */
void vt3_platform_shutdown(void);
int  vt3_platform_pump_events(void);           /* returns 0 once quit is requested */
void vt3_platform_drawable_size(int *w, int *h);
void vt3_platform_swap(void);
void *vt3_platform_gl_proc(const char *name);

#endif /* VT3_PLATFORM_H */
