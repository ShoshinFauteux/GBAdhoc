#!/usr/bin/env python3
"""mksynth.py OUT SIZE -- a synthetic MBC5+RAM+BATTERY Game Boy Color cart of
SIZE bytes (1, 2 or 8 MiB) for the cartridge-transfer measurements.

It is the host tests' busy program (tools/tests/gblink_session_tests.py:
it runs, writes its cartridge RAM and draws), padded with deterministic,
incompressible filler, so the largest cartridge a Game Boy can address
(8 MiB) can be sent across the link without any commercial game.  The same
arguments always give the same bytes (SHA-1 of the 8 MiB image:
508a795139d581445d7c03cb0771af1d30501434)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', 'tests'))
from gblink_session_tests import rom  # noqa: E402

size = int(sys.argv[2])
base = bytearray(rom(True, 0x77, 0x3C, 0x20000))
base[0x147] = 0x1B              # MBC5+RAM+BATTERY
base[0x148] = {1 << 20: 5, 2 << 20: 6, 8 << 20: 8}[size]
base[0x149] = 0x03              # 32 KiB of cartridge RAM
c = 0
for i in range(0x134, 0x14D):
    c = (c - base[i] - 1) & 0xFF
base[0x14D] = c
d = bytearray(size)
d[:len(base)] = base
for i in range(len(base), size):      # deterministic filler, not compressible
    d[i] = (i * 2654435761 >> 13) & 0xFF
with open(sys.argv[1], 'wb') as fh:
    fh.write(bytes(d))
