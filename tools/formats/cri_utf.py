"""Reader for CRI "@UTF" tables (big-endian), used by CRI Atom cue sheets (ACB).

A @UTF table is a schema (column list) followed by rows. Offsets are relative
to byte 8 of the table (the byte after the 4-byte magic and 4-byte size).

Header (big-endian, offsets from the start of the table):
    0x00  "@UTF"
    0x04  u32  table size (bytes after this field)
    0x08  u16  unknown (0 or 1 seen)
    0x0A  u16  row data offset (relative to 0x08)
    0x0C  u32  string pool offset (relative to 0x08)
    0x10  u32  blob pool offset (relative to 0x08)
    0x14  u32  table name (string pool offset)
    0x18  u16  column count
    0x1A  u16  row width in bytes
    0x1C  u32  row count
    0x20  column definitions, then row data

Column definition: u8 flags, then u32 name (string pool offset), then (if the
storage bits say so) a constant value or per-row values.
    flags & 0x0F  type: 0 u8, 1 s8, 2 u16, 3 s16, 4 u32, 5 s32, 6 u64, 7 s64,
                        8 f32, 0xA string, 0xB blob (u32 offset + u32 size)
    flags & 0xF0  storage: 0x10 none, 0x30 constant (value stored in schema),
                           0x50 per row (value stored in the row)

Known from the SE.xpac payloads (CRI Atom cue sheets, see docs/formats.md).
This parser was written against those payloads. Unknown flag combinations raise.
"""
import struct

TYPE_SIZES = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4, 0xA: 4, 0xB: 8}
STORAGE_NONE = 0x10
STORAGE_CONST = 0x30
STORAGE_ROW = 0x50


class UtfTable(object):
    def __init__(self, name, columns, rows):
        self.name = name
        self.columns = columns  # list of (name, type_code)
        self.rows = rows  # list of dict column name -> value

    def __repr__(self):
        return "<UtfTable %s cols=%d rows=%d>" % (self.name, len(self.columns), len(self.rows))


def _cstr(data, pos):
    end = data.index(b"\0", pos)
    return data[pos:end].decode("utf-8", "replace")


def _read_value(data, pos, type_code, strings_base, blob_base):
    fmt_size = TYPE_SIZES[type_code]
    if type_code == 0xA:
        off = struct.unpack_from(">I", data, pos)[0]
        return _cstr(data, strings_base + off), pos + 4
    if type_code == 0xB:
        off, size = struct.unpack_from(">II", data, pos)
        start = blob_base + off
        return bytes(data[start : start + size]), pos + 8
    if type_code == 8:
        return struct.unpack_from(">f", data, pos)[0], pos + 4
    fmt = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q"}[type_code]
    return struct.unpack_from(fmt, data, pos)[0], pos + fmt_size


def parse(data, base=0):
    """Parse one @UTF table at data[base:]. Returns UtfTable."""
    if data[base : base + 4] != b"@UTF":
        raise ValueError("no @UTF magic at %d" % base)
    b8 = base + 8
    row_off, strings_off, blob_off, name_off, ncol, row_width, nrows = struct.unpack_from(
        ">HIIIHHI", data, base + 0x0A
    )
    # 0x0A is u16 row_off; the struct above starts at 0x0A
    strings_base = b8 + strings_off
    blob_base = b8 + blob_off
    table_name = _cstr(data, strings_base + name_off)

    pos = base + 0x20
    columns = []  # (name, type, storage, const_value, has_row)
    for _ in range(ncol):
        flags = data[pos]
        pos += 1
        name_ptr = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        col_name = _cstr(data, strings_base + name_ptr)
        storage = flags & 0xF0
        type_code = flags & 0x0F
        const_value = None
        if storage == STORAGE_CONST:
            const_value, pos = _read_value(data, pos, type_code, strings_base, blob_base)
        elif storage not in (STORAGE_NONE, STORAGE_ROW):
            raise ValueError("unknown storage 0x%02x for column %s" % (flags, col_name))
        columns.append((col_name, type_code, storage, const_value))

    rows = []
    row_start = b8 + row_off
    for r in range(nrows):
        p = row_start + r * row_width
        row = {}
        for col_name, type_code, storage, const_value in columns:
            if storage == STORAGE_CONST:
                row[col_name] = const_value
            elif storage == STORAGE_ROW:
                value, p = _read_value(data, p, type_code, strings_base, blob_base)
                row[col_name] = value
            else:
                row[col_name] = None
        rows.append(row)
    return UtfTable(table_name, [(c[0], c[1]) for c in columns], rows)


def parse_all(data, base=0):
    """Parse the table at base. Blobs are returned as bytes, text as str."""
    return parse(data, base)
