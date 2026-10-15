#!/usr/bin/env python3
"""Writes the Android adaptive-icon foreground SVG from vt3_logo_icon.svg: an opaque full-bleed red
canvas with the logo scaled into the 66 of 108 dp safe zone. Usage: make_android_foreground.py OUT.svg"""
import sys

src = open(__file__.replace("make_android_foreground.py", "vt3_logo_icon.svg"), encoding="utf-8").read()
head = src[:src.index("  <g clip-path")]
start = src.index('  <g transform="translate(512 512) scale(')
end = src.index("  </g>\n</svg>")
letters = src[start:end].replace("scale(0.8)", "scale(0.66)")
out = head + '  <rect x="0" y="0" width="1024" height="1024" fill="url(#red)"/>\n'
out += '  <ellipse cx="512" cy="520" rx="400" ry="260" fill="url(#oval)" transform="rotate(-10 512 520)"/>\n'
out += letters + "  </g>\n</svg>\n"
open(sys.argv[1], "w", encoding="utf-8").write(out)
print("wrote", sys.argv[1])
