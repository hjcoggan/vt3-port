#!/usr/bin/env python3
"""Recompiler test runner.

For each test program and build variant:
  1. Compile the test to a PE32 exe with i686-w64-mingw32-gcc (no CRT, kernel32 only).
  2. Run the exe under Wine. Its stdout and exit code are the expected result.
  3. Translate the same exe with recomp.py, compile the C natively for this host,
     and run the result.
  4. Pass only if stdout and the exit code both match.

Build products go under dump/recomp/ (never in the repo).

Usage: run_tests.py [-k SUBSTR] [--keep]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RT = os.path.join(os.path.dirname(HERE), 'rt')
# VT3_RECOMP overrides the translator, for example a committed copy, to check that a new test fails without its rule.
RECOMP = os.environ.get('VT3_RECOMP') or os.path.join(HERE, '..', 'recomp.py')
# VT3_RUNTIME overrides the runtime tree, for example a committed snapshot under dump/recomp/.
REPO_RUNTIME = os.environ.get('VT3_RUNTIME') or os.path.join(HERE, '..', '..', '..', 'runtime')
ROOT = '/Users/heath/Virtua Tennis 3/dump/recomp'
PY = '/Users/heath/Virtua Tennis 3/tools/venv/bin/python'
GCC = 'i686-w64-mingw32-gcc'
CC = 'clang'
WINE = '/Users/heath/Virtua Tennis 3/tools/wine/Wine Devel.app/Contents/Resources/wine/bin/wine'
PREFIX = os.path.join(ROOT, 'wineprefix')
# SDL3 from Homebrew: the runtime's vt3core (platform, renderer, input, audio) links against it,
# as it does in the game build (dump/recomp/vt3_nocd/build_native.py).
SDL = subprocess.run(['brew', '--prefix', 'sdl3'], capture_output=True, text=True, check=True).stdout.strip()
# vt3core carries the Vulkan backend on the desktop too (Homebrew's loader and headers).
VK = subprocess.run(['brew', '--prefix', 'vulkan-loader'], capture_output=True, text=True).stdout.strip() or '/opt/homebrew'
VKH = subprocess.run(['brew', '--prefix', 'vulkan-headers'], capture_output=True, text=True).stdout.strip() or '/opt/homebrew'
SDL_LINK = ['-L', os.path.join(SDL, 'lib'), '-lSDL3', '-Wl,-rpath,' + os.path.join(SDL, 'lib'),
            '-L', os.path.join(VK, 'lib'), '-lvulkan', '-Wl,-rpath,' + os.path.join(VK, 'lib'),
            '-framework', 'OpenGL']

BASE_CFLAGS = ['-fno-stack-protector', '-fno-builtin', '-fno-math-errno', '-fno-tree-loop-distribute-patterns',
               '-fno-asynchronous-unwind-tables', '-fno-ident', '-nostdlib', '-nostartfiles']
BASE_LDFLAGS = ['-Wl,-e,__vt_start', '-lkernel32', '-lgcc']

# Each test has a list of variants: (name, extra compile flags).
VARIANTS = {
    't01_int': [('O0', ['-O0']), ('O2', ['-O2'])],
    't02_flags': [('O0', ['-O0']), ('O2', ['-O2'])],
    't03_loops': [('O0', ['-O0']), ('O2', ['-O2'])],
    't04_switch': [('O0', ['-O0']), ('O2', ['-O2'])],
    't05_recursion': [('O0', ['-O0']), ('O2', ['-O2'])],
    't06_fnptr': [('O0', ['-O0']), ('O2', ['-O2'])],
    't07_imports': [('O0', ['-O0']), ('O2', ['-O2'])],
    't08_sse': [('sse_O0', ['-O0', '-msse2', '-mfpmath=sse']),
                ('sse_O2', ['-O2', '-msse2', '-mfpmath=sse'])],
    't09_x87': [('x87_O0', ['-O0', '-mfpmath=387']),
                ('x87_O2', ['-O2', '-mfpmath=387'])],
    't10_structs': [('O0', ['-O0']), ('O2', ['-O2'])],
    't11_strings': [('Os', ['-Os']),
                    ('rep_O2', ['-O2', '-minline-all-stringops', '-mstringop-strategy=rep_byte'])],
    't12_bits': [('O0', ['-O0']), ('O2', ['-O2'])],
    't13_calls': [('O0', ['-O0']), ('O2', ['-O2'])],
    't16_entry': [('O0', ['-O0']), ('O2', ['-O2'])],
    't20_modulo': [('O0', ['-O0']), ('O2', ['-O2'])],
    't21_constidx': [('O0', ['-O0']), ('O2', ['-O2'])],
    't22_tails': [('O0', ['-O0']), ('O2', ['-O2'])],
    't17_seek': [('O0', ['-O0']), ('O2', ['-O2'])],
    't18_save': [('O0', ['-O0']), ('O2', ['-O2'])],
    't19_overlapped': [('O0', ['-O0']), ('O2', ['-O2'])],
    't15_mmx': [('O0', ['-O0', '-mmmx', '-msse']), ('O2', ['-O2', '-mmmx', '-msse'])],
    't14_sim': [('sse_O2', ['-O2', '-msse2', '-mfpmath=sse']),
                ('x87_O2', ['-O2', '-mfpmath=387'])],
}


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def wine_env():
    env = dict(os.environ)
    env['WINEPREFIX'] = PREFIX
    env['WINEDEBUG'] = '-all'
    return env


def cmake_sources(name, keep):
    """The .c files runtime/CMakeLists.txt lists for add_library(NAME STATIC ...), with set(VAR ...)
    lists expanded (the core library's are; the desktop branch wins, as in build_native.py)."""
    text = open(os.path.join(REPO_RUNTIME, 'CMakeLists.txt')).read()
    variables = {}
    for m in re.finditer(r'set\((\w+)\s+((?:[^()]|\([^()]*\))*?)\)', text, re.S):
        variables[m.group(1)] = m.group(2).split()
    block = text[text.index(f'add_library({name} STATIC'):]
    block = block[:block.index(')')]
    entries = []
    for tok in block.split()[2:]:
        m = re.fullmatch(r'\$\{(\w+)\}', tok)
        entries += variables.get(m.group(1), []) if m else [tok]
    return [os.path.join(REPO_RUNTIME, p) for p in entries if p.endswith('.c') and keep(p)]


def shim_sources():
    """The shim sources that runtime/CMakeLists.txt builds into vt3shim. Single source of truth."""
    return cmake_sources('vt3shim', lambda p: 'shim/src/' in p)


def core_sources():
    """The vt3core sources (platform, renderer, audio, input, ADX) that CMake builds, as the game links them."""
    return cmake_sources('vt3core', lambda p: p.startswith('src/'))


def build_runtime(objdir):
    """Compile the native runtime (guest arena, shim) and the translator's loader.

    The shim is runtime/shim: the only Win32 implementation. The shim sources are the ones
    CMake builds. The adapters are generated from the same sources.
    """
    os.makedirs(objdir, exist_ok=True)
    shim = os.path.join(REPO_RUNTIME, 'shim')
    adapters = os.path.join(objdir, 'vt3_shim_adapters.c')
    shim_src = shim_sources()
    r = run(['/Users/heath/Virtua Tennis 3/tools/venv/bin/python', '-I',
             os.path.join(RT, 'gen_adapters.py'), shim, adapters] + shim_src)
    if r.returncode:
        sys.exit(f"adapter generation failed:\n{r.stdout.decode()}{r.stderr.decode()}")
    sources = [
        os.path.join(RT, 'vt3rt.c'),
        os.path.join(RT, 'vt3native.c'),
        os.path.join(RT, 'vt3main.c'),
        os.path.join(RT, 'vt3simd.c'),
        os.path.join(RT, 'vt3fpu.c'),
        adapters,
    ] + core_sources() + shim_src
    incs = ['-I', RT, '-I', os.path.join(REPO_RUNTIME, 'include'), '-I', os.path.join(shim, 'include'),
            '-I', os.path.join(SDL, 'include'), '-I', os.path.join(VKH, 'include')]
    objs = []
    for src in sources:
        obj = os.path.join(objdir, os.path.basename(src).replace('.c', '.o'))
        r = run([CC, '-O2', '-std=gnu11', '-w', '-ffp-contract=off'] + incs + ['-c', src, '-o', obj])
        if r.returncode:
            sys.exit(f"runtime build failed for {src}:\n{r.stderr.decode()}")
        objs.append(obj)
    return objs


    objs = []
    for src, _ in sources:
        obj = os.path.join(objdir, os.path.basename(src).replace('.c', '.o'))
        r = run([CC, '-O2', '-std=gnu11', '-w', '-ffp-contract=off'] + incs + ['-c', src, '-o', obj])
        if r.returncode:
            sys.exit(f"runtime build failed for {src}:\n{r.stderr.decode()}")
        objs.append(obj)
    return objs


# A test can give the size of a jump table it contains: /* TABLE <label> <count> [lo] */, where
# <label> is a global label placed on the jump. The harness resolves the label in the link map and
# writes the translator's table-size file (JUMP_ADDR COUNT [LO]).
TABLE_NOTE = re.compile(r'/\*\s*TABLE\s+(\S+)\s+(\d+)(?:\s+(-?\d+))?\s*\*/')
MAP_SYM = re.compile(r'^\s*0x([0-9a-fA-F]+)\s+(\S+)\s*$')


def write_table_sizes(src, mapfile, out):
    """Write the table-size file from the TABLE notes in src. Returns False, and writes nothing, if there are none."""
    notes = TABLE_NOTE.findall(open(src).read())
    if not notes:
        return False
    # The link map writes C-style names: the leading underscore of an assembler label is dropped.
    syms = {}
    for line in open(mapfile, errors='replace'):
        m = MAP_SYM.match(line)
        if m:
            syms[m.group(2).lstrip('_')] = int(m.group(1), 16)
    with open(out, 'w') as f:
        for label, count, lo in notes:
            key = label.lstrip('_')
            if key not in syms:
                sys.exit(f"{src}: TABLE label {label} is not in the link map")
            f.write(f"{syms[key]:x} {count} {lo or '0'}\n")
    return True


def build_one(test, vname, vflags, objs):
    tdir = os.path.join(ROOT, 'tests', test, vname)
    shutil.rmtree(tdir, ignore_errors=True)
    os.makedirs(tdir)
    src = os.path.join(HERE, test + '.c')
    exe = os.path.join(tdir, 'prog.exe')
    mapfile = os.path.join(tdir, 'prog.map')
    r = run([GCC] + vflags + BASE_CFLAGS + ['-I', HERE, src, '-o', exe] + BASE_LDFLAGS
            + ['-Wl,-Map=' + mapfile])
    if r.returncode:
        return None, f"compile failed: {r.stderr.decode()[-800:]}"
    tables = os.path.join(tdir, 'tables.txt')
    has_tables = write_table_sizes(src, mapfile, tables)

    # Expected output: the original exe under Wine.
    w = run(['arch', '-x86_64', WINE, 'prog.exe'], cwd=tdir, env=wine_env())
    expected = (w.stdout, w.returncode)

    # Translate.
    extra = ['--table-sizes', tables] if has_tables else []
    tr = run([PY, '-I', RECOMP, exe, '--out', tdir, '--name', 'mod'] + extra)
    tlog = tr.stdout.decode() + tr.stderr.decode()
    open(os.path.join(tdir, 'translate.log'), 'w').write(tlog)
    if tr.returncode:
        return expected, f"translate failed (rc={tr.returncode}): {tlog[-600:]}"

    # Compile the translated module and link with the runtime.
    modc = os.path.join(tdir, 'mod.c')
    modo = os.path.join(tdir, 'mod.o')
    r = run([CC, '-O2', '-std=gnu11', '-w', '-ffp-contract=off', '-I', RT, '-c', modc, '-o', modo])
    if r.returncode:
        return expected, f"C compile failed: {r.stderr.decode()[-1200:]}"
    native = os.path.join(tdir, 'native')
    r = run([CC, '-o', native, modo] + objs + ['-lm', '-lpthread'] + SDL_LINK)
    if r.returncode:
        return expected, f"link failed: {r.stderr.decode()[-800:]}"

    n = run([native, os.path.join(tdir, 'image.bin')], cwd=tdir)
    return expected, (n.stdout, n.returncode, n.stderr.decode())


def first_diff(a, b):
    la, lb = a.splitlines(), b.splitlines()
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else '<missing>'
        y = lb[i] if i < len(lb) else '<missing>'
        if x != y:
            return f"line {i + 1}: wine={x!r} native={y!r}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-k', default='', help='only tests whose name contains this')
    args = ap.parse_args()

    objs = build_runtime(os.path.join(ROOT, 'rtobj'))
    results = []
    for test, variants in VARIANTS.items():
        if args.k and args.k not in test:
            continue
        for vname, vflags in variants:
            label = f"{test}/{vname}"
            expected, got = build_one(test, vname, vflags, objs)
            if expected is None:
                results.append((label, 'FAIL', got))
                print(f"{label:24s} FAIL  {got[:200]}", flush=True)
                continue
            if isinstance(got, str):
                results.append((label, 'FAIL', got))
                print(f"{label:24s} FAIL  {got[:200]}", flush=True)
                continue
            out, rc, err = got
            wout, wrc = expected
            ok = (out == wout and rc == wrc)
            if ok:
                results.append((label, 'PASS', f"{len(out)} bytes, exit {rc}"))
            else:
                detail = f"exit wine={wrc} native={rc}"
                d = first_diff(wout.decode('latin-1'), out.decode('latin-1'))
                if d:
                    detail += "; " + d
                if err.strip():
                    detail += "; stderr: " + err.strip().splitlines()[-1][:200]
                results.append((label, 'FAIL', detail))
            print(f"{label:24s} {results[-1][1]}  {results[-1][2][:200]}", flush=True)

    summary = os.path.join(ROOT, 'summary.txt')
    with open(summary, 'w') as f:
        for label, st, detail in results:
            f.write(f"{st}  {label}  {detail}\n")
    npass = sum(1 for r in results if r[1] == 'PASS')
    print(f"\n{npass}/{len(results)} passed. Summary: {summary}")
    return 0 if npass == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
