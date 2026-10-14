#!/usr/bin/env python3
"""Static recompiler prototype: x86-32 PE32 (i386) to portable C.

Usage:
    recomp.py IMAGE.exe --out DIR [--name NAME] [--allow-unsupported] [-v]

Writes into DIR:
    NAME.c      translated functions and the module tables (vt3_mod)
    image.bin   flattened guest image, byte 0 at ImageBase, SizeOfImage bytes

Nothing here embeds game bytes into C. Data lives in image.bin. Generated
output belongs under dump/, never in the repo. See docs/recomp-design.md.

Model in brief:
  * Functions are found by recursive descent from the entry point, exports,
    direct call targets, and code addresses named by base relocations.
  * A guest function is one C function taking a vt3_cpu*. The GPRs are C
    locals, spilled to the cpu struct around calls and returns.
  * The guest stack is real. call pushes the guest return address, and ret
    pops it. Stdcall and cdecl need no special casing in the translator.
  * Flags are eager C locals. The native C optimiser deletes any flag that
    no later code reads.
  * A jump inside a function is a goto. A jump to another function entry is a
    tail call. Indirect calls and jumps go through vt_icall, a lookup keyed by
    guest address.
"""

import argparse
import os
import re
import struct
import sys
from collections import Counter

import capstone
import pefile
from capstone import x86 as CX


M32 = 0xFFFFFFFF
IMAGE_SCN_MEM_EXECUTE = 0x20000000
PREFIX_WORDS = {'rep', 'repe', 'repz', 'repne', 'repnz', 'lock'}
GPR32 = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx',
         'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
REG8 = {'al': ('eax', 0), 'cl': ('ecx', 0), 'dl': ('edx', 0), 'bl': ('ebx', 0),
        'ah': ('eax', 8), 'ch': ('ecx', 8), 'dh': ('edx', 8), 'bh': ('ebx', 8)}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
# Condition suffix -> C expression over the flag locals.
CC = {
    'o': 'of', 'no': '!of', 'b': 'cf', 'c': 'cf', 'nae': 'cf',
    'ae': '!cf', 'nb': '!cf', 'nc': '!cf', 'e': 'zf', 'z': 'zf',
    'ne': '!zf', 'nz': '!zf', 'be': '(cf || zf)', 'na': '(cf || zf)',
    'a': '!(cf || zf)', 'nbe': '!(cf || zf)', 's': 'sf', 'ns': '!sf',
    'p': 'pf', 'pe': 'pf', 'np': '!pf', 'po': '!pf',
    'l': '(sf != of)', 'nge': '(sf != of)', 'ge': '(sf == of)', 'nl': '(sf == of)',
    'le': '(zf || sf != of)', 'ng': '(zf || sf != of)',
    'g': '(!zf && sf == of)', 'nle': '(!zf && sf == of)',
}
LOOPS = {'loop', 'loope', 'loopz', 'loopne', 'loopnz'}
TERMINAL = {'hlt', 'ud2', 'int3', 'int'}
RET_MN = {'ret', 'retf', 'iret', 'iretd'}
# Imports that never return. Calls to them end the path during analysis.
NORETURN_IMPORTS = {'ExitProcess', 'ExitThread', 'TerminateProcess', 'abort', '_exit', 'exit'}


class TranslateError(Exception):
    pass


class DecodeError(Exception):
    def __init__(self, addr, why):
        super().__init__(f"{addr:#x}: {why}")
        self.addr = addr


class Unsupported(Exception):
    pass


# ----------------------------------------------------------------------------
# Image and decoding
# ----------------------------------------------------------------------------

class Op:
    __slots__ = ('kind', 'reg', 'imm', 'size', 'base', 'index', 'scale', 'disp', 'seg')

    def __init__(self, kind, reg=None, imm=0, size=0, base=None, index=None,
                 scale=1, disp=0, seg=None):
        self.kind = kind
        self.reg = reg
        self.imm = imm
        self.size = size
        self.base = base
        self.index = index
        self.scale = scale
        self.disp = disp
        self.seg = seg


class Ins:
    __slots__ = ('addr', 'size', 'mn', 'ops', 'rep', 'repne', 'lock', 'text')

    def __init__(self, addr, size, mn, ops, rep, repne, lock, text):
        self.addr, self.size, self.mn, self.ops = addr, size, mn, ops
        self.rep, self.repne, self.lock, self.text = rep, repne, lock, text


class Image:
    def __init__(self, path):
        pe = pefile.PE(path)
        oh = pe.OPTIONAL_HEADER
        self.pe = pe
        self.base = oh.ImageBase
        self.size = oh.SizeOfImage
        self.entry = self.base + oh.AddressOfEntryPoint
        self.mem = bytearray(self.size)
        hdr = pe.header[:oh.SizeOfHeaders]
        self.mem[:len(hdr)] = hdr
        self.exec_ranges = []
        for s in pe.sections:
            raw = s.get_data()
            vsize = s.Misc_VirtualSize or len(raw)
            n = min(len(raw), vsize, self.size - s.VirtualAddress)
            self.mem[s.VirtualAddress:s.VirtualAddress + n] = raw[:n]
            if s.Characteristics & IMAGE_SCN_MEM_EXECUTE:
                lo = self.base + s.VirtualAddress
                self.exec_ranges.append((lo, lo + vsize))
        # Base relocations: HIGHLOW sites, whose current value is a VA.
        self.reloc_sites = []
        for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for r in blk.entries:
                if r.type == pefile.RELOCATION_TYPE['IMAGE_REL_BASED_HIGHLOW']:
                    self.reloc_sites.append(self.base + r.rva)
        self.imports = []  # (iat_slot_va, dll, name)
        for ent in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            dll = ent.dll.decode('latin-1')
            for imp in ent.imports:
                name = imp.name.decode('latin-1') if imp.name else f"ORD{imp.ordinal}"
                self.imports.append((imp.address, dll, name))
        self.exports = []  # (va, name)
        for sym in getattr(getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None), 'symbols', []) or []:
            if sym.address:
                nm = sym.name.decode('latin-1') if sym.name else f"ORD{sym.ordinal}"
                self.exports.append((self.base + sym.address, nm))

    def import_name_at(self, slot):
        """'DLL!Name' for an IAT slot address, or None."""
        for s, dll, nm in self.imports:
            if s == slot:
                return f"{dll}!{nm}"
        return None

    def in_image(self, addr):
        return self.base <= addr < self.base + self.size

    def is_exec(self, addr):
        return any(lo <= addr < hi for lo, hi in self.exec_ranges)

    def read32(self, addr):
        if not self.in_image(addr) or addr - self.base + 4 > self.size:
            raise DecodeError(addr, "table entry outside image")
        return struct.unpack_from('<I', self.mem, addr - self.base)[0]

    def code_bytes(self, addr, n=16):
        if not self.is_exec(addr):
            return None
        off = addr - self.base
        return bytes(self.mem[off:off + n])


def _mk_op(md, o):
    if o.type == CX.X86_OP_REG:
        return Op('reg', reg=md.reg_name(o.reg), size=o.size)
    if o.type == CX.X86_OP_IMM:
        return Op('imm', imm=o.imm, size=o.size)
    if o.type == CX.X86_OP_MEM:
        m = o.mem
        return Op('mem', base=md.reg_name(m.base) if m.base else None,
                  index=md.reg_name(m.index) if m.index else None,
                  scale=m.scale, disp=m.disp, size=o.size,
                  seg=md.reg_name(m.segment) if m.segment else None)
    return Op('other')


class Decoder:
    def __init__(self, img):
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.cache = {}

    def at(self, addr):
        """Decode the instruction at addr, or None if it is not code."""
        if addr in self.cache:
            return self.cache[addr]
        ins = None
        code = self.img.code_bytes(addr)
        if code:
            for ci in self.md.disasm(code, addr, 1):
                words = ci.mnemonic.split()
                rep = repne = lock = False
                while len(words) > 1 and words[0] in PREFIX_WORDS:
                    w = words.pop(0)
                    rep |= w in ('rep', 'repe', 'repz')
                    repne |= w in ('repne', 'repnz')
                    lock |= w == 'lock'
                ops = [_mk_op(self.md, o) for o in ci.operands]
                ins = Ins(ci.address, ci.size, words[0], ops, rep, repne, lock,
                          f"{ci.mnemonic} {ci.op_str}".strip())
        self.cache[addr] = ins
        return ins


# ----------------------------------------------------------------------------
# Operand helpers (pure string generation)
# ----------------------------------------------------------------------------

def reg_info(name):
    """Return (full32, shift, size) for a GPR name, or None."""
    if name in GPR32:
        return name, 0, 4
    if name in REG16:
        return REG16[name], 0, 2
    if name in REG8:
        full, sh = REG8[name]
        return full, sh, 1
    return None


def gpr(name):
    ri = reg_info(name)
    if ri is None:
        raise Unsupported(f"register {name}")
    return ri


def size_of(op):
    if op.kind == 'reg':
        ri = reg_info(op.reg)
        return ri[2] if ri else (op.size or 4)
    if op.kind == 'mem':
        return op.size or 4
    return 4


def ea(op):
    # 16-bit addressing ([bx+si], 0x67 prefix) is not supported. Its register names are not
    # C identifiers in the generated code, so refuse rather than emit broken C.
    for r in (op.base, op.index):
        if r and r not in GPR32:
            raise Unsupported(f"{r}-based addressing")
    parts = []
    if op.base:
        parts.append(op.base)
    if op.index:
        parts.append(op.index if op.scale == 1 else f"{op.index} * {op.scale}u")
    if op.disp or not parts:
        parts.append(f"0x{op.disp & M32:x}u")
    e = " + ".join(parts)
    if op.seg == 'fs':
        e = f"({e}) + c->fs_base"
    return f"((uint32_t)({e}))"


# Flat segment model: segment registers hold the Windows selectors. Loads return them.
# Stores are ignored, because nothing reads a segment base except through FS.
SEG_VALUE = {'cs': 0x1B, 'ds': 0x23, 'es': 0x23, 'ss': 0x23, 'fs': 0x3B, 'gs': 0x00}


def val(op, sz):
    """C expression for the operand's value, as uint32_t, masked to its width."""
    if op.kind == 'imm':
        return f"0x{op.imm & MASK[sz]:x}u"
    if op.kind == 'reg' and op.reg in SEG_VALUE:
        return f"0x{SEG_VALUE[op.reg]:x}u"
    if op.kind == 'mem':
        return f"rd{8 * sz}({ea(op)})"
    if op.kind == 'reg':
        full, sh, rs = gpr(op.reg)
        if rs == 4:
            return full
        if rs == 2:
            return f"({full} & 0xFFFFu)"
        if sh == 0:
            return f"({full} & 0xFFu)"
        return f"(({full} >> 8) & 0xFFu)"
    raise Unsupported(f"operand kind {op.kind}")


def setreg(name, expr):
    full, sh, sz = gpr(name)
    if sz == 4:
        return f"{full} = (uint32_t)({expr});"
    if sz == 2:
        return f"{full} = ({full} & 0xFFFF0000u) | (((uint32_t)({expr})) & 0xFFFFu);"
    if sh == 0:
        return f"{full} = ({full} & 0xFFFFFF00u) | (((uint32_t)({expr})) & 0xFFu);"
    return f"{full} = ({full} & 0xFFFF00FFu) | ((((uint32_t)({expr})) & 0xFFu) << 8);"


def store(op, sz, expr):
    if op.kind == 'reg' and op.reg in SEG_VALUE:
        return ""
    if op.kind == 'reg':
        return setreg(op.reg, expr)
    if op.kind == 'mem':
        return f"wr{8 * sz}({ea(op)}, {expr});"
    raise Unsupported(f"store to {op.kind}")


def blk(*stmts):
    return "{ " + " ".join(stmts) + " }"


def accum(sz):
    return {1: ('al', '(eax & 0xFFu)'), 2: ('ax', '(eax & 0xFFFFu)'), 4: ('eax', 'eax')}[sz]


def zsp(bits):
    return [f"zf = (r == 0);", f"sf = (r >> {bits - 1}) & 1u;", "pf = vt_parity(r);"]


def fl_add(bits, cin):
    m = MASK[bits // 8]
    return [f"uint64_t full = (uint64_t)a + b{' + cf' if cin else ''};",
            f"r = (uint32_t)full & 0x{m:x}u;",
            f"cf = (uint32_t)(full >> {bits}) & 1u;",
            f"of = (((a ^ r) & (b ^ r)) >> {bits - 1}) & 1u;",
            "af = ((a ^ b ^ r) >> 4) & 1u;"] + zsp(bits)


def fl_sub(bits, sbb):
    m = MASK[bits // 8]
    return [f"uint32_t bi = {'cf' if sbb else '0u'};",
            "cf = ((uint64_t)a < (uint64_t)b + bi);",
            f"r = (a - b - bi) & 0x{m:x}u;",
            f"of = (((a ^ b) & (a ^ r)) >> {bits - 1}) & 1u;",
            "af = ((a ^ b ^ r) >> 4) & 1u;"] + zsp(bits)


FLAG_NEUTRAL = {'mov', 'lea', 'push', 'pop', 'movzx', 'movsx', 'movss', 'movsd', 'movaps',
                'movups', 'movd', 'movq', 'fld', 'fst', 'fstp', 'fild', 'fistp', 'fxch'}


def flag_neutral(ins):
    """True if the instruction leaves EFLAGS alone."""
    return ins.mn in FLAG_NEUTRAL


def is_jcc(mn):
    return mn[0] == 'j' and mn[1:] in CC


# ----------------------------------------------------------------------------
# Analysis: function entries, jump tables, reachability
# ----------------------------------------------------------------------------

class JTable:
    """An indirect jump through a table. `bad` is set when the table cannot be trusted.

    A table is accepted only with a cmp/ja (or cmp/jae) bound and at least two
    entries, and every entry must be code. Otherwise `bad` holds the reason, and
    the jump traps at run time and is counted as unsupported.
    """
    __slots__ = ('addr', 'table', 'count', 'targets', 'bad')

    def __init__(self, addr, table, count, targets, bad=None):
        self.addr, self.table, self.count = addr, table, count
        self.targets, self.bad = targets, bad


class Analysis:
    def __init__(self, img, dec, log, table_sizes=None):
        self.img = img
        self.dec = dec
        self.log = log
        self.E = {}           # function entry -> hard?
        self.tables = {}      # indirect jump addr -> JTable or None
        self.table_slots = set()
        self.soft_bad = set()
        self.unbounded = {}   # jump addr -> (table, index register), for function-level bounds
        self.table_sizes = dict(table_sizes or {})   # explicit sizes, each with a source note
        self._jcc_index = None
        self.failed = {}      # entry -> reason, for entries whose body would not decode

    # -- instruction flow ---------------------------------------------------

    def _windows(self, end, maxback=24):
        """All instruction sequences that decode exactly up to end."""
        out = []
        for back in range(1, maxback + 1):
            start = end - back
            seq, a = [], start
            while a < end:
                ins = self.dec.at(a)
                if ins is None:
                    break
                seq.append(ins)
                a += ins.size
            if a == end and seq:
                out.append(seq)
        return out

    def _bound_before(self, end, idx_reg):
        """Entry count for a jump at end whose index is idx_reg, or None.

        Three patterns bound an index, in order of preference:
          1. cmp idx,N ; ja default (index 0..N) or jae/jnb default (0..N-1), just before end.
          2. and idx,M as the last write to idx before end (index 0..M).
          3. cmp idx,N ; jb end (the table dispatch is the taken target, index 0..N-1).
        """
        def neutral(x):
            # Instructions that leave EFLAGS alone are skipped, unless they write the index.
            return flag_neutral(x) and not (
                x.ops and x.ops[0].kind == 'reg' and x.ops[0].reg == idx_reg)

        for seq in self._windows(end):
            k = len(seq) - 1
            while k >= 0 and neutral(seq[k]):
                k -= 1
            if k < 0 or seq[k].mn not in ('ja', 'jae', 'jnb'):
                continue
            j = seq[k]
            # Between the compare and the branch, the same rule applies.
            m = k - 1
            while m >= 0 and neutral(seq[m]):
                m -= 1
            if m < 0:
                continue
            c = seq[m]
            if c.mn == 'cmp' and len(c.ops) == 2 and c.ops[0].kind == 'reg' and c.ops[0].reg == idx_reg:
                n = self._cmp_limit(c, seq[:m])
                if n is not None:
                    return n + 1 if j.mn == 'ja' else n
        for seq in self._windows(end):
            for ins in reversed(seq):
                if len(ins.ops) >= 1 and ins.ops[0].kind == 'reg' and ins.ops[0].reg == idx_reg:
                    if ins.mn == 'and' and len(ins.ops) == 2 and ins.ops[1].kind == 'imm':
                        m = ins.ops[1].imm & M32
                        if m >= 1:
                            return m + 1
                    if ins.mn not in ('cmp', 'test'):
                        break
            else:
                continue
            break
        # The dispatch can also be reached by a taken jb from far away in the function.
        for j in self._jcc_sites(end):
            jb = self.dec.at(j)
            if jb is None or jb.mn not in ('jb', 'jnae', 'jc') or jb.ops[0].imm & M32 != end:
                continue
            for k in range(2, 8):
                c = self.dec.at(j - k)
                if c is not None and c.addr + c.size == j and c.mn == 'cmp' and len(c.ops) == 2 \
                        and c.ops[0].kind == 'reg' and c.ops[0].reg == idx_reg and c.ops[1].kind == 'imm':
                    return c.ops[1].imm & M32
        return None

    def _function_bound(self, jaddr, seen):
        """Bound for an unbounded jump, from the function's own instructions.

        Two patterns, both found by walking back in address order through the function:
          * the last write to the index is and idx,M (index 0..M), or
          * a ja/jae default branch, preceded by cmp idx,N, with flag-neutral moves allowed
            between them (the index is bounded by N, or N+1 for ja)."""
        table, idx = self.unbounded[jaddr]

        def neutral(x):
            return x.mn in ('mov', 'lea', 'push', 'pop') and not (
                x.ops and x.ops[0].kind == 'reg' and x.ops[0].reg == idx)

        back = sorted((x for x in seen if x < jaddr), reverse=True)
        for a in back:
            ins = seen[a]
            if neutral(ins):
                continue
            if ins.ops and ins.ops[0].kind == 'reg' and ins.ops[0].reg == idx and ins.mn == 'and' \
                    and len(ins.ops) == 2 and ins.ops[1].kind == 'imm' and (ins.ops[1].imm & M32) >= 1:
                return (ins.ops[1].imm & M32) + 1
            if ins.mn in ('ja', 'jae', 'jnb'):
                # The compare sets the flags, so it is the next flag-setter back.
                for b in back:
                    if b >= a:
                        continue
                    c = seen[b]
                    if neutral(c):
                        continue
                    if c.mn == 'cmp' and len(c.ops) == 2 and c.ops[0].kind == 'reg' and c.ops[0].reg == idx:
                        n = self._cmp_limit(c, [seen[x] for x in sorted(x for x in seen if x < b)])
                        if n is not None:
                            return n + 1 if ins.mn == 'ja' else n
                    return None
                return None
            if ins.ops and ins.ops[0].kind == 'reg' and ins.ops[0].reg == idx and ins.mn not in ('cmp', 'test'):
                return None
        return None

    def resolve_function_bounds(self, jumps, seen):
        """Resolve unbounded jumps among seen. Returns True if any table was resolved."""
        progressed = False
        for jaddr in jumps:
            if jaddr not in self.unbounded or jaddr not in seen:
                continue
            count = self._function_bound(jaddr, seen)
            if count is None:
                continue
            table, idx = self.unbounded.pop(jaddr)
            try:
                self.tables[jaddr] = self._table_from_count(jaddr, table, count)
            except DecodeError:
                self.tables[jaddr] = JTable(jaddr, table, 0, [], bad="table outside image")
            progressed = True
        return progressed

    def _jcc_sites(self, target):
        """Addresses of short or near conditional branches whose byte encoding targets target.

        Built once from the executable bytes. Candidates are checked by decoding, so
        a byte pattern inside another instruction does no harm."""
        if self._jcc_index is None:
            index = {}
            for lo, hi in self.img.exec_ranges:
                off, end = lo - self.img.base, hi - self.img.base
                buf = self.img.mem
                for p in range(off, end - 1):
                    op = buf[p]
                    if op in (0x72, 0x73, 0x76, 0x77):                 # jb, jae, jbe, ja rel8
                        t = self.img.base + p + 2 + struct.unpack_from('<b', buf, p + 1)[0]
                        index.setdefault(t, []).append(self.img.base + p)
                    elif op == 0x0F and p + 5 < end and buf[p + 1] in (0x82, 0x83, 0x86, 0x87):
                        t = self.img.base + p + 6 + struct.unpack_from('<i', buf, p + 2)[0]
                        index.setdefault(t, []).append(self.img.base + p)
            self._jcc_index = index
        return self._jcc_index.get(target, [])

    def table_for(self, ins):
        """Return a JTable for an indirect jump, or None if it is not a table jump."""
        if ins.addr in self.tables:
            return self.tables[ins.addr]
        op = ins.ops[0]
        res = None
        if op.kind == 'mem' and op.index and not op.base and op.scale == 4:
            byte_count = self._byte_index_count(ins.addr, op.index)
            if byte_count is not None:
                try:
                    res = self._table_from_count(ins.addr, op.disp & M32, byte_count)
                except DecodeError:
                    res = JTable(ins.addr, op.disp & M32, 0, [], bad="byte table outside image")
            else:
                res = self._make_table(ins.addr, op.disp & M32, op.index, ins.addr)
        elif op.kind == 'reg':
            seq = self._windows(ins.addr)
            load = None
            for s in seq:
                if s and s[-1].mn == 'mov' and len(s[-1].ops) == 2 and \
                        s[-1].ops[0].kind == 'reg' and s[-1].ops[0].reg == op.reg and \
                        s[-1].ops[1].kind == 'mem' and s[-1].ops[1].index and \
                        not s[-1].ops[1].base and s[-1].ops[1].scale == 4:
                    load = s[-1]
                    break
            if load is not None:
                m = load.ops[1]
                res = self._make_table(ins.addr, m.disp & M32, m.index, load.addr)
        self.tables[ins.addr] = res
        return res

    def _cmp_limit(self, cmp_ins, before):
        """The constant the compare tests the index against: an immediate, or a register
        loaded by mov reg, imm earlier in `before` (instructions in address order)."""
        opnd = cmp_ins.ops[1]
        if opnd.kind == 'imm':
            return opnd.imm & M32
        if opnd.kind != 'reg':
            return None
        for x in reversed(before):
            if x.ops and x.ops[0].kind == 'reg' and x.ops[0].reg == opnd.reg:
                if x.mn == 'mov' and len(x.ops) == 2 and x.ops[1].kind == 'imm':
                    return x.ops[1].imm & M32
                return None
        return None

    def _byte_index_count(self, jaddr, idx):
        """Dispatch count for movzx idx, byte [idx + T]; jmp [idx*4 + D].

        The compare before the movzx bounds the byte-table index to N entries. The dispatch
        index is then a byte, so it is at most the largest byte in those N entries."""
        for seq in self._windows(jaddr):
            if not seq:
                continue
            last = seq[-1]
            # movzx dst, byte [src + T]: the dispatch index is dst. The compare bounds src.
            if last.mn != 'movzx' or len(last.ops) != 2 or last.ops[0].kind != 'reg' or \
                    last.ops[0].reg != idx or last.ops[1].kind != 'mem':
                continue
            m = last.ops[1]
            if not m.base or m.index or m.disp is None:
                continue
            pre = self._bound_before(last.addr, m.base)
            if pre is None:
                return None
            tbl = m.disp & M32
            try:
                vals = [self.img.mem[tbl + i - self.img.base] for i in range(pre)]
            except IndexError:
                return None
            return max(vals) + 1 if vals else None
        return None

    def _make_table(self, jaddr, table, idx_reg, bound_end):
        if jaddr in self.table_sizes:
            return self._table_from_count(jaddr, table, self.table_sizes[jaddr])
        count = self._bound_before(bound_end, idx_reg)
        if count is None:
            # Not bounded locally. Function-level resolution may still bound it.
            self.unbounded[jaddr] = (table, idx_reg)
            return JTable(jaddr, table, 0, [], bad="no cmp/ja bound")
        return self._table_from_count(jaddr, table, count)

    def _table_from_count(self, jaddr, table, count):
        if count < 2:
            self.log(f"error: jump at {jaddr:#x}: bound gives {count} entries")
            return JTable(jaddr, table, count, [], bad=f"bound gives {count} entries")
        targets = []
        for i in range(count):
            try:
                v = self.img.read32(table + 4 * i)
            except DecodeError:
                return JTable(jaddr, table, count, [], bad=f"entry {i} outside image")
            if not self.img.is_exec(v):
                return JTable(jaddr, table, count, [], bad=f"entry {i} = {v:#x} is not code")
            if v not in targets:
                targets.append(v)
        for i in range(count):
            self.table_slots.add(table + 4 * i)
        return JTable(jaddr, table, count, sorted(targets))

    def flow(self, ins):
        """(targets, falls_through) for one instruction's control flow."""
        mn = ins.mn
        if mn in RET_MN or mn in TERMINAL:
            return [], False
        if mn == 'jmp':
            op = ins.ops[0]
            if op.kind == 'imm':
                return [op.imm & M32], False
            tb = self.table_for(ins)
            if tb is not None:
                return ([] if tb.bad else list(tb.targets)), False
            return [], False  # indirect tail jump: codegen emits vt_icall
        if is_jcc(mn) or mn in LOOPS or mn in ('jecxz', 'jcxz'):
            return [ins.ops[0].imm & M32], True
        if mn == 'call':
            # Calls that never return end the path. So does a fallthrough that is not
            # code, or is int3 padding: MSVC pads the end of a function that finishes with
            # a call that does not return. Codegen traps if such a call does return.
            if self.is_noreturn_call(ins):
                return [], False
            nxt = self.dec.at(ins.addr + ins.size)
            if nxt is None or nxt.mn == 'int3':
                return [], False
        return [], True

    def is_noreturn_call(self, ins):
        op = ins.ops[0]
        if op.kind == 'imm':
            return False
        if op.kind == 'mem' and op.base is None and op.index is None:
            name = self.img.import_name_at(op.disp & M32)
            return name is not None and name.split('!')[-1] in NORETURN_IMPORTS
        return False

    # -- entry closure ------------------------------------------------------

    def _add(self, work, a, hard):
        if a in self.E:
            if hard and not self.E[a]:
                self.E[a] = True
            return
        self.E[a] = hard
        work.append(a)

    def _scan(self, f, hard, work):
        seen, todo = set(), [f]
        try:
            while True:
                while todo:
                    a = todo.pop()
                    if a in seen or (a != f and a in self.E):
                        continue
                    ins = self.dec.at(a)
                    if ins is None:
                        raise DecodeError(a, f"not code (from function {f:#x})")
                    seen.add(a)
                    if ins.mn == 'call' and ins.ops[0].kind == 'imm':
                        self._add(work, ins.ops[0].imm & M32, hard)
                    targets, falls = self.flow(ins)
                    todo.extend(targets)
                    if falls:
                        todo.append(a + ins.size)
                # Jumps left unbounded locally may now be bounded from this function.
                pending = [a for a in seen if a in self.unbounded]
                if not self.resolve_function_bounds(pending, {a: self.dec.at(a) for a in seen}):
                    break
                for a in pending:
                    if a in self.tables and self.tables[a] and not self.tables[a].bad:
                        todo.extend(self.tables[a].targets)
        except DecodeError as e:
            # A failed entry is not a reason to stop the run. Codegen emits a trap for it,
            # and the report lists it.
            if hard:
                self.failed[f] = str(e)
            self.soft_bad.add(f)

    def closure(self, roots):
        work = []
        for a, hard in roots:
            self._add(work, a, hard)
        while work:
            f = work.pop()
            self._scan(f, self.E[f], work)

    def code_pointer_candidates(self):
        """Code addresses the program stores. Returns (immediates, data) as two sets.

        immediates: operands of translated code, for example push offset thunk.
        data: dwords in non-executable sections that point into executable ones.
        Neither is a relocation in VT3.exe, and neither is a call target, so direct
        discovery never sees them."""
        imm, cands = set(), set()
        for e in list(self.E):
            try:
                seen = self.reach(e)
            except DecodeError:
                continue
            for ins in seen.values():
                for op in ins.ops:
                    if op.kind == 'imm' and self.img.is_exec(op.imm & M32):
                        imm.add(op.imm & M32)
        for sec in self.img.pe.sections:
            if sec.Characteristics & IMAGE_SCN_MEM_EXECUTE:
                continue
            lo = self.img.base + sec.VirtualAddress
            hi = lo + (sec.Misc_VirtualSize or sec.SizeOfRawData)
            for a in range(lo, min(hi, self.img.base + self.img.size) - 3, 4):
                v = struct.unpack_from('<I', self.img.mem, a - self.img.base)[0]
                if self.img.is_exec(v):
                    cands.add(v)
        return imm, cands

    def covered_bytes(self):
        covered = set()
        for e in self.E:
            try:
                for a, ins in self.reach(e).items():
                    covered.update(range(a, a + ins.size))
            except DecodeError:
                pass
        return covered

    def add_validated(self, imm, data):
        """Add pointer candidates that look like functions. Returns (accepted, added).

        A candidate must start outside known code (a candidate inside a body is a case label
        or a branch target), decode in full, and end in a way that leaves the function: a
        ret, or a jmp out of the body (a thunk that tail-jumps). Data-like bytes
        (add [eax], al) reject it. Jump-table targets are never entries."""
        covered = self.covered_bytes()
        table_targets = set()
        for tb in self.tables.values():
            if tb and not tb.bad:
                table_targets.update(tb.targets)
        good = []
        for c in sorted(imm | data):
            if c in self.E or c in table_targets or self.dec.at(c) is None:
                continue
            if c in covered:
                continue
            try:
                seen = self.reach(c)
            except DecodeError:
                continue
            if not self._leaves_function(seen):
                continue
            if any(i.mn == 'add' and 'byte ptr [eax], al' in i.text for i in seen.values()):
                continue
            # A body with an instruction the translator cannot lower is data decoded as code.
            # Blockers that are only rejected jump tables do not count against it.
            try:
                _, _, unsup = gen_function(self, c, False)
            except (DecodeError, TranslateError):
                continue
            if any(not why.startswith('jump table') for _, why, _ in unsup):
                continue
            good.append((c, False))
        before = len(self.E)
        self.closure(good)
        return len(good), len(self.E) - before

    def _leaves_function(self, seen):
        """True if the body ends by returning or by jumping out of the body."""
        for a, ins in seen.items():
            if ins.mn in RET_MN:
                return True
            if ins.mn == 'jmp':
                if ins.ops[0].kind != 'imm' or (ins.ops[0].imm & M32) not in seen:
                    return True
            if ins.mn == 'call' and not self.flow(ins)[1]:
                return True   # a call that ends the path: noreturn, or padding after it
        return False

    def build_entries(self, reloc_sites, seeds=(), scan_pointers=True):
        hard_roots = [(self.img.entry, True)] + [(a, True) for a, _ in self.img.exports]
        hard_roots += [(a, True) for a in seeds if self.img.is_exec(a)]
        self.closure(hard_roots)
        self.pointer_stats = []
        # A relocation site holds an absolute VA. If that VA is code, it is a
        # candidate entry (function pointer, or a code address in data). Slots
        # that we already read as jump-table entries are not candidates.
        soft = []
        for site in reloc_sites:
            if site in self.table_slots:
                continue
            v = self.img.read32(site)
            if self.img.is_exec(v):
                soft.append((v, False))
        self.closure(soft)
        if scan_pointers:
            for _ in range(3):
                imm, data = self.code_pointer_candidates()
                validated, added = self.add_validated(imm, data)
                self.pointer_stats.append((len(imm | data), validated, added))
                if added == 0:
                    break

    # -- codegen-time reachability ------------------------------------------

    def reach(self, entry):
        for _ in range(4):
            seen = self._reach_once(entry)
            pending = [a for a in seen if a in self.unbounded]
            if not pending or not self.resolve_function_bounds(pending, seen):
                break
        order = sorted(seen)
        for x, y in zip(order, order[1:]):
            if x + seen[x].size > y:
                raise DecodeError(y, f"overlaps instruction at {x:#x}")
        return seen

    def _reach_once(self, entry):
        seen, todo = {}, [entry]
        while todo:
            a = todo.pop()
            if a in seen or (a != entry and a in self.E):
                continue
            ins = self.dec.at(a)
            if ins is None:
                raise DecodeError(a, "not code")
            seen[a] = ins
            targets, falls = self.flow(ins)
            todo.extend(targets)
            if falls:
                todo.append(a + ins.size)
        return seen


# ----------------------------------------------------------------------------
# Code generation
# ----------------------------------------------------------------------------

def fname(a):
    return f"fn_{a:08x}"


class FnGen:
    """Generates C for one guest function given the final entry set."""

    def __init__(self, an, entry):
        self.an = an
        self.entry = entry
        self.unsupported = []

    def branch(self, t):
        if t != self.entry and t in self.an.E:
            return f"{{ VT_SPILL(); {fname(t)}(c); return; }}"
        return f"goto L_{t:x};"

    def gen(self, ins):
        """Statements for one instruction (control flow included)."""
        mn = ins.mn
        if mn in RET_MN:
            extra = ins.ops[0].imm & 0xFFFF if ins.ops else 0
            return [f"{{ esp += {4 + extra}u; VT_SPILL(); return; }}"]
        if mn == 'call':
            op = ins.ops[0]
            ret = ins.addr + ins.size
            if op.kind == 'imm':
                t = op.imm & M32
                call = f"{fname(t)}(c);" if t in self.an.E else f"vt_icall(c, 0x{t:x}u);"
                return [f"{{ esp -= 4u; wr32(esp, 0x{ret:x}u); VT_SPILL(); {call} VT_RELOAD(); }}"]
            return [f"{{ uint32_t t = {val(op, 4)}; esp -= 4u; wr32(esp, 0x{ret:x}u); "
                    f"VT_SPILL(); vt_icall(c, t); VT_RELOAD(); }}"]
        if mn == 'jmp':
            op = ins.ops[0]
            if op.kind == 'imm':
                return [self.branch(op.imm & M32)]
            tb = self.an.tables.get(ins.addr)
            if tb is not None and tb.bad:
                raise Unsupported(f"jump table ({tb.bad})")
            if tb is not None:
                cases = "".join(f" case 0x{t:x}u: {self.branch(t)}" for t in tb.targets)
                return [f"{{ uint32_t t = {val(op, 4)}; switch (t) {{{cases} "
                        f"default: vt_die_at(0x{ins.addr:x}u, t); }} }}"]
            return [f"{{ uint32_t t = {val(op, 4)}; VT_SPILL(); vt_icall(c, t); return; }}"]
        if is_jcc(mn):
            return [f"if ({CC[mn[1:]]}) {self.branch(ins.ops[0].imm & M32)}"]
        if mn in LOOPS:
            cond = {'loop': 'ecx != 0', 'loope': 'ecx != 0 && zf', 'loopz': 'ecx != 0 && zf',
                    'loopne': 'ecx != 0 && !zf', 'loopnz': 'ecx != 0 && !zf'}[mn]
            return [f"ecx -= 1u;", f"if ({cond}) {self.branch(ins.ops[0].imm & M32)}"]
        if mn in ('jecxz', 'jcxz'):
            return [f"if (ecx == 0) {self.branch(ins.ops[0].imm & M32)}"]
        return plain(ins)


def plain(ins):
    mn, ops = ins.mn, ins.ops
    if mn in ('nop', 'fnop', 'fwait', 'wait', 'emms', 'prefetchnta', 'prefetcht0', 'prefetcht1'):
        return []
    special = simd_or_misc(ins)
    if special is not None:
        return special
    if mn == 'mov':
        d, s = ops
        sz = size_of(d)
        return [blk(store(d, sz, val(s, sz)))]
    if mn in ('movzx', 'movsx'):
        d, s = ops
        ssz = size_of(s)
        v = val(s, ssz)
        if mn == 'movsx':
            v = f"vt_sx({v}, {8 * ssz})"
        return [blk(store(d, size_of(d), v))]
    if mn == 'lea':
        d, s = ops
        return [blk(store(d, size_of(d), ea(s)))]
    if mn in ('add', 'adc', 'sub', 'sbb', 'and', 'or', 'xor', 'cmp', 'test'):
        return [alu(ins)]
    if mn in ('inc', 'dec'):
        return [incdec(ins, mn == 'inc')]
    if mn == 'neg':
        d = ops[0]
        sz = size_of(d)
        bits, m, sb = 8 * sz, MASK[sz], 1 << (8 * sz - 1)
        body = [f"uint32_t a = {val(d, sz)};", f"uint32_t r = (0u - a) & 0x{m:x}u;",
                "cf = (a != 0);", f"of = (a == 0x{sb:x}u);", "af = ((a ^ r) >> 4) & 1u;"] + \
            zsp(bits) + [store(d, sz, 'r')]
        return [blk(*body)]
    if mn == 'not':
        d = ops[0]
        sz = size_of(d)
        return [blk(f"uint32_t a = {val(d, sz)};", store(d, sz, f"(~a) & 0x{MASK[sz]:x}u"))]
    if mn in ('xchg', 'xadd', 'cmpxchg'):
        return [xchg_family(ins)]
    if mn == 'push':
        o = ops[0]
        if o.kind == 'imm':
            return [f"esp -= 4u; wr32(esp, 0x{o.imm & M32:x}u);"]
        return [blk(f"uint32_t v = {val(o, 4)};", "esp -= 4u;", "wr32(esp, v);")]
    if mn == 'pop':
        o = ops[0]
        return [blk("uint32_t v = rd32(esp);", "esp += 4u;", store(o, 4, 'v'))]
    if mn == 'leave':
        return [blk("esp = ebp;", "ebp = rd32(esp);", "esp += 4u;")]
    if mn == 'pushfd':
        return [blk("uint32_t f = (cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | "
                    "(df << 10) | (of << 11));", "esp -= 4u;", "wr32(esp, f);")]
    if mn == 'popfd':
        return [blk("uint32_t f = rd32(esp);", "esp += 4u;",
                    "cf = f & 1u; pf = (f >> 2) & 1u; af = (f >> 4) & 1u; zf = (f >> 6) & 1u;",
                    "sf = (f >> 7) & 1u; df = (f >> 10) & 1u; of = (f >> 11) & 1u;")]
    if mn == 'lahf':
        return [blk("uint32_t f = cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7);",
                    setreg('ah', 'f'))]
    if mn == 'sahf':
        return [blk("uint32_t ah = (eax >> 8) & 0xFFu;", "cf = ah & 1u; pf = (ah >> 2) & 1u;",
                    "af = (ah >> 4) & 1u; zf = (ah >> 6) & 1u; sf = (ah >> 7) & 1u;")]
    if mn == 'cld':
        return ["df = 0u;"]
    if mn == 'std':
        return ["df = 1u;"]
    if mn == 'clc':
        return ["cf = 0u;"]
    if mn == 'stc':
        return ["cf = 1u;"]
    if mn == 'cmc':
        return ["cf = !cf;"]
    if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror'):
        return [shift(ins)]
    if mn in ('shld', 'shrd'):
        return [shld(ins)]
    if mn in ('imul', 'mul', 'div', 'idiv'):
        return [muldiv(ins)]
    if mn in ('bt', 'bts', 'btr', 'btc'):
        return [bittest(ins)]
    if mn in ('bsf', 'bsr'):
        d, s = ops
        sz = size_of(d)
        pick = "__builtin_ctz(v)" if mn == 'bsf' else "(31u - __builtin_clz(v))"
        return [blk(f"uint32_t v = {val(s, sz)};", "if (v == 0) { zf = 1u; } else "
                    f"{{ zf = 0u; {store(d, sz, pick)} }}")]
    if mn in ('tzcnt', 'lzcnt', 'popcnt'):
        d, s = ops
        sz = size_of(d)
        bits = 8 * sz
        if mn == 'popcnt':
            return [blk(f"uint32_t v = {val(s, sz)};", f"uint32_t r = (uint32_t)__builtin_popcount(v);",
                        "cf = 0u; of = 0u; sf = 0u; af = 0u; pf = 0u; zf = (v == 0u);", store(d, sz, 'r'))]
        if mn == 'tzcnt':
            pick = f"(v ? (uint32_t)__builtin_ctz(v) : {bits}u)"
        else:
            pick = f"(v ? (uint32_t)__builtin_clz(v) - {32 - bits}u : {bits}u)"
        return [blk(f"uint32_t v = {val(s, sz)};", f"uint32_t r = {pick};",
                    "cf = (v == 0u); zf = (r == 0u); of = 0u; sf = 0u; af = 0u; pf = 0u;",
                    store(d, sz, 'r'))]
    if mn == 'bswap':
        r = ops[0].reg
        return [blk(f"uint32_t v = {val(ops[0], 4)};", setreg(r, "__builtin_bswap32(v)"))]
    if mn.startswith('set') and mn[3:] in CC:
        d = ops[0]
        return [blk(store(d, 1, f"(({CC[mn[3:]]}) ? 1u : 0u)"))]
    if mn.startswith('cmov') and mn[4:] in CC:
        d, s = ops
        sz = size_of(d)
        return [f"if ({CC[mn[4:]]}) {blk(store(d, sz, val(s, sz)))}"]
    if mn in ('cdq',):
        return ["edx = (uint32_t)((int32_t)eax >> 31);"]
    if mn == 'cwde':
        return ["eax = vt_sx(eax & 0xFFFFu, 16);"]
    if mn == 'cbw':
        return [setreg('ax', "vt_sx(eax & 0xFFu, 8)")]
    if mn == 'cwd':
        return ["edx = (edx & 0xFFFF0000u) | (((eax & 0x8000u) ? 0xFFFFu : 0u));"]
    if mn in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw',
              'lodsd', 'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd') and \
            not any(o.kind == 'reg' and o.reg.startswith('xmm') for o in ops):
        return [string_op(ins)]
    if mn == 'rdtsc':
        return [blk("uint64_t t = vt_rdtsc();", "eax = (uint32_t)t;", "edx = (uint32_t)(t >> 32);")]
    if mn.startswith('f') and mn not in ('fwait',):
        return x87(ins)
    if mn in PACKED_MN:
        return _packed(ins)
    if mn in SSE_MN or (mn in ('movsd', 'cmpsd') and any(o.kind == 'reg' and o.reg.startswith('xmm') for o in ops)):
        return sse(ins)
    raise Unsupported(mn)


def alu(ins):
    d, s = ins.ops
    sz = size_of(d) if d.kind != 'imm' else size_of(s)
    bits = 8 * sz
    mn = ins.mn
    pre = [f"uint32_t a = {val(d, sz)};", f"uint32_t b = {val(s, sz)};", "uint32_t r;"]
    if mn in ('add', 'adc'):
        body = fl_add(bits, mn == 'adc')
    elif mn in ('sub', 'sbb', 'cmp'):
        body = fl_sub(bits, mn == 'sbb')
    else:
        op = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[mn]
        body = [f"r = (a {op} b) & 0x{MASK[sz]:x}u;", "cf = 0u;", "of = 0u;", "af = 0u;"] + zsp(bits)
    if mn not in ('cmp', 'test'):
        body.append(store(d, sz, 'r'))
    return blk(*pre, *body)


def incdec(ins, inc):
    d = ins.ops[0]
    sz = size_of(d)
    bits, m, sb = 8 * sz, MASK[sz], 1 << (8 * sz - 1)
    if inc:
        body = [f"r = (a + 1u) & 0x{m:x}u;", f"of = (r == 0x{sb:x}u);"]
    else:
        body = [f"r = (a - 1u) & 0x{m:x}u;", f"of = (a == 0x{sb:x}u);"]
    body += ["af = ((a ^ 1u ^ r) >> 4) & 1u;"] + zsp(bits) + [store(d, sz, 'r')]
    return blk(f"uint32_t a = {val(d, sz)};", "uint32_t r;", *body)


def xchg_family(ins):
    mn = ins.mn
    d, s = ins.ops
    sz = size_of(d) if d.kind == 'reg' else size_of(s)
    bits = 8 * sz
    if mn == 'xchg':
        return blk(f"uint32_t a = {val(d, sz)};", f"uint32_t b = {val(s, sz)};",
                   store(d, sz, 'b'), store(s, sz, 'a'))
    if mn == 'xadd':
        return blk(f"uint32_t a = {val(d, sz)};", f"uint32_t b = {val(s, sz)};", "uint32_t r;",
                   *fl_add(bits, False), store(s, sz, 'a'), store(d, sz, 'r'))
    acc_name, acc_expr = accum(sz)
    return blk(f"uint32_t b = {val(d, sz)};", f"uint32_t a = {acc_expr};", "uint32_t r;",
               *fl_sub(bits, False),
               f"if (r == 0u) {{ {store(d, sz, val(s, sz))} }} else {{ {setreg(acc_name, 'b')} }}")


def shift(ins):
    d, s = ins.ops
    sz = size_of(d)
    bits, m = 8 * sz, MASK[sz]
    mn = ins.mn
    cnt = f"(({val(s, 1)}) & 0x1Fu)"
    lines = [f"uint32_t a = {val(d, sz)};", f"uint32_t cnt = {cnt};", "uint32_t r = a;", "if (cnt != 0u) {"]
    if mn in ('shl', 'sal'):
        lines += [f"uint64_t t = (uint64_t)a << cnt;", f"r = (uint32_t)t & 0x{m:x}u;",
                  f"cf = (uint32_t)(t >> {bits}) & 1u;",
                  f"of = ((r >> {bits - 1}) & 1u) ^ cf;"] + zsp(bits)
    elif mn == 'shr':
        lines += ["cf = (a >> (cnt - 1u)) & 1u;", "r = a >> cnt;",
                  f"of = (a >> {bits - 1}) & 1u;"] + zsp(bits)
    elif mn == 'sar':
        lines += [f"int32_t sa = (int32_t)vt_sx(a, {bits});", "cf = ((uint32_t)(sa >> (cnt - 1u))) & 1u;",
                  f"r = (uint32_t)(sa >> cnt) & 0x{m:x}u;", "of = 0u;"] + zsp(bits)
    elif mn == 'rol':
        lines += [f"uint32_t k = cnt % {bits}u;",
                  f"r = k ? (((a << k) | (a >> ({bits}u - k))) & 0x{m:x}u) : a;",
                  f"cf = r & 1u;", f"of = (((r >> {bits - 1}) & 1u) ^ cf);"]
    elif mn == 'ror':
        lines += [f"uint32_t k = cnt % {bits}u;",
                  f"r = k ? (((a >> k) | (a << ({bits}u - k))) & 0x{m:x}u) : a;",
                  f"cf = (r >> {bits - 1}) & 1u;",
                  f"of = (((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1u);"]
    lines += ["}", store(d, sz, 'r')]
    return blk(*lines)


def shld(ins):
    d, s, c = ins.ops
    if size_of(d) != 4:
        raise Unsupported("shld 16-bit")
    cnt = f"(({val(c, 1)}) & 0x1Fu)"
    if ins.mn == 'shld':
        core = ["uint64_t t = (((uint64_t)a << 32) | b) << cnt;", "r = (uint32_t)(t >> 32);",
                "cf = (a >> (32u - cnt)) & 1u;"]
    else:
        core = ["uint64_t t = (((uint64_t)b << 32) | a) >> cnt;", "r = (uint32_t)t;",
                "cf = (a >> (cnt - 1u)) & 1u;"]
    return blk(f"uint32_t a = {val(d, 4)};", f"uint32_t b = {val(s, 4)};", f"uint32_t cnt = {cnt};",
               "uint32_t r = a;", "if (cnt != 0u) {", *core, "of = 0u;", *zsp(32), "}",
               store(d, 4, 'r'))


def muldiv(ins):
    mn = ins.mn
    if mn == 'imul' and len(ins.ops) >= 2:
        d = ins.ops[0]
        sz = size_of(d)
        bits = 8 * sz
        if len(ins.ops) == 2:
            a_expr, b_expr = val(d, sz), val(ins.ops[1], sz)
        else:
            a_expr, b_expr = val(ins.ops[1], sz), val(ins.ops[2], sz)
        return blk(f"uint32_t lo;",
                   f"int64_t p = (int64_t)(int32_t)vt_sx({a_expr}, {bits}) * (int64_t)(int32_t)vt_sx({b_expr}, {bits});",
                   f"lo = (uint32_t)p & 0x{MASK[sz]:x}u;",
                   store(d, sz, "lo"),
                   f"cf = of = ((int64_t)(int32_t)vt_sx(lo, {bits}) != p);")
    s = ins.ops[0]
    sz = size_of(s)
    bits = 8 * sz
    v = val(s, sz)
    signed = mn in ('imul', 'idiv')
    if mn in ('mul', 'imul'):
        acc = accum(sz)[1] if sz != 4 else 'eax'
        lines = [f"uint32_t b = {v};", f"uint32_t a = {acc};"]
        if signed:
            lines += [f"int64_t p = (int64_t)(int32_t)vt_sx(a, {bits}) * (int64_t)(int32_t)vt_sx(b, {bits});",
                      f"uint32_t lo = (uint32_t)p & 0x{MASK[sz]:x}u;",
                      f"cf = of = ((int64_t)(int32_t)vt_sx(lo, {bits}) != p);"]
            pw = "(uint64_t)p"
        else:
            lines += ["uint64_t p = (uint64_t)a * b;", f"uint32_t lo = (uint32_t)p & 0x{MASK[sz]:x}u;",
                      f"cf = of = ((p >> {bits}) != 0);"]
            pw = "p"
        if sz == 1:
            lines.append(setreg('ax', pw))
        elif sz == 2:
            lines += [setreg('ax', pw), setreg('dx', f"({pw}) >> 16")]
        else:
            lines += [setreg('eax', pw), setreg('edx', f"({pw}) >> 32")]
        return blk(*lines)
    # div / idiv
    lines = [f"uint32_t dv = {v};"]
    if mn == 'div':
        if sz == 4:
            lines += ["uint64_t n = ((uint64_t)edx << 32) | eax;", "if (dv == 0u) vt_die_at(0u, 0xDEu);",
                      "uint64_t q = n / dv;", "if (q > 0xFFFFFFFFu) vt_die_at(0u, 0xDEu);",
                      "eax = (uint32_t)q; edx = (uint32_t)(n % dv);"]
        elif sz == 2:
            lines += ["uint32_t n = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu);", "uint32_t d16 = dv & 0xFFFFu;",
                      "if (d16 == 0u) vt_die_at(0u, 0xDEu);", "uint32_t q = n / d16;",
                      "if (q > 0xFFFFu) vt_die_at(0u, 0xDEu);", setreg('ax', 'q'), setreg('dx', 'n % d16')]
        else:
            lines += ["uint32_t n = eax & 0xFFFFu;", "uint32_t d8 = dv & 0xFFu;",
                      "if (d8 == 0u) vt_die_at(0u, 0xDEu);", "uint32_t q = n / d8;",
                      "if (q > 0xFFu) vt_die_at(0u, 0xDEu);", setreg('al', 'q'), setreg('ah', 'n % d8')]
    else:
        if sz == 4:
            lines += ["int64_t n = (int64_t)(((uint64_t)edx << 32) | eax);", "int64_t d = (int32_t)dv;",
                      "if (d == 0 || (d == -1 && n == INT64_MIN)) vt_die_at(0u, 0xDEu);",
                      "int64_t q = n / d;", "if (q != (int32_t)q) vt_die_at(0u, 0xDEu);",
                      "eax = (uint32_t)q; edx = (uint32_t)(n % d);"]
        elif sz == 2:
            lines += ["int32_t n = (int32_t)(((uint32_t)(edx & 0xFFFFu) << 16) | (eax & 0xFFFFu));",
                      "int32_t d = (int16_t)dv;", "if (d == 0) vt_die_at(0u, 0xDEu);",
                      "int32_t q = n / d;", "if (q != (int16_t)q) vt_die_at(0u, 0xDEu);",
                      setreg('ax', '(uint32_t)q'), setreg('dx', '(uint32_t)(n % d)')]
        else:
            lines += ["int32_t n = (int16_t)(eax & 0xFFFFu);", "int32_t d = (int8_t)dv;",
                      "if (d == 0) vt_die_at(0u, 0xDEu);", "int32_t q = n / d;",
                      "if (q != (int8_t)q) vt_die_at(0u, 0xDEu);",
                      setreg('al', '(uint32_t)q'), setreg('ah', '(uint32_t)(n % d)')]
    return blk(*lines)


def bittest(ins):
    mn = ins.mn
    d, s = ins.ops
    if d.kind == 'mem' and s.kind == 'reg':
        # Bit string: the offset can address memory beyond the operand.
        addr = (f"(uint32_t)({ea(d)} + (uint32_t)(((int32_t){val(s, 4)}) >> 5) * 4u)")
        pre = [f"uint32_t ba = {addr};", f"uint32_t bit = {val(s, 4)} & 31u;",
               f"uint32_t a = rd32(ba);"]
        wr = f"wr32(ba, {modify(mn)});"
    else:
        sz = size_of(d)
        bits = 8 * sz
        pre = [f"uint32_t a = {val(d, sz)};", f"uint32_t bit = {val(s, 4)} & {bits - 1}u;"]
        wr = store(d, sz, modify(mn)) if mn != 'bt' else ""
    body = ["cf = (a >> bit) & 1u;"] + ([wr] if wr else [])
    return blk(*pre, *body)


def modify(mn):
    return {'bt': 'a', 'bts': '(a | (1u << bit))', 'btr': '(a & ~(1u << bit))',
            'btc': '(a ^ (1u << bit))'}[mn]


def string_op(ins):
    mn = ins.mn
    base, sfx = mn[:-1], mn[-1]
    sz = {'b': 1, 'w': 2, 'd': 4}[sfx]
    step = f"(df ? 0u - {sz}u : {sz}u)"
    if base == 'movs':
        body = [blk(f"wr{8 * sz}(edi, rd{8 * sz}(esi));", f"esi += {step};", f"edi += {step};")]
    elif base == 'stos':
        body = [blk(f"wr{8 * sz}(edi, {accum(sz)[1]});", f"edi += {step};")]
    elif base == 'lods':
        body = [blk(setreg(accum(sz)[0], f"rd{8 * sz}(esi)"), f"esi += {step};")]
    elif base == 'scas':
        body = [blk(f"uint32_t a = {accum(sz)[1]};", f"uint32_t b = rd{8 * sz}(edi);", "uint32_t r;",
                    *fl_sub(8 * sz, False), f"edi += {step};")]
    elif base == 'cmps':
        body = [blk(f"uint32_t a = rd{8 * sz}(esi);", f"uint32_t b = rd{8 * sz}(edi);", "uint32_t r;",
                    *fl_sub(8 * sz, False), f"esi += {step};", f"edi += {step};")]
    else:
        raise Unsupported(mn)
    if ins.rep or ins.repne:
        if base in ('scas', 'cmps'):
            stop = "if (zf) break;" if ins.repne else "if (!zf) break;"
            return blk("while (ecx != 0u) {", *body, "ecx -= 1u;", stop, "}")
        return blk("while (ecx != 0u) {", *body, "ecx -= 1u;", "}")
    return blk(*body)


# -- SSE (scalar float, plus the bitwise and move forms VC emits) --------------

SSE_MN = {
    'movss', 'movsd', 'movaps', 'movups', 'movapd', 'movupd', 'movdqa', 'movdqu', 'movd', 'movq',
    'addss', 'addsd', 'subss', 'subsd', 'mulss', 'mulsd', 'divss', 'divsd', 'sqrtss', 'sqrtsd',
    'minss', 'minsd', 'maxss', 'maxsd', 'ucomiss', 'ucomisd', 'comiss', 'comisd',
    'cvtsi2ss', 'cvtsi2sd', 'cvttss2si', 'cvttsd2si', 'cvtss2si', 'cvtsd2si',
    'cvtss2sd', 'cvtsd2ss', 'xorps', 'xorpd', 'andps', 'andpd', 'andnps', 'andnpd',
    'orps', 'orpd', 'pxor', 'pand', 'pandn', 'por',
}


def _xmm(op):
    """XMM index for a register operand, None for memory. MMX and GPR operands are unsupported here."""
    if op.kind == 'reg':
        if op.reg and op.reg.startswith('xmm'):
            return int(op.reg[3:])
        raise Unsupported(f"non-xmm register operand {op.reg}")
    return None


def _xmm_maybe(op):
    """XMM index, or None for memory and for a general or MMX register operand."""
    if op.kind == 'reg' and op.reg and op.reg.startswith('xmm'):
        return int(op.reg[3:])
    return None


def _f32src(op):
    i = _xmm(op)
    return f"c->xmm[{i}].f32[0]" if i is not None else f"vt_ldf32({ea(op)})"


def _f64src(op):
    i = _xmm(op)
    return f"c->xmm[{i}].f64[0]" if i is not None else f"vt_ldf64({ea(op)})"


def _x128src(op):
    i = _xmm(op)
    return f"c->xmm[{i}]" if i is not None else f"vt_ldx({ea(op)})"


def sse(ins):
    mn, ops = ins.mn, ins.ops
    if mn in ('movss', 'movsd'):
        d, s = ops
        n, bits = (4, 32) if mn == 'movss' else (8, 64)
        di, si = _xmm(d), _xmm(s)
        if di is not None and si is not None:
            return [f"c->xmm[{di}].u{bits}[0] = c->xmm[{si}].u{bits}[0];"]
        if di is not None:
            return [blk(f"c->xmm[{di}].u64[0] = 0; c->xmm[{di}].u64[1] = 0;",
                        f"c->xmm[{di}].u{bits}[0] = rd{bits}({ea(s)});")]
        return [f"wr{bits}({ea(d)}, c->xmm[{si}].u{bits}[0]);"]
    if mn in ('movaps', 'movups', 'movapd', 'movupd', 'movdqa', 'movdqu'):
        d, s = ops
        di = _xmm(d)
        if di is not None:
            return [f"c->xmm[{di}] = {_x128src(s)};"]
        return [f"vt_stx({ea(d)}, c->xmm[{_xmm(s)}]);"]
    if mn in ('movd', 'movq'):
        d, s = ops
        di = _xmm_maybe(d)
        if di is not None:
            if mn == 'movd':
                v = val(s, 4)
                return [blk(f"c->xmm[{di}].u64[0] = 0; c->xmm[{di}].u64[1] = 0;",
                            f"c->xmm[{di}].u32[0] = {v};")]
            v = f"rd64({ea(s)})" if s.kind == 'mem' else f"c->xmm[{_xmm(s)}].u64[0]"
            return [blk(f"c->xmm[{di}].u64[0] = {v}; c->xmm[{di}].u64[1] = 0;")]
        si = _xmm(s)  # the source must be xmm or memory
        if mn == 'movd':
            return [store(d, 4, f"c->xmm[{si}].u32[0]")]
        if d.kind == 'mem':
            return [f"wr64({ea(d)}, c->xmm[{si}].u64[0]);"]
        return [f"c->xmm[{_xmm(d)}].u64[0] = c->xmm[{si}].u64[0];"]
    if mn in ('xorps', 'xorpd', 'andps', 'andpd', 'andnps', 'andnpd', 'orps', 'orpd',
              'pxor', 'pand', 'pandn', 'por'):
        d, s = ops
        di = _xmm(d)
        op = {'xor': '^', 'and': '&', 'or': '|'}
        if mn.startswith('andn') or mn == 'pandn':
            lines = [f"vt_xmm t = {_x128src(s)};",
                     f"c->xmm[{di}].u64[0] = ~c->xmm[{di}].u64[0] & t.u64[0];",
                     f"c->xmm[{di}].u64[1] = ~c->xmm[{di}].u64[1] & t.u64[1];"]
        else:
            o = {'xorps': '^', 'xorpd': '^', 'pxor': '^', 'andps': '&', 'andpd': '&', 'pand': '&',
                 'orps': '|', 'orpd': '|', 'por': '|'}[mn]
            lines = [f"vt_xmm t = {_x128src(s)};",
                     f"c->xmm[{di}].u64[0] {o}= t.u64[0];",
                     f"c->xmm[{di}].u64[1] {o}= t.u64[1];"]
        return [blk(*lines)]
    if mn in ('addss', 'subss', 'mulss', 'divss', 'addsd', 'subsd', 'mulsd', 'divsd'):
        d, s = ops
        di = _xmm(d)
        is64 = mn.endswith('sd')
        oc = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/'}[mn[:3]]
        if is64:
            return [blk(f"double s = {_f64src(s)};", f"c->xmm[{di}].f64[0] = c->xmm[{di}].f64[0] {oc} s;")]
        return [blk(f"float s = {_f32src(s)};", f"c->xmm[{di}].f32[0] = c->xmm[{di}].f32[0] {oc} s;")]
    if mn in ('sqrtss', 'sqrtsd'):
        d, s = ops
        di = _xmm(d)
        if mn == 'sqrtsd':
            return [f"c->xmm[{di}].f64[0] = sqrt({_f64src(s)});"]
        return [f"c->xmm[{di}].f32[0] = sqrtf({_f32src(s)});"]
    if mn in ('minss', 'maxss', 'minsd', 'maxsd'):
        d, s = ops
        di = _xmm(d)
        cmp_op = '<' if mn.startswith('min') else '>'
        if mn.endswith('sd'):
            return [blk(f"double s = {_f64src(s)};", f"double t = c->xmm[{di}].f64[0];",
                        f"c->xmm[{di}].f64[0] = (t {cmp_op} s) ? t : s;")]
        return [blk(f"float s = {_f32src(s)};", f"float t = c->xmm[{di}].f32[0];",
                    f"c->xmm[{di}].f32[0] = (t {cmp_op} s) ? t : s;")]
    if mn in ('ucomiss', 'comiss', 'ucomisd', 'comisd'):
        d, s = ops
        if mn.endswith('sd'):
            return [blk(f"uint32_t f = vt_fcmp_d(c->xmm[{_xmm(d)}].f64[0], {_f64src(s)});", *_eflags_from_f())]
        return [blk(f"uint32_t f = vt_fcmp_d(c->xmm[{_xmm(d)}].f32[0], {_f32src(s)});", *_eflags_from_f())]
    if mn in ('cvtsi2ss', 'cvtsi2sd'):
        d, s = ops
        di = _xmm(d)
        if mn == 'cvtsi2ss':
            return [f"c->xmm[{di}].f32[0] = (float)(int32_t)({val(s, 4)});"]
        return [f"c->xmm[{di}].f64[0] = (double)(int32_t)({val(s, 4)});"]
    if mn in ('cvttss2si', 'cvttsd2si', 'cvtss2si', 'cvtsd2si'):
        d, s = ops
        src = _f64src(s) if 'sd2si' in mn else _f32src(s)
        trunc = 1 if mn.startswith('cvtt') else 0
        return [blk(f"double s = (double)({src});", store(d, 4, f"vt_cvt_i32(s, {trunc})"))]
    if mn == 'cvtss2sd':
        d, s = ops
        return [f"c->xmm[{_xmm(d)}].f64[0] = (double){_f32src(s)};"]
    if mn == 'cvtsd2ss':
        d, s = ops
        return [f"c->xmm[{_xmm(d)}].f32[0] = (float){_f64src(s)};"]
    raise Unsupported(mn)


# ---- MMX and SSE2 integer ops. The lane arithmetic is in rt/vt3simd.c. -------
PKOPS = {
    'paddb': ('VT_PK_ADD', 8), 'paddw': ('VT_PK_ADD', 16), 'paddd': ('VT_PK_ADD', 32), 'paddq': ('VT_PK_ADD', 64),
    'psubb': ('VT_PK_SUB', 8), 'psubw': ('VT_PK_SUB', 16), 'psubd': ('VT_PK_SUB', 32), 'psubq': ('VT_PK_SUB', 64),
    'paddsb': ('VT_PK_ADDS', 8), 'paddsw': ('VT_PK_ADDS', 16),
    'paddusb': ('VT_PK_ADDUS', 8), 'paddusw': ('VT_PK_ADDUS', 16),
    'psubsb': ('VT_PK_SUBS', 8), 'psubsw': ('VT_PK_SUBS', 16),
    'psubusb': ('VT_PK_SUBUS', 8), 'psubusw': ('VT_PK_SUBUS', 16),
    'pmullw': ('VT_PK_MULLO', 16), 'pmulhw': ('VT_PK_MULHI', 16), 'pmulhuw': ('VT_PK_MULHIU', 16),
    'pavgb': ('VT_PK_AVG', 8), 'pavgw': ('VT_PK_AVG', 16),
    'pmaxsw': ('VT_PK_MAX', 16), 'pmaxub': ('VT_PK_MAXU', 8),
    'pminsw': ('VT_PK_MIN', 16), 'pminub': ('VT_PK_MINU', 8),
    'pcmpeqb': ('VT_PK_EQ', 8), 'pcmpeqw': ('VT_PK_EQ', 16), 'pcmpeqd': ('VT_PK_EQ', 32),
    'pcmpgtb': ('VT_PK_GT', 8), 'pcmpgtw': ('VT_PK_GT', 16), 'pcmpgtd': ('VT_PK_GT', 32),
    'pand': ('VT_PK_AND', 64), 'pandn': ('VT_PK_ANDN', 64), 'por': ('VT_PK_OR', 64), 'pxor': ('VT_PK_XOR', 64),
}
PKSHIFT = {'psllw': (0, 16), 'pslld': (0, 32), 'psllq': (0, 64),
           'psrlw': (1, 16), 'psrld': (1, 32), 'psrlq': (1, 64),
           'psraw': (2, 16), 'psrad': (2, 32)}
PKUNPACK = {'punpcklbw': (8, 0), 'punpcklwd': (16, 0), 'punpckldq': (32, 0), 'punpcklqdq': (64, 0),
            'punpckhbw': (8, 1), 'punpckhwd': (16, 1), 'punpckhdq': (32, 1), 'punpckhqdq': (64, 1)}
PKPACK = {'packssdw': 0, 'packsswb': 1, 'packuswb': 2}
CMP_PRED = {'eq': 0, 'lt': 1, 'le': 2, 'unord': 3, 'neq': 4, 'nlt': 5, 'nle': 6, 'ord': 7}


def _vreg(op):
    """('xmm', i) or ('mm', i) for a register operand, None for memory."""
    if op.kind == 'reg':
        if op.reg.startswith('xmm'):
            return ('xmm', int(op.reg[3:]))
        if op.reg.startswith('mm'):
            return ('mm', int(op.reg[2:]))
        raise Unsupported(f"register {op.reg}")
    return None


def _vkind(op):
    """Like _vreg, but None for any register that is not MMX or XMM."""
    if op.kind == 'reg':
        if op.reg.startswith('xmm'):
            return ('xmm', int(op.reg[3:]))
        if op.reg.startswith('mm'):
            return ('mm', int(op.reg[2:]))
    return None


def _width(op):
    """Operand width in bytes for a packed register or memory operand: 8 for MMX, 16 for XMM."""
    r = _vreg(op)
    if r is not None:
        return 8 if r[0] == 'mm' else 16
    return 16


def _vload(op, nb):
    """C expression for an operand as vt_xmm. Memory loads nb bytes."""
    r = _vreg(op)
    if r is None:
        return f"vt_ldx({ea(op)})" if nb == 16 else f"vt_ldmm({ea(op)})"
    if r[0] == 'xmm':
        return f"c->xmm[{r[1]}]"
    return f"vt_mmv(c->mm[{r[1]}])"


def _vstore(op, nb, var):
    r = _vreg(op)
    if r is None:
        raise Unsupported("packed store to memory")
    if r[0] == 'xmm':
        return f"c->xmm[{r[1]}] = {var};"
    return f"c->mm[{r[1]}] = {var}.u64[0];"


def simd_or_misc(ins):
    """Statements for the MMX/SSE2 integer forms and single-instruction gaps, or None."""
    mn, ops = ins.mn, ins.ops
    if mn in PKOPS:
        op, bits = PKOPS[mn]
        d, s = ops
        nb = _width(d)
        return [blk(f"vt_xmm dv = {_vload(d, nb)};", f"vt_xmm sv = {_vload(s, nb)};",
                    f"vt_pbinop({op}, {bits}, &dv, &sv, {nb});", _vstore(d, nb, 'dv'))]
    if mn in PKSHIFT:
        kind, bits = PKSHIFT[mn]
        d, s = ops
        nb = _width(d)
        if s.kind == 'imm':
            cnt = f"{s.imm & 0xFF}ull"
        else:
            r = _vreg(s)
            if r is None:
                cnt = f"rd64({ea(s)})"
            elif r[0] == 'mm':
                cnt = f"c->mm[{r[1]}]"
            else:
                cnt = f"c->xmm[{r[1]}].u64[0]"
        return [blk(f"vt_xmm dv = {_vload(d, nb)};",
                    f"vt_pshift({kind}, {bits}, &dv, (uint64_t){cnt}, {nb});", _vstore(d, nb, 'dv'))]
    if mn in PKUNPACK:
        bits, high = PKUNPACK[mn]
        d, s = ops
        nb = _width(d)
        return [blk(f"vt_xmm dv = {_vload(d, nb)};", f"vt_xmm sv = {_vload(s, nb)};",
                    f"vt_punpck({bits}, {high}, &dv, &sv, {nb});", _vstore(d, nb, 'dv'))]
    if mn in PKPACK:
        d, s = ops
        nb = _width(d)
        return [blk(f"vt_xmm dv = {_vload(d, nb)};", f"vt_xmm sv = {_vload(s, nb)};",
                    f"vt_ppack({PKPACK[mn]}, &dv, &sv, {nb});", _vstore(d, nb, 'dv'))]
    if mn == 'pshufw':
        d, s, im = ops
        return [blk(f"vt_xmm dv; vt_xmm sv = {_vload(s, 8)};", f"vt_pshufw(&dv, &sv, {im.imm & 0xFF});",
                    _vstore(d, 8, 'dv'))]
    if mn in ('pshuflw', 'pshufhw'):
        d, s, im = ops
        return [blk(f"vt_xmm dv; vt_xmm sv = {_vload(s, 16)};",
                    f"vt_pshufhalf(&dv, &sv, {im.imm & 0xFF}, {1 if mn == 'pshufhw' else 0});",
                    _vstore(d, 16, 'dv'))]
    if mn == 'pmaddwd':
        d, s = ops
        nb = _width(d)
        return [blk(f"vt_xmm dv = {_vload(d, nb)};", f"vt_xmm sv = {_vload(s, nb)};",
                    f"vt_pmaddwd(&dv, &sv, {nb});", _vstore(d, nb, 'dv'))]
    if mn == 'pextrw':
        d, s, im = ops
        nb = _width(s)
        return [blk(f"vt_xmm sv = {_vload(s, nb)};",
                    store(d, 4, f"vt_pextrw(&sv, {im.imm & 0xFF}, {nb})"))]
    if mn == 'pinsrw':
        d, s, im = ops
        nb = _width(d)
        v = val(s, 4) if s.kind == 'reg' else val(s, 2)
        return [blk(f"vt_xmm dv = {_vload(d, nb)};", f"vt_pinsrw(&dv, {v}, {im.imm & 0xFF}, {nb});",
                    _vstore(d, nb, 'dv'))]
    if mn == 'pmovmskb':
        d, s = ops
        nb = _width(s)
        return [blk(f"vt_xmm sv = {_vload(s, nb)};", store(d, 4, f"vt_pmovmskb(&sv, {nb})"))]
    if mn in ('movd', 'movq') and any(_vkind(o) and _vkind(o)[0] == 'mm' for o in ops):
        d, s = ops
        kd, ks = _vkind(d), _vkind(s)
        if kd and kd[0] == 'mm':
            if ks and ks[0] == 'mm':
                src = f"c->mm[{ks[1]}]"
            elif s.kind == 'mem':
                src = f"rd64({ea(s)})" if mn == 'movq' else f"rd32({ea(s)})"
            else:
                src = val(s, 4)
            return [f"c->mm[{kd[1]}] = (uint64_t)({src});"]
        if mn == 'movd':
            return [store(d, 4, f"(uint32_t)c->mm[{ks[1]}]")]
        if d.kind == 'mem':
            return [f"wr64({ea(d)}, c->mm[{ks[1]}]);"]
        return [f"c->xmm[{kd[1]}].u64[0] = c->mm[{ks[1]}]; c->xmm[{kd[1]}].u64[1] = 0;"]
    if mn in ('cvtps2pi', 'cvttps2pi'):
        d, s = ops
        trunc = 1 if mn.startswith('cvtt') else 0
        return [blk(f"vt_xmm sv = {_vload(s, 16)};",
                    f"c->mm[{_vreg(d)[1]}] = (uint64_t)vt_cvt_i32((double)sv.f32[0], {trunc}) | "
                    f"((uint64_t)vt_cvt_i32((double)sv.f32[1], {trunc}) << 32);")]
    if mn == 'cvtpi2ps':
        d, s = ops
        return [blk(f"vt_xmm sv = {_vload(s, 8)};", f"c->xmm[{_vreg(d)[1]}].f32[0] = (float)(int32_t)sv.u32[0];",
                    f"c->xmm[{_vreg(d)[1]}].f32[1] = (float)(int32_t)sv.u32[1];")]
    m = re.match(r'^cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ps|pd|ss|sd)$', mn)
    if m:
        d, s = ops
        pred = CMP_PRED[m.group(1)]
        dbl = 1 if m.group(2)[1] == 'd' else 0
        scalar = 1 if m.group(2)[0] == 's' else 0
        return [blk(f"vt_xmm dv = c->xmm[{_xmm(d)}];", f"vt_xmm sv = {_vload(s, 16)};",
                    f"vt_cmpf({pred}, {dbl}, {scalar}, &dv, &sv);", f"c->xmm[{_xmm(d)}] = dv;")]
    if mn in ('rcpss', 'rsqrtss'):
        d, s = ops
        f = _f32src(s)
        expr = f"1.0f / {f}" if mn == 'rcpss' else f"1.0f / sqrtf({f})"
        return [f"c->xmm[{_xmm(d)}].f32[0] = {expr};"]
    if mn == 'int3':
        return [f"vt_die_at(0x{ins.addr:x}u, 3u);"]
    if mn == 'cpuid':
        return ["VT_SPILL(); vt_cpuid(c); VT_RELOAD();"]
    if mn == 'xlatb':
        return [blk("uint32_t t = rd8(ebx + (eax & 0xFFu));", "eax = (eax & 0xFFFFFF00u) | (t & 0xFFu);")]
    if mn == 'stmxcsr':
        return [f"wr32({ea(ops[0])}, 0x1F80u);"]
    if mn == 'ldmxcsr':
        return []
    if mn in ('rcl', 'rcr'):
        d, s = ops
        sz = size_of(d)
        bits = 8 * sz
        m = MASK[sz]
        n_mod = "(cnt_raw & 0x1Fu) % 9u" if bits == 8 else ("(cnt_raw & 0x1Fu) % 17u" if bits == 16
                                                          else "(cnt_raw & 0x1Fu)")
        if mn == 'rcl':
            step = f"uint32_t t = (a >> {bits - 1}) & 1u; a = ((a << 1) | cf) & 0x{m:x}u; cf = t;"
            of_expr = f"((a >> {bits - 1}) & 1u) ^ cf"
        else:
            step = f"uint32_t t = a & 1u; a = (a >> 1) | (cf << {bits - 1}); cf = t;"
            of_expr = f"((a >> {bits - 1}) & 1u) ^ ((a >> {bits - 2}) & 1u)"
        return [blk(f"uint32_t a = {val(d, sz)};", f"uint32_t cnt_raw = {val(s, 1)};",
                    f"uint32_t n = {n_mod};",
                    f"for (uint32_t i = 0; i < n; i++) {{ {step} }}",
                    "if (n == 1u) { of = " + of_expr + " & 1u; }",
                    store(d, sz, 'a'))]
    if mn == 'fnstenv' or mn == 'fstenv':
        return [f"vt_fnstenv(c, {ea(ops[0])});"]
    if mn == 'fldenv':
        return [f"vt_fldenv(c, {ea(ops[0])});"]
    if mn in ('fnsave', 'fsave'):
        return [f"vt_fnsave(c, {ea(ops[0])});"]
    if mn == 'frstor':
        return [f"vt_frstor(c, {ea(ops[0])});"]
    return None


PACKED_ARITH = {
    'addps': ('f32', 4, '+'), 'subps': ('f32', 4, '-'), 'mulps': ('f32', 4, '*'), 'divps': ('f32', 4, '/'),
    'addpd': ('f64', 2, '+'), 'subpd': ('f64', 2, '-'), 'mulpd': ('f64', 2, '*'), 'divpd': ('f64', 2, '/'),
    'paddd': ('u32', 4, '+'), 'psubd': ('u32', 4, '-'), 'paddq': ('u64', 2, '+'), 'psubq': ('u64', 2, '-'),
    'paddw': ('u16', 8, '+'), 'psubw': ('u16', 8, '-'), 'paddb': ('b', 16, '+'), 'psubb': ('b', 16, '-'),
}
PACKED_MN = set(PACKED_ARITH) | {
    'unpcklps', 'unpckhps', 'unpcklpd', 'unpckhpd', 'punpckldq', 'punpckhdq', 'movlhps', 'movhlps',
    'shufps', 'shufpd', 'pshufd', 'cvttps2dq', 'cvtdq2ps', 'movhps', 'movlps', 'movhpd', 'movlpd',
    'cvtdq2pd', 'cvttpd2dq', 'cvtps2pd', 'cvtpd2ps',
}


def _packed(ins):
    mn, ops = ins.mn, ins.ops
    if mn in PACKED_ARITH:
        t, n, oc = PACKED_ARITH[mn]
        d, s = ops
        di = _xmm(d)
        return [blk(f"vt_xmm s = {_x128src(s)};",
                    f"for (int i = 0; i < {n}; i++) c->xmm[{di}].{t}[i] = c->xmm[{di}].{t}[i] {oc} s.{t}[i];")]
    if mn in ('unpcklps', 'punpckldq', 'unpckhps', 'punpckhdq'):
        d, s = ops
        di = _xmm(d)
        base = 0 if mn in ('unpcklps', 'punpckldq') else 2
        return [blk(f"vt_xmm s = {_x128src(s)};", "vt_xmm r;",
                    f"r.u32[0] = c->xmm[{di}].u32[{base}]; r.u32[1] = s.u32[{base}];",
                    f"r.u32[2] = c->xmm[{di}].u32[{base + 1}]; r.u32[3] = s.u32[{base + 1}];",
                    f"c->xmm[{di}] = r;")]
    if mn in ('unpcklpd', 'unpckhpd'):
        d, s = ops
        di = _xmm(d)
        base = 0 if mn == 'unpcklpd' else 1
        return [blk(f"vt_xmm s = {_x128src(s)};", "vt_xmm r;",
                    f"r.u64[0] = c->xmm[{di}].u64[{base}]; r.u64[1] = s.u64[{base}];",
                    f"c->xmm[{di}] = r;")]
    if mn == 'movlhps':
        d, s = ops
        return [f"c->xmm[{_xmm(d)}].u64[1] = c->xmm[{_xmm(s)}].u64[0];"]
    if mn == 'movhlps':
        d, s = ops
        return [f"c->xmm[{_xmm(d)}].u64[0] = c->xmm[{_xmm(s)}].u64[1];"]
    if mn in ('shufps', 'shufpd', 'pshufd'):
        d, s, im = ops
        di = _xmm(d)
        imm = im.imm & 0xFF
        if mn == 'shufps':
            return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;",
                        f"r.u32[0] = c->xmm[{di}].u32[{imm & 3}];",
                        f"r.u32[1] = c->xmm[{di}].u32[{(imm >> 2) & 3}];",
                        f"r.u32[2] = s.u32[{(imm >> 4) & 3}];",
                        f"r.u32[3] = s.u32[{(imm >> 6) & 3}];", f"c->xmm[{di}] = r;")]
        if mn == 'shufpd':
            return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;",
                        f"r.u64[0] = c->xmm[{di}].u64[{imm & 1}];",
                        f"r.u64[1] = s.u64[{(imm >> 1) & 1}];", f"c->xmm[{di}] = r;")]
        return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;",
                    *[f"r.u32[{i}] = s.u32[{(imm >> (2 * i)) & 3}];" for i in range(4)],
                    f"c->xmm[{di}] = r;")]
    if mn == 'cvttps2dq':
        d, s = ops
        di = _xmm(d)
        return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;",
                    *[f"r.u32[{i}] = vt_cvt_i32((double)s.f32[{i}], 1);" for i in range(4)],
                    f"c->xmm[{di}] = r;")]
    if mn == 'cvtdq2ps':
        d, s = ops
        di = _xmm(d)
        return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;",
                    *[f"r.f32[{i}] = (float)(int32_t)s.u32[{i}];" for i in range(4)],
                    f"c->xmm[{di}] = r;")]
    if mn == 'cvtdq2pd':
        d, s = ops
        di = _xmm(d)
        return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;",
                    "r.f64[0] = (double)(int32_t)s.u32[0]; r.f64[1] = (double)(int32_t)s.u32[1];",
                    f"c->xmm[{di}] = r;")]
    if mn == 'cvttpd2dq':
        d, s = ops
        di = _xmm(d)
        return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;", "r.u64[0] = 0; r.u64[1] = 0;",
                    "r.u32[0] = vt_cvt_i32(s.f64[0], 1); r.u32[1] = vt_cvt_i32(s.f64[1], 1);",
                    f"c->xmm[{di}] = r;")]
    if mn == 'cvtps2pd':
        d, s = ops
        di = _xmm(d)
        return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;",
                    "r.f64[0] = (double)s.f32[0]; r.f64[1] = (double)s.f32[1];", f"c->xmm[{di}] = r;")]
    if mn == 'cvtpd2ps':
        d, s = ops
        di = _xmm(d)
        return [blk(f"vt_xmm s = {_x128src(s)}; vt_xmm r;", "r.u64[0] = 0; r.u64[1] = 0;",
                    "r.f32[0] = (float)s.f64[0]; r.f32[1] = (float)s.f64[1];", f"c->xmm[{di}] = r;")]
    if mn in ('movhps', 'movhpd', 'movlps', 'movlpd'):
        d, s = ops
        half = 1 if mn.startswith('movh') else 0
        if d.kind == 'mem':
            return [f"wr64({ea(d)}, c->xmm[{_xmm(s)}].u64[{half}]);"]
        return [f"c->xmm[{_xmm(d)}].u64[{half}] = rd64({ea(s)});"]
    raise Unsupported(mn)


def _eflags_from_f():
    return ["cf = f & 1u; zf = (f >> 1) & 1u; pf = (f >> 2) & 1u; of = 0u; sf = 0u; af = 0u;"]


# -- x87 (double precision internally; see design note for the gap) ----------

def x87(ins):
    """x87 lowering. Values are 80-bit (rt/vt3fpu.c). Each operation rounds to the
    precision and rounding mode of the live control word, c->fcw."""
    mn, ops = ins.mn, ins.ops
    CW = "c->fcw"

    def sti(op):
        return int(op.reg[3:-1]) if op.reg.startswith('st(') else 0

    def src(op):
        """80-bit value of a memory or st(i) operand."""
        if op.kind == 'mem':
            return f"vt_fld{ {4: 32, 8: 64, 10: 80}[op.size] }({ea(op)})"
        return f"FST({sti(op)})"

    if mn == 'fld':
        o = ops[0]
        return [blk(f"vt_x80 t = {src(o)};", "FPUSH(t);")]
    if mn in ('fst', 'fstp'):
        pop = mn == 'fstp'
        o = ops[0] if ops else None
        if o is None or o.kind == 'reg':
            i = sti(o) if o is not None else 1
            return [blk(f"FST({i}) = FST(0);", "FPOP();" if pop else "")]
        if o.size == 10:
            store = f"vt_fst80({ea(o)}, FST(0));"
        else:
            store = f"vt_fst{ {4: 32, 8: 64}[o.size] }({ea(o)}, FST(0), {CW});"
        return [blk(store, "FPOP();" if pop else "")]
    if mn == 'fild':
        o = ops[0]
        cast, rd = {2: ('int16_t', 'rd16'), 4: ('int32_t', 'rd32'), 8: ('int64_t', 'rd64')}[o.size]
        return [blk(f"vt_x80 t = vt_fild((int64_t)({cast})({rd}({ea(o)})));", "FPUSH(t);")]
    if mn in ('fist', 'fistp', 'fisttp'):
        o = ops[0]
        rc = "3u" if mn == 'fisttp' else "((c->fcw >> 10) & 3u)"
        bits = {2: 16, 4: 32, 8: 64}[o.size]
        wr = {2: 'wr16', 4: 'wr32', 8: 'wr64'}[o.size]
        pop = mn in ('fistp', 'fisttp')
        return [blk(f"int64_t iv = vt_fint(FST(0), {rc}, {bits});", f"{wr}({ea(o)}, (uint64_t)iv);",
                    "FPOP();" if pop else "")]
    arith = {'fadd': 'vt_fadd', 'fsub': 'vt_fsub', 'fsubr': 'vt_fsub', 'fmul': 'vt_fmul',
             'fdiv': 'vt_fdiv', 'fdivr': 'vt_fdiv'}
    base = mn[:-1] if (mn.endswith('p') and mn[:-1] in arith) else mn
    if base in arith:
        pop = base != mn
        reverse = base in ('fsubr', 'fdivr')   # the destination gets src op dst
        if len(ops) == 0:
            dst, sv = 1, "FST(0)"
        elif len(ops) == 1:
            if ops[0].kind == 'mem':
                dst, sv = 0, src(ops[0])
            elif pop:
                dst, sv = sti(ops[0]), "FST(0)"       # DE forms: st(i) op= st(0)
            else:
                dst, sv = 0, f"FST({sti(ops[0])})"    # D8 forms: st(0) op= st(i)
        else:
            dst, sv = sti(ops[0]), src(ops[1])
        fn = arith[base]
        call = f"{fn}(s, d, {CW})" if reverse else f"{fn}(d, s, {CW})"
        return [blk(f"vt_x80 s = {sv};", f"vt_x80 d = FST({dst});", f"FST({dst}) = {call};",
                    "FPOP();" if pop else "")]
    if mn in ('fiadd', 'fimul', 'fisub', 'fisubr', 'fidiv', 'fidivr'):
        o = ops[0]
        cast, rd = {2: ('int16_t', 'rd16'), 4: ('int32_t', 'rd32')}[o.size]
        fn, reverse = {'fiadd': ('vt_fadd', 0), 'fimul': ('vt_fmul', 0), 'fisub': ('vt_fsub', 0),
                       'fisubr': ('vt_fsub', 1), 'fidiv': ('vt_fdiv', 0), 'fidivr': ('vt_fdiv', 1)}[mn]
        sv = f"vt_fild((int64_t)({cast})({rd}({ea(o)})))"
        call = f"{fn}(s, d, {CW})" if reverse else f"{fn}(d, s, {CW})"
        return [blk(f"vt_x80 s = {sv};", "vt_x80 d = FST(0);", f"FST(0) = {call};")]
    if mn in ('fchs', 'fabs', 'fsqrt', 'frndint'):
        f = {'fchs': 'vt_fneg(FST(0))', 'fabs': 'vt_fabs(FST(0))',
             'fsqrt': f'vt_fsqrt(FST(0), {CW})', 'frndint': f'vt_frndint(FST(0), {CW})'}[mn]
        return [f"FST(0) = {f};"]
    if mn == 'fxch':
        i = next((sti(o) for o in ops if sti(o) != 0), 1)
        return [blk("vt_x80 t = FST(0);", f"FST(0) = FST({i});", f"FST({i}) = t;")]
    consts = {'fld1': 'VT_FC_1', 'fldz': 'VT_FC_ZERO', 'fldpi': 'VT_FC_PI', 'fldl2e': 'VT_FC_L2E',
              'fldln2': 'VT_FC_LN2', 'fldlg2': 'VT_FC_LG2', 'fldl2t': 'VT_FC_L2T'}
    if mn in consts:
        return [f"FPUSH(vt_fconst({consts[mn]}));"]
    if mn in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ftst'):
        pops = {'fcomp': 1, 'fucomp': 1, 'fcompp': 2, 'fucompp': 2}.get(mn, 0)
        if mn == 'ftst':
            sv = "vt_fconst(VT_FC_ZERO)"
        elif not ops:
            sv = "FST(1)"
        elif ops[0].kind == 'mem':
            sv = src(ops[0])
        else:
            sv = f"FST({sti(ops[0])})"
        return [blk(f"uint32_t f = vt_fcmp(FST(0), {sv});",
                    "c->fsw = (c->fsw & ~0x4500u) | ((f & 1u) << 8) | (((f >> 2) & 1u) << 10) | (((f >> 1) & 1u) << 14);",
                    *(["FPOP();"] * pops))]
    if mn in ('fcomi', 'fcomip', 'fucomi', 'fucomip', 'fcompi', 'fucompi'):
        pop = mn.endswith('p') or mn.endswith('pi')
        # Two operands: st(0) against st(i). One operand: st(0) against that operand. The
        # one-operand form is fcompi st(i), which is not always st(1).
        if len(ops) > 1:
            sv = f"FST({sti(ops[1])})"
        elif ops:
            sv = f"FST({sti(ops[0])})"
        else:
            sv = "FST(1)"
        return [blk(f"uint32_t f = vt_fcmp(FST(0), {sv});",
                    "cf = f & 1u; zf = (f >> 1) & 1u; pf = (f >> 2) & 1u; of = 0u; sf = 0u; af = 0u;",
                    "FPOP();" if pop else "")]
    if mn == 'fnstsw' or mn == 'fstsw':
        o = ops[0]
        expr = "((c->fsw & 0xC7FFu) | ((c->ftop & 7u) << 11))"
        if o.kind == 'reg':
            return [setreg('ax', expr)]
        return [f"wr16({ea(o)}, {expr});"]
    if mn in ('fldcw',):
        return [f"c->fcw = rd16({ea(ops[0])});"]
    if mn in ('fnstcw', 'fstcw'):
        return [f"wr16({ea(ops[0])}, c->fcw);"]
    if mn in ('fnclex', 'fclex'):
        return ["c->fsw &= ~0x80FFu;"]
    if mn in ('fninit', 'finit'):
        return ["c->fcw = 0x37Fu; c->fsw = 0u; c->ftop = 0u;"]
    if mn == 'fincstp':
        return ["c->ftop = (c->ftop + 1u) & 7u;"]
    if mn == 'fdecstp':
        return ["c->ftop = (c->ftop - 1u) & 7u;"]
    if mn == 'ffree':
        return []
    if mn in ('fsin', 'fcos'):
        op = 'VT_FD_SIN' if mn == 'fsin' else 'VT_FD_COS'
        return [f"FST(0) = vt_fdbl1(FST(0), {CW}, {op});"]
    if mn == 'fptan':
        return [blk(f"FST(0) = vt_fdbl1(FST(0), {CW}, VT_FD_TAN);", "FPUSH(vt_fconst(VT_FC_1));")]
    if mn == 'f2xm1':
        return [f"FST(0) = vt_fdbl1(FST(0), {CW}, VT_FD_F2XM1);"]
    if mn == 'fpatan':
        return [blk("vt_x80 s = FST(0); vt_x80 d = FST(1);",
                    f"FST(1) = vt_fdbl2(d, s, {CW}, VT_FD2_PATAN);", "FPOP();")]
    if mn == 'fyl2x':
        return [blk(f"FST(1) = vt_fdbl2(FST(0), FST(1), {CW}, VT_FD2_YL2X);", "FPOP();")]
    if mn == 'fscale':
        return [f"FST(0) = vt_fdbl2(FST(0), FST(1), {CW}, VT_FD2_SCALE);"]
    if mn == 'fprem':
        return [f"FST(0) = vt_fdbl2(FST(0), FST(1), {CW}, VT_FD2_PREM); c->fsw &= ~0x400u;"]
    if mn == 'fxam':
        return ["c->fsw = (c->fsw & ~0x4700u) | vt_fxam(FST(0));"]
    raise Unsupported(mn)


# ----------------------------------------------------------------------------
# Module generation
# ----------------------------------------------------------------------------

def gen_function(an, entry, hard):
    """Return (lines, n_insns, unsupported_list) for a function, or raise."""
    seen = an.reach(entry)
    order = sorted(seen)
    gen = FnGen(an, entry)
    out = [f"void {fname(entry)}(vt3_cpu *c) {{", "  VT_LOAD();", "  VT_FLAGS();"]
    # The labels are emitted in address order, but a body can hold blocks below its entry
    # (reached by a jump from the entry). Control must start at the entry, not at order[0].
    if entry not in seen:
        raise TranslateError(f"internal: entry {entry:#x} is not in its own body")
    if order[0] != entry:
        out.append(f"goto L_{entry:x};")
    unsup = []
    for idx, a in enumerate(order):
        ins = seen[a]
        nxt = a + ins.size
        out.append(f"/* {a:08x}: {ins.text} */")
        out.append(f"L_{a:x}: ;")
        try:
            out.extend(gen.gen(ins))
        except Unsupported as e:
            unsup.append((a, str(e), ins.text))
            out.append(f"vt_unsupported_at(0x{a:x}u, \"{ins.text}\");")
        mn = ins.mn
        falls = an.flow(ins)[1]
        if falls:
            if idx + 1 < len(order) and order[idx + 1] == nxt:
                pass
            elif nxt in seen:
                out.append(f"goto L_{nxt:x};")
            elif nxt in an.E:
                out.append(f"{{ VT_SPILL(); {fname(nxt)}(c); return; }}")
            else:
                raise TranslateError(f"internal: flow to {nxt:#x} from {a:#x} not covered")
        elif mn == 'call':
            # The path ends at this call (see Analysis.flow). If the callee does return,
            # the code after it is not translated, so stop loudly instead of falling through.
            out.append(f'vt_unsupported_at(0x{nxt:x}u, "returned from a call that does not return");')
    out.append("}")
    return out, len(order), unsup


def write_module(img, an, out_dir, name, allow_unsup, log):
    fns = sorted(an.E)
    body, unsup_hard, ninsn, stubs = [], [], 0, 0
    mnemonics = Counter()
    for entry in fns:
        hard = an.E[entry]
        try:
            lines, n, unsup = gen_function(an, entry, hard)
            ninsn += n
            for a, why, text in unsup:
                mnemonics[why] += 1
                if hard:
                    unsup_hard.append((entry, a, text))
            body.extend(lines)
        except (DecodeError, TranslateError) as e:
            stubs += 1
            if hard:
                an.failed[entry] = str(e)
            body.extend([f"void {fname(entry)}(vt3_cpu *c) {{",
                         f"  vt_die_at(0x{entry:x}u, 0u); /* entry did not decode: {e} */",
                         "}"])
    decl = [f"static void {fname(a)}(vt3_cpu *c);" for a in fns]
    fn_tab = [f"  {{0x{a:08x}u, {fname(a)}, \"sub_{a:08x}\"}}," for a in fns]
    imp_tab = [f"  {{0x{slot:08x}u, \"{dll}\", \"{nm}\"}}," for slot, dll, nm in img.imports]
    head = [
        "/* Generated by vt3-port tools/recomp/recomp.py. Do not commit: derived from a game binary. */",
        "#include \"vt3rt.h\"",
        "",
    ] + decl + [""]
    tail = [
        "",
        "static const vt3_fn vt3_fn_tab[] = {",
        *fn_tab,
        "};",
        "static const vt3_import vt3_imp_tab[] = {",
        *imp_tab,
        "};",
        "const vt3_module vt3_mod = {",
        f"  .name = \"{name}\",",
        f"  .image_base = 0x{img.base:08x}u,",
        f"  .image_size = 0x{img.size:08x}u,",
        f"  .entry = 0x{img.entry:08x}u,",
        "  .image_file = \"image.bin\",",
        "  .fns = vt3_fn_tab, .nfns = sizeof vt3_fn_tab / sizeof vt3_fn_tab[0],",
        "  .imps = vt3_imp_tab, .nimps = sizeof vt3_imp_tab / sizeof vt3_imp_tab[0],",
        "};",
        "",
    ]
    c_path = os.path.join(out_dir, f"{name}.c")
    with open(c_path, 'w') as f:
        f.write("\n".join(head + body + tail))
    with open(os.path.join(out_dir, 'image.bin'), 'wb') as f:
        f.write(bytes(img.mem))
    return c_path, ninsn, stubs, unsup_hard, mnemonics


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('--out', required=True)
    ap.add_argument('--name', default=None)
    ap.add_argument('--allow-unsupported', action='store_true',
                    help='emit traps for unsupported instructions and exit 0')
    ap.add_argument('--seeds', default=None,
                    help='file of function entries, one per "// ==== NAME @ ADDR" header (Ghidra dump)')
    ap.add_argument('--table-sizes', default=None,
                    help='file of "JUMP_ADDR COUNT [# source]" lines for jump tables whose size is known')
    ap.add_argument('--no-pointer-scan', action='store_true',
                    help='do not add code addresses found in immediates and data')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args(argv)

    def log(msg):
        if args.verbose or msg.startswith('warning') or msg.startswith('error'):
            print(msg, file=sys.stderr)

    os.makedirs(args.out, exist_ok=True)
    name = args.name or os.path.splitext(os.path.basename(args.image))[0]
    name = ''.join(ch if ch.isalnum() else '_' for ch in name)
    img = Image(args.image)
    dec = Decoder(img)
    sizes = {}
    if args.table_sizes:
        with open(args.table_sizes) as f:
            for line in f:
                body = line.split('#')[0].split()
                if len(body) == 2:
                    sizes[int(body[0], 16)] = int(body[1])
    an = Analysis(img, dec, log, table_sizes=sizes)
    seeds = []
    if args.seeds:
        with open(args.seeds, errors='replace') as f:
            for line in f:
                m = re.match(r'^// ==== .* @ ([0-9a-fA-F]{8})\s*$', line)
                if m:
                    seeds.append(int(m.group(1), 16))
        print(f"seeds: {len(seeds)} entries from {args.seeds}")
    an.build_entries(img.reloc_sites, seeds, scan_pointers=not args.no_pointer_scan)
    for i, (ncand, nvalid, nadded) in enumerate(getattr(an, 'pointer_stats', [])):
        print(f"pointer scan round {i + 1}: {ncand} candidates, {nvalid} accepted, {nadded} new entries")
    if an.soft_bad:
        log(f"note: {len(an.soft_bad)} soft entries do not scan cleanly")
    try:
        c_path, ninsn, stubs, unsup_hard, mnemonics = write_module(img, an, args.out, name,
                                                                   args.allow_unsupported, log)
    except TranslateError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    hard_fns = sum(1 for v in an.E.values() if v)
    print(f"functions: {len(an.E)} ({hard_fns} hard, {len(an.E) - hard_fns} soft, {stubs} soft stubs)")
    print(f"instructions emitted: {ninsn}")
    print(f"jump tables: {sum(1 for t in an.tables.values() if t)} "
          f"({sum(1 for t in an.tables.values() if t and t.bad)} rejected)")
    print(f"output: {c_path}")
    if mnemonics:
        print("unsupported (soft and hard):", dict(mnemonics.most_common()))
    if an.failed:
        print(f"failed entries (trapped at run time): {len(an.failed)}")
        for entry, why in list(an.failed.items())[:10]:
            print(f"  {entry:#x}: {why}", file=sys.stderr)
    if unsup_hard:
        for entry, a, text in unsup_hard[:40]:
            print(f"error: hard function {entry:#x}: unsupported at {a:#x}: {text}", file=sys.stderr)
        if not args.allow_unsupported:
            return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
