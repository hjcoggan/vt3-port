/*
 * Game-side input. The DirectInput8 shim reads this state. The platform layer
 * fills it from SDL events (see src/input/input_sdl.c for the mapping).
 */
#ifndef VT3_INPUT_H
#define VT3_INPUT_H

#include <stdint.h>

enum vt3_button {
    VT3_BTN_UP     = 1u << 0,
    VT3_BTN_DOWN   = 1u << 1,
    VT3_BTN_LEFT   = 1u << 2,
    VT3_BTN_RIGHT  = 1u << 3,
    VT3_BTN_A      = 1u << 4,
    VT3_BTN_B      = 1u << 5,
    VT3_BTN_X      = 1u << 6,
    VT3_BTN_Y      = 1u << 7,
    VT3_BTN_START  = 1u << 8,
    VT3_BTN_BACK   = 1u << 9,
    VT3_BTN_LB     = 1u << 10,
    VT3_BTN_RB     = 1u << 11,
    VT3_BTN_LSTICK = 1u << 12,
    VT3_BTN_RSTICK = 1u << 13,
    VT3_BTN_GUIDE  = 1u << 14
};

enum vt3_axis {
    VT3_AXIS_LX = 0,   /* -1..1, left = negative */
    VT3_AXIS_LY,       /* -1..1, up = negative */
    VT3_AXIS_RX,
    VT3_AXIS_RY,
    VT3_AXIS_LT,       /* 0..1 */
    VT3_AXIS_RT,       /* 0..1 */
    VT3_AXIS_COUNT
};

typedef struct vt3_input_state {
    uint32_t buttons;              /* bitmask of enum vt3_button */
    float    axis[VT3_AXIS_COUNT];
} vt3_input_state;

/*
 * Merged game state (buttons from keyboard and pad, axes from the pad).
 * The raw state below keeps the sources apart, for the DirectInput shim.
 */
void vt3_input_init(void);
void vt3_input_set_button(uint32_t mask, int down);
void vt3_input_set_axis(enum vt3_axis axis, float value);
const vt3_input_state *vt3_input_get(void);

/* Raw keyboard. Scancodes are SDL_Scancode values, which are USB HID usages (0..511). */
#define VT3_KEY_COUNT 512
typedef struct vt3_key_event {
    uint32_t scancode;
    int      down;
    uint32_t ms;                       /* event time, milliseconds (from SDL) */
} vt3_key_event;

void vt3_input_set_key(uint32_t scancode, int down, uint32_t ms);
int  vt3_input_key_down(uint32_t scancode);
/* Copies up to max queued key transitions, oldest first. Consumes them unless peek is nonzero. */
uint32_t vt3_input_take_keys(vt3_key_event *out, uint32_t max, int peek);

/* Raw gamepad buttons, in enum vt3_button bits. The d-pad is UP, DOWN, LEFT, RIGHT. */
void     vt3_input_set_pad_button(uint32_t mask, int down);
uint32_t vt3_input_pad_buttons(void);
void     vt3_input_set_pad_connected(int connected);
int      vt3_input_pad_connected(void);

#endif /* VT3_INPUT_H */
