"""D3D9 shader bytecode (vs_1_1 .. vs_3_0, ps_1_1 .. ps_3_0): finder, walker and basic disassembler.

Token format (little-endian u32, as stored in D3D9 compiled shaders):

    version token   0xFFFE0xyy (vertex) or 0xFFFF0xyy (pixel), yy = major, minor bytes
                    (0xFFFE0300 is vs_3_0, 0xFFFF0200 is ps_2_0, 0xFFFF0104 is ps_1_4)
    instruction     low 16 bits = opcode; bits 24..27 = operand token count (SM2+)
    comment         opcode 0xFFFE; bits 16..30 = length in tokens (CTAB, DCL data, etc.)
    operand         bit 31 set. Register number bits 0..10, register type in bits 28..30
                    (high bits) and 11..12 (low bits). Source operands: swizzle bits 16..23,
                    source modifier bits 24..27. Destination: write mask bits 16..19,
                    result modifier bits 20..23, shift bits 24..27.
    end token       0x0000FFFF

The walker checks: every opcode is in the known set; in SM2+, operand tokens have bit 31
set and the next instruction token does not; in SM1, operands are the tokens with bit 31
set until the next opcode token; the stream ends at 0x0000FFFF.

Sources: Microsoft D3D9 SDK shader-bytecode documentation (d3dx9shader.h / d3d9types.h
constant names), reproduced from memory and checked against the game's own shaders.
"""
import struct

END = 0x0000FFFF
COMMENT = 0xFFFE

# opcode -> (name, has_dst)
OPCODES = {
    0: ("nop", False), 1: ("mov", True), 2: ("add", True), 3: ("sub", True), 4: ("mad", True),
    5: ("mul", True), 6: ("rcp", True), 7: ("rsq", True), 8: ("dp3", True), 9: ("dp4", True),
    10: ("min", True), 11: ("max", True), 12: ("slt", True), 13: ("sge", True), 14: ("exp", True),
    15: ("log", True), 16: ("lit", True), 17: ("dst", True), 18: ("lrp", True), 19: ("frc", True),
    20: ("m4x4", True), 21: ("m4x3", True), 22: ("m3x4", True), 23: ("m3x3", True), 24: ("m3x2", True),
    25: ("call", False), 26: ("callnz", False), 27: ("loop", False), 28: ("ret", False),
    29: ("endloop", False), 30: ("label", False), 31: ("dcl", False), 32: ("pow", True),
    33: ("crs", True), 34: ("sgn", True), 35: ("abs", True), 36: ("nrm", True), 37: ("sincos", True),
    38: ("rep", False), 39: ("endrep", False), 40: ("if", False), 41: ("ifc", False), 42: ("else", False),
    43: ("endif", False), 44: ("break", False), 45: ("breakc", False), 46: ("mova", True),
    47: ("defb", True), 48: ("defi", True),
    # pixel shader 1.x / 1.4 / 2.x / 3.x texture ops
    64: ("texcoord", True), 65: ("texkill", True), 66: ("tex", True), 67: ("texbem", True),
    68: ("texbeml", True), 69: ("texreg2ar", True), 70: ("texreg2gb", True), 71: ("texm3x2pad", True),
    72: ("texm3x2tex", True), 73: ("texm3x3pad", True), 74: ("texm3x3tex", True), 76: ("texm3x3spec", True),
    77: ("texm3x3vspec", True), 78: ("expp", True), 79: ("logp", True), 80: ("cnd", True),
    81: ("def", True), 82: ("texreg2rgb", True), 83: ("texdp3tex", True), 84: ("texm3x2depth", True),
    85: ("texdp3", True), 86: ("texm3x3", True), 87: ("texdepth", True), 88: ("cmp", True),
    89: ("bem", True), 90: ("dp2add", True), 91: ("dsx", True), 92: ("dsy", True),
    93: ("texldd", True), 94: ("setp", True), 95: ("texldl", True), 96: ("breakp", False),
}
# Opcode 66 is "tex" in ps 1.x and "texld" in ps 2.x/3.x; the name is kept as "tex" here.

REG_NAMES = {0: "r", 1: "v", 2: "c", 4: "rast", 5: "oD", 6: "oT", 8: "oC", 9: "oDepth",
             10: "s", 11: "c", 12: "c", 13: "c", 14: "b", 15: "aL", 16: "r", 17: "misc",
             18: "l", 19: "p", 7: "i"}
RAST = {0: "oPos", 1: "oFog", 2: "oPts"}
USAGE = ["position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent",
         "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample"]
SWZ = "xyzw"
SRCMOD = {0: "", 1: "-", 2: "bias(", 3: "-bias(", 4: "bx2(", 5: "-bx2(", 6: "", 7: "x2(", 8: "-x2(",
          9: "dz(", 10: "dw(", 11: "abs(", 12: "-abs(", 13: "!"}


def version(tok):
    """Return (kind, major, minor) for a version token, or None."""
    hi = tok >> 16
    if hi == 0xFFFE:
        kind = "vs"
    elif hi == 0xFFFF:
        kind = "ps"
    else:
        return None
    return kind, (tok >> 8) & 0xFF, tok & 0xFF


# Shader model versions that D3D9 can produce. Anything else is treated as data.
ALLOWED = {
    ("vs", 1, 0), ("vs", 1, 1), ("vs", 2, 0), ("vs", 2, 1), ("vs", 3, 0),
    ("ps", 1, 0), ("ps", 1, 1), ("ps", 1, 2), ("ps", 1, 3), ("ps", 1, 4),
    ("ps", 2, 0), ("ps", 2, 1), ("ps", 3, 0),
}
# Opcodes legal in SM1 (vs_1_x and ps_1_x): arithmetic 1..24, texture ops 64..90 for ps.
SM1_OPCODES = set(range(1, 25)) | set(range(64, 91)) | {0, 28, 81}


def _check_version(major, minor, kind):
    return (kind, major, minor) in ALLOWED


def walk(data, start, max_tokens=8192):
    """Walk a shader that starts at data[start] (the version token).

    Returns a dict with kind, major, minor, size, tokens, instructions (list of
    (opcode, token_index, operand_tokens)), and opcode histogram. Returns None if
    the bytes are not a valid shader stream.
    """
    if start + 8 > len(data):
        return None
    v = version(struct.unpack_from("<I", data, start)[0])
    if v is None:
        return None
    kind, major, minor = v
    if not _check_version(major, minor, kind):
        return None
    sm1 = major == 1
    p = start + 4
    tokens = 1
    instructions = []
    hist = {}
    comments = []
    while True:
        if p + 4 > len(data) or tokens > max_tokens:
            return None
        t = struct.unpack_from("<I", data, p)[0]
        p += 4
        tokens += 1
        if t == END:
            break
        if t & 0x80000000:
            return None  # operand without an opcode
        op = t & 0xFFFF
        if op == COMMENT:
            ln = (t >> 16) & 0x7FFF
            comments.append((p - 4 - start, bytes(data[p : p + 4 * ln])))
            p += 4 * ln
            tokens += ln
            continue
        if op not in OPCODES or op == 0:
            return None  # opcode 0 (nop) is what zero bytes decode to; the compiler does not emit it
        if sm1 and op not in SM1_OPCODES:
            return None
        if sm1:
            ops = []
            while p + 4 <= len(data):
                q = struct.unpack_from("<I", data, p)[0]
                if not (q & 0x80000000):
                    break
                ops.append(q)
                p += 4
                tokens += 1
                if tokens > max_tokens:
                    return None
        else:
            ln = (t >> 24) & 0xF
            ops = []
            for k in range(ln):
                if p + 4 > len(data):
                    return None
                q = struct.unpack_from("<I", data, p)[0]
                # def/defi/defb carry raw constant values after the destination register;
                # those values need not have bit 31 set.
                raw_ok = op in (47, 48, 81) and k >= 1
                if not raw_ok and not (q & 0x80000000):
                    return None
                ops.append(q)
                p += 4
                tokens += 1
            # the next token must be an opcode or the end token
            if p + 4 <= len(data):
                nxt = struct.unpack_from("<I", data, p)[0]
                if nxt & 0x80000000:
                    return None
        name = opcode_name(op, kind, major)
        hist[name] = hist.get(name, 0) + 1
        instructions.append((op, t, ops))
    if not instructions:
        return None  # a shader with no instructions is not a real blob
    return {
        "kind": kind, "major": major, "minor": minor,
        "start": start, "end": p, "size": p - start, "tokens": tokens,
        "instructions": instructions, "histogram": hist, "comments": comments,
    }


def parse_ctab(payload):
    """Parse a 'CTAB' comment payload (the bytes after the comment token).

    Returns dict(creator, target, constants=[(name, regset, regindex, regcount)]) or None.
    Offsets inside the CTAB are relative to the start of the constant table struct,
    which begins right after the 4-byte 'CTAB' fourcc.
    """
    if payload[:4] != b"CTAB":
        return None
    base = 4
    size, creator, version, count, info, flags, target = struct.unpack_from("<7I", payload, base)

    def cstr(off):
        a = base + off
        if a >= len(payload):
            return ""
        end = payload.find(b"\0", a)
        return payload[a : end if end >= 0 else len(payload)].decode("latin-1")

    consts = []
    for k in range(count):
        q = base + info + 20 * k
        name_off, regset, regindex, regcount, _res, _ty, _dv = struct.unpack_from("<IHHHHII", payload, q)
        consts.append((cstr(name_off), regset, regindex, regcount))
    return {"creator": cstr(creator), "version": version, "target": cstr(target), "constants": consts}


def opcode_name(op, kind, major):
    """Opcode 66 is 'tex' in ps 1.x and 'texld' in ps 2.x and later."""
    if op == 66 and kind == "ps" and major >= 2:
        return "texld"
    return OPCODES[op][0]


def _reg(tok, kind, major):
    rtype = ((tok >> 28) & 0x7) | ((tok >> 8) & 0x18)
    num = tok & 0x7FF
    if rtype == 4:
        return RAST.get(num, "rast%d" % num)
    if rtype == 3:
        return ("t%d" if (kind == "ps" and major < 3) else "a%d") % num
    if rtype == 17:
        return ["vPos", "vFace"][num] if num < 2 else "misc%d" % num
    if rtype == 7 and major >= 3:
        return "o%d" % num
    if rtype == 6:
        return "oT%d" % num
    if rtype == 5:
        return "oD%d" % num
    prefix = REG_NAMES.get(rtype, "r%d_" % rtype)
    return "%s%d" % (prefix, num)


def _dst_text(tok, kind, major):
    name = _reg(tok, kind, major)
    mask = (tok >> 16) & 0xF
    if mask in (0, 0xF):
        return name
    return name + "." + "".join(SWZ[i] for i in range(4) if mask & (1 << i))


def _src_text(tok, kind, major, rel=None):
    name = _reg(tok, kind, major)
    if rel is not None:
        name += "[%s]" % rel
    swz = (tok >> 16) & 0xFF
    if swz != 0xE4:
        comps = "".join(SWZ[(swz >> (2 * i)) & 3] for i in range(4))
        if len(set(comps)) == 1:
            name += "." + comps[0]
        else:
            name += "." + comps
    mod = (tok >> 24) & 0xF
    pre = SRCMOD.get(mod, "")
    post = ")" if mod in (2, 3, 4, 5, 7, 8, 9, 10, 11, 12) else ""
    return pre + name + post


def disassemble(shader):
    """Return a list of text lines for a walk() result."""
    kind, major = shader["kind"], shader["major"]
    lines = ["%s_%d_%d" % (kind, major, shader["minor"])]
    for op, t, ops in shader["instructions"]:
        name = opcode_name(op, kind, major)
        if op == 31 and len(ops) >= 2:
            # SM2/3 dcl: operand 0 = usage token (usage bits 0..4, index bits 16..19,
            # texture type bits 27..30 for samplers), operand 1 = the register.
            utok, rtok = ops[0], ops[1]
            reg_text = _dst_text(rtok, kind, major)
            if ((rtok >> 28) & 7) | ((rtok >> 8) & 0x18) == 10:  # sampler
                tex = {2: "2d", 3: "cube", 4: "volume"}.get((utok >> 27) & 0xF, "?")
                lines.append("dcl_%s %s" % (tex, reg_text))
            elif kind == "ps" and major < 3:
                # ps 1.x/2.x: the usage token is ignored; the register alone is declared
                lines.append("dcl %s" % reg_text)
            else:
                usage = utok & 0x1F
                idx = (utok >> 16) & 0xF
                usage_name = USAGE[usage] if usage < len(USAGE) else str(usage)
                lines.append("dcl_%s%s %s" % (usage_name, idx if idx else "", reg_text))
            continue
        if op in (25, 26, 27, 28, 29, 30, 38, 39, 42, 43, 44, 96) and not ops:
            lines.append(name)
            continue
        if op in (47, 48, 81) and ops:  # defb / defi / def: destination, then raw constant values
            dst = _dst_text(ops[0], kind, major)
            if op == 81:
                vals = [struct.unpack("<f", struct.pack("<I", q))[0] for q in ops[1:]]
                lines.append("%s %s, %s" % (name, dst, ", ".join("%g" % v for v in vals)))
            elif op == 48:
                vals = [struct.unpack("<i", struct.pack("<I", q))[0] for q in ops[1:]]
                lines.append("%s %s, %s" % (name, dst, ", ".join(str(v) for v in vals)))
            else:
                vals = ["true" if q else "false" for q in ops[1:]]
                lines.append("%s %s, %s" % (name, dst, ", ".join(vals)))
            continue
        texts = []
        dst_done = not OPCODES[op][1]
        for q in ops:
            if not dst_done:
                texts.append(_dst_text(q, kind, major))
                dst_done = True
                continue
            texts.append(_src_text(q, kind, major))
        # SM2+ modifier bits on destination
        lines.append("%s %s" % (name, ", ".join(texts)))
    return lines


def find_candidates(data):
    """Yield offsets where a plausible version token starts (cheap pre-filter)."""
    import re
    rx = re.compile(rb"[\x00-\x05][\x00-\x03]\xfe\xff|[\x00-\x05][\x00-\x03]\xff\xff")
    for m in rx.finditer(data):
        yield m.start()


def scan(data):
    """Yield every valid shader found in data as a walk() result (non-overlapping by start)."""
    last_end = -1
    for i in find_candidates(data):
        if i < last_end:
            continue
        r = walk(data, i)
        if r is None:
            continue
        r["start"] = i
        last_end = r["end"]
        yield r
