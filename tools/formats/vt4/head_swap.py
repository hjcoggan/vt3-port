#!/usr/bin/env python3
"""head_swap.py GAME_XPAC: put VT4 Djokovic's head (Face_lodL), hair (Hair_fix_lodL) and face, hair
and eye textures into VT3 player entry 746 (Ancic), in place. The payload keeps its size and the
textures keep their offsets; the mesh section is padded so it ends in the same 4 KB page."""
import io, mmap, os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))   # tools/formats (capx)
DUMP = os.environ.get('VT3_DUMP', os.path.join(HERE, '../../../../dump'))   # game data, outside the repo
import capx
from vt3_player import parse_blocks
from vt4mesh import vertex_buffers, index_buffers, batches, decode, strip_to_tris

VT4 = os.path.join(DUMP, 'vt4/entries/b7807.bin')
TEX4 = os.path.join(DUMP, 'vt4/textures/b7807')
HEAD = np.array([0.0, 16.034, 0.226])   # Head bone bind position (VT4 inverse bind)
# VT3 bone slot (register / 3) of each VT4 bone, from matching VT3 entry 746 mesh 0 to VT4 Shadow.
SLOT = {'Hip': 0, 'Spine': 1, 'Spine1': 2, 'Spine2': 3, 'Neck': 46, 'Head': 47,
        'RightShoulder': 4, 'LeftShoulder': 25}


def slot(name):
    return SLOT.get(name, 47)   # face bones (jaw, lips, brows ...) follow the head


def unorm8(v):
    return np.clip(np.round((np.asarray(v, float) + 1.0) * 127.5), 0, 255).astype(np.uint8)


def vt4_mesh(d, name):
    vb = [v for v in vertex_buffers(d) if v['name'] == name + '-VB'][0]
    els, kinds = decode(d, vb), vb['parts'][0][2]
    tris = []
    for part, ib in zip(vb['parts'], [i for i in index_buffers(d) if i['name'].split('-')[0] == name]):
        base = part[0] // vb['stride']
        tris += [(a + base, b + base, c + base) for a, b, c in strip_to_tris(ib['idx'])]
    return els, kinds, np.array(tris, np.int64)


def head_block(d):
    """VT3 stride 48: pos, normal u8x4, tangent u8x4, 4 float weights, 4 bone registers, uv."""
    els, kinds, tris = vt4_mesh(d, 'Face_lodL')
    pos, nrm, uv, tan, bidx, bw = (e.astype(np.float64) for e in els)
    nrm, tan, uv = nrm / 16384, tan / 16384, uv / 65536 + 0.5
    bones = [b for b in batches(d) if b['vb'] == 'Face_lodL-VB'][0]['bones']
    n = len(pos)
    rec = np.zeros(n, np.dtype([('p', '<f4', 3), ('n', 'u1', 4), ('t', 'u1', 4), ('w', '<f4', 4),
                                ('b', 'u1', 4), ('uv', '<f4', 2)]))
    rec['p'], rec['uv'] = pos, uv
    rec['n'][:, :3] = unorm8(nrm[:, :3])
    rec['t'][:, :3], rec['t'][:, 3] = unorm8(tan[:, :3]), np.where(tan[:, 3] < 0, 0, 255)
    for i in range(n):   # merge influences that land on the same VT3 bone
        acc = {}
        for k in range(4):
            if bw[i, k] > 0:
                s = slot(bones[int(bidx[i, k])])
                acc[s] = acc.get(s, 0.0) + bw[i, k] / 255.0
        items = sorted(acc.items(), key=lambda x: -x[1])[:4]
        tot = sum(w for _, w in items)
        for k, (s, w) in enumerate(items):
            rec['w'][i, k], rec['b'][i, k] = w / tot, s * 3
    return rec, tris


def hair_block(d):
    """VT3 stride 28 (rigid, head-local): pos, normal u8x4, tangent u8x4, uv."""
    els, kinds, tris = vt4_mesh(d, 'Hair_fix_lodL')
    pos, nrm, uv, tan = (e.astype(np.float64) for e in els[:4])
    nrm, tan, uv = nrm / 16384, tan / 16384, uv / 65536 + 0.5
    rec = np.zeros(len(pos), np.dtype([('p', '<f4', 3), ('n', 'u1', 4), ('t', 'u1', 4), ('uv', '<f4', 2)]))
    rec['p'], rec['uv'] = pos, uv   # Hair_fix_lodL is already head-local with world axes
    rec['n'][:, :3], rec['t'][:, :3] = unorm8(nrm[:, :3]), unorm8(tan[:, :3])
    rec['t'][:, 3] = np.where(tan[:, 3] < 0, 0, 255)
    return rec, tris


def first_vertex_ok(rec, tris):
    """Put a vertex whose first u16 is >= 0x2000 first, so the index list ends where the data begins."""
    raw = rec.view(np.uint8).reshape(len(rec), -1)
    j = int(np.argmax(raw[:, 0].astype(int) | (raw[:, 1].astype(int) << 8) >= 0x2000))
    order = np.arange(len(rec)); order[[0, j]] = order[[j, 0]]
    remap = np.empty_like(order); remap[order] = np.arange(len(order))
    return rec[order], remap[tris]


def block_bytes(rec, tris, start, pad_to=None):
    """[u16 triangle list][zeros to a 16-byte payload offset][vertices], starting at payload offset
    START; pad_to grows it with unused vertices + one
    degenerate triangle so the block is exactly that many bytes (or one stride short)."""
    rec, tris = first_vertex_ok(rec, tris)
    def build(extra):
        r = np.concatenate([rec, np.repeat(rec[:1], extra)]) if extra else rec
        t = np.concatenate([tris, [[len(r) - 1] * 3]]) if extra else tris
        idx = t.astype('<u2').tobytes()
        return idx + b'\0' * (-(start + len(idx)) % 16) + r.tobytes()
    out = build(0)
    if pad_to:
        extra = max(1, (pad_to - len(out) - 6) // rec.itemsize)
        out = build(extra)
        assert len(out) <= pad_to, (len(out), pad_to)
    return out


def dxt1_mips(png, levels):
    im, out = Image.open(png).convert('RGB'), b''
    for k in range(levels):
        w, h = max(1, im.width >> k), max(1, im.height >> k)
        buf = io.BytesIO()
        im.resize((w, h), Image.LANCZOS).save(buf, 'DDS', pixel_format='DXT1')
        out += buf.getvalue()[128:]
    return out


def main():
    d4 = open(VT4, 'rb').read()
    fh = open(sys.argv[1], 'r+b')
    m = mmap.mmap(fh.fileno(), 0)
    e = [x for x in capx.read_table(m) if x.index == 746][0]
    pay = bytearray(m[e.offset:e.offset + e.size])
    blocks = parse_blocks(bytes(pay))
    assert len(blocks) == 19 and blocks[5]['stride'] == 48 and blocks[18]['stride'] == 28
    old_end = blocks[-1]['end']
    out = bytearray(pay[:blocks[5]['list_off']])
    hr, ht = head_block(d4)
    out += block_bytes(hr, ht, len(out))
    out += pay[blocks[6]['list_off']:blocks[18]['list_off']]   # blocks 6..17 move as they are
    rr, rt = hair_block(d4)
    out += block_bytes(rr, rt, len(out), pad_to=old_end - len(out))
    assert old_end - 4096 < len(out) <= old_end, (hex(len(out)), hex(old_end))
    pay[:len(out)] = out
    pay[len(out):old_end] = bytes(old_end - len(out))
    replace_textures(pay)
    assert len(pay) == e.size
    m[e.offset:e.offset + e.size] = bytes(pay)
    m.flush()
    nb = parse_blocks(bytes(pay))
    print('blocks after:', len(nb), 'head verts', nb[5]['n'], 'hair verts', nb[18]['n'],
          'mesh end', hex(nb[-1]['end']), 'was', hex(old_end))


def replace_textures(pay):
    """Face and eye re-encoded as DXT1, hair copied (same DXT5 size and mips); in place."""
    for slot, name, how in ((8, 'P_DJV_B_face_IT_dds', 'dxt1'), (19, 'P_DJV_B_eye_IT_dds', 'dxt1'),
                            (22, 'P_DJV_B_hair_IT_dds', 'copy')):
        hits = [i for i in range(0x8E000, len(pay), 4096) if pay[i:i + 4] == b'DDS ']
        o = hits[slot]
        h, w = struct.unpack_from('<2I', pay, o + 12)
        levels = max(1, struct.unpack_from('<I', pay, o + 28)[0])
        src = os.path.join(TEX4, name)
        data = dxt1_mips(src + '.png', levels) if how == 'dxt1' else open(src + '.dds', 'rb').read()[128:]
        size = (o + 128)
        end = hits[slot + 1] if slot + 1 < len(hits) else len(pay)
        assert len(data) <= end - size, (name, len(data), end - size)
        pay[size:size + len(data)] = data
        print('texture', slot, name, w, h, levels, how, len(data))



def block_exact(rec, tris, n, nidx):
    """Same layout as block_bytes, padded to exactly n vertices and nidx indices (unused copies of
    vertex 0 and degenerate triangles on the last one), so the block keeps its size and position
    and the model descriptor (entry 247) stays valid."""
    rec, tris = first_vertex_ok(rec, tris)
    assert len(rec) <= n and 3 * len(tris) <= nidx and nidx % 3 == 0, (len(rec), n, len(tris), nidx)
    rec = np.concatenate([rec, np.repeat(rec[:1], n - len(rec))])
    tris = np.concatenate([tris, np.full((nidx // 3 - len(tris), 3), n - 1)])
    return tris.astype('<u2').tobytes(), rec.tobytes()


def main_exact():
    """Replace blocks 5 (head) and 18 (hair) in place, block sizes unchanged."""
    d4 = open(VT4, 'rb').read()
    fh = open(sys.argv[1], 'r+b')
    m = mmap.mmap(fh.fileno(), 0)
    e = [x for x in capx.read_table(m) if x.index == 746][0]
    pay = bytearray(m[e.offset:e.offset + e.size])
    blocks = parse_blocks(bytes(pay))
    for k, (rec, tris) in ((5, head_block(d4)), (18, hair_block(d4))):
        b = blocks[k]
        idx, verts = block_exact(rec, tris, b['n'], len(b['idx']))
        assert len(verts) == b['n'] * b['stride']
        pay[b['list_off']:b['list_off'] + len(idx)] = idx
        pay[b['vstart']:b['vstart'] + len(verts)] = verts
    replace_textures(pay)
    assert len(pay) == e.size
    m[e.offset:e.offset + e.size] = bytes(pay)
    m.flush()
    nb = parse_blocks(bytes(pay))
    assert [(b['list_off'], b['vstart'], b['n'], len(b['idx'])) for b in nb] == \
           [(b['list_off'], b['vstart'], b['n'], len(b['idx'])) for b in blocks]
    print('blocks unchanged in place; head and hair replaced')


if __name__ == '__main__':
    main_exact()
