/* Backend-neutral input state. Written from the event thread, read by the shim. */
#include "vt3/vt3_input.h"

#include <string.h>

#define VT3_KEY_QUEUE 64

static vt3_input_state g_state;             /* merged: keyboard and pad buttons, pad axes */

static uint8_t g_keys[VT3_KEY_COUNT];       /* raw keyboard, 1 = down */
static vt3_key_event g_queue[VT3_KEY_QUEUE];
static uint32_t g_q_head, g_q_len;          /* ring of key transitions, oldest at head */

static uint32_t g_pad;                      /* raw gamepad buttons, enum vt3_button bits */
static int g_pad_connected;

void vt3_input_init(void)
{
    memset(&g_state, 0, sizeof g_state);
    memset(g_keys, 0, sizeof g_keys);
    memset(g_queue, 0, sizeof g_queue);
    g_q_head = g_q_len = 0;
    g_pad = 0;
    g_pad_connected = 0;
}

void vt3_input_set_button(uint32_t mask, int down)
{
    if (down)
        g_state.buttons |= mask;
    else
        g_state.buttons &= ~mask;
}

void vt3_input_set_axis(enum vt3_axis axis, float value)
{
    if ((unsigned)axis < VT3_AXIS_COUNT)
        g_state.axis[axis] = value;
}

const vt3_input_state *vt3_input_get(void)
{
    return &g_state;
}

void vt3_input_set_key(uint32_t scancode, int down, uint32_t ms)
{
    if (scancode >= VT3_KEY_COUNT) return;
    uint8_t v = down ? 1 : 0;
    if (g_keys[scancode] == v) return;
    g_keys[scancode] = v;
    if (g_q_len == VT3_KEY_QUEUE) return;   /* full: the oldest transitions are kept */
    uint32_t tail = (g_q_head + g_q_len) % VT3_KEY_QUEUE;
    g_queue[tail].scancode = scancode;
    g_queue[tail].down = v;
    g_queue[tail].ms = ms;
    g_q_len++;
}

int vt3_input_key_down(uint32_t scancode)
{
    return scancode < VT3_KEY_COUNT && g_keys[scancode];
}

uint32_t vt3_input_take_keys(vt3_key_event *out, uint32_t max, int peek)
{
    uint32_t n = max < g_q_len ? max : g_q_len;
    for (uint32_t i = 0; i < n; i++) out[i] = g_queue[(g_q_head + i) % VT3_KEY_QUEUE];
    if (!peek) {
        g_q_head = (g_q_head + n) % VT3_KEY_QUEUE;
        g_q_len -= n;
    }
    return n;
}

void vt3_input_set_pad_button(uint32_t mask, int down)
{
    if (down)
        g_pad |= mask;
    else
        g_pad &= ~mask;
}

uint32_t vt3_input_pad_buttons(void)
{
    return g_pad;
}

void vt3_input_set_pad_connected(int connected)
{
    g_pad_connected = connected != 0;
}

int vt3_input_pad_connected(void)
{
    return g_pad_connected;
}
