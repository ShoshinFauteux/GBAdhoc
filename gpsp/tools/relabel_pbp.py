#!/usr/bin/env python3
"""Change an existing PBP's XMB title without rebuilding its executable."""
from pathlib import Path
import struct
import sys

def relabel(data, title):
    out = bytearray(data)
    assert out[:4] == b'\0PBP'
    sections = struct.unpack_from('<8I', out, 8)
    sfo = sections[0]
    magic, version, keys, values, count = struct.unpack_from('<5I', out, sfo)
    assert magic == 0x46535000
    for i in range(count):
        entry = sfo + 20 + i * 16
        key, fmt, length, capacity, value = struct.unpack_from('<HHIII', out, entry)
        if bytes(out[sfo+keys+key:]).split(b'\0', 1)[0] != b'TITLE':
            continue
        encoded = title.encode('utf-8') + b'\0'
        if len(encoded) > capacity:
            raise ValueError('New title exceeds existing SFO capacity')
        start = sfo + values + value
        assert start + capacity <= sections[1]
        out[start:start+capacity] = encoded.ljust(capacity, b'\0')
        struct.pack_into('<I', out, entry + 4, len(encoded))
        assert bytes(out[sections[1]:]) == data[sections[1]:]
        return bytes(out)
    raise ValueError('PBP has no TITLE')

if __name__ == '__main__':
    source, destination, title = sys.argv[1:]
    Path(destination).write_bytes(relabel(Path(source).read_bytes(), title))
