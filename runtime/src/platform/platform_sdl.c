/* Platform layer on SDL3: window, GL 3.3 core context, events, input. */
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"

#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdlib.h>
#include <string.h>

/* Defined in src/input/input_sdl.c */
void vt3_input_sdl_event(const SDL_Event *e);
void vt3_input_sdl_poll(void);
void vt3_input_sdl_shutdown(void);
void vt3_input_sdl_script_start(void);      /* VT3_INPUT_SCRIPT: scripted key presses, off by default */
void vt3_input_sdl_open_attached(void);     /* gamepads attached before the pump runs */

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static int g_vk;              /* Vulkan: a surface window, no GL context. The renderer reads
                               * vt3_platform_is_vulkan() to pick its backend. */

void vt3_config_defaults(vt3_config *c)
{
    c->title = "VT3 runtime";
    c->window_w = 640;
    c->window_h = 480;
    c->logical_w = 640;
    c->logical_h = 480;
    c->aspect_w = 4;
    c->aspect_h = 3;
    c->integer_scale = 0;
    c->vsync = 1;
    c->audio = 1;
    c->frames = 0;
    c->game_root = NULL;
}

/* Vulkan is the desktop default when an instance can be made here (MoltenVK on macOS), and OpenGL
 * is the fallback. VT3_RENDERER=gl forces OpenGL and VT3_RENDERER=vulkan forces Vulkan. */
static int wants_vulkan(void)
{
    const char *r = getenv("VT3_RENDERER");
    if (r && strcmp(r, "gl") == 0) return 0;
    if (r && strcmp(r, "vulkan") == 0) return 1;
    return vt3_vk_probe();
}

int vt3_platform_is_vulkan(void)
{
    return g_vk;
}

int vt3_platform_init(const vt3_config *c)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        VT3_ERROR("SDL_Init failed: %s", SDL_GetError());
        return -1;
    }

    g_vk = wants_vulkan();
    if (!g_vk) {                 /* the GL path must see exactly the context it used to */
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        /* macOS only gives a 3.2+ core context when forward-compatible is set. */
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    }

    /* Start at the primary display's usable size so the game scales to the screen instead of a fixed
     * 4:3 window. The frame the game renders follows the logical size the shim hands it (see
     * docs/WIDESCREEN.md), so the image fills this window at its own aspect. */
    int win_w = c->window_w, win_h = c->window_h;
    SDL_Rect disp;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &disp) && disp.w > 0 && disp.h > 0) {
        win_w = disp.w;
        win_h = disp.h;
    }
    g_win = SDL_CreateWindow(c->title, win_w, win_h,
                             (g_vk ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) | SDL_WINDOW_RESIZABLE |
                                 SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_win) {
        VT3_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return -1;
    }

    if (!g_vk) {
        g_ctx = SDL_GL_CreateContext(g_win);
        if (!g_ctx) {
            VT3_ERROR("SDL_GL_CreateContext (3.3 core) failed: %s", SDL_GetError());
            return -1;
        }
        SDL_GL_MakeCurrent(g_win, g_ctx);
        SDL_GL_SetSwapInterval(c->vsync ? 1 : 0);
    }

    /* Keep the window user-resizable on every platform so it can be scaled to any screen. */
    SDL_SetWindowResizable(g_win, true);
    int pw, ph, ww, wh;
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    SDL_GetWindowSize(g_win, &ww, &wh);
    VT3_INFO("window: %dx%d points, %dx%d pixels", ww, wh, pw, ph);
    vt3_input_sdl_open_attached();
    vt3_input_sdl_script_start();
    return 0;
}

void vt3_platform_shutdown(void)
{
    vt3_input_sdl_shutdown();
    if (g_ctx) SDL_GL_DestroyContext(g_ctx);
    if (g_win) SDL_DestroyWindow(g_win);
    g_ctx = NULL;
    g_win = NULL;
    SDL_Quit();
}
/* Window events waiting for the shim. The pump fills it; vt3_platform_next_event drains it. */
#define VT3_EVQ_CAP 64
static vt3_event g_evq[VT3_EVQ_CAP];
static int g_evq_n;

static void evq_push(vt3_event_kind kind, int w, int h)
{
    if (g_evq_n == VT3_EVQ_CAP) return;   /* a full queue drops the newest event */
    g_evq[g_evq_n].kind = kind;
    g_evq[g_evq_n].w = w;
    g_evq[g_evq_n].h = h;
    g_evq_n++;
}

int vt3_platform_next_event(vt3_event *ev)
{
    if (g_evq_n == 0) return 0;
    *ev = g_evq[0];
    memmove(g_evq, g_evq + 1, (size_t)(g_evq_n - 1) * sizeof *g_evq);
    g_evq_n--;
    return 1;
}

int vt3_platform_pump_events(void)
{
    SDL_Event e;
    int running = 1;
    if (!g_win) return 1;   /* before init there is no window and no events */
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            evq_push(VT3_EV_CLOSE, 0, 0);
            running = 0;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            evq_push(VT3_EV_RESIZE, e.window.data1, e.window.data2);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            evq_push(VT3_EV_FOCUS, 0, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            evq_push(VT3_EV_BLUR, 0, 0);
            break;
        case SDL_EVENT_KEY_DOWN:
            /* Escape is not quit here. Steam Input sends it for the B button, so quitting on it
             * closed the game. Quit is the window close button or Cmd+Q (SDL_EVENT_QUIT). */
            vt3_input_sdl_event(&e);
            break;
        default:
            vt3_input_sdl_event(&e);
            break;
        }
    }
    vt3_input_sdl_poll();
    return running;
}

void vt3_platform_drawable_size(int *w, int *h)
{
    SDL_GetWindowSizeInPixels(g_win, w, h);
}

void vt3_platform_swap(void)
{
    if (g_vk) return;   /* the Vulkan renderer presents through its own queue; there is no GL context */
    SDL_GL_SwapWindow(g_win);
}

void *vt3_platform_gl_proc(const char *name)
{
    return (void *)SDL_GL_GetProcAddress(name);
}

const char *const *vt3_platform_vk_instance_extensions(uint32_t *count)
{
    return g_vk && g_win ? SDL_Vulkan_GetInstanceExtensions(count) : NULL;
}

int vt3_platform_vk_create_surface(void *instance, void *surface)
{
    if (!g_vk || !g_win) return -1;
    /* SDL owns the CAMetalLayer the surface lives on, so it creates it too. */
    if (!SDL_Vulkan_CreateSurface(g_win, (VkInstance)instance, NULL, (VkSurfaceKHR *)surface)) {
        VT3_ERROR("vk surface: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return -1;
    }
    return 0;
}
