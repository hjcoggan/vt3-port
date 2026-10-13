#!/usr/bin/env python3
"""Generate the D3D9 COM prototypes, slot order and unimplemented stubs.

The source of truth is the mingw-w64 d3d9.h, preprocessed here, so every member
name and order comes from the SDK header the layouts were checked against.

Usage: gen_d3d9_com.py MINGW_INCLUDE_DIR SHIM_ROOT

Writes:
  SHIM_ROOT/include/win32/d3d9_com.h   WINAPI prototypes for every IDirect3D9 and
                                       IDirect3DDevice9 method, and the slot lists.
                                       gen_adapters.py turns each prototype into a
                                       stdcall adapter when it has a definition.
  SHIM_ROOT/src/d3d9_com_unimpl.c      a stub for every method not in IMPLEMENTED.
                                       Each stub logs once and returns E_NOTIMPL
                                       (or 0 for non-HRESULT returns).

Parameters are guest-width: pointers are GPTR, floats are DWORD (their bits), and
every enum or integer is UINT. This is what a stdcall caller pushes.
"""
import os
import re
import subprocess
import sys

# Methods with a real implementation in src/d3d9_*.c. Everything else gets a stub.
IMPLEMENTED = {
    "IDirect3D9": {
        "QueryInterface", "AddRef", "Release", "RegisterSoftwareDevice", "GetAdapterCount",
        "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes",
        "GetAdapterDisplayMode", "CheckDeviceType", "CheckDeviceFormat",
        "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "CheckDeviceFormatConversion",
        "GetDeviceCaps", "GetAdapterMonitor", "CreateDevice",
    },
    "IDirect3DDevice9": {
        "QueryInterface", "AddRef", "Release", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode",
        "GetCreationParameters", "TestCooperativeLevel", "GetAvailableTextureMem",
        "EvictManagedResources", "Reset", "Present", "BeginScene", "EndScene", "Clear",
        "SetRenderState", "GetRenderState", "SetTextureStageState", "GetTextureStageState",
        "SetSamplerState", "GetSamplerState", "SetViewport", "GetViewport", "SetFVF", "GetFVF",
        "SetVertexShader", "GetVertexShader", "SetPixelShader", "GetPixelShader",
        "SetVertexShaderConstantF", "GetVertexShaderConstantF", "SetPixelShaderConstantF",
        "GetPixelShaderConstantF", "SetStreamSource", "GetStreamSource", "SetIndices",
        "GetIndices", "SetTexture", "GetTexture", "SetRenderTarget", "GetRenderTarget",
        "SetDepthStencilSurface", "GetDepthStencilSurface",
    },
}

INTERFACES = ["IDirect3D9", "IDirect3DDevice9"]

RET_MAP = {"HRESULT": "HRESULT", "ULONG": "DWORD", "UINT": "UINT", "BOOL": "BOOL", "WINBOOL": "BOOL",
           "HMONITOR": "GPTR",
           # float returns come back in ST(0), which gen_adapters.py cannot express. Only
           # GetNPatchMode returns float, and nothing in the trace calls it.
           "float": "DWORD",
           "DWORD": "DWORD", "void": "void"}

# Non-pointer types that are one 32-bit stack slot as is.
SCALAR = {"UINT", "INT", "DWORD", "BOOL"}


def preprocess(include_dir):
    src = "#include <d3d9.h>\n"
    r = subprocess.run(["i686-w64-mingw32-gcc", "-E", "-x", "c", "-I", include_dir, "-"],
                       input=src.encode(), capture_output=True)
    if r.returncode:
        sys.exit("preprocess failed:\n" + r.stderr.decode())
    return r.stdout.decode(errors="replace")


def struct_members(text, name):
    m = re.search(r'struct\s+' + name + r'\s*\{', text)
    if not m:
        sys.exit(f"struct {name} not found in the SDK header")
    i, depth = m.end(), 1
    while depth and i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
        i += 1
    body = text[m.end():i - 1]
    pat = re.compile(r'(?P<ret>[^;{}()]*?)\(\s*(?:__attribute__\s*\(\([^)]*\)\)\s*)?'
                     r'(?:WINAPI\s+|STDMETHODCALLTYPE\s+|__stdcall\s+)?\*\s*(?P<name>\w+)\s*\)\s*'
                     r'\((?P<params>[^)]*)\)\s*;', re.S)
    out = []
    for mm in pat.finditer(body):
        out.append((mm.group("name"), " ".join(mm.group("ret").split()), mm.group("params")))
    return out


def split_params(text):
    text = " ".join(text.split())
    if text in ("", "void"):
        return []
    return [p.strip() for p in text.split(",")]


def map_param(p, index):
    """Return (c_type, name) for a guest-width parameter.

    Some SDK declarations leave a parameter unnamed (DrawIndexedPrimitive's first one).
    Those get a positional name.
    """
    name_m = re.search(r'(\w+)\s*$', p)
    unnamed = name_m is None or re.fullmatch(r'\s*(const\s+)?\w+\s*', p) is not None and "*" not in p and \
        len(p.split()) == 1
    if unnamed:
        name = f"arg{index}"
        body = p
    else:
        name = name_m.group(1)
        body = p[:name_m.start()].strip()
    if "*" in p:
        return "GPTR", name
    words = [w for w in body.split() if w not in ("const", "volatile", "struct", "enum")]
    base = words[-1] if words else ""
    if base == "float":
        return "DWORD", name
    if base == "WINBOOL":
        return "BOOL", name
    if base == "int":
        return "INT", name
    if base in SCALAR:
        return base, name
    if base in ("HWND", "HANDLE", "HMODULE"):
        return "GPTR", name
    if base.startswith("D3D") or base == "DWORD" or base == "UINT":
        return "UINT", name
    if base == "REFIID":
        return "GPTR", name
    sys.exit(f"unmapped parameter type in {p!r}")


def map_ret(ret):
    base = ret.replace("__attribute__((__stdcall__))", "").strip()
    if base in RET_MAP:
        return RET_MAP[base]
    sys.exit(f"unmapped return type {ret!r}")


def c_ident(name):
    return name if name not in ("register", "int", "char", "double", "long", "short") else name + "_"


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    include_dir, shim_root = argv[1], argv[2]
    text = preprocess(include_dir)

    protos = []      # (iface, name, ret, [(ctype, pname)])
    slots = {}
    for iface in INTERFACES:
        members = struct_members(text, iface + "Vtbl")
        slots[iface] = [n for n, _, _ in members]
        for name, ret, params in members:
            ps = [map_param(p, i) for i, p in enumerate(split_params(params)[1:])]   # drop the This pointer
            protos.append((iface, name, map_ret(ret), ps))
    # The SDK header lists the IUnknown methods in the IDirect3DDevice9 vtable too.
    for iface in INTERFACES:
        assert len(slots[iface]) == {"IDirect3D9": 17, "IDirect3DDevice9": 119}[iface], iface

    hdr = [
        "/* Generated by runtime/shim/tools/gen_d3d9_com.py from the mingw-w64 d3d9.h. Do not edit. */",
        "/* Slot order is the SDK's. Each stdcall adapter is emitted by gen_adapters.py. */",
        "#ifndef VT3_WIN32_D3D9_COM_H",
        "#define VT3_WIN32_D3D9_COM_H",
        "",
        '#include "win32/types.h"',
        "",
    ]
    for iface in INTERFACES:
        hdr.append(f"/* {iface}: slot order, X-macro over every member. */")
        hdr.append(f"#define VT3_{iface.upper()}_SLOTS(X) \\")
        hdr.append("    " + " \\\n    ".join(f"X({n})" for n in slots[iface]))
        hdr.append("")
    for iface, name, ret, ps in protos:
        plist = ", ".join(["GPTR This"] + [f"{t} {c_ident(n)}" for t, n in ps])
        hdr.append(f"{ret} WINAPI {iface}_{name}({plist});")
    hdr.extend(["", "#endif /* VT3_WIN32_D3D9_COM_H */", ""])
    with open(os.path.join(shim_root, "include", "win32", "d3d9_com.h"), "w") as f:
        f.write("\n".join(hdr))

    src = [
        "/* Generated by runtime/shim/tools/gen_d3d9_com.py. Do not edit. */",
        "/* Stubs for the D3D9 COM methods that do not have a real implementation yet. */",
        '#include "shim_internal.h"',
        '#include "win32/d3d9_com.h"',
        "",
    ]
    n_stub = 0
    for iface, name, ret, ps in protos:
        if name in IMPLEMENTED[iface]:
            continue
        n_stub += 1
        plist = ", ".join(["GPTR This"] + [f"{t} {c_ident(n)}" for t, n in ps])
        src.append(f"{ret} WINAPI {iface}_{name}({plist})")
        src.append("{")
        src.append("    (void)This;")
        for _, n in ps:
            src.append(f"    (void)({c_ident(n)});")
        src.append(f'    vt3s_unimpl("{iface}::{name}");')
        if ret == "void":
            pass
        elif ret == "HRESULT":
            src.append("    return E_NOTIMPL;")
        else:
            src.append("    return 0;")
        src.append("}")
        src.append("")
    with open(os.path.join(shim_root, "src", "d3d9_com_unimpl.c"), "w") as f:
        f.write("\n".join(src))

    missing = [(i, n) for i, n, _, _ in protos if n not in IMPLEMENTED[i]]
    print(f"d3d9_com: {len(protos)} methods, {len(protos) - n_stub} implemented, {n_stub} stubbed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
