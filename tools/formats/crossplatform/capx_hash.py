"""CAPX (.xpac) asset-name hash for Virtua Tennis 3.

Recovered from the PC, PSP and PS3 prototype data (see docs/formats-crossplatform.md).

The hash of an asset is computed from its packer path:

  1. Take the path with '/' replaced by '\\' and upper-cased, including the
     extension, for example  RESOURCE\\SOUND\\SE\\CRIAU\\FOOT.CSB
  2. h = 0; for each byte c of the path read from the END to the START:
         h = (h * 131 + c) mod 2^32
  3. The result is the record's name_hash.

Equivalent BKDR-style loop: forward over the reversed string.

The archive table is sorted by this hash. Each record's fourth field (index_id)
is the asset's rank in the packer's sorted list: sort the paths by the same
upper-cased key, and entry i is the one whose rank is i.

Only the standard library is used. Run `python3 capx_hash.py --selftest`.
"""
import sys

MASK32 = 0xFFFFFFFF
MULT = 131


def capx_key(path):
    """Packer key: upper-case, backslash separators. Used for hashing and sorting."""
    return path.replace("/", "\\").upper()


def capx_name_hash(path):
    """Return the 32-bit CAPX name hash for an asset path (any separator, any case)."""
    h = 0
    for c in reversed(capx_key(path).encode("ascii")):
        h = (h * MULT + c) & MASK32
    return h


def assign_indices(paths):
    """Return {index_id: path} using the packer's sort order (rank of capx_key)."""
    return {i: p for i, p in enumerate(sorted(paths, key=capx_key))}


# Check values: (path as a loose-file name, name_hash) seen in the archives.
# Values are checked against the PC archives in data2/Resource.
SELFTEST_VECTORS = [
    ("resource/sound/SE/CriAu/Foot.csb", 0x1C76F5CD),
    ("resource/sound/KANSEI/E_BG_M_03.adx", 0x64778362),
    ("resource/sound/VOICE_ADX/BALL_GAMES/ENG_M/E_BG_M_03.adx", 0x53113EEC),
    ("resource/language/ch_advise.bin", 0xE5BDB105),
    ("RESOURCE\\LANGUAGE\\CH_ADVISE.BIN", 0xE5BDB105),  # case and separator do not matter
]


def selftest():
    failures = 0
    for path, want in SELFTEST_VECTORS:
        got = capx_name_hash(path)
        status = "ok" if got == want else "FAIL"
        if got != want:
            failures += 1
        print("%-4s %08x %s" % (status, got, path))
    if failures:
        print("%d vector(s) failed" % failures)
        return 1
    print("all %d vectors passed" % len(SELFTEST_VECTORS))
    return 0


def main(argv):
    if argv[:1] == ["--selftest"]:
        return selftest()
    if not argv:
        print("usage: capx_hash.py --selftest | capx_hash.py PATH [PATH ...]")
        return 2
    for p in argv:
        print("%08x  %s" % (capx_name_hash(p), p))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
