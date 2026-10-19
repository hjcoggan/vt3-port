/*
 * Platform layer on Android: the NativeActivity window, its lifecycle events and its input queue.
 * android_main (src/platform/android_entry.c) owns the android_app and calls the handlers here
 * from the glue's looper, so everything runs on the game thread. The renderer presents to the
 * window through vt3_platform_native_window; there is no GL context.
 */
#include "vt3/vt3_log.h"
#include "vt3/vt3_platform.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <stdatomic.h>
#include <string.h>

/* Defined in src/input/input_android.c */
int vt3_input_android_event(const AInputEvent *e);
/* Defined in src/ui/touch_controls.c */
void vt3_touch_release_all(void);

#define EVENT_QUEUE 32

static struct android_app *g_app;
static ANativeWindow *g_window;
static int g_quit;
static vt3_event g_queue[EVENT_QUEUE];
static int g_qhead, g_qcount;

void vt3_config_defaults(vt3_config *c)
{
    c->title = "VT3";
    c->window_w = 1280;
    c->window_h = 960;
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

static void push_event(vt3_event_kind kind, int w, int h)
{
    if (g_qcount == EVENT_QUEUE) return;     /* a full queue drops the newest event */
    vt3_event *e = &g_queue[(g_qhead + g_qcount) % EVENT_QUEUE];
    e->kind = kind;
    e->w = w;
    e->h = h;
    g_qcount++;
}

/* The glue's command callback. The game does not start until the window exists. */
void vt3_platform_android_cmd(struct android_app *app, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        g_window = app->window;
        VT3_INFO("android: window %dx%d", ANativeWindow_getWidth(g_window), ANativeWindow_getHeight(g_window));
        break;
    case APP_CMD_TERM_WINDOW:
        g_window = NULL;
        vt3_touch_release_all();          /* no surface: drop anything the controls held */
        push_event(VT3_EV_BLUR, 0, 0);
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (g_window) {
            push_event(VT3_EV_RESIZE, ANativeWindow_getWidth(g_window), ANativeWindow_getHeight(g_window));
        }
        break;
    case APP_CMD_GAINED_FOCUS:
        push_event(VT3_EV_FOCUS, 0, 0);
        break;
    case APP_CMD_LOST_FOCUS:
        vt3_touch_release_all();
        push_event(VT3_EV_BLUR, 0, 0);
        break;
    case APP_CMD_DESTROY:
        g_quit = 1;
        push_event(VT3_EV_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

/* The glue's input callback: keys and pads go to the input layer. Returns 1 when handled. */
int vt3_platform_android_input(struct android_app *app, AInputEvent *e)
{
    (void)app;
    return vt3_input_android_event(e);
}

void vt3_platform_android_attach(struct android_app *app)
{
    g_app = app;
    app->onAppCmd = vt3_platform_android_cmd;
    app->onInputEvent = vt3_platform_android_input;
}

int vt3_platform_init(const vt3_config *c)
{
    (void)c;
    if (!g_app) {
        VT3_ERROR("vt3_platform_init: no android_app (the entry point did not attach)");
        return -1;
    }
    /* Pump until the window exists: the game needs a surface before it draws. */
    while (!g_window && !g_quit) {
        int events = 0;
        struct android_poll_source *source = NULL;
        if (ALooper_pollOnce(-1, NULL, &events, (void **)&source) >= 0 && source) source->process(g_app, source);
    }
    return g_quit ? -1 : 0;
}

void vt3_platform_shutdown(void)
{
    g_window = NULL;
}

int vt3_platform_pump_events(void)
{
    if (!g_app) return 0;
    for (;;) {
        int events = 0;
        struct android_poll_source *source = NULL;
        int rc = ALooper_pollOnce(0, NULL, &events, (void **)&source);
        if (rc < 0) break;                     /* ALOOPER_POLL_TIMEOUT: nothing pending */
        if (source) source->process(g_app, source);
        if (g_app->destroyRequested) g_quit = 1;
    }
    return !g_quit;
}

int vt3_platform_next_event(vt3_event *ev)
{
    if (g_qcount == 0) return 0;
    *ev = g_queue[g_qhead];
    g_qhead = (g_qhead + 1) % EVENT_QUEUE;
    g_qcount--;
    return 1;
}

void vt3_platform_drawable_size(int *w, int *h)
{
    if (g_window) {
        *w = ANativeWindow_getWidth(g_window);
        *h = ANativeWindow_getHeight(g_window);
    } else {
        *w = 0;
        *h = 0;
    }
}

void vt3_platform_swap(void)
{
    /* The renderer presents its own swapchain. */
}

void *vt3_platform_gl_proc(const char *name)
{
    (void)name;
    return NULL;
}

void *vt3_platform_native_window(void)
{
    return g_window;
}
