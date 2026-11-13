#!/usr/bin/env python3
"""ro_margin.py ELF [--min N]: how far the RO pad's end sits inside its 64 B step.

The frontend's .rodata pad (LAYOUT_PAD_RO_BYTES, psp/main_psp.c) follows the linker's merged
string table, and the input section after the pad is 64 B aligned, so everything after it
(instance A's tables: layout.lock's netpacket_iface) moves in 64 B steps.  The string table
holds the version string "v1.1.0-<sha7>", and the linker folds a table entry that is a suffix
of another string into it: the table is 4 B shorter for a commit whose short hash ends like an
entry ("59" for ...f0ce959), up to 12 B for three entries.  A pad that ends within those few
bytes of a step puts A's tables 64 B away for such a commit, with no source change.

Prints where the pad ends inside its 64 B step: the bytes above the step below it (the table
may shrink by less than that) and the alignment zeros up to the next section (it may grow by
that).  Exit 1 when either is under --min (default 16).

The pad is found as the zero run after the version string that ends on a 64 B boundary and
is at least as long as the pad; its length comes from the symbol-less run itself, so the
pad's size is read from the build manifest beside the ELF (pspMakeFlags / layoutPadRoBytes)
or given with --pad N."""
import argparse
import json
import os
import re
import struct
import sys


def rodata(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        raise SystemExit("ro_margin: %s is not a little-endian ELF32" % path)
    shoff, = struct.unpack_from("<I", d, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 46)
    def sh(i):
        return struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
    stroff = sh(shstrndx)[4]
    for i in range(shnum):
        name, _t, _f, addr, off, size = sh(i)[:6]
        end = d.index(b"\0", stroff + name)
        if d[stroff + name:end] == b".rodata":
            return addr, d[off:off + size]
    raise SystemExit("ro_margin: %s has no .rodata" % path)


def pad_of(elf, given):
    if given is not None:
        return given
    m = os.path.join(os.path.dirname(os.path.abspath(elf)), "build-manifest.json")
    if os.path.exists(m):
        txt = json.dumps(json.load(open(m)))
        hit = re.search(r"LAYOUT_PAD_RO_BYTES=(\d+)", txt)
        if hit:
            return int(hit.group(1))
    mk = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "psp", "Makefile")
    hit = re.search(r"^LAYOUT_PAD_RO_BYTES \?= (\d+)", open(mk).read(), re.M)
    if not hit:
        raise SystemExit("ro_margin: no pad size (--pad N)")
    return int(hit.group(1))


def margin(addr, d, pad):
    ver = d.find(b"v1.1.0-")
    if ver < 0:
        raise SystemExit("ro_margin: no version string in .rodata")
    i = ver
    while i < len(d):
        if d[i]:
            i += 1
            continue
        j = i
        while j < len(d) and d[j] == 0:
            j += 1
        if j - i >= max(pad, 8) and (addr + j) % 64 == 0:
            return d[ver:ver + 14].decode("ascii", "replace"), addr + i, addr + j, j - i - pad
        i = j
    raise SystemExit("ro_margin: no zero run of the pad's size ends on a 64 B step")


def selftest():
    # a table of 61 B (+3 to its 4 B alignment = 64), pad 20, next section at the 64 B step
    def img(table, pad):
        body = b"v1.1.0-0000000\0" + b"x" * (table - 16) + b"\0"
        body += b"\0" * pad
        body += b"\0" * (-len(body) % 64) + b"\xff" * 64
        return body
    for table, pad, want in ((64, 20, 44), (60, 20, 48), (64, 61, 3), (60, 61, 7)):
        _v, _a, _b, m = margin(0x1000, img(table, pad), pad)
        # the run also counts the table's own terminator (1 B)
        if m - 1 != want:
            raise SystemExit("selftest: table %d pad %d: margin %d, want %d" % (table, pad, m - 1, want))
    # mutant: a 4 B shorter table at a 3 B margin crosses the step (the run ends 64 B earlier)
    a = margin(0x1000, img(64, 61), 61)[2]
    b = margin(0x1000, img(60, 61 + 4), 61)[2]
    if a != b:
        raise SystemExit("selftest: same step expected with the pad grown by the fold")
    a = margin(0x1000, img(128, 68), 68)[2]      # the pad ends 4 B above a step
    b = margin(0x1000, img(124, 68), 68)[2]      # ... and a 4 B fold puts it on the step
    if a - b != 64:
        raise SystemExit("selftest: a 4 B shorter table 4 B above a step must move the next section")
    print("ro_margin selftest: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="?")
    ap.add_argument("--pad", type=int)
    ap.add_argument("--min", type=int, default=16)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    if not a.elf:
        ap.error("ELF required")
    addr, d = rodata(a.elf)
    pad = pad_of(a.elf, a.pad)
    ver, lo, hi, m = margin(addr, d, pad)
    m -= 1                                   # the run's first byte is the table's last terminator
    ok = a.min <= m <= 64 - a.min
    print("ro_margin: %s  pad %d B, next section 0x%x: the pad ends %d B above a 64 B step and %d B "
          "under the next (the table may shrink %d B / grow %d B): %s"
          % (ver, pad, hi, 64 - m, m, 64 - m - 1, m, "ok" if ok else "TOO CLOSE (min %d)" % a.min))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
