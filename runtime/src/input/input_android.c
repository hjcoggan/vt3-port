/*
 * Input on Android: key and motion events from the NativeActivity input queue, mapped onto the
 * same game state the SDL layer fills (src/input/input_sdl.c). Keyboard keys use HID usages (the
 * scancode values SDL uses), so the DirectInput shim reads them the same way on both platforms.
 */
#include "vt3/vt3_input.h"
#include "vt3/vt3_log.h"

#include <android/input.h>
#include <android/keycodes.h>

static int g_pad_seen;

/* Android keycode -> HID usage (the key number the DirectInput shim reports). 0 = no key. */
static uint32_t hid_for_keycode(int32_t code)
{
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) return 0x04u + (uint32_t)(code - AKEYCODE_A);
    if (code >= AKEYCODE_1 && code <= AKEYCODE_9) return 0x1Eu + (uint32_t)(code - AKEYCODE_1);
    if (code == AKEYCODE_0) return 0x27u;
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12) return 0x3Au + (uint32_t)(code - AKEYCODE_F1);
    switch (code) {
    case AKEYCODE_ENTER:        return 0x28u;
    case AKEYCODE_ESCAPE:       return 0x29u;
    case AKEYCODE_DEL:          return 0x2Au;   /* the backspace key */
    case AKEYCODE_TAB:          return 0x2Bu;
    case AKEYCODE_SPACE:        return 0x2Cu;
    case AKEYCODE_DPAD_RIGHT:   return 0x4Fu;
    case AKEYCODE_DPAD_LEFT:    return 0x50u;
    case AKEYCODE_DPAD_DOWN:    return 0x51u;
    case AKEYCODE_DPAD_UP:      return 0x52u;
    case AKEYCODE_CTRL_LEFT:    return 0xE0u;
    case AKEYCODE_SHIFT_LEFT:   return 0xE1u;
    case AKEYCODE_ALT_LEFT:     return 0xE2u;
    default:                    return 0;
    }
}

/* Keyboard keys that also drive a game button (the same table as input_sdl.c). */
static uint32_t button_for_hid(uint32_t hid)
{
    switch (hid) {
    case 0x52: return VT3_BTN_UP;       /* arrows */
    case 0x51: return VT3_BTN_DOWN;
    case 0x50: return VT3_BTN_LEFT;
    case 0x4F: return VT3_BTN_RIGHT;
    case 0x1A: return VT3_BTN_UP;       /* W */
    case 0x16: return VT3_BTN_DOWN;     /* S */
    case 0x04: return VT3_BTN_LEFT;     /* A */
    case 0x07: return VT3_BTN_RIGHT;    /* D */
    case 0x1D: return VT3_BTN_A;        /* Z: Top Spin */
    case 0x1B: return VT3_BTN_B;        /* X: Slice */
    case 0x06: return VT3_BTN_Y;        /* C: Lob */
    case 0xE1: return VT3_BTN_LB;       /* Left Shift: Instruct COM partner */
    case 0x19: return VT3_BTN_BACK;     /* V: Change camera */
    case 0x28: return VT3_BTN_START;    /* Return: Pause */
    case 0x2A: return VT3_BTN_BACK;     /* Backspace */
    default:   return 0;
    }
}

/* Gamepad keycodes -> game buttons (the d-pad keycodes are in the keyboard path too). */
static uint32_t pad_button_for_keycode(int32_t code)
{
    switch (code) {
    case AKEYCODE_DPAD_UP:      return VT3_BTN_UP;
    case AKEYCODE_DPAD_DOWN:    return VT3_BTN_DOWN;
    case AKEYCODE_DPAD_LEFT:    return VT3_BTN_LEFT;
    case AKEYCODE_DPAD_RIGHT:   return VT3_BTN_RIGHT;
    case AKEYCODE_BUTTON_A:     return VT3_BTN_A;
    case AKEYCODE_BUTTON_B:     return VT3_BTN_B;
    case AKEYCODE_BUTTON_X:     return VT3_BTN_X;
    case AKEYCODE_BUTTON_Y:     return VT3_BTN_Y;
    case AKEYCODE_BUTTON_START: return VT3_BTN_START;
    case AKEYCODE_BUTTON_SELECT: return VT3_BTN_BACK;
    case AKEYCODE_BUTTON_L1:
    case AKEYCODE_BUTTON_L2:    return VT3_BTN_LB;
    case AKEYCODE_BUTTON_R1:
    case AKEYCODE_BUTTON_R2:    return VT3_BTN_RB;
    case AKEYCODE_BUTTON_THUMBL: return VT3_BTN_LSTICK;
    case AKEYCODE_BUTTON_THUMBR: return VT3_BTN_RSTICK;
    case AKEYCODE_BUTTON_MODE:  return VT3_BTN_GUIDE;
    default:                    return 0;
    }
}

static int is_pad(int32_t source)
{
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
           (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
}

static void note_pad(void)
{
    if (!g_pad_seen) {
        g_pad_seen = 1;
        vt3_input_set_pad_connected(1);
        VT3_INFO("input: gamepad connected");
    }
}

static void set_pad(uint32_t mask, int down)
{
    if (!mask) return;
    vt3_input_set_button(mask, down);
    vt3_input_set_pad_button(mask, down);
}

static void key_event(const AInputEvent *e)
{
    int32_t code = AKeyEvent_getKeyCode(e);
    int down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
    uint32_t ms = (uint32_t)(AKeyEvent_getEventTime(e) / 1000000);
    int32_t source = AInputEvent_getSource(e);
    if (is_pad(source)) {
        note_pad();
        set_pad(pad_button_for_keycode(code), down);
        return;
    }
    uint32_t hid = hid_for_keycode(code);
    if (!hid) return;
    vt3_input_set_key(hid, down, ms);
    uint32_t mask = button_for_hid(hid);
    if (mask) vt3_input_set_button(mask, down);
}

static float axis(const AInputEvent *e, int32_t which)
{
    return AMotionEvent_getAxisValue(e, which, 0);
}

static void motion_event(const AInputEvent *e)
{
    note_pad();
    /* Hat: the d-pad on pads that report it as an axis. */
    float hx = axis(e, AMOTION_EVENT_AXIS_HAT_X), hy = axis(e, AMOTION_EVENT_AXIS_HAT_Y);
    set_pad(VT3_BTN_LEFT, hx < -0.5f);
    set_pad(VT3_BTN_RIGHT, hx > 0.5f);
    set_pad(VT3_BTN_UP, hy < -0.5f);
    set_pad(VT3_BTN_DOWN, hy > 0.5f);
    vt3_input_set_axis(VT3_AXIS_LX, axis(e, AMOTION_EVENT_AXIS_X));
    vt3_input_set_axis(VT3_AXIS_LY, axis(e, AMOTION_EVENT_AXIS_Y));
    vt3_input_set_axis(VT3_AXIS_RX, axis(e, AMOTION_EVENT_AXIS_Z));
    vt3_input_set_axis(VT3_AXIS_RY, axis(e, AMOTION_EVENT_AXIS_RZ));
    /* Triggers come as LTRIGGER/RTRIGGER on most pads and as BRAKE/GAS on some. */
    float lt = axis(e, AMOTION_EVENT_AXIS_LTRIGGER), rt = axis(e, AMOTION_EVENT_AXIS_RTRIGGER);
    if (lt == 0.0f) lt = axis(e, AMOTION_EVENT_AXIS_BRAKE);
    if (rt == 0.0f) rt = axis(e, AMOTION_EVENT_AXIS_GAS);
    vt3_input_set_axis(VT3_AXIS_LT, lt);
    vt3_input_set_axis(VT3_AXIS_RT, rt);
}

/* The glue's input callback. Returns 1 when the event was used. */
int vt3_input_android_event(const AInputEvent *e)
{
    int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(e);
        /* Volume keys belong to the system: consuming them hides the volume HUD. */
        if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN ||
            code == AKEYCODE_VOLUME_MUTE) return 0;
        key_event(e);
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && is_pad(AInputEvent_getSource(e))) {
        motion_event(e);
        return 1;
    }
    return 0;
}
