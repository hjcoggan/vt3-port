# Native widescreen (Hor+)

Virtua Tennis 3 renders Hor+ widescreen on both macOS and Android: the frame fills the
whole display, the 3D camera keeps its vertical field of view and widens horizontally,
and the HUD is laid out for the wider frame. Nothing is stretched or cropped, and there
are no letterbox/pillarbox bars at the frame's own aspect.

This document is the reference for the mechanism. The code lives in
`runtime/shim/src/d3d9_gpu.c` (`vt3s_display_prepare()`, `write_widescreen_ini()`,
`widescreen_aspect()`, `widescreen_enable()`, `choose_target()`, `d3d9_gpu_prepare()`),
called once from `vt3shim_init()` in `runtime/shim/src/shim_core.c`.

## Why not patch the projection or stretch the image

Simple stretching (render 4:3, blit to the full panel) distorts circles and player models;
that was explicitly rejected. Patching the projection matrix would also be the wrong fight:
the game **already ships** a widescreen mode, and using it means the camera, the HUD layout
and the UI scaling all agree with each other for free.

## The game's own WIDESCREEN support

`vt3.ini` (parsed by the guest at `0x403790`, keys are `KEY = VALUE` with spaces and `=`
as its own token) has a `WIDESCREEN` key that lands in the config block at `0x8598a0`:

| INI key | config offset | meaning |
|---|---|---|
| `DX/ADAPTER` | `+0x104` | adapter index |
| `DX/RES/WIDTH` | `+0x108` | back-buffer width (float) |
| `DX/RES/HEIGHT` | `+0x10c` | back-buffer height (float) |
| `DX/WINDOWED` | `+0x118` | **do not set** — forces a 640x480 fallback |
| `VSYNC` | `+0x119` | |
| `WIDESCREEN` | `+0x122` | widescreen flag |
| `LANGUAGE` | `+0x144` | |

With `WIDESCREEN = 1`:

* the engine's widescreen flag is set (the config byte is copied into the engine object at
  `+0x9c`/`+0x9d` by the engine constructor at `0x4732f8`, engine pointer at `0x9abe88`);
* the 3D camera picks the wide aspect path at `0x4496a8` (gated on `[0x9abe88]+0x9c`), keeping
  the vertical FOV and widening horizontally — true Hor+;
* the HUD lays itself out for the wider frame.

The flag is **read once at startup**, before the renderer is built, so the shim writes it
into the INI *before the guest runs*; it cannot be toggled per frame by writing config.

## The camera aspect is a fixed 16:9 constant, not the resolution

The wide path does not derive its aspect from `DX/RES/*`. It uses the guest float constant
at `0x6cf0e8` (the 4:3 path uses `0x75f494` = 1.3333). On the disc that constant is exactly
16:9 (1.7778), so on a 19.5:9 phone panel the frame would still be pillarboxed inside a 16:9
window. The shim therefore **patches that constant to the actual frame aspect**
(`widescreen_aspect()`, e.g. 2340/1080 = 2.1667 on a Galaxy S26, 2234/1080 = 2.069 on the Mac).
That keeps the vertical FOV and widens only horizontally, at any panel ratio.

## Drive the resolution through the game's own config, not the D3D9 present params

The game caches its configured resolution and uses it for the per-frame viewport and the HUD.
Overriding the D3D9 back buffer / presentation behind the game's back only paints the top-left
corner (the game keeps its own viewport at the old size). So the shim:

1. at `vt3shim_init()` time brings up the platform window (`d3d9_gpu_prepare()`), reads the
   **drawable** size, and picks a frame at the drawable's aspect with height capped at 1080 px
   (`VT3_WIDE_MAX_H`; a 1440p phone panel is ~4x the pixels for no visible gain at arm's length);
2. merges `DX/RES/WIDTH`, `DX/RES/HEIGHT` and `WIDESCREEN = 1` into `<game_root>/vt3.ini`,
   keeping every other key the player put there;
3. patches the camera aspect constant to that frame's aspect;
4. re-asserts the engine widescreen flag every frame in `d3d9_gpu_begin()` (the engine can be
   rebuilt between frames).

The game then reads the INI itself and builds its renderer, viewport, HUD and camera around
the same widescreen frame — everything agrees.

## Environment variables

| Variable | Effect |
|---|---|
| `VT3_NO_WIDESCREEN` | disable all of the above (game keeps its own resolution/aspect) |
| `VT3_RENDER_HEIGHT` | override the 1080 px frame-height cap |

## Resizing

The logical frame aspect is fixed at launch (it is baked into the game's cached resolution and
the patched camera constant). The window is user-resizable on macOS and scales the frame to the
new size; resizing to a different aspect letterboxes the frame in the standard way rather than
distorting it. A launch at the display's own aspect fills the screen with no bars.

## Android: surface pre-rotation

`vk.c` had to be fixed for the frame to appear full and upright on Adreno:

* `create_swapchain()` uses `preTransform = IDENTITY` when the surface supports it, because
  Android reports `currentExtent` already in the display's current orientation (3120x1440 in
  landscape on a 1440x3120 panel) while `currentTransform` is still `ROTATE_90`. An
  extent-plus-transform mismatch makes the compositor rotate the image again (sideways and
  squashed);
* `vk_present()` ignores `VK_SUBOPTIMAL_KHR` on Android (this is the expected result of the
  identity pre-rotation choice; rebuilding would recreate the swapchain every frame).

## Deploying data on Android

`adb push` creates `data2/` as `shell:ext_data_rw 2770`, and the app may not be in that group,
so it cannot traverse the tree (every XPAC stat fails with `EACCES`). The shim also has to
*write* `vt3.ini` into `data2/`. `android/deploy_data.sh` therefore ends with:

```bash
adb -s SERIAL shell chmod -R a+rwX /sdcard/Android/data/net.vt3port.game/files/data2
```

Run it after every data push. Do not `chmod` `files/` itself — the FUSE sdcard rejects it
(`Operation not permitted`) and it is already the app's own directory.

## Verification

* **macOS**, drawable 3840x1856 (aspect 2.069): shim logs `widescreen: .../vt3.ini = 2234x1080`,
  `CreateDevice ... (2234x1080 back buffer)`. Main menu fills the window with no bars; an
  in-match 3D capture shows more side scenery with no distortion and corner-anchored HUD.
* **Galaxy S26** (3120x1440 panel, aspect 2.167), Android 16: shim logs
  `widescreen: /sdcard/Android/data/net.vt3port.game/files/data2/vt3.ini = 2340x1080` and
  `CreateDevice ... (2340x1080 back buffer, renderer)`. A screencap has live game content to all
  four edges (no black bars) — see `dump/android/s26_widescreen.png`.
