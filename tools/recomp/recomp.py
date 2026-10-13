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


def val(op, sz):
    """C expression for the operand's value, as uint32_t, masked to its width."""
    if op.kind == 'imm':
        return f"0x{op.imm & MASK[sz]:x}u"
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


def is_jcc(mn):
    return mn[0] == 'j' and mn[1:] in CC


# ----------------------------------------------------------------------------
# Analysis: function entries, jump tables, reachability
# ----------------------------------------------------------------------------

class JTable:
    __slots__ = ('addr', 'table', 'count', 'targets', 'heuristic')

    def __init__(self, addr, table, count, targets, heuristic):
        self.addr, self.table, self.count = addr, table, count
        self.targets, self.heuristic = targets, heuristic


class Analysis:
    def __init__(self, img, dec, log):
        self.img = img
        self.dec = dec
        self.log = log
        self.E = {}           # function entry -> hard?
        self.tables = {}      # indirect jump addr -> JTable or None
        self.bad_tables = set()
        self.table_slots = set()
        self.soft_bad = set()

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
        """Find 'cmp idx,N ; ja/jae default' immediately before end."""
        for seq in self._windows(end):
            if len(seq) < 2:
                continue
            j, c = seq[-1], seq[-2]
            if j.mn not in ('ja', 'jae', 'jnb'):
                continue
            if c.mn != 'cmp' or len(c.ops) != 2:
                continue
            if c.ops[0].kind != 'reg' or c.ops[0].reg != idx_reg or c.ops[1].kind != 'imm':
                continue
            n = c.ops[1].imm & M32
            return n + 1 if j.mn == 'ja' else n
        return None

    def table_for(self, ins):
        """Return a JTable for an indirect jump, or None if it is not a table jump."""
        if ins.addr in self.tables:
            return self.tables[ins.addr]
        op = ins.ops[0]
        res = None
        if op.kind == 'mem' and op.index and not op.base and op.scale == 4:
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

    def _make_table(self, jaddr, table, idx_reg, bound_end):
        count = self._bound_before(bound_end, idx_reg)
        heuristic = count is None
        if heuristic:
            count = 0
            while count < 1024 and self.img.in_image(table + 4 * count) and \
                    self.img.is_exec(self.img.read32(table + 4 * count)):
                count += 1
            self.log(f"warning: jump at {jaddr:#x}: no bound found, heuristic count {count}")
        targets = []
        try:
            for i in range(count):
                v = self.img.read32(table + 4 * i)
                if not self.img.is_exec(v):
                    raise DecodeError(jaddr, f"table entry {i} -> {v:#x} not code")
                if v not in targets:
                    targets.append(v)
        except DecodeError:
            self.bad_tables.add(jaddr)
            raise
        for i in range(count):
            self.table_slots.add(table + 4 * i)
        return JTable(jaddr, table, count, sorted(targets), heuristic)

    def flow(self, ins):
        """(targets, falls_through) for one instruction's control flow."""
        mn = ins.mn
        if mn in RET_MN or mn in TERMINAL:
            return [], False
        if mn == 'jmp':
            op = ins.ops[0]
            if op.kind == 'imm':
                return [op.imm & M32], False
            if ins.addr in self.bad_tables:
                raise DecodeError(ins.addr, "bad jump table")
            tb = self.table_for(ins)
            if tb is not None:
                return list(tb.targets), False
            return [], False  # indirect tail jump: codegen emits vt_icall
        if is_jcc(mn) or mn in LOOPS or mn in ('jecxz', 'jcxz'):
            return [ins.ops[0].imm & M32], True
        if mn == 'call':
            # Calls that never return end the path. A fallthrough that is not
            # code (padding after a noreturn call) also ends it.
            if self.is_noreturn_call(ins):
                return [], False
            if self.dec.at(ins.addr + ins.size) is None:
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
        except DecodeError as e:
            if hard:
                raise TranslateError(f"hard function {f:#x}: {e}") from e
            self.soft_bad.add(f)

    def closure(self, roots):
        work = []
        for a, hard in roots:
            self._add(work, a, hard)
        while work:
            f = work.pop()
            self._scan(f, self.E[f], work)

    def build_entries(self, reloc_sites):
        hard_roots = [(self.img.entry, True)] + [(a, True) for a, _ in self.img.exports]
        self.closure(hard_roots)
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

    # -- codegen-time reachability ------------------------------------------

    def reach(self, entry):
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
        order = sorted(seen)
        for x, y in zip(order, order[1:]):
            if x + seen[x].size > y:
                raise DecodeError(y, f"overlaps instruction at {x:#x}")
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
            return [blk(f"uint32_t f = vt_fcmp(c->xmm[{_xmm(d)}].f64[0], {_f64src(s)});", *_eflags_from_f())]
        return [blk(f"uint32_t f = vt_fcmp(c->xmm[{_xmm(d)}].f32[0], {_f32src(s)});", *_eflags_from_f())]
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
    mn, ops = ins.mn, ins.ops

    def sti(op):
        return int(op.reg[3:-1]) if op.reg.startswith('st(') else 0

    if mn in ('fld',):
        o = ops[0]
        if o.kind == 'mem':
            fn = {4: 'vt_ldf32', 8: 'vt_ldf64', 10: 'vt_ldf80'}[o.size]
            return [blk(f"double t = {fn}({ea(o)});", "FPUSH(t);")]
        return [blk(f"double t = FST({sti(o)});", "FPUSH(t);")]
    if mn in ('fst', 'fstp'):
        o = ops[0] if ops else None
        pop = mn == 'fstp'
        if o is None or o.kind == 'reg':
            i = sti(o) if o is not None else 1
            return [blk(f"FST({i}) = FST(0);", "FPOP();" if pop else "")]
        fn = {4: 'vt_stf32', 8: 'vt_stf64', 10: 'vt_stf80'}[o.size]
        return [blk(f"{fn}({ea(o)}, FST(0));", "FPOP();" if pop else "")]
    if mn in ('fild',):
        o = ops[0]
        cast = {2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}[o.size]
        rdf = {2: 'rd16', 4: 'rd32', 8: 'rd64'}[o.size]
        return [blk(f"double t = (double)({cast})({rdf}({ea(o)}));", "FPUSH(t);")]
    if mn in ('fist', 'fistp', 'fisttp'):
        o = ops[0]
        # fist/fistp round per the RC field of the control word. gcc switches RC
        # to truncate around (int) casts. fisttp always truncates.
        rc = "3u" if mn == 'fisttp' else "((c->fcw >> 10) & 3u)"
        bits = {2: 16, 4: 32, 8: 64}[o.size]
        wr = {2: 'wr16', 4: 'wr32', 8: 'wr64'}[o.size]
        pop = mn in ('fistp', 'fisttp')
        return [blk(f"int64_t iv = vt_fint(FST(0), {rc}, {bits});", f"{wr}({ea(o)}, (uint64_t)iv);",
                    "FPOP();" if pop else "")]
    if mn in ('fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr', 'faddp', 'fsubp', 'fsubrp',
              'fmulp', 'fdivp', 'fdivrp'):
        pop = mn.endswith('p')
        base = mn[:-1] if pop else mn
        oc = {'fadd': '+', 'fsub': '-', 'fsubr': 'r-', 'fmul': '*', 'fdiv': '/', 'fdivr': 'r/'}[base]
        if len(ops) == 0:
            dst, src = 1, None
        elif len(ops) == 1:
            # One operand. Memory or st(i) without a pop: ST(0) op= src (D8 form).
            # st(i) with a pop: ST(i) op= ST(0) (DE form, operand is the destination).
            if ops[0].kind == 'mem':
                dst, src = 0, ops[0]
            elif pop:
                dst, src = sti(ops[0]), None
            else:
                dst, src = 0, ops[0]
        else:
            dst = sti(ops[0])
            src = ops[1]
        if src is None:
            sv = "FST(0)"
        elif src.kind == 'mem':
            sv = f"{'vt_ldf32' if src.size == 4 else 'vt_ldf64'}({ea(src)})"
        else:
            sv = f"FST({sti(src)})"
        if oc.startswith('r'):
            expr = f"s {oc[1:]} d"
        else:
            expr = f"d {oc} s"
        return [blk(f"double s = {sv};", f"double d = FST({dst});", f"FST({dst}) = {expr};",
                    "FPOP();" if pop else "")]
    if mn in ('fchs', 'fabs', 'fsqrt', 'frndint', 'fxch', 'fld1', 'fldz', 'fldpi'):
        if mn == 'fchs':
            return ["FST(0) = -FST(0);"]
        if mn == 'fabs':
            return ["FST(0) = fabs(FST(0));"]
        if mn == 'fsqrt':
            return ["FST(0) = sqrt(FST(0));"]
        if mn == 'frndint':
            return ["FST(0) = vt_round(FST(0), (c->fcw >> 10) & 3u);"]
        if mn == 'fxch':
            # capstone lists fxch as (st(0), st(i)). The swapped slot is the non-zero one.
            i = next((sti(o) for o in ops if sti(o) != 0), 1)
            return [blk(f"double t = FST(0);", f"FST(0) = FST({i});", f"FST({i}) = t;")]
        const = {'fld1': '1.0', 'fldz': '0.0', 'fldpi': '3.14159265358979323846'}[mn]
        return [blk(f"double t = {const};", "FPUSH(t);")]
    if mn in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ftst'):
        pops = {'fcomp': 1, 'fucomp': 1, 'fcompp': 2, 'fucompp': 2}.get(mn, 0)
        if mn == 'ftst':
            sv = "0.0"
        elif not ops:
            sv = "FST(1)"
        elif ops[0].kind == 'mem':
            sv = f"{'vt_ldf32' if ops[0].size == 4 else 'vt_ldf64'}({ea(ops[0])})"
        else:
            sv = f"FST({sti(ops[0])})"
        return [blk(f"uint32_t f = vt_fcmp(FST(0), {sv});",
                    "c->fsw = (c->fsw & ~0x4500u) | ((f & 1u) << 8) | (((f >> 2) & 1u) << 10) | (((f >> 1) & 1u) << 14);",
                    *(["FPOP();"] * pops))]
    if mn in ('fcomi', 'fcomip', 'fucomi', 'fucomip', 'fcompi', 'fucompi'):
        pop = mn.endswith('p') or mn.endswith('pi')
        sv = f"FST({sti(ops[1])})" if len(ops) > 1 else "FST(1)"
        return [blk(f"uint32_t f = vt_fcmp(FST(0), {sv});", *_eflags_from_f(), "FPOP();" if pop else "")]
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
            if hard:
                raise TranslateError(str(e))
            stubs += 1
            body.extend([f"void {fname(entry)}(vt3_cpu *c) {{",
                         f"  vt_die_at(0x{entry:x}u, 0u); /* soft entry did not decode: {e} */",
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
    an = Analysis(img, dec, log)
    an.build_entries(img.reloc_sites)
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
          f"({sum(1 for t in an.tables.values() if t and t.heuristic)} heuristic)")
    print(f"output: {c_path}")
    if mnemonics:
        print("unsupported (soft and hard):", dict(mnemonics.most_common()))
    if unsup_hard:
        for entry, a, text in unsup_hard[:40]:
            print(f"error: hard function {entry:#x}: unsupported at {a:#x}: {text}", file=sys.stderr)
        if not args.allow_unsupported:
            return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
