#!/usr/bin/env python3
"""Rebuild vt3_native: the translated game linked against the full runtime.

Inputs, in order: the loader (tools/recomp/rt), the generated stdcall adapters, the runtime's
vt3core library (platform, GL 3.3 renderer, SM2 translator, audio, input, ADX), the shim
(vt3shim), and the translated vt3.c. The source lists are read from runtime/CMakeLists.txt,
so this build cannot drift from the CMake build. The runtime's own main and self-test
(src/main.c, tests/) are left out, because the game's entry is vt3main.c.

Usage: build_native.py [OUT_DIR] [--root SOURCE_TREE]

OUT_DIR receives vt3_native, with its objects in OUT_DIR/obj, so two builds never share an
object and a running game is not disturbed. It defaults to dump/recomp/vt3_nocd, which holds
the translated vt3.c. A new OUT_DIR gets a link to that vt3.c and a copy of its compiled object,
so only the runtime is compiled (vt3.c is about 100 MB and slow to compile).
--root builds another source tree, such as an exported older commit, instead of this checkout.

The game data and the translated code live outside the repo, in ../dump (set VT3_DUMP to move
it). SDL3 comes from Homebrew (brew --prefix sdl3).
"""
import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BASE = os.path.dirname(REPO)
DUMP = os.environ.get('VT3_DUMP', os.path.join(BASE, 'dump'))
DEFAULT_OUT = os.path.join(DUMP, 'recomp', 'vt3_nocd')
PY = os.path.join(BASE, 'tools', 'venv', 'bin', 'python')
CC = 'clang'


def parse_args(argv):
    out, root = DEFAULT_OUT, REPO
    rest = list(argv)
    while rest:
        a = rest.pop(0)
        if a == '--root':
            root = os.path.abspath(rest.pop(0))
        else:
            out = os.path.abspath(a)
    return out, root


OUT, ROOT = parse_args(sys.argv[1:])
OBJ = os.path.join(OUT, 'obj')
RT = os.path.join(ROOT, 'tools/recomp/rt')
RUNTIME = os.path.join(ROOT, 'runtime')
SHIM = os.path.join(RUNTIME, 'shim')


def cmake_library(name):
    """The .c files that runtime/CMakeLists.txt lists for add_library(NAME STATIC ...).

    Entries may be CMake variables (set(VAR ...) blocks, as the core library uses for its
    platform-specific lists), so those are expanded here. Only the desktop branch is wanted."""
    text = open(os.path.join(RUNTIME, 'CMakeLists.txt')).read()
    variables = {}
    for m in re.finditer(r'set\((\w+)\s+((?:[^()]|\([^()]*\))*?)\)', text, re.S):
        variables[m.group(1)] = m.group(2).split()
    block = text[text.index(f'add_library({name} STATIC'):]
    block = block[:block.index(')')]
    entries = []
    for tok in block.split()[2:]:
        m = re.fullmatch(r'\$\{(\w+)\}', tok)
        entries += variables.get(m.group(1), []) if m else [tok]
    return [os.path.join(RUNTIME, p) for p in entries if p.endswith('.c')]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd[:4])} ...\n{r.stdout}{r.stderr}")
    return r


def sdl_prefix():
    return subprocess.run(['brew', '--prefix', 'sdl3'], capture_output=True, text=True,
                          check=True).stdout.strip()


def vulkan_prefix():
    """Homebrew's MoltenVK and Vulkan loader (+ headers). The CMake build resolves this through
    find_package(Vulkan); this build drives the compiler directly, so it asks brew."""
    r = subprocess.run(['brew', '--prefix', 'vulkan'], capture_output=True, text=True)
    p = r.stdout.strip() if r.returncode == 0 else ''
    return p or '/opt/homebrew'


def prepare_out(translated, obj):
    """A new OUT_DIR links the translated vt3.c and reuses its compiled object when it is current."""
    if OUT != DEFAULT_OUT and not os.path.exists(translated):
        os.symlink(os.path.join(DEFAULT_OUT, 'vt3.c'), translated)
    if os.path.exists(obj) or OUT == DEFAULT_OUT:
        return
    seeds = [p for p in glob.glob(os.path.join(DEFAULT_OUT, 'obj', '*_vt3.o'))
             if os.path.getmtime(p) >= os.path.getmtime(translated)]
    if seeds:
        shutil.copy2(max(seeds, key=os.path.getmtime), obj)   # copy2 keeps the mtime the reuse check reads


def main():
    os.makedirs(OBJ, exist_ok=True)
    shim_src = cmake_library('vt3shim')
    core_src = cmake_library('vt3core')
    adapters = os.path.join(OBJ, 'vt3_shim_adapters.c')
    gen = run([PY, '-I', os.path.join(RT, 'gen_adapters.py'), SHIM, adapters] + shim_src)
    if gen.stdout.strip():
        print(gen.stdout.strip())

    sdl = sdl_prefix()
    vk = vulkan_prefix()
    translated = os.path.join(OUT, 'vt3.c')
    sources = [os.path.join(RT, s) for s in ('vt3rt.c', 'vt3main.c', 'vt3simd.c', 'vt3fpu.c')]
    sources.append(adapters)
    sources += core_src
    sources += shim_src
    sources.append(translated)

    incs = ['-I', RT,
            '-I', os.path.join(RUNTIME, 'include'),
            '-I', os.path.join(SHIM, 'include'),
            '-I', os.path.join(sdl, 'include'),
            '-I', os.path.join(vk, 'include')]
    flags = ['-O2', '-std=gnu11', '-w', '-ffp-contract=off']
    objs = []
    for i, src in enumerate(sources):
        obj = os.path.join(OBJ, f'{i:03d}_{os.path.basename(src).replace(".c", ".o")}')
        if src == translated:
            prepare_out(translated, obj)
            # vt3.c is about 100 MB and slow to compile. Reuse its object unless the source changed.
            if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
                objs.append(obj)
                continue
        level = ['-O1'] + flags[1:] if src == translated else flags   # same level as the first build
        run([CC] + level + incs + ['-c', src, '-o', obj])
        objs.append(obj)

    exe = os.path.join(OUT, 'vt3_native')
    run([CC, '-o', exe] + objs
        + ['-L', os.path.join(sdl, 'lib'), '-lSDL3', '-Wl,-rpath,' + os.path.join(sdl, 'lib'),
           '-L', os.path.join(vk, 'lib'), '-lvulkan',
           '-framework', 'OpenGL', '-lm', '-lpthread'])
    print('built', exe)


if __name__ == '__main__':
    main()
