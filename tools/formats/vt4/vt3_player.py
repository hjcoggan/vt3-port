#!/usr/bin/env python3
"""Decode the player model in game.xpac entry 746 (e746.bin) and export OBJ.

Block layout (details in REPORT.md):  [u16 triangle list][pad to 16][vertices].
The list ends at the first u16 >= 0x2000; there is no per-mesh header, and the
record size S is inferred from where the next list starts.  Record fields:

    44: pos@0 normal@12 w4@16 bone@32 uv@36   <- the documented body format
    48: pos@0 normal@12 tangent@16 w4@20 bone@36 uv@40
    40: pos@0 normal@12 tangent@16 w2@20 bone@28 uv@32
    28: pos@0 normal@12 tangent@16 uv@20      (no skinning)
    24: pos@0 normal@12 uv@16                 (no skinning)
    20: pos@0 normal@12 uv@16                 (not confirmed)
     8: two floats                            (2-D data, not exported)

normal/tangent are 4 bytes decoded as b/127.5-1.  Bone slot = register / 3.
"""
import os
import struct
import numpy as np

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "e746.bin")
OUTDIR, LMT = os.path.dirname(SRC), 0x2000
LAYOUT = {44: (12, 16, 4, 32, 36), 48: (12, 20, 4, 36, 40), 40: (12, 20, 2, 28, 32),
          28: (12, None, 0, None, 20), 24: (12, None, 0, None, 16),
          20: (12, None, 0, None, 16), 8: (None, None, 0, None, None)}
STRIDES = (44, 48, 40, 28, 24, 20, 8)


def first_big(d, p):
    while p + 2 <= len(d) and struct.unpack_from("<H", d, p)[0] < LMT:
        p += 2
    return p


def list_start(d, p, need=96):
    return p + 2 * need <= len(d) and bool((np.frombuffer(d[p:p + 2 * need], "<u2") < LMT).all())


def load(d, vs, n, s):
    """pos/nrm/w/bone/uv arrays for n records of stride s at vs (others omitted)."""
    buf = np.frombuffer(d[vs:vs + n * s], dtype=np.uint8).reshape(n, s)
    nrm_o, w_o, nw, b_o, uv_o = LAYOUT[s]
    f = {}
    if nrm_o is not None:
        f["pos"] = np.frombuffer(buf[:, :12].copy().tobytes(), "<f4").reshape(n, 3).astype(float)
        f["nrm"] = buf[:, 12:15].astype(float) / 127.5 - 1.0
    if w_o is not None:
        f["w"] = np.frombuffer(buf[:, w_o:w_o + 4 * nw].copy().tobytes(), "<f4").reshape(n, nw).astype(float)
    if b_o is not None:
        f["bone"] = buf[:, b_o:b_o + 4]
    if uv_o is not None and uv_o + 8 <= s:
        f["uv"] = np.frombuffer(buf[:, uv_o:uv_o + 8].copy().tobytes(), "<f4").reshape(n, 2).astype(float)
    return f


def score(d, vs, n, s):
    """0..1: how much the n records at vs of stride s look like vertices."""
    if vs + n * s > len(d):
        return 0.0
    f = load(d, vs, n, s)
    if "pos" not in f:
        u = f.get("uv", np.zeros((n, 2)))
        return float((np.isfinite(u).all(1) & (np.abs(u) <= 4).all(1)).mean())
    ok = np.isfinite(f["pos"]).all(1) & (np.abs(f["pos"]) <= 30).all(1)
    if "w" in f:
        ok &= np.isfinite(f["w"]).all(1) & (np.abs(f["w"].sum(1) - 1) <= 0.02)
    if "uv" in f:
        ok &= np.isfinite(f["uv"]).all(1) & (np.abs(f["uv"]) <= 4).all(1)
    return float(ok.mean())


def parse_blocks(d):
    blocks, p = [], 4
    while len(blocks) < 64:
        t, best = first_big(d, p), None
        for vs in ((t & ~15), (t & ~15) + 16):
            for e in range(max(p, vs - 16), vs + 1, 2):
                if d[e:vs] != b"\0" * (vs - e) or (e - p) % 2:
                    continue
                idx = np.frombuffer(d[p:e], "<u2")
                if len(idx) < 64 or len(idx) % 3:
                    continue
                n = int(idx.max()) + 1
                for s in STRIDES:
                    q = vs + n * s
                    if q <= len(d) and (list_start(d, q) or (q + 64 <= len(d) and not any(d[q:q + 64]))):
                        key = (score(d, vs, n, s), s == 44, -s)
                        best = (key, s, vs, idx.copy(), n, q) if best is None or key > best[0] else best
                break
        if best is None:
            break
        _, s, vs, idx, n, q = best
        blocks.append(dict(list_off=p, vstart=vs, stride=s, idx=idx, n=n, end=q))
        p = q
    return blocks


def faces(idx, strip):
    if not strip:
        return idx[:len(idx) // 3 * 3].astype(np.int64).reshape(-1, 3)
    return np.stack([idx[:-2].astype(np.int64), idx[1:-1].astype(np.int64), idx[2:].astype(np.int64)], 1)


def facing(t, pos, nrm):
    """Fraction of faces whose CCW normal agrees with the vertex normals."""
    if not len(t):
        return -1.0
    fn = np.cross(pos[t[:, 1]] - pos[t[:, 0]], pos[t[:, 2]] - pos[t[:, 0]])
    ln = np.linalg.norm(fn, axis=1)
    ok = ln > 1e-9
    if not ok.any():
        return -1.0
    vn = nrm[t[:, 0]] + nrm[t[:, 1]] + nrm[t[:, 2]]
    return float((((fn * vn).sum(1) / np.maximum(ln, 1e-12))[ok] > 0).mean())


def write_obj(name, pos, uv, t):
    with open(os.path.join(OUTDIR, name + ".obj"), "w") as f:
        f.write("# %s\n" % name)
        f.writelines("v %.6f %.6f %.6f\n" % tuple(p) for p in pos)
        if uv is not None:
            f.writelines("vt %.6f %.6f\n" % tuple(x) for x in uv)
        for a, b, c in t:
            f.write("f %d %d %d\n" % (a + 1, b + 1, c + 1) if uv is None else
                    "f %d/%d %d/%d %d/%d\n" % (a + 1, a + 1, b + 1, b + 1, c + 1, c + 1))
    return name + ".obj"


def main():
    d = open(SRC, "rb").read()
    blocks = parse_blocks(d)
    fmt = "%3d 0x%07x 0x%07x %6d %6d %7d %6s  %-35s %s"
    print("%3s %9s %9s %6s %6s %7s %6s  %-35s %s" %
          ("mesh", "list@", "verts@", "stride", "verts", "indices", "kind", "bbox", "file"))
    for k, b in enumerate(blocks):
        f = load(d, b["vstart"], b["n"], b["stride"])
        row = (k, b["list_off"], b["vstart"], b["stride"], b["n"], len(b["idx"]))
        if "pos" not in f:
            print(fmt % (*row, "2-D", "-", "skipped"))
            continue
        qi, qs = facing(faces(b["idx"], 0), f["pos"], f["nrm"]), facing(faces(b["idx"], 1), f["pos"], f["nrm"])
        strip = qs > qi + 0.05
        t = faces(b["idx"], strip)
        t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
        lo, hi = np.round(f["pos"].min(0), 2), np.round(f["pos"].max(0), 2)
        ok = np.isfinite(f["pos"]).all() and max(qi, qs) >= 0.9
        fn = write_obj("mesh_%d" % k, f["pos"], f.get("uv"), t) if ok else "skipped"
        print(fmt % (*row, "strip" if strip else "list", "%s .. %s" % (lo.tolist(), hi.tolist()), fn))
    print("model section 0x000000..0x%06x (%d blocks)" % (blocks[-1]["end"], len(blocks)))


if __name__ == "__main__":
    main()
