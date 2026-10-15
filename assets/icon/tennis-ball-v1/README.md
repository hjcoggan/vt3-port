# Tennis-ball icon (v1), kept for another project

The first app icon: a tennis ball on a court-green rounded square.

- `macos/vt3_icon.svg`: the source, on a 1024 px canvas with the macOS icon grid.
- `macos/vt3_icon_1024.png`: the rendered master, masked to the rounded square (transparent outside).
- `macos/AppIcon.icns`: the icon set built from that master.
- `macos/mask_icon.swift`, `macos/make_icns.sh`: the masking and icon-set steps.
- `android/res/`: the adaptive-icon resources (background, foreground vector, and the mipmap XML).

Superseded for VT3 by the logo-style icon. Nothing here is referenced by the build.
