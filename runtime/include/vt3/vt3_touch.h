/*
 * On-screen touch controls. The Android build draws a translucent controller over the game and
 * feeds it into the same input state the gamepad uses (vt3/vt3_input.h), so the DirectInput shim
 * reads it exactly like a real pad.
 *
 * The controls are laid out in the renderer's logical pixels and drawn through the fixed-function
 * (XYZRHW) path, so Vulkan and OpenGL render them the same way. Touch coordinates arrive in
 * drawable pixels and are mapped back through the present letterbox.
 *
 * Enabled by default on Android; VT3_TOUCH=1 forces it on elsewhere (draw only, no touch input).
 */
#ifndef VT3_TOUCH_H
#define VT3_TOUCH_H

/* 1 when the controls are shown and consume touch events. */
int vt3_touch_enabled(void);

/* One pointer's new state. x, y are in drawable (window) pixels; down is 1 for press, 0 for
 * release. Pointers are identified by the platform's pointer id (a finger keeps its id while
 * down, so a drag stays on the control it started on). */
void vt3_touch_pointer(int id, int down, float x, float y);

/* Release every pointer (ACTION_CANCEL, focus loss, window teardown). */
void vt3_touch_release_all(void);

/* Per frame: refresh the view (drawable -> logical mapping) and draw the controls. Call after the
 * game has drawn its frame and before the renderer presents, so the overlay is on top. */
void vt3_touch_frame(int drawable_w, int drawable_h);

#endif /* VT3_TOUCH_H */
