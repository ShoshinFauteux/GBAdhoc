#!/usr/bin/env python3
"""test_layout_lock.py -- tools/layout/layout_lock.py and the committed lock.

* the committed tools/layout/layout.lock parses, has [release] and [lab], has
  no `pad` line and no pad left in psp/Makefile (every pad is a pin since
  code-health C3-5), and every section has the same pins: the hot order's row
  pins (each named by a `@pin` row of psp/layout/hot_order.txt) and the six
  region pins, in address order;
* on a synthetic nm: write then check passes; a moved symbol, a closed hole,
  a missing symbol and a resized `main` each fail (mutants);
* `solve` is gone (C3-5);
* a .bss shuffle that keeps `_end` but moves the JIT base fails on the base;
* build.sh's policy (`strict`, `verdict`; critique-9 SF3): which arms are
  enforced, ME_LOWLAT 0/1/2/unset, MGIFT, rc 0/1/2 and LAYOUT_LOCK=0, and
  build.sh passes every switch the policy reads.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "layout", "layout_lock.py")
LOCK = os.path.join(ROOT, "tools", "layout", "layout.lock")
ORDER = os.path.join(ROOT, "psp", "layout", "hot_order.txt")
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
    check(not re.search(r"^pad\s", lock, re.M), "layout.lock has no `pad` line (C3-5)")
    check(not mkpads - {"LAYOUT_PAD_TEXT", "LAYOUT_PAD_BSS"},
          "psp/Makefile defaults no layout pad (only the sweep's TEXT/BSS): %s" % sorted(mkpads))
    check(not os.path.exists(os.path.join(ROOT, "tools", "layout", "lab-pads.env")),
          "tools/layout/lab-pads.env is gone (C3-5)")
    kinds = set(re.findall(r"^(\w+)\s", "\n".join(l for l in lock.splitlines()
                                              if l and not l.startswith(("#", "["))), re.M))
    check(kinds <= {"at", "size", "gap", "tail", "pin"}, "layout.lock uses known entry kinds: %s" % sorted(kinds))
    rows = []
    for l in open(ORDER):
        f = l.split("#", 1)[0].split()
        if f and f[0] == "@pin":
            if "a" in f[2]:
                rows.append("row_" + f[1])
            if "b" in f[2]:
                rows.append("row_%s_b" % f[1])
    check(len(rows) == len(set(rows)) and rows, "hot_order.txt's @pin rows name %d distinct pins" % len(rows))
    for fl in ("release", "lab", "harness", "harness64", "diagnostic", "soak"):
        sec = re.search(r"^\[%s\]$(.*?)(?=^\[|\Z)" % fl, lock, re.M | re.S)
        pins = re.findall(r"^pin\s+(\w+)\s+(0x[0-9a-f]{8})\b", sec.group(1) if sec else "", re.M)
        names = [n for n, a in pins]
        check(sorted(names) == sorted(rows + ["frame_loop", "fe_head", "fe_common", "fe_tail", "core", "ro"]),
              "[%s] has a pin for every @pin row and the six region pins (%d)" % (fl, len(names)))
        check([int(a, 16) for n, a in pins] == sorted(int(a, 16) for n, a in pins)
              and names[-6:] == ["frame_loop", "fe_head", "fe_common", "fe_tail", "core", "ro"],
              "[%s] lists its pins in address order, the region pins last" % fl)

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
        check(re.search(r"^gap .* 32$", txt, re.M) is not None, "a gap: 32 B")
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

        rc, out = run("solve", "release", m, "--lock", lk)
        check(rc == 2, "solve is gone (code-health C3-5: every hold is a pin)")

        # the JIT base and the .bss behind it (critique-8 MF3): .bss grows 16 B
        # before the JIT cache and shrinks 16 B after it.  _end does not move,
        # so only an `at` on the cache base catches it.
        bss = SYMS + [
            (0x09024c68, 0x10000, "B", "rom_branch_hash"),
            (0x090fd500, None, "B", "rom_translation_cache_static"),
            (0x092fd500, None, "B", "ram_translation_cache_static"),
            (0x0937f98c, None, "B", "_end"),
        ]
        jl = os.path.join(d, "j.lock")
        open(jl, "w").write("[release]\nat rom_branch_hash 0\nat rom_translation_cache_static 0\n"
                            "at ram_translation_cache_static 0\nat _end 0\n")
        write_nm(nm, bss)
        rc, out = run("write", "release", nm, "--lock", jl)
        rc, out = run("check", "release", nm, "--lock", jl)
        check(rc == 0 and "0 differ" in out, "JIT lock: check passes on the recorded ELF")
        jm = shifted(shifted(bss, 0x09024c68, 16), 0x092fd500 + 16, -16)
        write_nm(m, jm)
        rc, out = run("check", "release", m, "--lock", jl, "-q")
        check(rc == 1 and "rom_translation_cache_static" in out and "_end" not in out,
              "mutant: .bss +16 before the JIT, -16 after -> the cache base fails, _end does not")
    pin_tests()
    policy_tests()
    print("%d failure(s)" % fails)
    return 1 if fails else 0


PIN_LOCK = """[release]
at   exit_cb                  0x12d0
pin  main      0x08931c64
pin  fe_head   0x089392d0
pin  core      0x08a33e98
"""
PIN_NM = [
    (0x08930b20, None, "A", "__pin_from_main"),
    (0x08931c64, None, "A", "__pin_at_main"),
    (0x08937c98, None, "A", "__pin_from_fe_head"),
    (0x089392d0, None, "A", "__pin_at_fe_head"),
    (0x089392d0, 0x20, "t", "exit_cb"),
    (0x08a338d8, None, "A", "__pin_from_core"),
    (0x08a33e98, None, "A", "__pin_at_core"),
]


def pin_tests():
    """The pins (code-health C3-3) and the overrun policy (critique C-3
    point 1): check holds the absolute address, names a wrap as a spent page;
    write never moves a pin; regen refuses without an explicit choice."""
    with tempfile.TemporaryDirectory() as d:
        nm, lk = os.path.join(d, "p.nm"), os.path.join(d, "p.lock")
        write_nm(nm, PIN_NM)
        open(lk, "w").write(PIN_LOCK)
        rc, out = run("check", "release", nm, "--lock", lk)
        check(rc == 0 and "0 differ" in out and "gap 5688 B" in out,
              "pins: check passes where the link put them, and shows the gap")
        rc, out = run("pins", "release", nm, "--lock", lk)
        check(out.strip() == "release main=4420 fe_head=5688 core=1472",
              "pins: the manifest line is the flavour and each gap")
        # stats (code-health C3-6): the slack a change used against a reference
        grown = os.path.join(d, "g.nm")
        write_nm(grown, [(a + (40 if n == "__pin_from_fe_head" else 0), s, t, n) for a, s, t, n in PIN_NM])
        rc, out = run("stats", "release", grown, "--lock", lk, "--ref", nm)
        check(rc == 0 and re.search(r"fe_head\s+5648\s+5688\s+\+40", out) is not None
              and "slack used in all: +40 B" in out and "0 checked value(s) differ" in out,
              "stats: a region 40 B longer used 40 B of its pin's slack, nothing else moved")
        wrapped = [(a + (8192 if a >= 0x08937c98 else 0), s, t, n) for a, s, t, n in PIN_NM]
        write_nm(nm, wrapped)
        rc, out = run("check", "release", nm, "--lock", lk, "-q")
        check(rc == 1 and "MOVED    pin   fe_head" in out and "heap page(s) spent" in out
              and "--spend-page" in out, "pins: a wrap fails the check and says a page was spent")
        rc, out = run("pins", "release", nm, "--lock", lk)
        check("fe_head=MOVED" in out, "pins: a moved pin is MOVED in the manifest line")
        rc, out = run("write", "release", nm, "--lock", lk)
        check("0x089392d0" in open(lk).read(), "pins: write never re-records a pin")
        rc, out = run("regen", "release", "--lock", lk)
        check(rc == 2 and "never decides" in out and open(lk).read().count("0x089392d0") == 1,
              "pins: regen with no choice refuses and changes nothing")
        rc, out = run("regen", "release", "--lock", lk, "--spend-page", "fe_head")
        txt = open(lk).read()
        check(rc == 0 and "0x0893b2d0" in txt and "0x08a35e98" in txt and "0x08931c64" in txt
              and "page spent" in out,
              "pins: --spend-page moves that pin and every later one +8 KiB, says so")
        rc, out = run("check", "release", nm, "--lock", lk, "-q")
        check(rc == 0, "pins: after --spend-page the wrapped build checks clean")
        rc, out = run("regen", "release", "--lock", lk, "--free-page", "fe_head")
        check(rc == 0 and "0x089392d0" in open(lk).read() and "page freed" in out,
              "pins: --free-page gives the page back")
        rc, out = run("regen", "release", "--lock", lk, "--move", "core=0x08a34000")
        check(rc == 0 and "0x08a34000" in open(lk).read() and "ABBA" in out,
              "pins: --move sets one pin and asks for a hardware ABBA")
        rc, out = run("regen", "release", "--lock", lk, "--spend-page", "nope")
        check(rc == 2, "pins: regen of an unknown pin refuses")
        # a row pin listed first in the lock but linked after `main` in address
        # terms is still "later": regen orders by address, not by lock line (C3-5)
        open(lk, "w").write("[release]\npin row_x 0x08920000\npin row_y 0x08800100\n" + PIN_LOCK.split("\n", 1)[1])
        rc, out = run("regen", "release", "--lock", lk, "--spend-page", "row_y")
        txt = open(lk).read()
        check(rc == 0 and "0x08802100" in txt and "0x08922000" in txt and "0x08933c64" in txt,
              "pins: --spend-page moves every pin at a higher address, whatever its lock line")


REL = ["GBA_LINK=1", "REWIND=1", "ONLINE=1", "CHEATS=1",
       "LAYOUT_PIN=1", "INPUT_FRESH=", "MGIFT=", "ME_LOWLAT=",
       "PSP_EXTRA=", "CORE_EXTRA=", "FE_EXTRA=", "GBALINK_LEAN_OUTLINE="]


def strict_of(*over):
    env = dict(kv.split("=", 1) for kv in REL)
    env.update(dict(kv.split("=", 1) for kv in over))
    rc, out = run("strict", *["%s=%s" % kv for kv in env.items()])
    return rc, out.strip()


def policy_tests():
    rc, out = strict_of()
    check(rc == 0 and out == "1", "strict: the release switches (all defaults) -> 1")
    for v, want in (("", "1"), ("2", "1"), ("0", "0"), ("1", "0")):
        rc, out = strict_of("ME_LOWLAT=" + v)
        check(out.split(" ")[0] == want, "strict: ME_LOWLAT=%r -> %s" % (v, want))
    for over in ("GBA_LINK=0", "REWIND=0", "ONLINE=0",
                 "CHEATS=0", "LAYOUT_PIN=0", "INPUT_FRESH=0", "MGIFT=0",
                 "PSP_EXTRA=FOO=1", "CORE_EXTRA=X=0",
                 "FE_EXTRA=-DX", "GBALINK_LEAN_OUTLINE=1"):
        rc, out = strict_of(over)
        check(out.startswith("0 ") and over.split("=")[0] in out,
              "strict: %s -> 0, and says why" % over)
    for over in ("INPUT_FRESH=1", "MGIFT=1", "GBALINK_LEAN_OUTLINE=0"):
        rc, out = strict_of(over)
        check(out == "1", "strict: %s (the default) stays strict" % over)
    rc, out = run("strict", "NOT_KV")
    check(rc == 2, "strict: a word that is not KEY=VALUE -> rc 2")

    cases = [
        # kind, flavour, rc, strict, LAYOUT_LOCK -> result, action
        ("release", "release", 0, 1, "", "release:ok", "ok"),
        ("release", "release", 1, 1, "", "release:differs", "die"),
        ("release", "release", 1, 1, "0", "release:differs", "die"),   # never for a release
        ("release", "release", 1, 0, "", "release:differs", "note"),   # variant arm
        ("release", "release", 2, 1, "", "release:unchecked", "die"),  # psp-nm failed
        ("release", "release", 2, 0, "", "release:unchecked", "note"),
        ("lab", "lab", 0, 1, "", "lab:ok", "ok"),
        ("lab", "lab", 1, 1, "", "lab:differs", "die"),
        ("lab", "lab", 1, 1, "0", "lab:differs", "note"),              # re-pin in progress
        ("lab", "lab", 1, 1, "1", "lab:differs", "die"),
        ("lab", "lab", 3, 1, "0", "lab:unchecked", "die"),
    ]
    for kind, fl, rcv, st, ll, res, act in cases:
        args = ["verdict", kind, fl, str(rcv), str(st)] + ([ll] if ll else [])
        rc, out = run(*args)
        check(rc == 0 and out.split() == [res, act],
              "verdict %s %s rc=%d strict=%d LAYOUT_LOCK=%r -> %s %s" % (kind, fl, rcv, st, ll, res, act))
    rc, out = run("verdict", "release", "release", "1")
    check(rc == 2, "verdict: missing arguments -> rc 2")

    # build.sh hands the policy every switch it reads, and nothing parses the
    # old inline rules any more.
    sh = open(os.path.join(ROOT, "tools", "build.sh")).read()
    i = sh.index('"$LL_PY" strict')
    call = sh[i:sh.index(")", sh.index("GBALINK_LEAN_OUTLINE=", i))]
    for kv in REL:
        k = kv.split("=")[0]
        check('"%s=' % k in call, "build.sh passes %s to layout_lock.py strict" % k)
    check("1111111" not in sh, "build.sh no longer carries the inline strictness string")


if __name__ == "__main__":
    sys.exit(main())
