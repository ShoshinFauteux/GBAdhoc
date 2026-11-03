#!/usr/bin/env python3
"""nv_cond_gen.py -- ARM condition 0xF ("never") test ROM (NV_COND_FIX, issue #10).

    python3 nv_cond_gen.py gen OUT.gba [--only CASE]   # write the ROM
    python3 nv_cond_gen.py check DUMP                  # judge a DR_RAMDUMP
    python3 nv_cond_gen.py check-ram EWRAM.bin         # judge a bare EWRAM image
    python3 nv_cond_gen.py cases                       # list the case names

No toolchain: the ROM is hand-encoded here, because the shape under test is data
in the instruction stream.  An ARM7TDMI never executes an NV word (GBATEK: reserved
from ARMv3; condition false costs 1S; mGBA and cpu.cc skip it).  Before
NV_COND_FIX the translator stored through a NULL backpatch_address, so
`gen --only exits` on a -DGPSP_NO_NV_COND_FIX twin is the issue #10 crash: the
store at the block-end check, `sh v0,0(v1)` with v1 = 0, last_condition 0x1F.

Every case runs from ROM, IWRAM and EWRAM (the three scan paths: scan_block(arm,
no), xlat_scan_arm_ram[_fast]): NV first, mid and last in a block, among
conditionals, as a branch target, in a loop, in every shape that would otherwise
be an exit point (B, BL, SWI, HLE div, BX, MOV pc, LDR pc, LDM pc, coprocessor)
or a side effect (flags, stores, loads, multiply, MSR).  RAM adds self-modifying
words (AL <-> NV, and SMC_IMM_PATCH's alternating immediate).  Then fixed
addresses: the crash's own word at 0x0300764C, a run of it that MAX_EXITS ends (the
crash's path), and NV at the last word before the RAM-end clamp 0x03007FF0, where
it is truly last in its block.

Results from EWRAM 0x02000100, five words a case: tag, r0..r3.  0x02000000 =
0xC0DE600D, 0x02000004 = end.  The expected values are this file's model; the
interpreter (gpsp_drc=disabled) must agree with them too.
"""
import struct, sys

EQ, NE, AL, NV = 0x0, 0x1, 0xE, 0xF
AND, EOR, SUB, RSB, ADD, CMP, ORR, MOV = 0, 1, 2, 3, 4, 10, 12, 13
MAGIC = 0xC0DE600D
RES = 0x02000100
ROM = 0x08000000
IWRAM_RUN = 0x03001000
EWRAM_RUN = 0x02010000


class Asm:
    def __init__(self, labels, base=ROM):
        self.o = []
        self.base = base
        self.labels = labels    # from the previous pass
        self.new = {}
        self.uniq = 0

    @property
    def pc(self):
        return self.base + 4 * len(self.o)

    def w(self, x):
        self.o.append(x & 0xFFFFFFFF)

    def label(self, n):
        self.new[n] = self.pc

    def fresh(self, stem):
        self.uniq += 1
        return "%s_%d" % (stem, self.uniq)

    def at(self, n):
        return self.labels.get(n, self.pc)

    def dp(self, op, rd, rn, imm, cond=AL, s=0, rot=0):
        assert 0 <= imm < 256
        self.w((cond << 28) | (1 << 25) | (op << 21) | (s << 20) | (rn << 16) |
               (rd << 12) | (rot << 8) | imm)

    def add(self, rd, rn, imm, cond=AL):
        self.dp(ADD, rd, rn, imm, cond)

    def mov(self, rd, imm, cond=AL):
        self.dp(MOV, rd, 0, imm, cond)

    def movr(self, rd, rm):
        self.w(0xE1A00000 | (rd << 12) | rm)

    def subs(self, rd, rn, imm):
        self.dp(SUB, rd, rn, imm, s=1)

    def cmp(self, rn, imm, cond=AL):
        self.dp(CMP, 0, rn, imm, cond, s=1)

    def cmpr(self, rn, rm):
        self.w(0xE1500000 | (rn << 16) | rm)

    def ld32(self, rd, v):
        self.mov(rd, v & 0xFF)
        self.dp(ORR, rd, rd, (v >> 8) & 0xFF, rot=12)
        self.dp(ORR, rd, rd, (v >> 16) & 0xFF, rot=8)
        self.dp(ORR, rd, rd, (v >> 24) & 0xFF, rot=4)

    def ldr(self, rd, rn, off=0):
        self.w(0xE5900000 | (rn << 16) | (rd << 12) | off)

    def str_(self, rd, rn, off=0):
        self.w(0xE5800000 | (rn << 16) | (rd << 12) | off)

    def ldrh(self, rd, rn):
        self.w(0xE1D000B0 | (rn << 16) | (rd << 12))

    def strh(self, rd, rn, off):
        self.w(0xE1C000B0 | (rn << 16) | (rd << 12) | ((off & 0xF0) << 4) | (off & 0xF))

    def b(self, n, cond=AL, link=0):
        off = (self.at(n) - self.pc - 8) >> 2
        self.w((cond << 28) | ((0xB if link else 0xA) << 24) | (off & 0xFFFFFF))

    def bl(self, n):
        self.b(n, link=1)

    def bx(self, rm, cond=AL):
        self.w((cond << 28) | 0x012FFF10 | rm)

    def call(self, rm):
        self.movr(14, 15)       # mov lr, pc: the word after the bx
        self.bx(rm)


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


# Each case: emit the body on `a` (r0..r3 zero on entry, r3 stays usable as a
# zero for cmp) and return the expected r0..r3.

@case
def first_in_block(a):
    L = a.fresh("L")
    a.mov(0, 1)
    a.b(L)
    a.label(L)
    a.add(0, 0, 0x10, NV)
    a.add(0, 0, 2)
    return 3,


@case
def mid_block(a):
    a.mov(0, 1)
    a.add(0, 0, 2)
    a.add(0, 0, 0x10, NV)
    a.add(0, 0, 4)
    return 7,


@case
def last_before_exit(a):
    L = a.fresh("L")
    a.mov(0, 1)
    a.add(0, 0, 0x10, NV)
    a.b(L)
    a.label(L)
    return 1,


@case
def last_after_cond_exit(a):
    X = a.fresh("X")
    a.cmp(3, 1)                     # Z clear
    a.b(X, EQ)
    a.add(0, 0, 0x10, NV)
    a.add(0, 0, 0x20, EQ)
    a.label(X)
    a.add(0, 0, 1)
    return 1,


@case
def run_of_nv(a):
    a.mov(0, 1)
    for k in range(8):
        a.add(0, 0, 0x10, NV)
    a.add(0, 0, 2)
    return 3,


@case
def mixed_z_set(a):
    a.cmpr(3, 3)
    a.add(0, 0, 1, EQ)
    a.add(0, 0, 2, NV)
    a.add(0, 0, 4, EQ)
    a.add(0, 0, 8, NE)
    a.add(0, 0, 16, NV)
    a.add(0, 0, 32)
    return 37,


@case
def mixed_z_clear(a):
    a.cmp(3, 1)
    a.add(0, 0, 1, EQ)
    a.add(0, 0, 2, NV)
    a.add(0, 0, 4, EQ)
    a.add(0, 0, 8, NE)
    a.add(0, 0, 16, NV)
    a.add(0, 0, 32)
    return 40,


@case
def stale_backpatch(a):
    # a false condition, NV, then AL: the old header left the first skip
    # re-patched past the NV word's code, which then ran.
    a.cmpr(3, 3)
    a.add(0, 0, 1, NE)
    a.add(0, 0, 2, NV)
    a.add(0, 0, 4)
    return 4,


@case
def true_cond_then_nv(a):
    a.cmpr(3, 3)
    a.add(0, 0, 1, EQ)
    a.add(0, 0, 2, NV)
    a.add(0, 0, 4)
    return 5,


@case
def nv_then_cond(a):
    a.cmpr(3, 3)
    a.add(0, 0, 1, NV)
    a.add(0, 0, 2, EQ)
    a.add(0, 0, 4, NE)
    a.add(0, 0, 8, NV)
    return 2,


@case
def nv_sets_no_flags(a):
    a.cmpr(3, 3)
    a.cmp(3, 1, NV)                 # would clear Z, set N and carry-clear
    a.dp(ADD, 2, 2, 0, NV, s=1)     # ADDSNV
    a.w(0xF328F20F)                 # MSRNV cpsr_f, #0xF0000000
    a.add(0, 0, 1, EQ)
    a.add(0, 0, 2, 6)               # VS
    return 1,


@case
def nv_branches(a):
    over, junk = a.fresh("over"), a.fresh("junk")
    a.mov(14, 0x55)
    a.b(junk, NV)
    a.b(junk, NV, link=1)
    a.w(0xFAFFFFFE)                 # BNV to itself
    a.w(0xFB7FFFFF)                 # BLNV to nowhere
    a.movr(1, 14)
    a.add(0, 0, 1)
    a.b(over)
    a.label(junk)
    a.mov(0, 0xFF)
    a.bx(14)
    a.label(over)
    return 1, 0x55,


@case
def nv_swi(a):
    a.mov(0, 100)
    a.mov(1, 7)
    a.w(0xFFFFFFFF)                 # SWI 0xFFFFFF
    a.w(0xFF060000)                 # HLE Div
    a.w(0xFF070000)                 # HLE DivArm
    a.w(0xFF000000)                 # SoftReset
    a.w(0xFFF5FFF6)                 # the issue #10 word
    a.w(0xE10F2000)                 # mrs r2, cpsr
    a.dp(AND, 2, 2, 0x1F)
    return 100, 7, 0x1F,


@case
def nv_pc_writes(a):
    a.movr(3, 13)
    a.mov(14, 0x40)
    a.w(0xF1A0F00E)                 # MOVNV pc, lr
    a.w(0xF1B0F00E)                 # MOVSNV pc, lr
    a.w(0xF12FFF1E)                 # BXNV lr
    a.w(0xF59DF000)                 # LDRNV pc, [sp]
    a.w(0xF8BD8000)                 # LDMNV sp!, {pc}
    a.w(0xF8FD8000)                 # LDMNV sp!, {pc}^
    a.w(0xF28FF004)                 # ADDNV pc, pc, #4
    a.dp(RSB, 0, 3, 0)              # rsb r0, r3, #0
    a.w(0xE080000D)                 # add r0, r0, sp: 0 unless sp moved
    a.mov(3, 0)
    return 0,


@case
def nv_side_effects(a):
    a.ld32(4, 0x02000040)
    a.mov(1, 0x5A)
    a.str_(1, 4)
    a.mov(2, 0xA5)
    a.w(0xF5842000)                 # STRNV r2, [r4]
    a.w(0xF5943000)                 # LDRNV r3, [r4]
    a.w(0xF8840004)                 # STMNV r4, {r2}
    a.w(0xF1D430B0)                 # LDRHNV r3, [r4]
    a.w(0xF0010292)                 # MULNV r1, r2, r2
    a.w(0xF1041092)                 # SWPNV r1, r2, [r4]
    a.w(0xF10F3000)                 # MRSNV r3, cpsr
    a.w(0xFC000000)                 # coprocessor shapes
    a.w(0xFD000000)
    a.w(0xFE000010)
    a.ldr(0, 4)
    return 0x5A, 0x5A, 0xA5,


@case
def target_is_nv(a):
    T = a.fresh("T")
    a.cmp(3, 1)                     # Z clear
    a.b(T, NE)
    a.add(0, 0, 1, EQ)
    a.label(T)
    a.add(0, 0, 2, NV)
    a.add(0, 0, 4, EQ)
    a.add(0, 0, 8)
    return 8,


@case
def target_after_nv(a):
    T = a.fresh("T")
    a.cmpr(3, 3)
    a.add(0, 0, 1, EQ)
    a.b(T, NE)
    a.add(0, 0, 2, EQ)
    a.add(0, 0, 4, NV)
    a.label(T)
    a.add(0, 0, 8, EQ)
    a.add(1, 1, 1, NE)
    return 11,


@case
def loop_over_nv(a):
    L = a.fresh("L")
    a.mov(1, 3)
    a.label(L)
    a.add(0, 0, 0x10, NV)
    a.add(0, 0, 1)
    a.subs(1, 1, 1)
    a.b(L, NE)
    return 3, 0,


@case
def data_entered(a):
    # A block that STARTS on data, the crash's shape: a word that is an NV SWI
    # followed by a word of another condition; then one of nothing but NV.
    over, F, G = a.fresh("over"), a.fresh("F"), a.fresh("G")
    a.b(over)
    a.label(F)
    a.w(0xFFF5FFF6)
    a.add(0, 0, 1)
    a.bx(14)
    a.label(G)
    for k in range(4):
        a.w(0xFFFFFFFF)
    a.add(0, 0, 2)
    a.bx(14)
    a.label(over)
    a.bl(F)
    a.bl(G)
    return 3,


# An NV word's two instruction-timing facts: it costs what `mov r0, r0` costs.
@case
def timing(a):
    a.ld32(4, 0x04000100)
    a.mov(5, 0x80)
    a.strh(5, 4, 2)                 # TM0: on, prescale 1
    a.ldrh(1, 4)
    for k in range(32):
        a.add(0, 0, 0x10, NV)
    a.ldrh(2, 4)
    for k in range(32):
        a.movr(0, 0)
    a.ldrh(3, 4)
    a.mov(5, 0)
    a.strh(5, 4, 2)
    a.w(0xE0420001)                 # sub r0, r2, r1
    a.w(0xE0433002)                 # sub r3, r3, r2
    a.w(0xE0400003)                 # sub r0, r0, r3
    a.mov(1, 0)
    a.mov(2, 0)
    a.mov(3, 0)
    return 0,


def begin(a, tag):
    for r in range(4):
        a.mov(r, 0)
    return tag


def rec(a, tag):
    a.ld32(11, tag)
    a.w(0xE48AB004)                 # str r11, [r10], #4
    a.w(0xE8AA000F)                 # stmia r10!, {r0-r3}


def smc_case(a):
    """RAM only.  `site`'s first word is rewritten between calls."""
    r0 = 0
    over, site = a.fresh("over"), a.fresh("site")
    a.b(over)
    a.label(site)
    a.add(0, 0, 1)
    a.add(0, 0, 0x10)
    a.bx(14)
    a.label(over)
    off = (a.at(site) - a.pc - 8) & 0xFFFFFFFF
    a.dp(SUB, 4, 15, (-off) & 0xFF)
    # SMC_IMM_PATCH's learning shape: same word, immediate changing
    loop = a.fresh("loop")
    a.mov(6, 0)
    a.label(loop)
    a.ld32(5, 0xE2800000)
    a.w(0xE1855006)                 # orr r5, r5, r6
    a.str_(5, 4)
    a.bl(site)
    a.add(6, 6, 1)
    a.cmp(6, 64)
    a.b(loop, NE)
    for i in range(64):
        r0 += i + 0x10
    # AL -> NV -> NV with another immediate -> AL -> conditional -> NV -> AL
    for word, delta in ((0xF2800001, 0), (0xF2800002, 0), (0xE2800002, 2),
                        (0x02800004, 4), (0xFFFFFFFF, 0), (0xE2800008, 8)):
        a.ld32(5, word)
        a.str_(5, 4)
        a.cmpr(3, 3)
        a.bl(site)
        r0 += delta + 0x10
    return r0,


def body(a, fn, n, exp):
    tag = begin(a, 0x4E560000 | n)
    e = fn(a)
    e = tuple(e) + (0,) * (4 - len(e))
    rec(a, tag)
    exp.append((tag,) + e)


# Fixed addresses, written and called from ROM: (name, base, words, entry, r0)
BX_LR = 0xE12FFF1E
FRAGS = {
    # the word of the issue #10 dump, at its address: s3 = last pc translated
    # 0300764C, s6 = last_condition 0x1F
    "stack": (0x0300764C, [0xFFF5FFF6, 0xE2800001, BX_LR], 0, 1),
    # ...and that dump's path: no condition in the block but NV, every word an
    # exit point, so MAX_EXITS ends the scan and the block-end patch takes NULL
    "exits": (0x03007300, [0xFFF5FFF6] * 32 + [0xE2800004, BX_LR], 0, 4),
    "allnv": (0x03007700, [0xFFFFFFFF] * 3 + [0xE2800002, BX_LR], 0, 2),
    # the RAM-end clamp: 0x03007FEC is the block's last word
    "ramend": (0x03007FE0, [0xE2800001, 0xE2800002, 0xE2800004, 0xF2800008,
                            BX_LR], 0, 7),
    "ramend1": (0x03007FE0, [0xE2800001, 0xE2800002, 0xE2800004, 0xF2800008,
                             BX_LR], 3, 0),
}


def frag(a, name, n, exp):
    base, words, entry, r0 = FRAGS[name]
    tag = begin(a, 0x4E570000 | n)
    a.ld32(4, base)
    for k, v in enumerate(words):
        a.ld32(5, v)
        a.str_(5, 4, k * 4)
    a.ld32(4, base + entry * 4)
    a.call(4)
    rec(a, tag)
    exp.append((tag, r0, 0, 0, 0))


def program(labels, only):
    a = Asm(labels)
    exp = []
    cases = [c for c in CASES if not only or c.__name__ == only]
    frags = [f for f in FRAGS if not only or only in ("issue", "fragments", f)]
    if only == "issue":
        cases, frags = [], ["stack"]
    if only == "fragments":
        cases = []

    a.b("init")
    while len(a.o) < 0xC0 // 4:
        a.w(0)
    a.label("init")
    a.w(0xE321F0DF)                 # msr cpsr_c, #0xDF: SYS, IRQ and FIQ off
    a.ld32(13, 0x03007F00)
    a.ld32(10, RES)
    if cases:
        # the same bytes from three places
        a.bl("common")
        a.ld32(6, IWRAM_RUN)
        a.bl("copy")
        a.ld32(4, IWRAM_RUN)
        a.call(4)
        a.ld32(4, IWRAM_RUN + labels.get("ram_only", 0) - labels.get("common", 0))
        a.call(4)
        a.ld32(6, EWRAM_RUN)
        a.bl("copy")
        a.ld32(4, EWRAM_RUN)
        a.call(4)
        a.ld32(4, EWRAM_RUN + labels.get("ram_only", 0) - labels.get("common", 0))
        a.call(4)
    for k, f in enumerate(frags):
        frag(a, f, k, exp)
    a.ld32(4, 0x02000000)
    a.ld32(5, MAGIC)
    a.str_(5, 4)
    a.str_(10, 4, 4)
    a.label("done")
    a.b("done")

    a.label("copy")                 # r6 = destination of [common, code_end)
    a.ld32(7, labels.get("common", 0))
    a.ld32(8, labels.get("code_end", 0))
    a.label("copy1")
    a.ldr(5, 7)
    a.str_(5, 6)
    a.add(7, 7, 4)
    a.add(6, 6, 4)
    a.w(0xE1570008)                 # cmp r7, r8
    a.b("copy1", NE)
    a.bx(14)

    a.label("common")
    a.movr(9, 14)
    expc = []
    for n, c in enumerate(cases):
        body(a, c, n, expc)
    a.bx(9)
    a.label("ram_only")
    a.movr(9, 14)
    exps = []
    if cases:
        body(a, smc_case, 0x80, exps)
    a.bx(9)
    a.label("code_end")
    if cases:
        rom_exp = list(expc)
        ram_exp = expc + exps
        exp = rom_exp + ram_exp + ram_exp + exp
    return a, exp


def assemble(only=None):
    labels = {}
    for i in range(3):
        a, exp = program(labels, only)
        labels = a.new
    return a, exp


def gen(out, only):
    a, exp = assemble(only)
    img = struct.pack("<%dI" % len(a.o), *a.o)
    img += b"\xff" * (0x10000 - len(img))
    open(out, "wb").write(img)
    print("%s: %d words, %d records" % (out, len(a.o), len(exp)))


def check_ewram(ew, only):
    a, exp = assemble(only)
    magic, end = struct.unpack_from("<II", ew, 0)
    bad = 0
    if magic != MAGIC:
        print("FAIL: never finished (0x02000000 = %08x, results to %08x)" % (magic, end))
        bad += 1
    names = [c.__name__ for c in CASES]
    for k, e in enumerate(exp):
        got = struct.unpack_from("<5I", ew, (RES & 0xFFFFF) + k * 20)
        if got != e:
            bad += 1
            t = e[0] & 0xFFFF
            nm = names[t] if (e[0] >> 16) == 0x4E56 and t < len(names) else "%04x" % t
            print("FAIL rec %d (%s %08x): got %s want %s" % (
                k, nm, e[0], " ".join("%x" % x for x in got[1:]),
                " ".join("%x" % x for x in e[1:])))
    if not bad and end != RES + 20 * len(exp):
        print("FAIL: end %08x, want %08x" % (end, RES + 20 * len(exp)))
        bad += 1
    print("%s: %d records, %d bad" % ("PASS" if not bad else "FAIL", len(exp), bad))
    return bad


def main():
    v = sys.argv[1] if len(sys.argv) > 1 else ""
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
    if v == "gen":
        gen(sys.argv[2], only)
    elif v == "check":
        d = open(sys.argv[2], "rb").read()
        sys.exit(1 if check_ewram(d[17 * 4 + 0x8000:], only) else 0)
    elif v == "check-ram":
        sys.exit(1 if check_ewram(open(sys.argv[2], "rb").read(), only) else 0)
    elif v == "cases":
        print("\n".join(c.__name__ for c in CASES))
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
