/* Backend-neutral input state. Written from the event thread, read by the shim. */
#include "vt3/vt3_input.h"

#include <string.h>

static vt3_input_state g_state;

void vt3_input_init(void)
{
    memset(&g_state, 0, sizeof g_state);
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
