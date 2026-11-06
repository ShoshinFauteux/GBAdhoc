#!/usr/bin/env python3
"""abi-exit-ci_elfmut.py IN OUT OP NAME [ARG] -- write a mutated COPY of an ELF32-LE for
the strong_syms.py mutant tests.
  bind   NAME 0|1|2     set the symtab binding of NAME (LOCAL/GLOBAL/WEAK)
  rename NAME NEW       rename NAME in .strtab (NEW must not be longer)
"""
import struct
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from elf32 import Elf  # noqa: E402

src, dst, op, name = sys.argv[1:5]
arg = sys.argv[5] if len(sys.argv) > 5 else None
e = Elf(src)
d = bytearray(e.data)
st = e.by_name['.symtab']
strs = e.sections[st['link']]
hits = 0
for k in range(st['size'] // 16):
    off = st['offset'] + 16 * k
    nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, off)
    if e._str(strs['offset'], nm) != name or shndx == 0:
        continue
    hits += 1
    if op == 'bind':
        d[off + 12] = (int(arg) << 4) | (info & 0xf)
    elif op == 'rename':
        a = strs['offset'] + nm
        new = arg.encode() + b'\0' * (len(name) - len(arg))
        assert len(arg) <= len(name)
        d[a:a + len(name)] = new
assert hits == 1, 'expected one defined %s, found %d' % (name, hits)
open(dst, 'wb').write(bytes(d))
print('%s %s %s -> %s' % (op, name, arg, dst))
