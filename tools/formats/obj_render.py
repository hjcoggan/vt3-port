#!/usr/bin/env python3
"""Render OBJ meshes to a PNG preview with three orthographic views.

Reads v, vt and f (a, a/b, a/b/c, a//c; 1-based, negative indices), fanning
polygons into triangles; front (along -Z, or -Y for --up z), side (along +X) and
top (down the up axis) are drawn by a numpy rasteriser with a z-buffer on one
shared scale (5% margin), flat two-sided Lambert from the viewer's light plus
0.3 ambient; palette colours, or a --texture PNG (nearest, v flip, wrap).

Usage:
    obj_render.py OUT.png MESH.obj [MESH.obj ...] [--size 512] [--up y|z]
                  [--texture MESH.obj=TEX.png ...]
    obj_render.py --selftest
"""
import argparse
import os
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

PALETTE = [(205, 85, 75), (75, 145, 205), (225, 190, 95), (110, 185, 120),
           (175, 125, 200), (220, 145, 95), (95, 185, 185), (200, 200, 205)]
VIEWS = {"y": [("front", (0, 0, -1), (0, 1, 0)), ("side", (1, 0, 0), (0, 1, 0)), ("top", (0, -1, 0), (0, 0, -1))],
         "z": [("front", (0, -1, 0), (0, 0, 1)), ("side", (1, 0, 0), (0, 0, 1)), ("top", (0, 0, -1), (0, 1, 0))]}
BG, LABEL, AMBIENT, MARGIN = (24, 24, 28), (235, 235, 235), 0.3, 0.05

def load_mesh(path, tex_path, color):
    """Return a mesh dict for one OBJ file, with a 1x1 palette or PNG texture."""
    pos, vt, tri = [], [], []
    with open(path, "r", errors="replace") as f:
        for w in (line.split() for line in f):
            if w[:1] == ["v"]:
                pos.append([float(x) for x in w[1:4]])
            elif w[:1] == ["vt"]:
                vt.append([float(w[1]), float(w[2])])
            elif w[:1] == ["f"]:
                c = [tok.split("/") for tok in w[1:]]
                i, j = [int(t[0]) for t in c], [int(t[1]) if len(t) > 1 and t[1] else 0 for t in c]
                i = [k - 1 if k > 0 else len(pos) + k for k in i]
                j = [(k - 1 if k > 0 else len(vt) + k) if k else -1 for k in j]
                tri += [(i[0], j[0], i[k], j[k], i[k + 1], j[k + 1]) for k in range(1, len(c) - 1)]
    if not pos: raise SystemExit("%s: no vertices" % path)
    p, t = np.array(pos, np.float64).reshape(-1, 3), np.array(tri, np.int64).reshape(-1, 6)
    j = t[:, 1::2]
    uv = np.where((j >= 0)[..., None], np.array(vt or [(0.0, 0.0)]).reshape(-1, 2)[np.maximum(j, 0)], 0.0)
    tex = (np.asarray(Image.open(tex_path).convert("RGB"), np.float32) / 255.0 if tex_path else np.array(color, np.float32).reshape(1, 1, 3) / 255.0)
    return {"name": path, "pos": p, "uv": uv, "tris": t[:, ::2], "tex": tex}

def draw_mesh(img, zb, s, mesh, light):
    """Scan-convert one mesh into img with a z-buffer and flat Lambert shading."""
    (h, w), (p, tex, tris) = zb.shape, (mesh["pos"], mesh["tex"], mesh["tris"])
    (th, tw), (u, v) = tex.shape[:2], (mesh["uv"][:, :, 0].T, mesh["uv"][:, :, 1].T)
    n = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    shade = AMBIENT + (1.0 - AMBIENT) * np.abs(n @ light / np.maximum(np.linalg.norm(n, axis=1), 1e-12))
    for k, (a, b, c) in enumerate(tris):
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = s[a], s[b], s[c]
        den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        xa = max(int(np.ceil(min(x0, x1, x2) - 0.5)), 0), min(int(np.floor(max(x0, x1, x2) - 0.5)) + 1, w)
        ya = max(int(np.ceil(min(y0, y1, y2) - 0.5)), 0), min(int(np.floor(max(y0, y1, y2) - 0.5)) + 1, h)
        if abs(den) < 1e-9 or xa[1] <= xa[0] or ya[1] <= ya[0]: continue
        xs, ys = np.arange(xa[0], xa[1]) + 0.5, np.arange(ya[0], ya[1])[:, None] + 0.5
        w0 = ((y1 - y2) * (xs - x2) + (x2 - x1) * (ys - y2)) / den
        w1 = ((y2 - y0) * (xs - x2) + (x0 - x2) * (ys - y2)) / den
        w2 = 1.0 - w0 - w1
        keep = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        z, box = z0 + w1 * (z1 - z0) + w2 * (z2 - z0), (slice(ya[0], ya[1]), slice(xa[0], xa[1]))
        keep &= z > zb[box]
        if not keep.any(): continue
        zb[box][keep] = z[keep]
        su = w0[keep] * u[0, k] + w1[keep] * u[1, k] + w2[keep] * u[2, k]
        sv = w0[keep] * v[0, k] + w1[keep] * v[1, k] + w2[keep] * v[2, k]
        col = tex[np.floor((1.0 - sv) * th).astype(int) % th, np.floor(su * tw).astype(int) % tw]
        img[box][keep] = np.clip(col * shade[k] * 255.0 + 0.5, 0, 255).astype(np.uint8)

def render(meshes, out_path, size, up):
    """Draw the three views side by side, returning the pixels-per-unit scale."""
    pts = np.concatenate([m["pos"] for m in meshes])
    views, extent = [], 0.0
    for name, d, u in VIEWS[up]:
        r, dv, upv = np.cross(d, u), np.asarray(d, np.float64), np.asarray(u, np.float64)
        a, b = pts @ r, pts @ upv
        extent = max(extent, a.max() - a.min(), b.max() - b.min())
        views.append((name, r, upv, -dv, (a.min() + a.max()) / 2, (b.min() + b.max()) / 2))
    s = (1.0 - 2.0 * MARGIN) * size / extent if extent > 0 else 1.0
    img, zb = np.full((size, 3 * size, 3), BG, np.uint8), np.full((size, 3 * size), -np.inf, np.float32)
    for k, (name, r, upv, light, cu, cv) in enumerate(views):
        for m in meshes:
            p = m["pos"]
            draw_mesh(img, zb, np.stack([k * size + size / 2 + (p @ r - cu) * s,
                                         size / 2 - (p @ upv - cv) * s, p @ light], 1), m, light)
    image = Image.fromarray(img); draw = ImageDraw.Draw(image)
    for k, view in enumerate(views): draw.text((k * size + 6, 6), view[0], fill=LABEL)
    image.save(out_path)
    return s

CUBE_QUADS = ((1, 2, 4, 3), (5, 7, 8, 6), (1, 5, 6, 2), (3, 4, 8, 7), (1, 3, 7, 5), (2, 6, 8, 4))
CUBE_OBJ = "\n".join(
    ["v %g %g %g" % p for p in [(x, y, z) for x in (-.5, .5) for y in (-.5, .5) for z in (-.5, .5)]]
    + ["vt %g %g" % t for t in ((0, 0), (1, 0), (1, 1), (0, 1))]
    + ["f " + " ".join("%d/%d" % (i, k + 1) for k, i in enumerate(q)) for q in CUBE_QUADS])
TETRA_OBJ = "v 2 0 0\nv 3 0 0\nv 2 1 0\nv 2 0 1\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 1 4 3\n"
CHECKER = np.array([[[255, 80, 40], [40, 120, 255]], [[40, 120, 255], [255, 80, 40]]], np.uint8)

def selftest():
    """Write a cube and a tetrahedron as OBJ, render them and check the image."""
    tmp = tempfile.mkdtemp(prefix="obj_render_")
    cube, tetra, png = (os.path.join(tmp, n) for n in ("cube.obj", "tetra.obj", "checker.png"))
    for path, text in ((cube, CUBE_OBJ), (tetra, TETRA_OBJ)):
        with open(path, "w") as f: f.write(text)
    Image.fromarray(CHECKER).save(png)
    size, both = 160, os.path.join(tmp, "both.png")
    render([load_mesh(cube, png, PALETTE[0]), load_mesh(tetra, None, PALETTE[1])], both, size, "y")
    covered = int((np.abs(np.asarray(Image.open(both).convert("RGB"), np.int64) - BG).sum(2) > 8).sum())
    assert covered > size * size // 20, "cube + tetrahedron render is blank: %d px" % covered
    expected = ((1.0 - 2.0 * MARGIN) * size) ** 2
    for up in ("y", "z"):
        path = os.path.join(tmp, "cube_%s.png" % up)
        scale = render([load_mesh(cube, png, PALETTE[0])], path, size, up)
        area = int((np.abs(np.asarray(Image.open(path).convert("RGB"), np.int64)[:, :size] - BG).sum(2) > 8).sum())
        off = (area - expected) / expected
        print("selftest --up %s: cube front view %+.1f%% of expected area (scale %.1f)" % (up, 100 * off, scale))
        assert abs(off) <= 0.20, "cube front view area off by %.1f%%" % (100 * off)
    print("selftest ok: %d px drawn for cube + tetrahedron" % covered)
    return 0

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", metavar="OUT.png"); ap.add_argument("meshes", nargs="*", metavar="MESH.obj")
    ap.add_argument("--size", type=int, default=512, help="panel size in pixels (default 512)")
    ap.add_argument("--up", choices=("y", "z"), default="y", help="up axis (default y)")
    ap.add_argument("--texture", action="append", default=[], metavar="MESH.obj=TEX.png", help="sample TEX.png")
    ap.add_argument("--selftest", action="store_true", help="check a generated cube and tetrahedron")
    args = ap.parse_args(argv)
    if args.selftest: return selftest()
    if not args.out or not args.meshes or args.size < 8: ap.error("need OUT.png, a MESH.obj and --size >= 8")
    tex = dict(spec.rpartition("=")[::2] for spec in args.texture)
    loaded = [load_mesh(p, tex.get(p) or tex.get(os.path.basename(p)), PALETTE[i % len(PALETTE)])
              for i, p in enumerate(args.meshes)]
    pts = np.concatenate([m["pos"] for m in loaded])
    print("combined bbox min %s max %s" % (np.round(pts.min(0), 4), np.round(pts.max(0), 4)))
    print("\n".join("  %-44s %6d triangles" % (m["name"], len(m["tris"])) for m in loaded))
    s = render(loaded, args.out, args.size, args.up)
    print("wrote %s (%dx%d, %.4g px per unit)" % (args.out, 3 * args.size, args.size, s))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
