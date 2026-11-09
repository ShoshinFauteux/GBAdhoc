#!/usr/bin/env python3
"""p0a_flip_text.py ELF -- flip one byte of .text in place (the runs_stb mutant of the
adversarial review 2026-11-07: a broken release-stable that --runs used to skip)."""
import struct
import sys

p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
shoff, = struct.unpack_from('<I', d, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)


def sh(i):
    return struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize)


stro = sh(shstrndx)[4]
for i in range(shnum):
    s = sh(i)
    nm = d[stro + s[0]:d.index(0, stro + s[0])].decode()
    if nm == '.text':
        d[s[4] + 0x1000] ^= 0xff
        print('flipped .text byte at file offset 0x%x' % (s[4] + 0x1000))
        break
else:
    sys.exit('no .text')
open(p, 'wb').write(d)
