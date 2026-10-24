#!/usr/bin/env python3
"""export_obj.py ENTRY.bin OUTDIR: one OBJ per main VT4 player mesh (full detail)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from vt4mesh import vertex_buffers, index_buffers, decode, strip_to_tris, write_obj

d = open(sys.argv[1], 'rb').read()
os.makedirs(sys.argv[2], exist_ok=True)
vbs = {vb['name'][:-3]: vb for vb in vertex_buffers(d)}
ibs = list(index_buffers(d))
for mesh in ('Face', 'Body', 'Wear', 'Pants', 'Shoes', 'Accessory', 'Eyelash', 'Hair_fix'):
    vb = vbs[mesh]
    el = decode(d, vb)
    uv_i = [i for i, (t, c) in enumerate(vb['parts'][0][2]) if t == 2 and c == 2][0]
    pos, uv = el[0].astype(float), el[uv_i].astype(float) / 65536 + 0.5
    tris, base = [], 0
    mine = [ib for ib in ibs if ib['name'].split('-')[0] == mesh]
    for part, ib in zip(vb['parts'], mine):   # parts and index buffers are in the same order
        base = part[0] // vb['stride']
        tris += [(a + base, b + base, c + base) for a, b, c in strip_to_tris(ib['idx'])]
    write_obj(os.path.join(sys.argv[2], mesh + '.obj'), pos, uv, tris)
    print(mesh, len(pos), 'verts', len(tris), 'tris', len(mine), 'index buffers')
