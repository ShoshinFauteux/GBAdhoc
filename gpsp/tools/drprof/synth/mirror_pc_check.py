#!/usr/bin/env python3
"""mirror_pc_check.py -- read a DR_RAMDUMP of mirror_pc.s (see its header).

    python3 mirror_pc_check.py ramdump.bin    # exit 0 = every case passed

The dump is dr_host's: r0-r16 (17 words), IWRAM (32 KiB), EWRAM (256 KiB).
A want of None is a call through an alias: stored, printed, not checked (the
GBA returns the alias there, the fixed dynarec the canonical address).
"""
import struct, sys

CASES = [("arm via alias", None), ("arm canonical", 0x03000408),
         ("exit H2", 5), ("exit F2", 0x03000488),
         ("thumb via alias", None), ("thumb canonical", 0x03000604),
         ("ewram via alias", None), ("ewram canonical", 0x02001008),
         ("ewexit H5", 6), ("ewexit F5", 0x02001108),
         ("smc alias", 1), ("smc alias after", 2), ("smc canon after", 2),
         ("smc canon alias-st", 3), ("smc alias alias-st", 3),
         ("stm alias first", 2), ("stm canon pasted", 10),
         ("stm alias pasted", 10)]
EW = 17 * 4 + 0x8000


def main(path):
    d = open(path, "rb").read()
    word = lambda off: struct.unpack_from("<I", d, EW + off)[0]
    bad = 0
    for i, (name, want) in enumerate(CASES):
        got = word(4 * i)
        if want is None:
            print("%-19s got %08x (not checked)" % (name, got))
            continue
        ok = got == want
        bad += not ok
        print("%-19s got %08x want %08x %s" % (name, got, want,
                                               "ok" if ok else "FAIL"))
    marker = word(0x104)
    print("rom fail count %d, marker %08x" % (word(0x100), marker))
    if marker != 0x600D600D:
        bad += 1
    print("PASS" if not bad else "FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
