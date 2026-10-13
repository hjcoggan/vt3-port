#!/usr/bin/env python3
"""Generate the D3D9 COM prototypes, slot order and unimplemented stubs.

The source of truth is the mingw-w64 d3d9.h, preprocessed here, so every member
name and order comes from the SDK header the layouts were checked against.

Usage: gen_d3d9_com.py MINGW_INCLUDE_DIR SHIM_ROOT

Writes:
  SHIM_ROOT/include/win32/d3d9_com.h   WINAPI prototypes for every method of every
                                       interface in INTERFACES, and the slot lists.
                                       gen_adapters.py turns each prototype into a
                                       stdcall adapter when it has a definition.
  SHIM_ROOT/src/d3d9_com_unimpl.c      a stub for every method not in IMPLEMENTED.
                                       A stub logs once and returns E_NOTIMPL (0 for
                                       non-HRESULT returns).

Rules enforced here:
  * A method with an output pointer (a `T **` parameter) must be implemented. A stub
    would leave the output empty, and the game would call through NULL. The generator
    refuses to run if that happens.
  * Parameters are guest-width: pointers are GPTR, floats are DWORD (their bits),
    and every enum or integer is UINT. This is what a stdcall caller pushes.
"""
import os
import re
import subprocess
import sys

# Interfaces the shim models. Each gets a vtable with the SDK's flattened slot order.
INTERFACES = [
    "IDirect3D9", "IDirect3DDevice9", "IDirect3DSwapChain9", "IDirect3DTexture9",
    "IDirect3DCubeTexture9", "IDirect3DVolumeTexture9", "IDirect3DVertexBuffer9",
    "IDirect3DIndexBuffer9", "IDirect3DSurface9", "IDirect3DVertexDeclaration9",
    "IDirect3DVertexShader9", "IDirect3DPixelShader9", "IDirect3DStateBlock9",
    "IDirect3DQuery9", "IDirect3DVolume9", "IDirectSound", "IDirectSoundBuffer",
    "ID3DXBuffer", "ID3DXConstantTable",
]

IUNKNOWN = {"QueryInterface", "AddRef", "Release"}

# Methods of IDirect3DResource9 that the resource interfaces implement.
RESOURCE = {"GetDevice", "SetPriority", "GetPriority", "PreLoad", "GetType"}

# Methods with a real implementation in src/d3d9_*.c. Everything else gets a stub.
IMPLEMENTED = {
    "IDirect3D9": IUNKNOWN | {
        "RegisterSoftwareDevice", "GetAdapterCount", "GetAdapterIdentifier",
        "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode",
        "CheckDeviceType", "CheckDeviceFormat", "CheckDeviceMultiSampleType",
        "CheckDepthStencilMatch", "CheckDeviceFormatConversion", "GetDeviceCaps",
        "GetAdapterMonitor", "CreateDevice",
    },
    "IDirect3DDevice9": IUNKNOWN | {
        "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
        "TestCooperativeLevel", "GetAvailableTextureMem", "EvictManagedResources", "Reset",
        "Present", "BeginScene", "EndScene", "Clear", "SetRenderState", "GetRenderState",
        "SetTextureStageState", "GetTextureStageState", "SetSamplerState", "GetSamplerState",
        "SetViewport", "GetViewport", "SetFVF", "GetFVF", "SetVertexShader", "GetVertexShader",
        "SetPixelShader", "GetPixelShader", "SetVertexShaderConstantF", "GetVertexShaderConstantF",
        "SetPixelShaderConstantF", "GetPixelShaderConstantF", "SetStreamSource",
        "GetStreamSource", "SetIndices", "GetIndices", "SetTexture", "GetTexture",
        "SetRenderTarget", "GetRenderTarget", "SetDepthStencilSurface", "GetDepthStencilSurface",
        "BeginStateBlock", "EndStateBlock", "CreateTexture", "CreateCubeTexture", "CreateVolumeTexture", "CreateVertexBuffer",
        "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface",
        "CreateOffscreenPlainSurface", "CreateVertexDeclaration", "SetVertexDeclaration",
        "GetVertexDeclaration", "CreateVertexShader", "CreatePixelShader", "CreateStateBlock",
        "CreateQuery", "GetBackBuffer", "GetSwapChain", "GetNumberOfSwapChains",
        "CreateAdditionalSwapChain", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
        "DrawIndexedPrimitiveUP", "StretchRect", "ValidateDevice",
    },
    "IDirect3DSwapChain9": IUNKNOWN | {"GetBackBuffer", "GetDisplayMode", "GetDevice",
                                       "GetPresentParameters", "Present"},
    "IDirect3DTexture9": IUNKNOWN | RESOURCE | {
        "GetLevelCount", "GetLevelDesc", "GetSurfaceLevel", "LockRect", "UnlockRect"},
    "IDirect3DCubeTexture9": IUNKNOWN | RESOURCE | {
        "GetLevelCount", "GetLevelDesc", "GetCubeMapSurface", "LockRect", "UnlockRect"},
    "IDirect3DVolumeTexture9": IUNKNOWN | RESOURCE | {"GetLevelCount", "GetVolumeLevel"},
    "IDirect3DVolume9": IUNKNOWN | {"GetDevice", "GetContainer"},
    "IDirectSound": IUNKNOWN | {"CreateSoundBuffer", "GetCaps", "DuplicateSoundBuffer",
                                "SetCooperativeLevel", "Compact", "GetSpeakerConfig",
                                "SetSpeakerConfig", "Initialize"},
    "ID3DXBuffer": IUNKNOWN | {"GetBufferPointer", "GetBufferSize"},
    "ID3DXConstantTable": IUNKNOWN,
    "IDirectSoundBuffer": IUNKNOWN | {"GetCaps", "GetCurrentPosition", "GetFormat", "GetVolume",
                                      "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock",
                                      "Play", "SetCurrentPosition", "SetFormat", "SetVolume",
                                      "SetPan", "SetFrequency", "Stop", "Unlock", "Restore"},
    "IDirect3DVertexBuffer9": IUNKNOWN | RESOURCE | {"Lock", "Unlock", "GetDesc"},
    "IDirect3DIndexBuffer9": IUNKNOWN | RESOURCE | {"Lock", "Unlock", "GetDesc"},
    "IDirect3DSurface9": IUNKNOWN | RESOURCE | {"GetContainer", "GetDesc", "LockRect",
                                                 "UnlockRect"},
    "IDirect3DVertexDeclaration9": IUNKNOWN | {"GetDeclaration", "GetDevice"},
    "IDirect3DVertexShader9": IUNKNOWN | {"GetDevice", "GetFunction"},
    "IDirect3DPixelShader9": IUNKNOWN | {"GetDevice", "GetFunction"},
    "IDirect3DStateBlock9": IUNKNOWN | {"GetDevice", "Capture", "Apply"},
    "IDirect3DQuery9": IUNKNOWN | {"GetDevice", "GetType", "GetDataSize", "Issue", "GetData"},
}

RET_MAP = {"HRESULT": "HRESULT", "ULONG": "DWORD", "UINT": "UINT", "BOOL": "BOOL",
           "WINBOOL": "BOOL", "DWORD": "DWORD", "void": "void", "HMONITOR": "GPTR",
           # float returns come back in ST(0), which gen_adapters.py cannot express.
           # Only GetNPatchMode returns float, and nothing in the trace calls it.
           "float": "DWORD", "void *": "GPTR", "LPVOID": "GPTR", "LPCSTR": "GPTR"}

SCALAR = {"UINT", "INT", "DWORD", "BOOL"}

# Object kind (D3D9_KIND_* in d3d9_internal.h) for each interface.
KIND = {
    "IDirect3D9": "D3D9_KIND_D3D9", "IDirect3DDevice9": "D3D9_KIND_DEVICE",
    "IDirect3DSwapChain9": "D3D9_KIND_SWAPCHAIN", "IDirect3DTexture9": "D3D9_KIND_TEXTURE",
    "IDirect3DCubeTexture9": "D3D9_KIND_CUBETEXTURE", "IDirect3DVolumeTexture9": "D3D9_KIND_VOLUMETEXTURE",
    "IDirect3DVertexBuffer9": "D3D9_KIND_VB", "IDirect3DIndexBuffer9": "D3D9_KIND_IB",
    "IDirect3DSurface9": "D3D9_KIND_SURFACE", "IDirect3DVertexDeclaration9": "D3D9_KIND_VDECL",
    "IDirect3DVertexShader9": "D3D9_KIND_VSHADER", "IDirect3DPixelShader9": "D3D9_KIND_PSHADER",
    "IDirect3DStateBlock9": "D3D9_KIND_STATEBLOCK", "IDirect3DQuery9": "D3D9_KIND_QUERY",
    "IDirect3DVolume9": "D3D9_KIND_VOLUME",
    "IDirectSound": "D3D9_KIND_DSOUND", "IDirectSoundBuffer": "D3D9_KIND_DSBUFFER",
    "ID3DXBuffer": "D3D9_KIND_D3DXBUFFER", "ID3DXConstantTable": "D3D9_KIND_D3DXCONSTTABLE",
}

# Bodies the generator writes itself (d3d9_com_generic.c). {KIND} is the interface kind,
# {OUT} the first output parameter. Anything not listed here is hand-written.
GENERIC = {
    "QueryInterface": "return d3d9_qi_kind(This, {RIID}, {PPV}, {KIND});",
    "AddRef": "return d3d9_add_ref(This);",
    "Release": "return d3d9_release(This);",
    "GetDevice": "return d3d9_get_device(This, {OUT});",
    "GetType": "return d3d9_resource_type({KIND});",
    "GetPriority": "return 0;",
    "SetPriority": "return 0;",
    "PreLoad": "(void)This;",
}
RESOURCE_KINDS = {"IDirect3DTexture9", "IDirect3DCubeTexture9", "IDirect3DVolumeTexture9",
                  "IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9", "IDirect3DSurface9"}


def preprocess(include_dir):
    src = "#include <d3d9.h>\n#include <dsound.h>\n#include <d3dx9core.h>\n#include <d3dx9shader.h>\n"
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


def iid_table(text):
    rows = []
    for m in re.finditer(r'DEFINE_GUID\(IID_(\w+),\s*([^)]*)\)\s*;', text):
        name, vals = m.group(1), [v.strip() for v in m.group(2).split(",")]
        if name not in INTERFACES:
            continue
        if len(vals) != 11:
            sys.exit(f"IID_{name}: expected 11 fields, got {len(vals)}")
        d1, d2, d3 = (int(vals[0], 0), int(vals[1], 0), int(vals[2], 0))
        d4 = [int(v, 0) for v in vals[3:]]
        b = list(d1.to_bytes(4, "little")) + list(d2.to_bytes(2, "little")) + list(d3.to_bytes(2, "little")) + d4
        rows.append((name, b))
    return rows


def is_output(param):
    """An output is a `T **` parameter or an LPLP typedef. A const pointer array is an input."""
    if re.search(r'\bconst\b', param, re.I):
        return False
    words = param.split()
    return "**" in param or (bool(words) and words[0].startswith("LPLP"))


def map_param(p, index):
    """Return (c_type, name) for a guest-width parameter."""
    name_m = re.search(r'(\w+)\s*$', p)
    unnamed = name_m is None or (re.fullmatch(r'\s*(const\s+)?\w+\s*', p) is not None
                                 and "*" not in p and len(p.split()) == 1)
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
    if base in ("float", "FLOAT"):
        return "DWORD", name
    if base == "int":
        return "INT", name
    if base == "WINBOOL":
        return "BOOL", name
    if base in SCALAR:
        return base, name
    if base in ("HWND", "HANDLE", "HMODULE", "HDC", "HMONITOR"):
        return "GPTR", name
    if base.startswith("LP"):           # LP* typedefs are pointers
        return "GPTR", name
    if base == "LONG":
        return "INT", name
    if base.startswith("D3D") or base in ("DWORD", "UINT"):
        return "UINT", name
    if base == "REFIID":
        return "GPTR", name
    sys.exit(f"unmapped parameter type in {p!r}")


def map_ret(ret):
    base = ret.replace("__attribute__((__stdcall__))", "").strip()
    if base in RET_MAP:
        return RET_MAP[base]
    if base.startswith("D3D"):          # enums (D3DRESOURCETYPE, ...) are 32-bit
        return "UINT"
    sys.exit(f"unmapped return type {ret!r}")


def c_ident(name):
    return name if name not in ("register", "int", "char", "double", "long", "short") else name + "_"


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    include_dir, shim_root = argv[1], argv[2]
    text = preprocess(include_dir)

    protos = []      # (iface, name, ret, [(ctype, pname)], has_output)
    slots = {}
    errors = []
    for iface in INTERFACES:
        members = struct_members(text, iface + "Vtbl")
        slots[iface] = [n for n, _, _ in members]
        impl = IMPLEMENTED.get(iface)
        if impl is None:
            sys.exit(f"{iface}: no IMPLEMENTED entry")
        for name, ret, params in members:
            plist = split_params(params)[1:]          # drop the This pointer
            ps = [map_param(p, i) for i, p in enumerate(plist)]
            out = any(is_output(p) for p in plist)
            protos.append((iface, name, map_ret(ret), ps))
            if name not in impl and out:
                errors.append(f"{iface}::{name} has an output pointer but is not implemented")
            if name not in impl and name not in IUNKNOWN and name not in RESOURCE:
                pass
    if errors:
        sys.exit("refusing to generate stubs for output methods:\n  " + "\n  ".join(errors))

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

    gen = [
        "/* Generated by runtime/shim/tools/gen_d3d9_com.py. Do not edit. */",
        "/* Boilerplate COM methods (IUnknown, IDirect3DResource9) and the IID per object kind. */",
        '#include "d3d9_internal.h"',
        "",
    ]
    raw = "".join(open(os.path.join(include_dir, h), encoding='latin-1').read()
                  for h in ('d3d9.h', 'dsound.h', 'd3dx9core.h', 'd3dx9shader.h'))
    iids = {n: b for n, b in iid_table(raw) if n in KIND}
    for name, b in iids.items():
        gen.append(f"static const uint8_t iid_{name}[16] = {{ " + ", ".join(f"0x{x:02X}" for x in b) + " };")
    gen += ["", "const uint8_t *d3d9_iid_of_kind(uint32_t kind)", "{", "    switch (kind) {"]
    for iface in INTERFACES:
        if iface in iids:
            gen.append(f"    case {KIND[iface]}: return iid_{iface};")
    gen += ["    default: return NULL;", "    }", "}", ""]
    n_generic = 0
    for iface, name, ret, ps in protos:
        if name not in IMPLEMENTED[iface] or name not in GENERIC:
            continue
        if name in ("GetType", "PreLoad", "GetPriority", "SetPriority") and iface not in RESOURCE_KINDS:
            continue
        if name == "GetDevice" and iface not in RESOURCE_KINDS and iface not in (
                "IDirect3DSwapChain9", "IDirect3DVertexDeclaration9", "IDirect3DVertexShader9",
                "IDirect3DPixelShader9", "IDirect3DStateBlock9", "IDirect3DQuery9", "IDirect3DVolume9"):
            continue
        outs = [n for (t, n), raw in zip(ps, [None] * len(ps)) if t == "GPTR" and n.startswith("pp")]
        out_name = next((n for t, n in ps if n.startswith("pp") or n.startswith("ppv")), "")
        # Parameter names come from the prototype: the D3DX headers name QueryInterface's
        # output 'out', not 'ppvObject'.
        body = (GENERIC[name].replace("{KIND}", KIND[iface])
                .replace("{OUT}", c_ident(out_name) if out_name else "0")
                .replace("{RIID}", c_ident(ps[0][1]) if ps else "0")
                .replace("{PPV}", c_ident(ps[1][1]) if len(ps) > 1 else "0"))
        plist = ", ".join(["GPTR This"] + [f"{t} {c_ident(n)}" for t, n in ps])
        gen.append(f"{ret} WINAPI {iface}_{name}({plist})")
        gen.append("{")
        gen.append("    (void)This;")
        for _, n in ps:
            gen.append(f"    (void)({c_ident(n)});")
        if ret != "void":
            gen.append("    " + body)
        else:
            gen.append("    " + body)
        gen.append("}")
        gen.append("")
        n_generic += 1
    with open(os.path.join(shim_root, "src", "d3d9_com_generic.c"), "w") as f:
        f.write("\n".join(gen))
    print(f"generic: {n_generic} boilerplate methods")

    print(f"d3d9_com: {len(protos)} methods in {len(INTERFACES)} interfaces, "
          f"{len(protos) - n_stub} implemented, {n_stub} stubbed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
