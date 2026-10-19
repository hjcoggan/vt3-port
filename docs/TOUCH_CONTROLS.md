# On-screen touch controls (Android)

An on-screen controller for the Android build, drawn by the runtime over the game: a translucent
minimal set of vector controls in the style of [rokobuljan/gamepad](https://github.com/rokobuljan/gamepad).

```
        [LB]                                    [RB]
                                                 (Y)
                                          (X)         (B)
                                                 (A)
              ( stick )              [SELECT]   [START]
```

* **Thumb stick** (lower-left) — movement. Drives the pad's left stick (`VT3_AXIS_LX/LY`) and, past a
  threshold, the d-pad bits as well, so menus that read the hat navigate too.
* **A / B / X / Y** (lower-right diamond) — Top Spin, Slice/Drop Shot, Lob (the same buttons the
  gamepad layout uses).
* **LB / RB** (top corners) — Instruct COM Partner / Tour Calendar.
* **SELECT / START** (bottom centre) — Change Camera / Pause.

The colours are the Xbox palette (A green, B red, X blue, Y yellow) at low alpha; `VT3_TOUCH_COLOR=0`
draws them monochrome.

## It hides itself when real hardware is in use

If a **physical keyboard or gamepad** is connected/used, the controls disappear and no longer consume
touch, so they never cover the game for a player on real hardware. Detection is from input: any key
event that is not from a gamepad/joystick source marks a keyboard present
(`vt3_input_set_keyboard_connected`), and any gamepad key/axis marks the pad
(`vt3_input_set_pad_connected`, `note_pad`). Both stay set for the rest of the session — Android
cannot report a device being removed without Java — so the controls hide on the first hardware input
and stay hidden. `VT3_TOUCH_FORCE=1` overrides this and always shows the controls (for testing).

## How it is built

* `runtime/src/ui/touch_controls.c` holds the layout, hit testing, drawing and the input mapping.
  Geometry is emitted as translucent triangles through the renderer's fixed-function (XYZRHW) path
  (`vt3_gfx_draw(..., tex = 0)`), which both the Vulkan and OpenGL backends implement, so it looks
  the same on both and scales with the frame. There is one draw call per frame.
* Labels are drawn from the public-domain 8x8 bitmap font in `runtime/src/ui/font8x8.h`
  (generated; see the file header for the source and licence).
* `d3d9_gpu_present()` calls `vt3_touch_frame()` after the game's frame and before present, so the
  controls land on top.
* Touch events are routed in `runtime/src/input/input_android.c` (`touch_event`), one call per
  pointer; a pointer keeps the control it started on until it lifts, so multi-touch (move + shoot)
  works. Coordinates are drawable pixels, mapped into logical pixels through the same letterbox
  present() uses (`vt3_gfx_view_rect`), so the controls are hit exactly where they are drawn.
* The state is written with `vt3_input_set_button` / `vt3_input_set_pad_button` /
  `vt3_input_set_axis`, i.e. into the same game state the gamepad fills, so the DirectInput shim
  reads it unchanged.
* Held input is released on focus loss and window teardown (`vt3_touch_release_all`).

## Environment

| Variable | Default | Effect |
|---|---|---|
| `VT3_TOUCH` | on for Android, off elsewhere | 1 shows the controls, 0 hides them |
| `VT3_TOUCH_FORCE` | unset | 1 ignores keyboard/gamepad presence (always show) |
| `VT3_TOUCH_SCALE` | 1.0 | size multiplier (0.4–2.5) |
| `VT3_TOUCH_ALPHA` | 1.0 | overall opacity (0.1–1.0) |
| `VT3_TOUCH_COLOR` | 1 | 0 for monochrome |
| `VT3_TOUCH_DEBUG` | unset | 1 logs every change of what the controls hold |

On a device these go in `vt3.env` beside the game data
(`/sdcard/Android/data/net.vt3port.game/files/vt3.env`).

## Testing without the phone

On a desktop build, `VT3_TOUCH=1` draws the controls (there is no touch input, so it is only a
layout check). On Android, `adb shell input tap X Y` / `input swipe X0 Y0 X1 Y1 DURATION` injects
touch through the real input stack and drives the controls.
