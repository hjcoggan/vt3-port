#!/usr/bin/env python3
"""Extract static meshes from the 0x0D model tables in game.xpac (PC release).

Model table (payload type 0x0D), verified on PC entries 594, 696, 959, 1080 and
on the PSP GAME.xpac entry 223:

    u32 count (13 for the O_ECO / O_CMN / O_PAN sets)
    u32 total payload size
    count x record (40 bytes): char name[32], u32 offset, u32 size
    offset and size are relative to the payload start.

Each sub-block starts with u32 1, then a u16 triangle list. The list ends at the
first value >= 0x2000. Vertices start at the next 16-byte boundary after the list.

Vertex layout (found by search, see docs/formats.md):

    +0   float32 x, y, z         position (y up)
    +12  u8 nx, ny, nz, 0        packed normal: n = byte / 127.5 - 1
    +16  float32 u, v            texture coordinate 0
    +24  (stride 32 and 36) 8 or 12 bytes, not decoded. Stride 36 seen only on
         O_ECO_alligator, which may be skinned.

Stride and normal offset are searched per block: the normal must be unit length
for (nearly) every vertex. Winding is checked: the face normal must agree with
the vertex normals (CCW, matches glTF).

Blocks with no index run (for example entry 594, whose blocks are matrix and
table data) are skipped and listed in the report.

Usage:
    mesh_extract.py GAME_XPAC ENTRY [OUTDIR] [--report]

ENTRY    payload index in game.xpac (for example 696)
OUTDIR   default: /Users/heath/Virtua Tennis 3/dump/extracted-en/meshes
--report print the per-block analysis only
"""
import argparse
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capx  # noqa: E402

DEFAULT_OUT = "/Users/heath/Virtua Tennis 3/dump/extracted-en/meshes"
STRIDES = list(range(16, 49, 4))
RUN_LIMIT = 0x2000


def align16(v):
    return (v + 15) & ~15


def read_model_table(payload):
    count, total = struct.unpack_from("<II", payload, 0)
    records = []
    for k in range(count):
        base = 8 + 40 * k
        name = payload[base : base + 32].split(b"\0")[0].decode("latin-1")
        off, size = struct.unpack_from("<II", payload, base + 32)
        records.append((name, off, size))
    return records


def index_run(payload, off, size):
    """u16 values from off+4 up to RUN_LIMIT or the end of the block."""
    vals = []
    i = off + 4
    end = off + size
    while i + 2 <= end:
        v = struct.unpack_from("<H", payload, i)[0]
        if v >= RUN_LIMIT:
            break
        vals.append(v)
        i += 2
    return vals, i


def unit_fraction(payload, start, stride, normal_off, n):
    unit = 0
    for v in range(n):
        b = payload[start + v * stride + normal_off : start + v * stride + normal_off + 3]
        x, y, z = b[0] / 127.5 - 1.0, b[1] / 127.5 - 1.0, b[2] / 127.5 - 1.0
        if abs(math.sqrt(x * x + y * y + z * z) - 1.0) < 0.03:
            unit += 1
    return unit / n


def facing_fraction(positions, normals, tris):
    good = bad = 0
    for a, b, c in tris:
        pa, pb, pc = positions[a], positions[b], positions[c]
        u = [pb[i] - pa[i] for i in range(3)]
        w = [pc[i] - pa[i] for i in range(3)]
        fn = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        length = math.sqrt(fn[0] ** 2 + fn[1] ** 2 + fn[2] ** 2)
        if length < 1e-9:
            continue
        vn = [normals[a][i] + normals[b][i] + normals[c][i] for i in range(3)]
        if sum(fn[i] * vn[i] for i in range(3)) / length > 0:
            good += 1
        else:
            bad += 1
    total = good + bad
    return (good / total) if total else 0.0, total


def analyse_block(payload, off, size):
    """Return a dict with the chosen layout, or None if the block has no index list.

    The vertex start is one of the two 16-byte boundaries around the end of the
    u16 run (the index list is everything before it). The stride, normal offset
    and start are chosen by the unit-normal fraction.
    """
    vals, run_end = index_run(payload, off, size)
    if len(vals) < 3:
        return None
    lo = run_end & ~15
    hi = align16(run_end)
    best = None
    for start in sorted({lo, hi}):
        count = (start - (off + 4)) // 2
        if count < 3:
            continue
        flat = [struct.unpack_from("<H", payload, off + 4 + 2 * k)[0] for k in range(count)]
        tri_count = count // 3
        n = max(flat[: tri_count * 3]) + 1
        for stride in STRIDES:
            if start + n * stride > off + size:
                continue
            for normal_off in range(0, stride - 3, 4):
                frac = unit_fraction(payload, start, stride, normal_off, n)
                key = (frac, -stride)
                if best is None or key > best["key"]:
                    best = {"key": key, "stride": stride, "start": start, "normal_off": normal_off,
                            "unit": frac, "vertices": n, "flat": flat[: tri_count * 3],
                            "tri_count": tri_count, "index_count": count}
    if best is None:
        return None
    if best["unit"] < 0.95:
        return {"index_count": best["index_count"], "triangles": best["tri_count"], "ok": False}
    stride, start, nof = best["stride"], best["start"], best["normal_off"]
    n = best["vertices"]
    flat = best["flat"]
    positions, normals, uvs = [], [], []
    for v in range(n):
        base = start + v * stride
        positions.append(struct.unpack_from("<3f", payload, base))
        b = payload[base + nof : base + nof + 3]
        normals.append((b[0] / 127.5 - 1.0, b[1] / 127.5 - 1.0, b[2] / 127.5 - 1.0))
        uvs.append(struct.unpack_from("<2f", payload, base + 16) if stride >= 24 else (0.0, 0.0))
    tris = [(flat[i], flat[i + 1], flat[i + 2]) for i in range(0, len(flat), 3)]
    facing, checked = facing_fraction(positions, normals, tris)
    return {
        "ok": True,
        "index_count": best["index_count"],
        "triangles": best["tri_count"],
        "vertices": n,
        "stride": stride,
        "vertex_start_rel": start - off,
        "normal_offset": nof,
        "unit_fraction": round(best["unit"], 4),
        "facing_fraction": round(facing, 4),
        "facing_checked": checked,
        "positions": positions,
        "normals": normals,
        "uvs": uvs,
        "indices": flat,
    }


def bbox(positions):
    mins = [min(p[i] for p in positions) for i in range(3)]
    maxs = [max(p[i] for p in positions) for i in range(3)]
    return mins, maxs


def write_glb(path, meshes):
    """Write a binary glTF 2.0 file with one node and one mesh per sub-block."""
    buf = bytearray()
    buffer_views, accessors, gltf_meshes, nodes = [], [], [], []

    def add_view(data, target=None):
        while len(buf) % 4:
            buf.append(0)
        off = len(buf)
        buf.extend(data)
        view = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        buffer_views.append(view)
        return len(buffer_views) - 1

    def add_accessor(view, count, comp, kind, mins=None, maxs=None):
        acc = {"bufferView": view, "componentType": comp, "count": count, "type": kind}
        if mins is not None:
            acc["min"] = mins
            acc["max"] = maxs
        accessors.append(acc)
        return len(accessors) - 1

    for m in meshes:
        pos = b"".join(struct.pack("<3f", *p) for p in m["positions"])
        nrm = b"".join(struct.pack("<3f", *n) for n in m["normals"])
        uv = b"".join(struct.pack("<2f", *t) for t in m["uvs"])
        idx = b"".join(struct.pack("<H", i) for i in m["indices"])
        mn, mx = bbox(m["positions"])
        vp = add_view(pos, 34962)
        vn = add_view(nrm, 34962)
        vt = add_view(uv, 34962)
        vi = add_view(idx, 34963)
        ap = add_accessor(vp, len(m["positions"]), 5126, "VEC3", mn, mx)
        an = add_accessor(vn, len(m["normals"]), 5126, "VEC3")
        at = add_accessor(vt, len(m["uvs"]), 5126, "VEC2")
        ai = add_accessor(vi, len(m["indices"]), 5123, "SCALAR")
        gltf_meshes.append({"name": m["name"], "primitives": [{
            "attributes": {"POSITION": ap, "NORMAL": an, "TEXCOORD_0": at}, "indices": ai, "mode": 4}]})
        nodes.append({"name": m["name"], "mesh": len(gltf_meshes) - 1})

    gltf = {
        "asset": {"version": "2.0", "generator": "vt3-port mesh_extract.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": gltf_meshes,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(buf)}],
    }
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    bin_data = bytes(buf) + b"\0" * ((4 - len(buf) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_data)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(js), b"JSON"))
        f.write(js)
        f.write(struct.pack("<I4s", len(bin_data), b"BIN\0"))
        f.write(bin_data)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive")
    ap.add_argument("entry", type=int)
    ap.add_argument("outdir", nargs="?", default=DEFAULT_OUT)
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args(argv)

    f, m = capx.open_archive(args.archive)
    try:
        entries = capx.read_table(m)
        if not 0 <= args.entry < len(entries):
            raise SystemExit("entry %d out of range" % args.entry)
        e = entries[args.entry]
        payload = bytes(m[e.offset : e.offset + e.size])
    finally:
        m.close()
        f.close()
    if payload[:4] != b"\x0d\x00\x00\x00":
        raise SystemExit("entry %d is not a 0x0D model table" % args.entry)

    stem = capx.archive_stem(args.archive)
    results = []
    meshes = []
    for name, off, size in read_model_table(payload):
        r = analyse_block(payload, off, size)
        if r is None:
            results.append({"name": name, "status": "no index list"})
            print("%-26s no index list (block %s)" % (name, hex(off)))
            continue
        if not r.get("ok"):
            results.append({"name": name, "status": "layout not found", "index_count": r["index_count"]})
            print("%-26s layout not found (indices %d)" % (name, r["index_count"]))
            continue
        mn, mx = bbox(r["positions"])
        results.append({k: v for k, v in r.items() if k not in ("positions", "normals", "uvs", "indices")} |
                       {"name": name, "status": "ok", "bbox_min": [round(x, 3) for x in mn],
                        "bbox_max": [round(x, 3) for x in mx]})
        print("%-26s stride=%2d normal@%2d verts=%4d tris=%4d unit=%.3f facing=%.3f bbox=%s..%s" % (
            name, r["stride"], r["normal_offset"], r["vertices"], r["triangles"], r["unit_fraction"],
            r["facing_fraction"], [round(x, 2) for x in mn], [round(x, 2) for x in mx]))
        meshes.append({"name": name, "positions": r["positions"], "normals": r["normals"],
                       "uvs": r["uvs"], "indices": r["indices"]})

    if args.report or not meshes:
        return 0
    os.makedirs(args.outdir, exist_ok=True)
    base = os.path.join(args.outdir, "%s_%04d" % (stem, args.entry))
    write_glb(base + ".glb", meshes)
    with open(base + ".json", "w") as f:
        json.dump({"archive": stem, "entry": args.entry, "blocks": results}, f, indent=1)
    print("wrote %s.glb (%d meshes)" % (base, len(meshes)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
