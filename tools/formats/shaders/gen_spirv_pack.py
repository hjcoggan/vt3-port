#!/usr/bin/env python3
"""Builds the SPIR-V pack the Vulkan renderer loads on the device.

The device has no GLSL compiler, so every shader it can meet is compiled here. Each SM2 binary
(the game's, and the ones rebuilt for D3DX) goes through the host translator (runtime/tools/sm2tool.c,
Vulkan mode) to GLSL 450, then glslangValidator makes SPIR-V. The pack is keyed by the FNV-1a 64 hash
of the GLSL text, which the runtime computes on the text it generates, so the two sides agree.

Usage: gen_spirv_pack.py OUT.pack [--bins DIR ...] [--glsl DIR ...]
  --bins: directories of SM2 binaries named <id>.<vs|ps>_2_0.bin (default: the extracted game set)
  --glsl: directories of built-in GLSL files named <name>_vs.glsl or <name>_fs.glsl

Writes OUT.pack and prints a line for each shader that failed. Pack layout (little endian):
  "VT3PACK1", u32 count, then per entry: u64 hash, u32 stage (0 vertex, 1 fragment), u32 size, SPIR-V.
"""
import glob
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))           # vt3-port
DUMP = os.path.abspath(os.path.join(ROOT, "..", "dump"))                 # Virtua Tennis 3/dump
GAME_BINS = os.path.join(DUMP, "runtime", "shaders", "bin")
REBUILT_BINS = os.path.join(DUMP, "runtime", "shaders", "rebuilt")
BUILTIN_GLSL = os.path.join(ROOT, "runtime", "src", "render", "glsl")
TOOL_SRC = os.path.join(ROOT, "runtime", "tools", "sm2tool.c")
TOOL_DIR = os.path.join(DUMP, "android", "tools")
TOOL = os.path.join(TOOL_DIR, "vt3sm2")
GLSLANG = "glslangValidator"


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def build_tool():
    os.makedirs(TOOL_DIR, exist_ok=True)
    srcs = [TOOL_SRC, os.path.join(ROOT, "runtime", "src", "render", "sm2_glsl.c"),
            os.path.join(ROOT, "runtime", "src", "core", "log.c")]
    inc = os.path.join(ROOT, "runtime", "include")
    subprocess.run(["cc", "-std=c11", "-O1", "-I", inc, "-o", TOOL] + srcs, check=True)


def write_rebuilt_bins():
    """The four D3DX-rebuilt shaders, as binaries, so they go through the same path."""
    sys.path.insert(0, HERE)
    import importlib.util
    spec = importlib.util.spec_from_file_location("gen_rebuilt", os.path.join(HERE, "gen_rebuilt_shaders.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    import sm2_asm
    os.makedirs(REBUILT_BINS, exist_ok=True)
    for name, _src, _len, _entry, kind, consts, listing in mod.SHADERS:
        tokens, k = sm2_asm.assemble(listing, consts)
        profile = "vs_2_0" if kind == "v" else "ps_2_0"
        with open(os.path.join(REBUILT_BINS, "%s.%s.bin" % (name, profile)), "wb") as f:
            f.write(sm2_asm.to_bytes(tokens))


def glslang(stage, src_path, spv_path):
    r = subprocess.run([GLSLANG, "-S", stage, "-V", src_path, "-o", spv_path], capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out_path = argv[1]
    bins = []
    glsl_dirs = [BUILTIN_GLSL]
    i = 2
    while i < len(argv):
        if argv[i] == "--bins":
            bins.append(argv[i + 1])
            i += 2
        elif argv[i] == "--glsl":
            glsl_dirs.append(argv[i + 1])
            i += 2
        else:
            raise SystemExit("unknown argument " + argv[i])
    if not bins:
        bins = [GAME_BINS]
    build_tool()
    write_rebuilt_bins()
    bins.append(REBUILT_BINS)

    entries = {}
    failed = []
    tmp = tempfile.mkdtemp(prefix="vt3pack")
    for d in bins:
        for path in sorted(glob.glob(os.path.join(d, "*.bin"))):
            name = os.path.basename(path)[:-4]
            stage = "vert" if ".vs_" in name else "frag"
            glsl = os.path.join(tmp, name + ".glsl")
            r = subprocess.run([TOOL, path, glsl], capture_output=True, text=True)
            if r.returncode:
                failed.append((name, "translate: " + (r.stderr.strip() or "failed")))
                continue
            text = open(glsl, "rb").read()
            key = fnv1a64(text)
            if key in entries:
                continue
            spv = os.path.join(tmp, name + ".spv")
            ok, msg = glslang(stage, glsl, spv)
            if not ok:
                failed.append((name, "glslang: " + msg.splitlines()[0] if msg else "glslang failed"))
                continue
            entries[key] = (0 if stage == "vert" else 1, open(spv, "rb").read(), name)
    for d in glsl_dirs:
        for path in sorted(glob.glob(os.path.join(d, "*_vs.glsl")) + glob.glob(os.path.join(d, "*_fs.glsl"))):
            name = os.path.basename(path)[:-5]
            stage = "vert" if name.endswith("_vs") else "frag"
            text = open(path, "rb").read()
            key = fnv1a64(text)
            spv = os.path.join(tmp, name + ".spv")
            ok, msg = glslang(stage, path, spv)
            if not ok:
                failed.append((name, "glslang: " + msg))
                continue
            entries[key] = (0 if stage == "vert" else 1, open(spv, "rb").read(), name)

    with open(out_path, "wb") as f:
        f.write(b"VT3PACK1")
        f.write(struct.pack("<I", len(entries)))
        for key in sorted(entries):
            stage, spv, _name = entries[key]
            f.write(struct.pack("<QII", key, stage, len(spv)))
            f.write(spv)
    for name, why in failed:
        print("FAILED %s: %s" % (name, why))
    print("pack: %d shaders, %d failed, %s" % (len(entries), len(failed), out_path))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
