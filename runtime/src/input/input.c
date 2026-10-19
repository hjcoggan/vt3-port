/* Backend-neutral input state. Written from the event thread, read by the shim. */
#include "vt3/vt3_input.h"

#include <string.h>
#include <time.h>

#define VT3_KEY_QUEUE 64

static vt3_input_state g_state;             /* merged: keyboard and pad buttons, pad axes */

static uint8_t g_keys[VT3_KEY_COUNT];       /* raw keyboard, 1 = down */
static vt3_key_event g_queue[VT3_KEY_QUEUE];
static uint32_t g_q_head, g_q_len;          /* ring of key transitions, oldest at head */

static uint32_t g_pad;                      /* raw gamepad buttons, enum vt3_button bits */
static int g_pad_connected;
static int g_keyboard_seen;                 /* a physical keyboard has delivered a key */

void vt3_input_init(void)
{
    memset(&g_state, 0, sizeof g_state);
    memset(g_keys, 0, sizeof g_keys);
    memset(g_queue, 0, sizeof g_queue);
    g_q_head = g_q_len = 0;
    g_pad = 0;
    g_pad_connected = 0;
    g_keyboard_seen = 0;
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

void vt3_input_set_keyboard_connected(int seen)
{
    g_keyboard_seen = seen != 0;
}

int vt3_input_keyboard_connected(void)
{
    return g_keyboard_seen;
}

/* Loading activity. Written from the guest threads (texture creation), read by the input script's
 * timer thread; both sides go through atomics. */
static uint64_t g_last_load_ns, g_start_ns;
static uint32_t g_loads;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void vt3_input_note_load(void)
{
    __atomic_store_n(&g_last_load_ns, mono_ns(), __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_loads, 1, __ATOMIC_RELAXED);
}

double vt3_input_seconds_since_load(void)
{
    uint64_t now = mono_ns();
    uint64_t last = __atomic_load_n(&g_last_load_ns, __ATOMIC_RELAXED);
    if (last == 0) {
        uint64_t zero = 0;
        __atomic_compare_exchange_n(&g_start_ns, &zero, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        last = __atomic_load_n(&g_start_ns, __ATOMIC_RELAXED);
    }
    return now > last ? (double)(now - last) / 1e9 : 0.0;
}

uint32_t vt3_input_load_count(void)
{
    return __atomic_load_n(&g_loads, __ATOMIC_RELAXED);
}
