"""Shared reader for CAPX (.xpac) archives in Virtua Tennis 3 (PC).

Layout (little-endian), see docs/formats.md:

    0x00  char[4]  magic        "CAPX"
    0x04  u32      count        number of table entries
    0x08  u32      header_size  0x10; the table starts here
    0x0C  u32      align        payload alignment (0x800 .. 0x6000 seen)
    0x10  record[count], 16 bytes each: u32 name_hash, u32 offset, u32 size, u32 index_id

The table is sorted ascending by name_hash. name_hash is not decoded yet.
index_id is a permutation of 0..count-1 (probably the original asset number).

Only the stdlib is used, so this module works in any Python 3.
"""
import mmap
import os
import struct

HEADER = struct.Struct("<4sIII")
RECORD = struct.Struct("<IIII")


class Entry(object):
    __slots__ = ("index", "hash", "offset", "size", "index_id")

    def __init__(self, index, name_hash, offset, size, index_id):
        self.index = index
        self.hash = name_hash
        self.offset = offset
        self.size = size
        self.index_id = index_id


def read_table(data):
    """Return the list of Entry records for an mmap or bytes object."""
    magic, count, header_size, _align = HEADER.unpack_from(data, 0)
    if magic != b"CAPX":
        raise ValueError("not a CAPX archive (magic=%r)" % magic)
    entries = []
    for i in range(count):
        h, off, size, uid = RECORD.unpack_from(data, header_size + i * RECORD.size)
        if off + size > len(data):
            raise ValueError("entry %d runs past end of file" % i)
        entries.append(Entry(i, h, off, size, uid))
    return entries


def open_archive(path):
    """Context manager style helper: returns (file, mmap). Close both when done."""
    f = open(path, "rb")
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    return f, m


def archive_stem(path):
    return os.path.splitext(os.path.basename(path))[0]


def payload(data, entry):
    return data[entry.offset : entry.offset + entry.size]


def iter_payloads(path):
    """Yield (entry, payload_bytes) for every entry in the archive at path."""
    f, m = open_archive(path)
    try:
        for e in read_table(m):
            yield e, bytes(m[e.offset : e.offset + e.size])
    finally:
        m.close()
        f.close()
