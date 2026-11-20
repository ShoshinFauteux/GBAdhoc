#!/usr/bin/env python3
"""stm_one_check.py -- read a DR_RAMDUMP of stm_one.s (see its header).

    python3 stm_one_check.py ramdump.bin      # exit 0 = every case passed

The dump is dr_host's: r0-r16 (17 words), IWRAM (32 KiB), EWRAM (256 KiB).
"""
import struct, sys

CASES = [("start A", 1), ("start B before", 7), ("start B after", 8),
         ("interior A before", 1), ("interior A after", 2), ("fixed B after", 9)]
EW = 17 * 4 + 0x8000


def main(path):
    d = open(path, "rb").read()
    word = lambda off: struct.unpack_from("<I", d, EW + off)[0]
    bad = 0
    for i, (name, want) in enumerate(CASES):
        got = word(4 * i)
        ok = got == want
        bad += not ok
        print("%-15s got %08x want %08x %s" % (name, got, want, "ok" if ok else "FAIL"))
    marker = word(0x104)
    print("rom fail count %d, marker %08x" % (word(0x100), marker))
    if marker != 0x600D600D:
        bad += 1
    print("PASS" if not bad else "FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
