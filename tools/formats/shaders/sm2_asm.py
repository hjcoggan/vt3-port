#!/usr/bin/env python3
"""A small SM2 assembler for the vs_2_0 and ps_2_0 shaders the runtime rebuilds.

Input is a listing: a version line (vs_2_0 or ps_2_0), then one instruction per line.
Supported: mov add sub mul mad dp3 dp4 rcp rsq, the `_sat` result modifier, `def`, and
`dcl_position`, `dcl_normal`, `dcl_texcoordN` (declarations of inputs). Registers: r# (temp),
v# (vertex input), c# (constant), t# (pixel texture coordinate input), oPos, oTN (vertex
texcoord output), oC0 (pixel colour output). Source operands may negate with a leading `-`
and take a swizzle (`.wzyx`, or one letter for a replicate). Destinations take a write mask.

Output: a list of uint32 tokens (version, optional CTAB comment, instructions, end token), which
the runtime loads like any compiled shader. The CTAB comment carries the constant names, so
ID3DXConstantTable can find them, as in shaders from D3DX.

Run the file directly to round-trip a listing through the disassembler in d3d9_sm.py.
"""
import re
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d9_sm  # noqa: E402

VERSIONS = {"vs_2_0": 0xFFFE0200, "ps_2_0": 0xFFFF0200}
OPS = {"mov": 1, "add": 2, "sub": 3, "mad": 4, "mul": 5, "rcp": 6, "rsq": 7, "dp3": 8, "dp4": 9}
RT = {"r": 0, "v": 1, "c": 2, "t": 3, "oPos": 4, "oD": 5, "oT": 6, "oC": 8}
DCL_USAGE = {"dcl_position": (0, 0), "dcl_normal": (3, 0)}
SAT = 1
MASK_BITS = {"x": 1, "y": 2, "z": 4, "w": 8}
COMP = {"x": 0, "y": 1, "z": 2, "w": 3}
END = 0x0000FFFF


def reg_token(name, index):
    """The operand token's register part: type in bits 28..30 and 11..12, number in 0..10."""
    if name not in RT:
        raise ValueError("unknown register %s" % name)
    t = RT[name]
    return 0x80000000 | (index & 0x7FF) | ((t & 7) << 28) | (((t >> 3) & 3) << 11)


def parse_reg(text):
    """'-r0.xyz' -> (negate, name, index, letters)."""
    neg = text.startswith("-")
    if neg:
        text = text[1:]
    letters = ""
    if "." in text:
        text, letters = text.split(".", 1)
    m = re.fullmatch(r"(oPos|oC|oT|oD|r|v|c|t)(\d*)", text)
    if not m or (m.group(2) == "" and m.group(1) != "oPos"):
        raise ValueError("bad operand %r" % text)
    return neg, m.group(1), int(m.group(2) or 0), letters


def src_swizzle(letters):
    if letters == "":
        letters = "xyzw"
    if len(letters) == 1:
        letters = letters * 4
    if len(letters) != 4 or any(c not in COMP for c in letters):
        raise ValueError("bad swizzle %r" % letters)
    return sum(COMP[c] << (2 * i) for i, c in enumerate(letters))


def dst_mask(letters):
    if letters == "":
        return 0xF
    m = 0
    for c in letters:
        if c not in MASK_BITS:
            raise ValueError("bad write mask %r" % letters)
        m |= MASK_BITS[c]
    return m


def emit_dst(text, sat):
    neg, name, idx, letters = parse_reg(text)
    if neg:
        raise ValueError("a destination cannot be negated: %s" % text)
    return reg_token(name, idx) | (dst_mask(letters) << 16) | ((SAT if sat else 0) << 20)


def emit_src(text):
    neg, name, idx, letters = parse_reg(text)
    return reg_token(name, idx) | (src_swizzle(letters) << 16) | ((1 if neg else 0) << 24)


def assemble(listing, constants=(), creator="vt3 rebuilt"):
    """listing: text. constants: (name, regset, regindex, regcount) for the CTAB comment.

    regset: 2 = float4, 3 = sampler (as in D3DXPARAMETER_REGISTER_SET). Returns (tokens, kind)."""
    lines = [ln.split("//")[0].strip() for ln in listing.splitlines()]
    lines = [ln for ln in lines if ln]
    if not lines or lines[0] not in VERSIONS:
        raise ValueError("first line must be vs_2_0 or ps_2_0")
    version = lines[0]
    kind = version[:2]
    tokens = [VERSIONS[version]]
    if constants:
        tokens.extend(ctab_comment(version, constants, creator))
    for ln in lines[1:]:
        head, _, rest = ln.partition(" ")
        args = [a.strip() for a in rest.split(",")] if rest else []
        if head in ("dcl_position", "dcl_normal") or head.startswith("dcl_texcoord"):
            if head.startswith("dcl_texcoord"):
                usage, index = 5, int(head[len("dcl_texcoord"):] or "0")
            else:
                usage, index = DCL_USAGE[head]
            tokens.append(d3d9_sm_op(31, 2))
            tokens.append(0x80000000 | usage | (index << 16))
            tokens.append(emit_dst(args[0], False))
            continue
        if head == "def":
            reg, values = args[0], [float(v) for v in args[1:]]
            if len(values) != 4:
                raise ValueError("def takes four values")
            tokens.append(d3d9_sm_op(81, 5))
            tokens.append(emit_dst(reg, False))
            tokens.extend(struct.unpack("<I", struct.pack("<f", v))[0] for v in values)
            continue
        sat = False
        if head.endswith("_sat"):
            sat = True
            head = head[:-4]
        if head not in OPS:
            raise ValueError("unsupported instruction %r" % head)
        dst = emit_dst(args[0], sat)
        srcs = [emit_src(a) for a in args[1:]]
        tokens.append(d3d9_sm_op(OPS[head], 1 + len(srcs)))
        tokens.append(dst)
        tokens.extend(srcs)
    tokens.append(END)
    return tokens, kind


def d3d9_sm_op(opcode, operand_count):
    """Instruction token: opcode in 0..15, operand token count in bits 24..27."""
    return opcode | (operand_count << 24)


def ctab_comment(version, constants, creator):
    """The 'CTAB' comment token and payload for the constants (names, register sets and indices)."""
    header_size = 28
    info_size = 20
    n = len(constants)
    strings = bytearray()

    def add_string(s):
        off = header_size + info_size * n + len(strings)
        strings.extend(s.encode("ascii") + b"\0")
        return off

    creator_off = add_string(creator)
    target_off = add_string(version)
    name_offs = [add_string(c[0]) for c in constants]
    body = bytearray()
    body += struct.pack("<7I", header_size + info_size * n + len(strings), creator_off,
                        VERSIONS[version] & 0xFFFF, n, header_size, 0, target_off)
    for (name, regset, regindex, regcount), name_off in zip(constants, name_offs):
        body += struct.pack("<IHHHHII", name_off, regset, regindex, regcount, 0, 0, 0)
    body += strings
    payload = b"CTAB" + bytes(body)
    while len(payload) % 4:
        payload += b"\0"
    words = len(payload) // 4
    if words > 0x7FFF:
        raise ValueError("CTAB too large")
    tokens = [0xFFFE | (words << 16)]
    tokens.extend(struct.unpack("<%dI" % words, payload))
    return tokens


def to_bytes(tokens):
    return struct.pack("<%dI" % len(tokens), *tokens)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    text = open(argv[1], encoding="utf-8").read()
    tokens, kind = assemble(text)
    data = to_bytes(tokens)
    walked = d3d9_sm.walk(data, 0)
    if walked is None:
        print("the tokens do not walk as a shader")
        return 1
    for line in d3d9_sm.disassemble(walked):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
