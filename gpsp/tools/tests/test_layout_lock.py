#!/usr/bin/env python3
"""test_layout_lock.py -- tools/layout/layout_lock.py and the committed lock.

* the committed tools/layout/layout.lock parses, has [release] and [lab], and
  every pad it names is a psp/Makefile variable;
* tools/layout/lab-pads.env names only psp/Makefile pads;
* on a synthetic nm: write then check passes; a moved symbol, a closed hole,
  a missing pad symbol and a resized `main` each fail (mutants);
* `solve` gives back the shift a pad needs, after an earlier pad's change,
  and refuses while a hole differs;
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
    check("LAYOUT_PAD_BSCAN_BYTES" in envpads, "lab-pads.env sets the BSCAN pad")
    gone = {"LAYOUT_PAD_BYTES", "LAYOUT_PAD_MAIN_BYTES", "LAYOUT_PAD_FE_BYTES",
            "LAYOUT_PAD_GB_BYTES", "LAYOUT_PAD_CORE_BYTES", "LAYOUT_PAD_RO_BYTES"}
    check(not (gone & (envpads | mkpads | named)),
          "the pads that are pins since C3-3/C3-4 are in no Makefile default, lab-pads.env or `pad` line")
    kinds = set(re.findall(r"^(\w+)\s", "\n".join(l for l in lock.splitlines()
                                              if l and not l.startswith(("#", "["))), re.M))
    check(kinds <= {"at", "size", "gap", "tail", "pad", "pin"}, "layout.lock uses known entry kinds: %s" % sorted(kinds))
    for fl in ("release", "lab", "harness", "harness64", "diagnostic", "soak"):
        sec = re.search(r"^\[%s\]$(.*?)(?=^\[|\Z)" % fl, lock, re.M | re.S)
        pins = re.findall(r"^pin\s+(\w+)\s+0x[0-9a-f]{8}\b", sec.group(1) if sec else "", re.M)
        check(pins == ["main", "fe_head", "fe_common", "fe_tail", "core", "ro"],
              "[%s] has the six pins in link order (ro: C3-4): %s" % (fl, pins))

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

        # solve refuses while a hole differs (critique-8 SF2): the same
        # shifts, plus the warm hole closed -> exit 3, no pad values
        write_nm(m, shifted(sv, 0x0880bb60, -32))
        rc, out = run("solve", "release", m, "--lock", lk, "--pads", pads)
        check(rc == 3 and "HOLE MOVED" in out and "->" not in out,
              "solve: a closed warm hole -> refuses, suggests no pads")
        write_nm(m, shifted(sv, 0x088ab1a0 + 480, -32))  # sv moved B +480
        rc, out = run("solve", "release", m, "--lock", lk, "--pads", pads)
        check(rc == 3 and "tail" in out and "->" not in out,
              "solve: B's hole closed -> refuses, suggests no pads")

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


REL = ["RFU_SCHED=1", "GBA_LINK=1", "REWIND=1", "ONLINE=1", "CHEATS=1",
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
    for over in ("RFU_SCHED=0", "RFU_SCHED=", "GBA_LINK=0", "REWIND=0", "ONLINE=0",
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
