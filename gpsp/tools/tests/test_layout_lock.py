#!/usr/bin/env python3
"""test_layout_lock.py -- tools/layout/layout_lock.py and the committed lock.

* the committed tools/layout/layout.lock parses, has [release] and [lab], and
  every pad it names is a psp/Makefile variable;
* tools/layout/lab-pads.env names only psp/Makefile pads;
* on a synthetic nm: write then check passes; a moved symbol, a closed hole,
  a missing pad symbol and a resized `main` each fail (mutants);
* `solve` gives back the shift a pad needs, after an earlier pad's change.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "layout", "layout_lock.py")
LOCK = os.path.join(ROOT, "tools", "layout", "layout.lock")
PADS = os.path.join(ROOT, "tools", "layout", "lab-pads.env")
MAKEFILE = os.path.join(ROOT, "psp", "Makefile")

fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1


def run(*args):
    p = subprocess.run([sys.executable, TOOL] + list(args),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    return p.returncode, p.stdout


SYMS = [
    # addr, size or None, type, name
    (0x08805080, 0xf10, "T", "update_gba"),
    (0x0880bb60, None, "t", "layout_pad_warm_lookup"),
    (0x0880bb80, 0x600, "T", "block_lookup_translate_thumb"),
    (0x088a7a60, 0x8, "T", "gbab_update_scanline"),
    (0x088ab1a0, 0x600, "T", "gbab_block_lookup_translate_thumb"),
    (0x088ab7c0, 0xe0, "t", "xs_trap_skip"),
    (0x0893b4e0, 0x83bc, "T", "main"),
    (0x08943cac, 0x644, "T", "layout_pad_cold"),
    (0x089442f0, 0x10, "t", "exit_cb"),
    (0x08981cd8, 0xdb8, "T", "layout_pad_fe"),
    (0x08982a90, 0x140, "t", "roster_valid"),
    (0x08b43b00, None, "D", "reg"),
    (0x08b43b00 + 0x100, None, "D", "reg"),  # a second copy: reg#1
]

LOCK_BODY = """# synthetic
[release]
at   main                     0
size main                     0
at   exit_cb                  0
at   roster_valid             0
at   reg#1                    0
gap  layout_pad_warm_lookup block_lookup_translate_thumb 0
tail gbab_block_lookup_translate_thumb 0
pad  LAYOUT_PAD_BSCAN_BYTES gbab_update_scanline main
pad  LAYOUT_PAD_BYTES       layout_pad_cold      exit_cb
pad  LAYOUT_PAD_FE_BYTES    layout_pad_fe        roster_valid
"""


def write_nm(path, syms):
    with open(path, "w") as f:
        f.write("00000000 a REG_R0\n")
        for a, s, t, n in sorted(syms, key=lambda r: (r[0], r[3])):
            if s is None:
                f.write("%08x %s %s\n" % (a, t, n))
            else:
                f.write("%08x %08x %s %s\n" % (a, s, t, n))


def shifted(syms, after, delta, resize=None):
    out = []
    for a, s, t, n in syms:
        if a > after:
            a += delta
        if resize and n == resize[0]:
            s = resize[1]
        out.append((a, s, t, n))
    return out


def main():
    # -- the committed files ------------------------------------------------
    mk = open(MAKEFILE, encoding="latin-1").read()
    mkpads = set(re.findall(r"^(LAYOUT_PAD_\w+)\s*\?=", mk, re.M))
    lock = open(LOCK, encoding="utf-8").read()
    check("[release]" in lock and "[lab]" in lock, "layout.lock has [release] and [lab]")
    named = set(re.findall(r"^pad\s+(\w+)", lock, re.M))
    check(named and named <= mkpads, "every `pad` in layout.lock is a psp/Makefile pad: %s" % sorted(named))
    envpads = set(re.findall(r"^(LAYOUT_PAD_\w+)=\d+$", open(PADS).read(), re.M))
    check(envpads and envpads <= mkpads, "lab-pads.env names only psp/Makefile pads: %s" % sorted(envpads))
    check({"LAYOUT_PAD_BYTES", "LAYOUT_PAD_RO_BYTES"} <= envpads, "lab-pads.env sets the text and RO pads")
    kinds = set(re.findall(r"^(\w+)\s", "\n".join(l for l in lock.splitlines()
                                              if l and not l.startswith(("#", "["))), re.M))
    check(kinds <= {"at", "size", "gap", "tail", "pad"}, "layout.lock uses known entry kinds: %s" % sorted(kinds))

    with tempfile.TemporaryDirectory() as d:
        nm = os.path.join(d, "a.nm")
        lk = os.path.join(d, "t.lock")
        write_nm(nm, SYMS)
        open(lk, "w").write(LOCK_BODY)
        rc, out = run("write", "release", nm, "--lock", lk)
        check(rc == 0, "write records the values")
        txt = open(lk).read()
        check("at   main                     0x14e0" in txt, "write keeps the spacing and records main 0x14e0")
        check("# synthetic" in txt, "write keeps comments")
        check(re.search(r"^gap .* 32$", txt, re.M) is not None, "the warm hole is 32 B")
        check(re.search(r"^tail gbab_block_lookup_translate_thumb 32$", txt, re.M) is not None, "B's hole is 32 B")
        check(re.search(r"^at   reg#1 +0x1c00$", txt, re.M) is not None, "SYM#N picks the N-th copy")
        rc, out = run("check", "release", nm, "--lock", lk)
        check(rc == 0 and "0 differ" in out, "check passes on the recorded ELF")

        # mutants
        m = os.path.join(d, "m.nm")
        write_nm(m, shifted(SYMS, 0x0893b4e0, 32))           # main's neighbours +32
        rc, out = run("check", "release", m, "--lock", lk, "-q")
        check(rc == 1 and "MOVED    at    exit_cb" in out, "mutant: a frontend region moved -> fails")
        write_nm(m, shifted(SYMS, 0x0880bb60, -32))          # warm hole closed
        rc, out = run("check", "release", m, "--lock", lk, "-q")
        check(rc == 1 and "MOVED    gap" in out, "mutant: warm hole closed -> fails")
        write_nm(m, [r for r in SYMS if r[3] != "layout_pad_warm_lookup"])
        rc, out = run("check", "release", m, "--lock", lk, "-q")
        check(rc == 1 and "MISSING" in out, "mutant: warm pad symbol gone -> fails")
        write_nm(m, shifted(SYMS, 0x088ab1a0, -32))          # B's hole closed
        rc, out = run("check", "release", m, "--lock", lk, "-q")
        check(rc == 1 and "tail" in out, "mutant: B's hole closed -> fails")
        write_nm(m, shifted(SYMS, 0x0893b4e0 + 0x83bc, 0, resize=("main", 0x83c0)))
        rc, out = run("check", "release", m, "--lock", lk, "-q")
        check(rc == 1 and "size  main" in out, "mutant: main resized -> fails")

        # solve: everything after B's stub moved +480, then roster_valid -64
        sv = shifted(shifted(SYMS, 0x088a7a60, 480), 0x08981cd8 + 480, -64)
        write_nm(m, sv)
        pads = os.path.join(d, "p.env")
        open(pads, "w").write("LAYOUT_PAD_BSCAN_BYTES=7704\nLAYOUT_PAD_BYTES=1600\nLAYOUT_PAD_FE_BYTES=3512\n")
        rc, out = run("solve", "release", m, "--lock", lk, "--pads", pads)
        check("7704 ->  7224" in out, "solve: BSCAN takes back the +480")
        check("1600 ->  1600" in out, "solve: the text pad needs nothing once BSCAN moved")
        check("3512 ->  3576" in out, "solve: FE takes the -64")
    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
