#!/usr/bin/env python3
"""Decode the vertex buffers of a VT4 SDO model file (big-endian, ColladaToSdo).

VertexBuffer record: "\x0cVertexBuffer\0", len-prefixed node path, len-prefixed name, then
  u8 flags, BE u32 vertex count, BE u32 bytes, u8 parts, and per part:
  BE u32 offset, BE u32 bytes, u8 element count, element count x (u8 type, u8 comps).
Types: 4 = float32, 2 = int16 (normals and tangents /16384, uv /65536 + 0.5), 1 = u8.
Vertex data follows the last part header.
"""
import re, struct, sys
import numpy as np

SIZE = {4: 4, 2: 2, 1: 1}
DT = {4: '>f4', 2: '>i2', 1: 'u1'}


def lp(d, p):
    n = d[p]
    return d[p + 1:p + 1 + n].decode('latin1'), p + 1 + n


def vertex_buffers(d):
    for m in re.finditer(rb'\x0cVertexBuffer\x00', d):
        path, p = lp(d, m.end())
        name, p = lp(d, p)
        flags, count, nbytes, parts = d[p], *struct.unpack_from('>II', d, p + 1), d[p + 9]
        p += 10
        heads = []
        for _ in range(parts):
            off, pb = struct.unpack_from('>II', d, p)
            ne = d[p + 8]
            els = [(d[p + 9 + 2 * i], d[p + 10 + 2 * i]) for i in range(ne)]
            heads.append((off, pb, els))
            p += 9 + 2 * ne
        stride = sum(SIZE[t] * c for t, c in heads[0][2])
        yield dict(path=path, name=name, flags=flags, count=count, bytes=nbytes, parts=heads,
                   stride=stride, data=p)


def decode(d, vb):
    """Per-element arrays for the whole buffer (all parts share one layout)."""
    els = vb['parts'][0][2]
    fields = [('e%d' % i, DT[t], (c,)) for i, (t, c) in enumerate(els)]
    arr = np.frombuffer(d, dtype=np.dtype(fields), count=vb['count'], offset=vb['data'])
    return [arr['e%d' % i] for i in range(len(els))]


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    for vb in vertex_buffers(d):
        ok = vb['stride'] * vb['count'] == vb['bytes']
        pos = decode(d, vb)[0].astype(np.float64)
        print('%-22s n=%5d stride %2d %s parts %d  bbox %s..%s' % (
            vb['name'], vb['count'], vb['stride'], 'ok' if ok else 'SIZE MISMATCH', len(vb['parts']),
            np.round(pos.min(0), 2), np.round(pos.max(0), 2)))


def index_buffers(d):
    """IndexBuffer record: tag, path, name '<mesh>-<material>-<part>-IB', u8 flags, BE u32 count,
    then count BE u16 indices (triangle strips, relative to the vertex buffer part)."""
    for m in re.finditer(rb'\x0bIndexBuffer\x00', d):
        path, p = lp(d, m.end())
        name, p = lp(d, p)
        count, = struct.unpack_from('>I', d, p + 1)
        yield dict(path=path, name=name, idx=np.frombuffer(d, '>u2', count, p + 5).astype(np.int64))


def strip_to_tris(s):
    tris = []
    for i in range(len(s) - 2):
        a, b, c = s[i], s[i + 1], s[i + 2]
        if a == b or b == c or a == c:
            continue
        tris.append((a, b, c) if i % 2 == 0 else (b, a, c))
    return tris


def write_obj(path, pos, uv, tris):
    with open(path, 'w') as f:
        for p in pos: f.write('v %.5f %.5f %.5f\n' % tuple(p))
        for t in uv: f.write('vt %.5f %.5f\n' % (t[0], 1.0 - t[1]))
        for a, b, c in tris: f.write('f %d/%d %d/%d %d/%d\n' % (a + 1, a + 1, b + 1, b + 1, c + 1, c + 1))


def lp16(d, p):
    n, = struct.unpack_from('>H', d, p)
    return d[p + 2:p + 2 + n].decode('latin1'), p + 2 + n


def batches(d):
    """Batch record: tag, a u8-length batch path ('<node path>/<mesh>-<material>-<n>'), more
    header strings, the NUL-terminated '...-VB' and '...-IB' paths, counts not yet decoded,
    BE u16 bone count, the bone paths (u16 lengths), BE u16 count again, and one 3x4 BE float
    matrix per bone (inverse bind pose, rows x, y, z). Rigid batches (hair, teeth, eyes) have no
    bone list and are skipped."""
    for m in re.finditer(rb'\x05Batch', d):
        p = m.end() + 1   # the tag ends with a NUL
        if d[p + 1:p + 12] != b'Scene_Root/':   # the word inside other data
            continue
        path, _ = lp(d, p)
        ib_end = d.find(b'-IB\x00', p) + 4
        vb_end = d.rfind(b'-VB\x00', p, ib_end) + 3
        vb = d[d.rfind(b'/', p, vb_end) + 1:vb_end].decode('latin1')
        q = d.find(b'Scene_Root/Reference/Hip', ib_end, ib_end + 64) - 2   # first bone path's length
        if q < 0:
            continue
        nb, = struct.unpack_from('>H', d, q - 2)
        bones, p = [], q
        for _ in range(nb):
            s, p = lp16(d, p)
            bones.append(s.split('/')[-1])
        n2, = struct.unpack_from('>H', d, p)
        assert n2 == nb, (path, nb, n2)
        inv = np.frombuffer(d, '>f4', nb * 12, p + 2).reshape(nb, 3, 4).astype(np.float64)
        yield dict(name=path.split('/')[-1], vb=vb, bones=bones, inv=inv)
