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
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RT = os.path.join(os.path.dirname(HERE), 'rt')
RECOMP = os.path.join(HERE, '..', 'recomp.py')
REPO_RUNTIME = os.path.join(HERE, '..', '..', '..', 'runtime')
ROOT = '/Users/heath/Virtua Tennis 3/dump/recomp'
PY = '/Users/heath/Virtua Tennis 3/tools/venv/bin/python'
GCC = 'i686-w64-mingw32-gcc'
CC = 'clang'
WINE = '/Users/heath/Virtua Tennis 3/tools/wine/Wine Devel.app/Contents/Resources/wine/bin/wine'
PREFIX = os.path.join(ROOT, 'wineprefix')

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


def shim_sources():
    """The shim sources that runtime/CMakeLists.txt builds into vt3shim. Single source of truth."""
    text = open(os.path.join(REPO_RUNTIME, 'CMakeLists.txt')).read()
    block = text[text.index('add_library(vt3shim STATIC'):]
    block = block[:block.index(')')]
    return [os.path.join(REPO_RUNTIME, p.strip()) for p in block.splitlines()
            if p.strip().endswith('.c') and 'shim/src/' in p]


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
        os.path.join(RT, 'vt3main.c'),
        os.path.join(RT, 'vt3simd.c'),
        adapters,
        os.path.join(REPO_RUNTIME, 'src', 'core', 'log.c'),
        os.path.join(REPO_RUNTIME, 'src', 'core', 'guest.c'),
    ] + shim_src
    incs = ['-I', RT, '-I', os.path.join(REPO_RUNTIME, 'include'), '-I', os.path.join(shim, 'include')]
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


def build_one(test, vname, vflags, objs):
    tdir = os.path.join(ROOT, 'tests', test, vname)
    shutil.rmtree(tdir, ignore_errors=True)
    os.makedirs(tdir)
    src = os.path.join(HERE, test + '.c')
    exe = os.path.join(tdir, 'prog.exe')
    r = run([GCC] + vflags + BASE_CFLAGS + ['-I', HERE, src, '-o', exe] + BASE_LDFLAGS)
    if r.returncode:
        return None, f"compile failed: {r.stderr.decode()[-800:]}"

    # Expected output: the original exe under Wine.
    w = run(['arch', '-x86_64', WINE, 'prog.exe'], cwd=tdir, env=wine_env())
    expected = (w.stdout, w.returncode)

    # Translate.
    tr = run([PY, '-I', RECOMP, exe, '--out', tdir, '--name', 'mod'])
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
    r = run([CC, '-o', native, modo] + objs + ['-lm', '-lpthread'])
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
