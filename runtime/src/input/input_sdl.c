/*
 * SDL3 events -> game input.
 *
 * The button tables below are a STUB. The default layout is the usual
 * Xbox-style one, chosen only so the runtime is usable for testing. Before
 * the game code is recovered, the real mapping should come from what
 * VT3.exe asks DirectInput8 for: the button offsets it reads and its own
 * defaults in the options screen.
 */
#include "vt3/vt3_input.h"
#include "vt3/vt3_log.h"

#include <SDL3/SDL.h>

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define VT3_MAX_PADS 4

typedef struct pad_button_map {
    SDL_GamepadButton sdl;
    uint32_t vt3;
} pad_button_map;

static const pad_button_map k_pad_map[] = {
    /* TODO(verify): confirm against VT3.exe's DirectInput usage. */
    { SDL_GAMEPAD_BUTTON_DPAD_UP,       VT3_BTN_UP },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN,     VT3_BTN_DOWN },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT,     VT3_BTN_LEFT },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,    VT3_BTN_RIGHT },
    { SDL_GAMEPAD_BUTTON_SOUTH,         VT3_BTN_A },       /* TODO(verify): A/B may be swapped */
    { SDL_GAMEPAD_BUTTON_EAST,          VT3_BTN_B },
    { SDL_GAMEPAD_BUTTON_WEST,          VT3_BTN_X },
    { SDL_GAMEPAD_BUTTON_NORTH,         VT3_BTN_Y },
    { SDL_GAMEPAD_BUTTON_START,         VT3_BTN_START },
    { SDL_GAMEPAD_BUTTON_BACK,          VT3_BTN_BACK },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, VT3_BTN_LB },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,VT3_BTN_RB },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK,    VT3_BTN_LSTICK },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK,   VT3_BTN_RSTICK },
    { SDL_GAMEPAD_BUTTON_GUIDE,         VT3_BTN_GUIDE },
};

typedef struct key_button_map {
    SDL_Scancode key;
    uint32_t vt3;
} key_button_map;

/* Keyboard fallback. Escape is not mapped: it does not quit (see platform_sdl.c). */
static const key_button_map k_key_map[] = {
    /* TODO(verify): keyboard layout is a placeholder. */
    { SDL_SCANCODE_UP,     VT3_BTN_UP },
    { SDL_SCANCODE_DOWN,   VT3_BTN_DOWN },
    { SDL_SCANCODE_LEFT,   VT3_BTN_LEFT },
    { SDL_SCANCODE_RIGHT,  VT3_BTN_RIGHT },
    { SDL_SCANCODE_Z,      VT3_BTN_A },
    { SDL_SCANCODE_X,      VT3_BTN_B },
    { SDL_SCANCODE_A,      VT3_BTN_X },
    { SDL_SCANCODE_S,      VT3_BTN_Y },
    { SDL_SCANCODE_RETURN, VT3_BTN_START },
    { SDL_SCANCODE_BACKSPACE, VT3_BTN_BACK },
};

static SDL_Gamepad *g_pads[VT3_MAX_PADS];

/* The raw pad state follows the open pads: connected while one is open, buttons cleared with the last. */
static void pads_changed(void)
{
    int any = 0;
    for (int i = 0; i < VT3_MAX_PADS; i++) any |= g_pads[i] != NULL;
    vt3_input_set_pad_connected(any);
    if (!any) vt3_input_set_pad_button(0xFFFFFFFFu, 0);
}

static void open_pad(SDL_JoystickID id)
{
    for (int i = 0; i < VT3_MAX_PADS; i++) {
        if (g_pads[i]) continue;
        g_pads[i] = SDL_OpenGamepad(id);
        if (g_pads[i]) VT3_INFO("gamepad %d: %s", i, SDL_GetGamepadName(g_pads[i]));
        pads_changed();
        return;
    }
}

static void close_pad(SDL_JoystickID id)
{
    for (int i = 0; i < VT3_MAX_PADS; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
        }
    }
    pads_changed();
}

void vt3_input_sdl_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        open_pad(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        close_pad(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        for (size_t i = 0; i < sizeof k_pad_map / sizeof k_pad_map[0]; i++) {
            if (k_pad_map[i].sdl == e->gbutton.button) {
                vt3_input_set_button(k_pad_map[i].vt3, e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                vt3_input_set_pad_button(k_pad_map[i].vt3, e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            }
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.repeat) break;
        vt3_input_set_key((uint32_t)e->key.scancode, e->type == SDL_EVENT_KEY_DOWN,
                          (uint32_t)(e->key.timestamp / 1000000u));
        for (size_t i = 0; i < sizeof k_key_map / sizeof k_key_map[0]; i++) {
            if (k_key_map[i].key == e->key.scancode) {
                vt3_input_set_button(k_key_map[i].vt3, e->type == SDL_EVENT_KEY_DOWN);
                break;
            }
        }
        break;
    default:
        break;
    }
}

/* Sticks and triggers, sampled once per frame. Deadzone is a TODO. */
void vt3_input_sdl_poll(void)
{
    SDL_Gamepad *pad = NULL;
    for (int i = 0; i < VT3_MAX_PADS && !pad; i++) pad = g_pads[i];
    if (!pad) {
        vt3_input_set_axis(VT3_AXIS_LX, 0); vt3_input_set_axis(VT3_AXIS_LY, 0);
        vt3_input_set_axis(VT3_AXIS_RX, 0); vt3_input_set_axis(VT3_AXIS_RY, 0);
        vt3_input_set_axis(VT3_AXIS_LT, 0); vt3_input_set_axis(VT3_AXIS_RT, 0);
        return;
    }
    const float s = 1.0f / 32767.0f;
    vt3_input_set_axis(VT3_AXIS_LX, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) * s);
    vt3_input_set_axis(VT3_AXIS_LY, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) * s);
    vt3_input_set_axis(VT3_AXIS_RX, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) * s);
    vt3_input_set_axis(VT3_AXIS_RY, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) * s);
    vt3_input_set_axis(VT3_AXIS_LT, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * s);
    vt3_input_set_axis(VT3_AXIS_RT, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * s);
}

/* ---- scripted input (VT3_INPUT_SCRIPT) ------------------------------------------
 * VT3_INPUT_SCRIPT="2.0:DOWN,2.5:RETURN": each entry presses a key or gamepad button at
 * that time (seconds after the platform starts) and releases it 0.12 s later. A timer
 * thread pushes the presses as SDL events with SDL_PushEvent, so they take the same path
 * as a real press: the SDL queue, vt3_input_sdl_event, then the raw keyboard state that
 * DirectInput reads. Off unless the variable is set.
 * Keys: UP DOWN LEFT RIGHT RETURN ESCAPE (keyboard), START (gamepad button).
 */
#define SCRIPT_MAX_ENTRIES 64
#define SCRIPT_HOLD_NS 120000000ull

typedef struct script_event {
    uint64_t at_ns;              /* offset from the script start */
    int is_pad;                  /* 0: keyboard scancode, 1: gamepad button */
    int code;                    /* SDL_Scancode or SDL_GamepadButton */
    int down;
    char name[16];
} script_event;

static script_event g_script[2 * SCRIPT_MAX_ENTRIES];
static int g_script_n;

static int script_lookup(const char *name, int *is_pad, int *code)
{
    static const struct { const char *name; int is_pad; int code; } k[] = {
        { "UP", 0, SDL_SCANCODE_UP },
        { "DOWN", 0, SDL_SCANCODE_DOWN },
        { "LEFT", 0, SDL_SCANCODE_LEFT },
        { "RIGHT", 0, SDL_SCANCODE_RIGHT },
        { "RETURN", 0, SDL_SCANCODE_RETURN },
        { "ESCAPE", 0, SDL_SCANCODE_ESCAPE },
        { "Z", 0, SDL_SCANCODE_Z },              /* A button, as in k_key_map */
        { "X", 0, SDL_SCANCODE_X },              /* B */
        { "A", 0, SDL_SCANCODE_A },              /* X */
        { "S", 0, SDL_SCANCODE_S },              /* Y */
        { "BACKSPACE", 0, SDL_SCANCODE_BACKSPACE }, /* BACK */
        { "START", 1, SDL_GAMEPAD_BUTTON_START },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (strcmp(name, k[i].name) == 0) {
            *is_pad = k[i].is_pad;
            *code = k[i].code;
            return 1;
        }
    }
    return 0;
}

/* Parses "t:KEY,t:KEY,..." into press and release events. Returns the number of entries. */
static int script_parse(const char *spec)
{
    char *copy = strdup(spec);
    if (!copy) return 0;
    int entries = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        char *end = NULL;
        double at = strtod(tok, &end);
        if (end == tok || *end != ':' || at < 0.0) {
            VT3_WARN("VT3_INPUT_SCRIPT: bad entry '%s' (want time:KEY)", tok);
            continue;
        }
        char name[16] = { 0 };
        size_t n = 0;
        for (const char *p = end + 1; *p && n < sizeof name - 1; p++) {
            if (*p != ' ') name[n++] = (char)toupper((unsigned char)*p);
        }
        int is_pad = 0, code = 0;
        if (!script_lookup(name, &is_pad, &code)) {
            VT3_WARN("VT3_INPUT_SCRIPT: unknown key '%s'", name);
            continue;
        }
        if (entries >= SCRIPT_MAX_ENTRIES) break;
        uint64_t t = (uint64_t)(at * 1000000000.0 + 0.5);
        for (int down = 1; down >= 0; down--) {
            script_event *e = &g_script[g_script_n++];
            e->at_ns = down ? t : t + SCRIPT_HOLD_NS;
            e->is_pad = is_pad;
            e->code = code;
            e->down = down;
            memcpy(e->name, name, sizeof e->name);
        }
        entries++;
    }
    free(copy);
    for (int i = 1; i < g_script_n; i++) {          /* sort by time; releases follow presses */
        script_event x = g_script[i];
        int j = i - 1;
        while (j >= 0 && g_script[j].at_ns > x.at_ns) {
            g_script[j + 1] = g_script[j];
            j--;
        }
        g_script[j + 1] = x;
    }
    return entries;
}

static void script_wait_until(uint64_t t)
{
    for (;;) {
        uint64_t now = SDL_GetTicksNS();
        if (now >= t) return;
        uint64_t left = t - now;
        SDL_DelayNS(left < 5000000ull ? left : 5000000ull);
    }
}

static void script_push(const script_event *e)
{
    SDL_Event ev;
    SDL_zero(ev);
    uint64_t now = SDL_GetTicksNS();
    if (e->is_pad) {
        ev.type = e->down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        ev.gbutton.timestamp = now;
        ev.gbutton.button = (Uint8)e->code;
        ev.gbutton.down = e->down != 0;
    } else {
        ev.type = e->down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        ev.key.timestamp = now;
        ev.key.scancode = (SDL_Scancode)e->code;
        ev.key.down = e->down != 0;
    }
    if (!SDL_PushEvent(&ev)) VT3_WARN("input script: SDL_PushEvent failed: %s", SDL_GetError());
}

static void *script_thread(void *arg)
{
    (void)arg;
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < g_script_n; i++) {
        script_wait_until(t0 + g_script[i].at_ns);
        script_push(&g_script[i]);
        VT3_INFO("input script: %s %s at %.2f s", g_script[i].name, g_script[i].down ? "down" : "up",
                 (double)g_script[i].at_ns / 1e9);
    }
    return NULL;
}

void vt3_input_sdl_script_start(void)
{
    const char *spec = getenv("VT3_INPUT_SCRIPT");
    if (!spec || !*spec) return;
    int entries = script_parse(spec);
    if (entries == 0) {
        VT3_WARN("VT3_INPUT_SCRIPT: no usable entries; no scripted input");
        return;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, script_thread, NULL) != 0) {
        VT3_WARN("VT3_INPUT_SCRIPT: could not start the timer thread");
        return;
    }
    pthread_detach(th);
    VT3_INFO("input script: %d entries scheduled from VT3_INPUT_SCRIPT", entries);
}

/* Opens the gamepads attached before the event pump runs. SDL reports them only as
 * SDL_EVENT_GAMEPAD_ADDED once the pump is running, and the game enumerates at start-up. */
void vt3_input_sdl_open_attached(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; i++) open_pad(ids[i]);
    SDL_free(ids);
    pads_changed();
}

void vt3_input_sdl_shutdown(void)
{
    for (int i = 0; i < VT3_MAX_PADS; i++) {
        if (g_pads[i]) SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = NULL;
    }
    pads_changed();
}
