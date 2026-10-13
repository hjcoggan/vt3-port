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

/* Keyboard fallback. Escape is handled by the platform layer as quit. */
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

static void open_pad(SDL_JoystickID id)
{
    for (int i = 0; i < VT3_MAX_PADS; i++) {
        if (g_pads[i]) continue;
        g_pads[i] = SDL_OpenGamepad(id);
        if (g_pads[i]) VT3_INFO("gamepad %d: %s", i, SDL_GetGamepadName(g_pads[i]));
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
                break;
            }
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.repeat) break;
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

void vt3_input_sdl_shutdown(void)
{
    for (int i = 0; i < VT3_MAX_PADS; i++) {
        if (g_pads[i]) SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = NULL;
    }
}
