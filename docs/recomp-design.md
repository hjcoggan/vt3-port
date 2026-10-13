# Recompiler design (prototype)

Status: 2026-10-14. Prototype for static x86-32 to C translation, tested on programs we compile ourselves. Code: [tools/recomp/](../tools/recomp/). The translator is `recomp.py`. The runtime is in `rt/`. Tests and the runner are in `tests/`.

No game code or data is in the repo. The generated C and `image.bin` go under `dump/recomp/`.

## Approach and why

The port is static recompilation: decode the PE32 image, translate each guest function into portable C, then compile that C natively for arm64. Nothing runs under Wine at run time.

- **C output, not LLVM IR.** The C compiler does register allocation, dead-flag removal, and instruction selection on arm64. The translator stays small and readable, and the output can be inspected and diffed by hand. An LLVM lift would add infrastructure for the same result.
- **capstone for decoding, not Ghidra P-code.** capstone is already in the venv and gives per-instruction operands with sizes. P-code is still an option for better flag and jump-table analysis later.
- **Recursive descent for functions.** Entries come from the entry point, exports, direct call targets, and code addresses named by base relocations. Relocation candidates are checked against jump-table slots, so case labels do not become functions.

## Pipeline

1. `pefile` loads the image into a flat byte array covering `SizeOfImage`, at `ImageBase`.
2. Entries are found in two passes. Hard entries: entry point, exports, and direct call targets. Soft entries: the values at base-relocation sites that point into an executable section, excluding jump-table slots. A soft entry that fails to decode becomes a trap stub. A hard entry that fails is an error.
3. Indirect jumps are resolved to jump tables. The table is `[disp + idx*4]`, or a `mov reg, [disp+idx*4]` followed by `jmp reg`. The entry count comes from the nearest `cmp idx, N` followed by `ja` (count N+1) or `jae` (count N). Targets are read from the image.
4. Each entry is emitted as one C function. Its body is every instruction reachable by flow that does not cross another entry. Branches inside the body are `goto`. A branch to another entry is a tail call. Calls to no-return imports (ExitProcess and similar) end a path.
5. Output: `NAME.c` (the functions plus the `vt3_mod` table) and `image.bin` (the flat image). The runtime loads `image.bin`, so the C file holds code only.

## Memory model

- One 4 GiB `MAP_NORESERVE` reservation (`vt_mem`). A guest address is an offset, so a host pointer is `vt_mem + addr`. Guest 32-bit pointers therefore work on a 64-bit host.
- Accessors are `rd8/16/32/64` and `wr8/16/32/64`, which use `memcpy` and are little-endian. The image sits at its `ImageBase` (0x400000 for these exes).
- Fixed regions: stack top 0x10000000 (1 MiB), heap at 0x20000000 (bump allocator), TEB at 0x7FFD0000 (FS base), and import tokens at 0xFFFF0000 and up.
- Risk for Android: a 4 GiB reservation may be refused on some devices. If so, the translator can record the highest address used and reserve only that.

## Registers, flags, and calls

- **GPRs.** `eax..edi` and `esp` are C locals in each function. They are loaded from `vt3_cpu` at entry and spilled around calls and at returns (`VT_LOAD`, `VT_SPILL`, `VT_RELOAD`).
- **The guest stack is real.** `call` writes the guest return address to memory, and `ret` pops it. This matters because mingw PIC code uses `call next; pop reg` and `__x86.get_pc_thunk` patterns. The translator emits `push`, `pop`, `esp` arithmetic, and `ret imm` as written, so stdcall and cdecl need no special handling. The translator never needs a function signature.
- **Flags are eager C locals.** Every flag-setting instruction computes CF, ZF, SF, OF, PF, and AF. The native optimizer removes the flags nothing reads. I chose this over lazy flags. Lazy flags add state and branches, and the C compiler already does liveness. The cost is bigger generated C and slower `-O0` builds. DF is a local that starts clear. Flags are not passed across calls, which matches what compilers assume.
- **Calls.** A direct call pushes the return address and calls the C function. An indirect call (`call reg`, `call [mem]`, IAT calls) goes to `vt_icall(c, target)`. That is a binary search over a table of `(guest address, function, name)`. The table holds every module function and every import token.
- **Imports.** At load, each IAT slot gets a unique token, and the token is the table key for a shim. A shim is `void f(vt3_cpu *c)`. It reads its arguments from `[esp+4+4i]`. A stdcall shim pops `4 + N` bytes, and a cdecl shim pops 4. An import with no shim aborts with its name when called.
- **Tail jumps** through IAT thunks (`jmp [slot]`) become `vt_icall` followed by `return`.

## Floating point

- **SSE.** Scalar `ss` and `sd` arithmetic, sqrt, min/max, compares (`ucomi`, `comi`), conversions, moves, and bitwise ops. Common packed ops that gcc emits at `-O2 -msse2`: `ps` and `pd` arithmetic, `unpck`, `shufps`, `pshufd`, `movhps`, `movlhps`, `cvttps2dq`, and `paddd`. The results match Wine bit-for-bit because host single and double arithmetic is IEEE and `FLT_EVAL_METHOD` is 0 on arm64. The native build uses `-ffp-contract=off`, so no FMA fusion.
- **x87.** The register stack is `double fp[8]` with `ftop`. Loads and stores of 80-bit values convert to and from `double`. Compares set C0, C2, C3 for `fcom` and EFLAGS for `fcomi`. `fnstsw` reads TOP from `ftop`. This is the main numerical gap: the 64-bit x87 mantissa is not emulated, so results can differ in the last bit. See the test note below.

## Test results

Build variants per test are at -O0 and -O2, plus -Os or the SSE and x87 flag variants where relevant. The reference is the same exe under Wine (WINEDEBUG=-all). A pass needs stdout and the exit code to match.

| Test | -O0 | -O2 | Other variants |
| --- | --- | --- | --- |
| t01_int | PASS | PASS | |
| t02_flags | PASS | PASS | |
| t03_loops | PASS | PASS | |
| t04_switch | PASS | PASS | |
| t05_recursion | PASS | PASS | |
| t06_fnptr | PASS | PASS | |
| t07_imports | PASS | PASS | |
| t08_sse | PASS | PASS | `-msse2 -mfpmath=sse` |
| t09_x87 | PASS | PASS | `-mfpmath=387` |
| t10_structs | PASS | PASS | |
| t11_strings | | | `-Os` PASS, `-minline-all-stringops -mstringop-strategy=rep_byte` PASS |
| t12_bits | PASS | PASS | |
| t13_calls | PASS | PASS | |
| t14_sim | | | SSE `-O2` PASS; x87 `-O2` FAIL |

Result: 27 of 28 variants pass. The one failure is t14 x87 at -O2. Its first mismatch is the first `bucket=` line (Wine `10`, native `13`). I have not confirmed the cause. My best guess is the double-precision x87 gap below, because the SSE build of the same program matches bit-for-bit. The earlier t09 failures were real translator bugs, now fixed: `fxch` picked the wrong slot, one-operand `fmul st(i)` and similar forms had reversed operands, and `fist` ignored the control word's rounding field. gcc sets that field to truncate for `(int)` casts.

Test files and what they cover:

- `t01_int`: 8, 16, and 32-bit arithmetic, shifts, extension, inc/dec, and neg.
- `t02_flags`: all signed and unsigned comparisons, setcc and cmov, and 64-bit add/sub/adc/sbb/shld/shrd.
- `t03_loops`: counted, while, do, break, continue, and goto loops, plus a bubble sort, sieve, and Collatz.
- `t04_switch`: dense jump tables, sparse compare trees, offset tables, and char switches.
- `t05_recursion`: fib, Ackermann, Hanoi, mutual recursion, and a struct parameter through recursion.
- `t06_fnptr`: function pointer tables, a comparator callback, a stdcall callee through a pointer, and a struct of callbacks.
- `t07_imports`: GetTickCount, HeapAlloc/HeapFree, lstrlenA through the IAT, and lstrlenA through a linker thunk.
- `t08_sse`: SSE scalar float and double, bit-exact output.
- `t09_x87`: x87 double and `long double`, with values rounded to 1/1000 (see gaps).
- `t10_structs`: nested structs, bit-fields, a linked list on the heap, and unions.
- `t11_strings`: memcpy, memset, byte compare, and strlen, with `rep movs`, `rep stos`, and `repnz scasb` forced at -Os and with `-minline-all-stringops`.
- `t12_bits`: bsf/bsr/tzcnt, bswap, rol/ror, bt/bts/btr, 32x32 mul, 64-bit division through libgcc, and variable 64-bit shifts.
- `t13_calls`: stdcall with 7 args, cdecl with 9 args, 64-bit args, a large struct returned by value, and varargs.
- `t14_sim`: a float particle simulation, built for both SSE and x87.

## First real input: VT3.exe (NoCD 1.0.1)

Input: `dump/patches/Virtua-Tennis-3_NoCD_Win_EN_for-version-101/VT3.exe`, read-only, SHA-256 `4bfcecc4abf16c505b18...`. Output under `dump/recomp/vt3_nocd/`. Numbers below are from the translator's own analysis, as of this run.

- **Image.** PE32 i386, ImageBase 0x400000, SizeOfImage 0x803000. Entry RVA 0x253b6d (VA 0x653b6d). Sections: `.text` 0x2c8000 bytes, `PSFD00`, `.rdata`, `.data`, `.rsrc`. **No base-relocation directory**, so no relocation-named entries are possible.
- **Imports.** 161 named imports, not the 160 first reported: KERNEL32 109, USER32 23, d3dx9_32 21, WINMM 4, d3d9 2, DSOUND 1, DINPUT8 1.
- **Function discovery.** Hard entries are the entry point and direct call targets: 1029 functions, 0 decode failures. Translation takes about 1.5 s. The output is `vt3.c` (6.9 MB, 49,409 instructions) and `image.bin` (8.4 MB). `vt3.c` passes a clang syntax-only check.
- **Translation coverage.** 1012 of 1029 functions contain no unsupported instruction. 17 functions contain 106 unsupported sites. The sites are MMX and SSE2 integer ops (`pextrw`, `psrlq`, `psllq`, `pcmpeqd`, `pmovmskb`, `pinsrw`, `pmaxsw`), x87 transcendentals and environment ops (`fscale`, `f2xm1`, `fptan`, `fpatan`, `fcos`, `fyl2x`, `fldl2e`, `fidiv`, `fnsave`, `frstor`), `rcr`, `stmxcsr`, `int3`, and segment-register moves. Most sit in one address range near 0x65xxxx. I haven't checked whether that range is CRT code, but the instruction mix suggests it is.
- **Reachability.** Direct call flow reaches 148 KB of the 2.92 MB of executable bytes (5.1%). The rest is reachable only through indirect calls: vtables, COM objects, and callbacks. This is the main discovery gap.
- **Candidate code pointers.** A scan of the non-executable sections finds 6,046 dwords that point into `.text`. 6,033 decode as instructions, and 5,912 reach cleanly. Together they reach about 730 KB. This is an upper bound. Some will be false positives, and none has been validated against a call site yet. The translator does not do this scan yet.
- **Jump tables.** 28 tables were found. 24 of them have no `cmp/ja` bound, so the count came from a heuristic. Some heuristic counts are 0 or 1, and those are not trustworthy. Each heuristic table becomes a `switch` in the output, so this is a known bug to fix before the output is used.
- **Imports in use.** Discovered code references 119 of the 161 imports through the IAT. The runtime has 12 shims, and all 12 match VT3 imports. So 149 imports have no implementation. The most-referenced are `GetLastError` (20 sites), `CloseHandle` (13), `InterlockedIncrement` and `InterlockedDecrement` (12 each), `WideCharToMultiByte` (8), `MessageBoxA` (8), `EnterCriticalSection` (8), `lstrlenA` (8), `lstrcpyA` (8), `GetModuleHandleA` (7), and `GetProcAddress` (7).
- **Exit status.** Without `--allow-unsupported`, the translator exits 2 on this input, because hard functions have unsupported instructions. The run used `--allow-unsupported` so that the coverage report could finish.

What coverage means for the port: the translator gets through the discovered code, but most of the game's code is not discovered yet, and most imports have no implementation. The next steps are the data-pointer scan, a fix for heuristic jump tables, and shims for the D3D, DirectSound, and DirectInput imports.

## Known gaps

- **Precision.** x87 is double precision internally. Windows uses an extended 64-bit mantissa by default, so x87 results can differ in the last bit. Fixing this needs a software 80-bit float in the runtime.
- **x87 functions missing.** Transcendentals (`fsin`, `fcos`, `f2xm1`, `fyl2x`), `fprem`, `fscale`, and `fbstp`. `rcl` and `rcr`. MMX. Other packed SSE ops are added as they appear.
- **Indirect jumps.** Only the absolute table form and the `mov reg; jmp reg` form are recovered. PIC tables with a base register are not handled. An indirect jump with no recoverable table becomes an `vt_icall` tail jump, and aborts if the target is not a known entry. A function reached only through such a jump is not translated.
- **Return addresses are not checked.** `ret` pops the value and discards it. Code that returns to an address it pushed itself will not work.
- **Soft entries.** Relocation-derived candidates may include data that happens to decode as code. Undecodable ones become stubs. Decodable ones are translated but are never called unless a real caller reaches them. VT3.exe has no relocations, so this path is empty for it. Function pointers there need a data-pointer scan, which is not implemented yet.
- **Heuristic jump tables.** A table with no `cmp/ja` bound is sized by reading entries until one is not code. On VT3.exe this gives counts of 0 and 1 for some tables, which is wrong. The fix is to reject unbounded tables.
- **Exceptions and segments.** No SEH. Divide-by-zero and other faults abort (exit 99) instead of raising. Segment registers other than FS are flat. FS is the TEB with only a few fields set.
- **Threads.** The `lock` prefix is ignored. The runtime is single-threaded. The shim heap is not thread-safe.
- **Heap.** Bump allocator. `HeapFree` does nothing, so memory is never reused.
- **Win32.** Shims cover 12 kernel32 functions: GetStdHandle, WriteFile, ExitProcess, GetTickCount, GetProcessHeap, HeapAlloc, HeapFree, lstrlenA, GetModuleHandleA, GetCommandLineA, Sleep, and GetLastError.
- **Address space.** The 4 GiB reservation is untested on Android.
- **Test caveats.** Wine's `HeapFree` leaves garbage in the upper bits of EAX. Test t07 compares `!= 0` to avoid depending on that.

## Next step

The real input, `VT.exe`, is still SafeDisc-encrypted, so it cannot be translated yet. Before that: (1) build an 80-bit software float for x87 so `t09` can compare bit-exact; (2) add SEH and the FS handler chain; (3) run the translator on a real MSVC-built PE once the SafeDisc unpacking produces one, and add shims for what it imports; (4) keep the GPRs in registers across the flow more often, and measure `-O2` code size on a large module.
