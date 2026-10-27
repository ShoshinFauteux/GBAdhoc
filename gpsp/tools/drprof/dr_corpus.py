#!/usr/bin/env python3
"""dr_corpus.py -- the CORPUS ORACLE: every switch against the whole ROM library.

    dr_corpus.py stage  [--lib DIR ...] [--bios FILE]   # copy + dedupe the library
    dr_corpus.py build  [VARIANT ...]                   # twin binaries (parallel)
    dr_corpus.py run    [--variants a,b] [--frames N] [--jobs J] [--only s1,s2]
    dr_corpus.py report                                 # re-print the last table
    dr_corpus.py diff   ROM A B [--context N]           # first divergence, field by field
    dr_corpus.py full   [...]                           # build + run (the one command)

Run in WSL/Linux (same as dr_oracle.py; docker image drprof-qemu).  State
lives in $DRCORPUS_HOME (default ~/drcorpus), NOT ~/drprof, so it never
collides with another agent's twin builds:

    corpus/roms/<slug>.gba  corpus/roms.tsv  corpus/gba_bios.bin
    bin/dr_host_<variant>          (built by dr_build.sh into src-<variant>/)
    runs/<variant>/<slug>/{hash.txt,host.log,stderr.txt,meta.json}
    runs/REPORT.txt

WHAT IT CHECKS.  dr_oracle.py gates a dynarec change on 7 fixtures (the
owner's save states).  The owner's guardrail (dynarec-campaign-guardrails) is
stricter: the WHOLE library.  Here every ROM boots from POWER-ON (no state, a
blank save, the official BIOS, RTC pinned by dr_host) and runs the same
generic input script (scripts/corpus_mash.txt) for --frames frames on every
variant.  Each frame's line carries the dr_oracle fields (audio hash + sample
count, r0-r15/CPSR, pc, IWRAM, EWRAM, I/O, palette, OAM, VRAM) plus `fb`, a
hash of the rendered picture (dr_host --fbhash).  A speed-only switch must
match `base` on every frame of every ROM.

CONTROL FIRST.  `base` runs twice (`base` and `base@2`, separate processes);
the ctrl column must PASS on every ROM or no other column means anything.

REPEATABLE.  A run is reused when its binary md5, script md5, ROM md5 and
frame count are unchanged (meta.json), so after adding a switch to VARIANTS
only its own column runs.  --force reruns everything.

Exit 0 = every cell PASS; 1 = a divergence; 2 = a run failed / control broke.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, "..", ".."))
HOME = os.environ.get("DRCORPUS_HOME", os.path.expanduser("~/drcorpus"))
IMAGE = os.environ.get("DRPROF_IMAGE", "drprof-qemu")
SCRIPT = os.path.join(HERE, "scripts", "corpus_mash.txt")
FIELDS = ["a", "n", "r", "pc", "i", "e", "io", "p", "o", "v", "fb"]

# ---- the switches ---------------------------------------------------------
# name -> (VDEFS on top of the shipped dynarec configuration, kind, note).
# kind: ref = the reference; switch = must be IDENTICAL to base;
#       assert = a checking build: must be IDENTICAL *and* must not abort.
# Add a future switch here (one line) and run `dr_corpus.py full NAME`.
DC = "-DDISPATCH_CACHE=1"
RW = "-DSMC_RETIRE_WINDOW=1"
SI = "-DSERIAL_IDLE_FAST=1"
JCD = "-DJIT_CODE_DISCIPLINE=1"
ALL3 = " ".join([DC, RW, SI])
VARIANTS = {
    "base":   ("", "ref", "shipped dynarec (tools/build.sh CORE_COMMON before candidate-dr)"),
    "dc":     (DC, "switch", "DISPATCH_CACHE alone"),
    "rw":     (RW, "switch", "SMC_RETIRE_WINDOW alone"),
    "si":     (SI, "switch", "SERIAL_IDLE_FAST alone"),
    "all":    (ALL3, "switch", "the candidate: all three"),
    "alljcd": (ALL3 + " " + JCD, "switch", "the candidate + JIT_CODE_DISCIPLINE"),
    "allpin": (ALL3 + " -DLAYOUT_PIN=1", "switch",
               "the candidate + LAYOUT_PIN (pinned translation-cache base)"),
    "beta":   (ALL3 + " " + JCD + " -DLAYOUT_PIN=1", "switch",
               "3.1.0 beta: the candidate + JIT_CODE_DISCIPLINE + LAYOUT_PIN"),
    "rwchk":  (RW + " -DSMC_RETIRE_WINDOW_CHECK=1", "assert",
               "SMC_RETIRE_WINDOW with its invariant asserted (aborts on violation)"),
}
# ---- DRCORPUS_SHIP=1: the campaign set (claude/link-speed) -----------------
# The reference is the configuration tools/build.sh ships today (CORE_COMMON:
# the three switches above + JIT_CODE_DISCIPLINE + ENTRY_GATES), and every
# new lever is a switch on top of it that must match it frame for frame.
# Use a DRCORPUS_HOME of its own: "base" there means this reference.
SHIP = ALL3 + " " + JCD + " -DENTRY_GATES=1"
if os.environ.get("DRCORPUS_SHIP") == "1":
    VARIANTS = {
        "base":  (SHIP, "ref", "shipped dynarec (tools/build.sh CORE_COMMON, rc-4.0.0)"),
        "sched": (SHIP + " -DSCHED_FAST=1 -DSCHED_FAST_DEFAULT=1", "switch",
                  "SCHED_FAST: update_gba skips the interrupt check when IE & IF == 0"),
        "irqio": (SHIP + " -DIRQ_IO_FAST=1 -DIRQ_IO_FAST_DEFAULT=1", "switch",
                  "IRQ_IO_FAST: 16-bit IE/IF/IME stores inline in the I/O store stub"),
        "fast2": (SHIP + " -DSCHED_FAST=1 -DSCHED_FAST_DEFAULT=1"
                  " -DIRQ_IO_FAST=1 -DIRQ_IO_FAST_DEFAULT=1", "switch",
                  "the 4.0.0 beta: SCHED_FAST + IRQ_IO_FAST both on"),
        "xlat":  (SHIP + " -DSCHED_FAST=1 -DSCHED_FAST_DEFAULT=1"
                  " -DIRQ_IO_FAST=1 -DIRQ_IO_FAST_DEFAULT=1 -DXLAT_FAST=1", "switch",
                  "fast2 + XLAT_FAST (claude/bench-xlat: cheaper translation, same output)"),
        "jcost": (SHIP + " -DSCHED_FAST=1 -DSCHED_FAST_DEFAULT=1"
                  " -DIRQ_IO_FAST=1 -DIRQ_IO_FAST_DEFAULT=1 -DXLAT_FAST=1 -DJIT_COST=1", "switch",
                  "xlat + JIT_COST (claude/jit-cost: user-mode publish, cheaper RAM ARM scan)"),
        "fast2off": (SHIP + " -DSCHED_FAST=1 -DIRQ_IO_FAST=1", "switch",
                  "both compiled in, both off (the config.ini kill switches)"),
        "rwoff": (SHIP + " -DSCHED_FAST=1 -DSCHED_FAST_DEFAULT=1"
                  " -DIRQ_IO_FAST=1 -DIRQ_IO_FAST_DEFAULT=1 -DREWIND", "switch",
                  "fast2 + REWIND compiled in, the toggle off (no page marks emitted)"),
        # NOT a speed-only switch: expected to DIFFER; the table says where
        "tlazy": (SHIP + " -DTIMER_LAZY=1 -DTIMER_LAZY_DEFAULT=1", "switch",
                  "TIMER_LAZY prototype: lazy overflows of timers with no IRQ/cascade"),
        # NOT a switch: the tree as it was BEFORE the direct-sound phase fix
        # (sound_timer kept only the fraction of the resampling phase), built
        # from this tree with refs/snd_phase_fraction_only.patch.  Expected to
        # DIFFER from base in the audio hash only, and only in games whose
        # FIFO runs above the output rate; dr_corpus_fields.py says where.
        "sndpre": (SHIP, "switch",
                   "reference BEFORE the direct-sound phase fix (audio-only difference expected)"),
    }
ORDER = list(VARIANTS)

# A variant built from a patched private copy of the tree (as the mutants
# are), for "this fix against the tree without it": name -> refs/<patch>.
REF_PATCHES = {"sndpre": "snd_phase_fraction_only.patch"}

# ---- mutants: is the corpus able to see a broken switch at all? ------------
# Each is a switch with a deliberate bug (tools/drprof/mutants/*.patch, applied
# to the twin build's private copy only).  A switch whose every mutant passes
# the corpus has NOT been tested by it -- the corpus never reaches its code
# path.  `dr_corpus.py mutants` must report every mutant CAUGHT.
# name -> (VDEFS, patch file or "", the switch it attacks, what the bug is)
XLAT_MUT = SHIP + " -DXLAT_FAST=1"
MUTANTS = {
    "m_negctl":    ("-DDRPROF_NEGCTL", "", "timing",
                    "+1 cycle per Thumb instruction (dr_oracle's own control)"),
    "m_dc_ram":    (DC, "dc_keep_on_ram_flush.patch", "dc",
                    "dispatch table survives a full RAM flush"),
    "m_dc_retire": (DC, "dc_keep_on_retire.patch", "dc",
                    "dispatch table survives a partial SMC retire"),
    "m_rw4":       (RW, "rw_window_4.patch", "rw",
                    "retire window 4 bytes instead of the longest live extent"),
    # A probe, not a bug: the 4-byte window WITH the invariant check aborts on
    # the first SMC event whose correctness needs more than 4 bytes of window.
    # Surviving it means no corpus event exercises the window's length.
    "m_rw4chk":    (RW + " -DSMC_RETIRE_WINDOW_CHECK=1", "rw_window_4.patch", "rw",
                    "probe: does any SMC event need a window > 4 bytes"),
    # EQUIVALENT in solo play (frcnt is read only when netplay_client_id != 0):
    # the corpus cannot catch these two by construction; kept as documentation.
    "m_si_poke":   (SI, "si_no_frcnt.patch", "si",
                    "fast path drops the POKE fake's cycle counter (solo-equivalent)"),
    "m_si_aw":     (SI, "si_no_aw_frcnt.patch", "si",
                    "fast path drops the AW1/AW2 fake's cycle counter (solo-equivalent)"),
    "m_si_irq":    (SI, "si_ignore_pending.patch", "si",
                    "fast path taken with a serial transfer pending"),
    "m_si_rfu":    (SI, "si_take_rfu.patch", "si",
                    "fast path also skips the RFU adapter"),
    # XLAT_FAST (claude/bench-xlat): each piece broken on purpose.
    "m_xf_dclear":  (XLAT_MUT, "xf_dclear_one.patch", "xlat",
                     "DCLEAR resets only the first filled dispatch slot"),
    "m_xf_noent":   (XLAT_MUT, "xf_gatemap_noent.patch", "xlat",
                     "GATEMAP ignores IWRAM entry gates"),
    "m_xf_limits":  (XLAT_MUT, "xf_limits_first.patch", "xlat",
                     "LIMITS records only each block's first pc"),
    "m_xf_exitpf":  (XLAT_MUT, "xf_exitpf_nobranch.patch", "xlat",
                     "EXITPF prefilter misses B (0xA) opcodes"),
    # JIT_COST (claude/jit-cost): the scan pieces broken on purpose.  PUBINL
    # cannot be seen here (qemu is coherent): tools/jitcoh's brk-pubinl-*
    # mutants are its check.
    "m_jc_win":     (XLAT_MUT + " -DJIT_COST=1", "jc_scanwin_3bytes.patch", "jcost",
                     "SCANWIN ignores the last bitmap byte of each 64-byte window"),
    "m_jc_cond":    (XLAT_MUT + " -DJIT_COST=1", "jc_scancond_bit7.patch", "jcost",
                     "SCANCOND table indexed without opcode bit 7"),
}


# ---- DRCORPUS_SMC=1: the smc-coverage set (claude/smc-coverage) ------------
# SMC policy switches are NOT timing-neutral (a selective retire keeps tags
# that a full flush clears, so later stores trap differently; the single path
# installs no gates), so against the shipped reference they are expected to
# differ in SMC-heavy titles: each such cell is a divergence to explain.  The
# correctness checks are therefore (1) covbs: the stale-entry checker on every
# block entry ABORTS on stale code (kind assert: must not die), and (2) the
# mutants, which run against DRCORPUS_SMCREF (default the shipped config; set
# DRCORPUS_SMCREF=covb in a home of its own so a mutant is judged against the
# switch it breaks, not against timing noise).
if os.environ.get("DRCORPUS_SMC") == "1":
    F2 = SHIP + " -DSCHED_FAST=1 -DSCHED_FAST_DEFAULT=1 -DIRQ_IO_FAST=1 -DIRQ_IO_FAST_DEFAULT=1"
    CLS_A = (" -DSMC_PARTIAL_SINGLE -DSMC_SINGLE_THUMB -DSMC_SINGLE_HALF"
             " -DSMC_SINGLE_ANYPC -DSMC_SINGLE_EWRAM")
    CLS_B = CLS_A + " -DSMC_BLOCK_EXACT"
    CLS_H = CLS_B + " -DSMC_SINGLE_UNTAG -DSMC_BLOCK_SAME -DSMC_SINGLE_HOT=64"
    CLS_C = CLS_A + " -DSMC_SINGLE_UNTAG -DSMC_SINGLE_HOT=64"   # candidate A
    STALE = " -DSTALE_CHECK -DSMC_FALLBACK_STATS -UDISPATCH_CACHE -DSTALE_ABORT=1"
    _ref = os.environ.get("DRCORPUS_SMCREF", "ship")
    _refdefs = F2 + {"ship": "", "cova": CLS_A, "covb": CLS_B, "covh": CLS_H, "cand": CLS_C}[_ref]
    VARIANTS = {
        "base": (_refdefs, "ref", "reference: %s (4.1.1 CORE_COMMON + fast2%s)" % (
            _ref, "" if _ref == "ship" else " + smc-coverage " + _ref)),
        "dr2":  (F2 + " -DSMC_PARTIAL_SINGLE", "switch", "SMC_PARTIAL_SINGLE (claude/bench-dr 82f099a)"),
        "cova": (F2 + CLS_A, "switch", "class A: + Thumb/STRH/any-writer/EWRAM single stores"),
        "covb": (F2 + CLS_B, "switch", "class A + SMC_BLOCK_EXACT (block stores)"),
        "covas": (F2 + CLS_A + STALE, "assert", "cova + stale-entry checker that aborts"),
        "covbs": (F2 + CLS_B + STALE, "assert", "covb + stale-entry checker that aborts"),
        "covu": (F2 + CLS_B + " -DSMC_SINGLE_UNTAG", "switch", "covb + SMC_SINGLE_UNTAG"),
        "covh": (F2 + CLS_H, "switch", "THE CANDIDATE: covb + UNTAG + BLOCK_SAME + SMC_SINGLE_HOT=64"),
        "covhs": (F2 + CLS_H + STALE, "assert", "covh + stale-entry checker that aborts"),
        "cand": (F2 + CLS_C, "switch", "CANDIDATE A: single-store classes + UNTAG + HOT=64 (no BLOCK_EXACT)"),
        "cands": (F2 + CLS_C + STALE, "assert", "cand + stale-entry checker that aborts"),
    }
    ORDER = list(VARIANTS)
    MUTANTS = {
        "m_cov_skip": (F2 + CLS_H + " -DSMC_COVERAGE_MUTANT_SKIP_LAST", "", "covh",
                       "smc_retire_span skips ARM blocks starting inside the written range"),
        "m_cand_skip": (F2 + CLS_C + " -DSMC_COVERAGE_MUTANT_SKIP_LAST", "", "cand",
                        "candidate A with smc_retire_span skipping ARM blocks starting in the range"),
        "m_cov_blk":  (F2 + CLS_H + " -DSMC_COVERAGE_MUTANT_BLOCK_IGNORE", "", "covh",
                       "SMC_BLOCK_EXACT word hits retire nothing (the 2.0.2 hole)"),
    }


# ---- DRCORPUS_SLOW=1: the slow-titles set (claude/slow-titles) ------------
# Reference = the cpu-levers candidate's twin flags (lab-combo3 + c3lat4 +
# cpu-levers DISPATCH_PRECISE/SMC_SAME_SKIP/SMC_SAME_ADAPT).  VRAM_EXEC is
# dormant until a game jumps into VRAM, SMC_RETIRE_LOCAL only shortens the
# retire walks: both must match base on every frame of every ROM.
if os.environ.get("DRCORPUS_SLOW") == "1":
    C3 = ("-DSMC_GATES -DSMC_GATES_SIMPLE -DSMC_GATES_RANKED=1 -DSMC_GATE_BITMAP=1"
          " -DSMC_PARTIAL_SAFE -DSMC_PARTIAL_STABLE_THUNK -DSMC_PARTIAL_DIRECT_LINKS"
          " -DGBA_PC_MASK=1 -DBADJUMP_SAFE=1 -DSMC_GATE_CHARGE=1 -DDISPATCH_CYCLE_CHECK=1"
          " -DDMA_SMC_FLUSH=1 -DDISPATCH_CACHE=1 -DSMC_RETIRE_WINDOW=1 -DSERIAL_IDLE_FAST=1"
          " -DJIT_CODE_DISCIPLINE=1 -DENTRY_GATES=1 -DSCHED_FAST=1 -DIRQ_IO_FAST=1 -DREWIND"
          " -DSMC_PARTIAL_SINGLE -DSMC_SINGLE_THUMB -DSMC_SINGLE_HALF -DSMC_SINGLE_ANYPC"
          " -DSMC_SINGLE_EWRAM -DSMC_SINGLE_UNTAG -DSMC_SINGLE_HOT=64 -DXLAT_FAST=1"
          " -DJIT_COST=1 -DTIMER_RELOAD_FIX -DDISPATCH_PRECISE=1 -DSMC_SAME_SKIP=1"
          " -DSMC_SAME_ADAPT=1")
    VX = " -DVRAM_EXEC=1"
    RL = " -DSMC_RETIRE_LOCAL=1"
    EG = " -DENTRY_GATES_RANGE=1"
    LS = " -DSMC_RETIRE_LIST=1"
    VARIANTS = {
        "base":  (C3, "ref", "cpu-levers candidate flags"),
        "vx":    (C3 + VX, "switch", "VRAM_EXEC (ported from claude/self-activate)"),
        "rl":    (C3 + RL, "switch", "SMC_RETIRE_LOCAL alone"),
        "rlchk": (C3 + RL + " -DSMC_RETIRE_LOCAL_CHECK=1", "assert",
                  "SMC_RETIRE_LOCAL with its invariant asserted (aborts on violation)"),
        "egchk": (C3 + EG + " -DENTRY_GATES_RANGE_CHECK=1", "assert",
                  "ENTRY_GATES_RANGE with its invariant asserted (aborts on violation)"),
        "ls":    (C3 + LS, "switch", "SMC_RETIRE_LIST alone"),
        "slow":  (C3 + VX + RL + EG + LS, "switch",
                  "the slow-titles candidate: VRAM_EXEC + SMC_RETIRE_LOCAL + ENTRY_GATES_RANGE + SMC_RETIRE_LIST"),
    }
    ORDER = list(VARIANTS)
    MUTANTS = {
        "m_rl_last": (C3 + RL + " -DSMC_RETIRE_LOCAL_CHECK=1 -DSMC_RETIRE_LOCAL_MUTANT=1", "", "rlchk",
                      "smc_back_note forgets the last chunk of every block"),
        "m_eg_last": (C3 + EG + " -DENTRY_GATES_RANGE_CHECK=1 -DENTRY_GATES_RANGE_MUTANT=1", "", "egchk",
                      "harvest range drops the upper half of the code range"),
    }


def vdefs(v):
    return VARIANTS[v][0] if v in VARIANTS else MUTANTS[v][0]

DEFAULT_LIBS = [
    "/mnt/c/Users/DrSto/OneDrive/Desktop/Roms",
    "/mnt/c/Users/DrSto/OneDrive/Desktop/Backup/GBAdhoc Backup/GBADHOC/roms",
    "/mnt/c/Users/DrSto/OneDrive/Desktop/GBA PSP Wirless Trading (Claude)/gpsp-adhoc/testdata",
]
DEFAULT_BIOS = [os.path.expanduser("~/drprof/fx/gba_bios.bin"),
                "/mnt/c/Users/DrSto/OneDrive/Desktop/GBA PSP Wirless Trading (Claude)/gpsp-adhoc/testdata/gba_bios.bin"]
MIN_ROM = 64 * 1024     # smaller .gba files are multiboot images / stubs, not cartridges


def md5file(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def slugify(name, md5):
    base = os.path.splitext(os.path.basename(name))[0]
    base = re.sub(r"\(.*?\)|\[.*?\]", "", base)
    base = re.sub(r"[^A-Za-z0-9]+", "_", base).strip("_").lower()[:28]
    return "%s-%s" % (base or "rom", md5[:6])


# ---- stage ----------------------------------------------------------------
def cmd_stage(o):
    rdir = os.path.join(HOME, "corpus", "roms")
    os.makedirs(rdir, exist_ok=True)
    seen = {}
    for lib in o.lib or DEFAULT_LIBS:
        if not os.path.isdir(lib):
            print("  (skip missing lib %s)" % lib)
            continue
        for root, _, files in os.walk(lib):
            for fn in sorted(files):
                if not fn.lower().endswith(".gba"):
                    continue
                p = os.path.join(root, fn)
                if os.path.getsize(p) < MIN_ROM:
                    continue
                m = md5file(p)
                if m not in seen:
                    seen[m] = p
    rows = []
    for m, p in sorted(seen.items(), key=lambda kv: os.path.basename(kv[1]).lower()):
        slug = slugify(p, m)
        dst = os.path.join(rdir, slug + ".gba")
        if not os.path.exists(dst) or md5file(dst) != m:
            shutil.copyfile(p, dst)      # read-only use of the source
        rows.append((slug, m, os.path.getsize(p), p))
    with open(os.path.join(HOME, "corpus", "roms.tsv"), "w") as f:
        for r in rows:
            f.write("%s\t%s\t%d\t%s\n" % r)
    bios = o.bios or next((b for b in DEFAULT_BIOS if os.path.exists(b)), None)
    if not bios:
        sys.exit("no gba_bios.bin found; pass --bios")
    shutil.copyfile(bios, os.path.join(HOME, "corpus", "gba_bios.bin"))
    print("staged %d unique ROMs (by md5) into %s; bios %s" % (len(rows), rdir, md5file(bios)[:8]))
    for r in rows:
        print("  %-36s %s %9d  %s" % (r[0], r[1][:8], r[2], os.path.basename(r[3])))


# Left out of every pass unless named with --only.  Payback is pure
# self-modifying code: ~1 fps under the twin, 93 of a pass's 107 minutes
# (owner, 2026-10-01).  Name it with --only payback for a deliberate SMC run.
# OWNER DECISION 2026-10-11: Payback is NOT part of the corpus gate ("that
# game is unplayable"): no release decision waits for it or is blocked by
# it.  What was seen that day before the runs were stopped: the direct-sound
# fix against its absence, identical for 5,188 frames, audio included (the
# tool's 2 h limit ended it); SCHED_FAST + IRQ_IO_FAST never completed.
EXCLUDE = ("payback",)


def roms(only="", skip=""):
    path = os.path.join(HOME, "corpus", "roms.tsv")
    if not os.path.exists(path):
        sys.exit("no corpus staged: run `dr_corpus.py stage` first")
    out = []
    for ln in open(path):
        slug, m, size, src = ln.rstrip("\n").split("\t")
        if only and not any(k in slug for k in only.split(",")):
            continue
        if skip and any(k in slug for k in skip.split(",")):
            continue
        if any(k in slug for k in EXCLUDE) and not (
                only and any(k in slug for k in only.split(","))):
            continue
        out.append({"slug": slug, "md5": m, "size": int(size), "src": src})
    return out


# ---- build ----------------------------------------------------------------
def build_one(v):
    defs = vdefs(v)
    env = dict(os.environ, DRPROF_HOME=HOME, DRPROF_SRC="src-" + v)
    if v in MUTANTS and MUTANTS[v][1]:
        env["DRPROF_PATCH"] = os.path.join(HERE, "mutants", MUTANTS[v][1])
    elif v in REF_PATCHES:
        env["DRPROF_PATCH"] = os.path.join(HERE, "refs", REF_PATCHES[v])
    t = time.time()
    p = subprocess.run(["bash", os.path.join(HERE, "dr_build.sh"), v, defs],
                       env=env, capture_output=True, text=True)
    return v, p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:], time.time() - t


def cmd_build(o):
    vs = o.variants or ORDER
    bad = [v for v in vs if v not in VARIANTS and v not in MUTANTS]
    if bad:
        sys.exit("unknown variant(s) %s; known: %s" % (bad, ", ".join(ORDER + list(MUTANTS))))
    os.makedirs(os.path.join(HOME, "bin"), exist_ok=True)
    rc = 0
    with cf.ThreadPoolExecutor(o.build_jobs) as ex:
        for v, r, tail, dt in ex.map(build_one, vs):
            print("  build %-8s rc=%d %4.0fs %s" % (v, r, dt, tail[0][:110] if tail else ""))
            rc |= r
    if rc:
        sys.exit("build failed")


# ---- run ------------------------------------------------------------------
def bin_path(v):
    return os.path.join(HOME, "bin", "dr_host_" + v.split("@")[0])


def run_dir(v, slug):
    return os.path.join(HOME, "runs", v, slug)


def run_one(v, rom, frames, smd5, force, dumps):
    """v may be 'base@2' (the determinism replica): same binary, own dir."""
    b = bin_path(v)
    if not os.path.exists(b):
        return v, rom["slug"], "nobin", 0.0
    meta = {"bin": md5file(b), "script": smd5, "rom": rom["md5"], "frames": frames,
            "defs": open(b + ".defs").read().strip() if os.path.exists(b + ".defs") else ""}
    d = run_dir(v, rom["slug"])
    mp = os.path.join(d, "meta.json")
    if not force and os.path.exists(mp) and os.path.exists(os.path.join(d, "hash.txt")):
        try:
            old = json.load(open(mp))
            if all(old.get(k) == meta[k] for k in ("bin", "script", "rom", "frames")):
                return v, rom["slug"], "cached", old.get("secs", 0.0)
        except ValueError:
            pass
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copyfile(SCRIPT, os.path.join(d, "script.txt"))
    rel = os.path.relpath(d, HOME)
    dump = ""
    if dumps:
        os.makedirs(os.path.join(d, "shots"))
        dump = "--dump-dir /w/%s/shots --dump-at %s" % (rel, dumps)
    # a hang (a mutant that loops in the dispatcher) must end as a failed run:
    # 3x the base run's own time, else 2 h
    tmo = 7200
    try:
        bm = json.load(open(os.path.join(run_dir("base", rom["slug"]), "meta.json")))
        if bm.get("rc") == 0 and bm.get("frames") == frames and not v.startswith("base"):
            tmo = int(bm["secs"] * 3) + 300
    except (OSError, ValueError, KeyError):
        pass
    cmd = ("cd /w/corpus && timeout %d /opt/qemu/bin/qemu-mipsel -cpu 74Kf /w/bin/%s "
           "--rom roms/%s.gba --bios-dir . --save /tmp/run.sav "
           "--script /w/%s/script.txt --frames %d --fbhash "
           "--hash /w/%s/hash.txt --log /w/%s/host.log %s"
           % (tmo, os.path.basename(b), rom["slug"], rel, frames, rel, rel, dump))
    t = time.time()
    p = subprocess.run(["docker", "run", "--rm", "-v", "%s:/w" % HOME, IMAGE, "bash", "-c", cmd],
                       capture_output=True, text=True)
    meta["secs"] = round(time.time() - t, 1)
    meta["rc"] = p.returncode
    with open(os.path.join(d, "stderr.txt"), "w") as f:
        f.write(p.stdout + p.stderr)
    json.dump(meta, open(mp, "w"), indent=1)
    return v, rom["slug"], "rc=%d" % p.returncode, meta["secs"]


def parse(path):
    rows = []
    if os.path.exists(path):
        for ln in open(path):
            kv = ln.split()
            # a run killed mid-write (crash/timeout) leaves a torn last line
            if ln.startswith("f=") and ln.endswith("\n") and all("=" in x for x in kv):
                rows.append(dict(x.split("=", 1) for x in kv))
    return rows


def compare(A, B):
    """-> (ok, short, detail).  A is the reference."""
    n = min(len(A), len(B))
    for i in range(n):
        if A[i] != B[i]:
            diff = [f for f in FIELDS if A[i].get(f) != B[i].get(f)]
            return False, "f%s:%s" % (A[i]["f"], ",".join(diff)), i
    if len(A) != len(B):
        return False, "len %d/%d" % (len(A), len(B)), n
    return True, "PASS", n


def run_status(v, slug):
    d = run_dir(v, slug)
    try:
        meta = json.load(open(os.path.join(d, "meta.json")))
    except (OSError, ValueError):
        return None, None, ""
    err = open(os.path.join(d, "stderr.txt")).read() if os.path.exists(os.path.join(d, "stderr.txt")) else ""
    return meta, parse(os.path.join(d, "hash.txt")), err


def cell(ref, v, slug, frames):
    meta, rows, err = run_status(v, slug)
    if meta is None:
        return "-", None
    viol = "VIOLATION" in err
    if meta.get("rc") != 0 or len(rows) < frames:
        rc = meta.get("rc")
        tag = ("HANG" if rc == 124 else
               "ABORT" if viol or "Abort" in err or rc in (134, 250) else "CRASH")
        why = "%s rc=%s @f%d" % (tag, meta.get("rc"), len(rows))
        if ref is not None:
            ok, short, _ = compare(ref, rows)
            if not ok and not short.startswith("len"):
                why += " (div %s)" % short
        return why, False
    if ref is None:
        return "?", None
    ok, short, _ = compare(ref, rows)
    return ("PASS" if ok else "FAIL " + short), ok


def activity(rows):
    """sanity that the script actually drives the game: distinct pictures
    and how many frames the picture changed on."""
    fbs = [r.get("fb") for r in rows]
    chg = sum(1 for a, b in zip(fbs, fbs[1:]) if a != b)
    return len(set(fbs)), chg


def report(vs, frames, out=sys.stdout, rlist=None):
    rlist = rlist or roms()
    cols = ["ctrl"] + [v for v in vs if v != "base"]
    lines = []
    w = max(24, max(len(r["slug"]) for r in rlist))
    hdr = "%-*s %6s %6s  " % (w, "rom", "pics", "chg") + "  ".join("%-14s" % c for c in cols)
    lines.append(hdr)
    tally = {c: [0, 0] for c in cols}
    worst = 0
    divs = []
    for r in rlist:
        bmeta, brows, berr = run_status("base", r["slug"])
        if bmeta is None or bmeta.get("rc") != 0 or len(brows) < frames:
            ref = None
            pics = chg = 0
        else:
            ref = brows
            pics, chg = activity(brows)
        cells = []
        for c in cols:
            v = "base@2" if c == "ctrl" else c
            if ref is None:
                txt, ok = ("BASE " + (cell(None, "base", r["slug"], frames)[0])), False
            else:
                txt, ok = cell(ref, v, r["slug"], frames)
            if ok is True:
                tally[c][0] += 1
            elif ok is False:
                tally[c][1] += 1
                worst = max(worst, 2 if c == "ctrl" or ref is None else 1)
                divs.append((r["slug"], c, txt))
            cells.append(txt)
        lines.append("%-*s %6d %6d  " % (w, r["slug"], pics, chg)
                     + "  ".join("%-14s" % t for t in cells))
    lines.append("")
    lines.append("%-*s %13s  " % (w, "PASS / FAIL", "")
                 + "  ".join("%-14s" % ("%d/%d" % tuple(tally[c])) for c in cols))
    verdict = {c: ("PASS" if tally[c][1] == 0 and tally[c][0] == len(rlist) else "FAIL")
               for c in cols}
    lines.append("%-*s %13s  " % (w, "VERDICT", "") + "  ".join("%-14s" % verdict[c] for c in cols))
    # bisect: a combined variant's failure is owned by the singles that fail on the same ROM
    singles = [c for c in cols if c in VARIANTS and VARIANTS[c][0] and " " not in VARIANTS[c][0]]
    if divs:
        lines.append("")
        lines.append("divergences (first frame:fields; bisected to single switches):")
        bad = {(s, c) for s, c, _ in divs}
        for s, c, t in divs:
            own = ""
            if c not in singles and c != "ctrl":
                fs = [x for x in singles if (s, x) in bad]
                own = "  <- " + (",".join(fs) if fs else "no single switch fails: interaction")
            lines.append("  %-*s %-8s %s%s" % (w, s, c, t, own))
    lines.append("")
    lines.append("RESULT: %s" % ("PASS" if worst == 0 else "FAIL" if worst == 1 else "ERROR"))
    txt = "\n".join(lines)
    print(txt, file=out)
    return worst, txt


def cmd_run(o):
    vs = o.variants or ORDER
    if "base" not in vs:
        vs = ["base"] + vs
    rlist = roms(o.only, o.skip)
    smd5 = md5file(SCRIPT)
    jobs = []
    for r in rlist:
        jobs.append(("base", r, o.dumps))
        jobs.append(("base@2", r, ""))
        for v in vs:
            if v != "base":
                jobs.append((v, r, ""))
    t0 = time.time()
    ran = 0
    with cf.ThreadPoolExecutor(o.jobs) as ex:
        futs = [ex.submit(run_one, v, r, o.frames, smd5, o.force, d) for v, r, d in jobs]
        for i, f in enumerate(cf.as_completed(futs), 1):
            v, slug, st, secs = f.result()
            if st != "cached":
                ran += 1
            if o.verbose or (st not in ("cached", "rc=0")):
                print("  [%3d/%d] %-8s %-34s %s %.0fs" % (i, len(jobs), v, slug, st, secs))
    wall = time.time() - t0
    print("%d runs (%d executed, %d cached) in %.0f s wall, %d ROMs x %d frames, %d jobs\n"
          % (len(jobs), ran, len(jobs) - ran, wall, len(rlist), o.frames, o.jobs))
    hdr = ["corpus oracle  %s  frames=%d  script=%s  roms=%d" % (
        time.strftime("%Y-%m-%d %H:%M"), o.frames, smd5[:8], len(rlist))]
    for v in vs:
        b = bin_path(v)
        hdr.append("  %-7s %s  %s" % (v, md5file(b)[:8] if os.path.exists(b) else "--------",
                                       VARIANTS.get(v, ("", "", "?"))[2]))
    hdr.append("  ctrl    = base vs a second base run (determinism floor)")
    hdr.append("  pics/chg = distinct pictures / frames the picture changed (base) -- is the game alive")
    hdr.append("")
    print("\n".join(hdr))
    worst, txt = report(vs, o.frames, rlist=rlist)
    if not o.only and not o.skip:
        with open(os.path.join(HOME, "runs", "REPORT.txt"), "w") as f:
            f.write("\n".join(hdr) + "\n" + txt + "\n\n%d runs, %d executed, %.0f s wall\n"
                    % (len(jobs), ran, wall))
        json.dump({"variants": vs, "frames": o.frames},
                  open(os.path.join(HOME, "runs", "last.json"), "w"))
    return worst


def cmd_mutants(o):
    """build + run the mutants; each must be CAUGHT (diverge or die) on >= 1 ROM."""
    given = bool(o.variants)
    ms = o.variants or list(MUTANTS)
    if not o.no_build:
        o.variants = ms
        cmd_build(o)
    rlist = roms(o.only, o.skip)
    smd5 = md5file(SCRIPT)
    jobs = [("base", r) for r in rlist] + [(m, r) for m in ms for r in rlist]
    t0 = time.time()
    with cf.ThreadPoolExecutor(o.jobs) as ex:
        futs = [ex.submit(run_one, v, r, o.frames, smd5, o.force, "") for v, r in jobs]
        for f in cf.as_completed(futs):
            v, slug, st, secs = f.result()
            if o.verbose or st not in ("cached", "rc=0"):
                print("  %-12s %-34s %s %.0fs" % (v, slug, st, secs))
    lines = ["mutation test  %s  frames=%d  roms=%d%s  (%.0f s wall)" % (
        time.strftime("%Y-%m-%d %H:%M"), o.frames, len(rlist),
        "  skip=" + o.skip if o.skip else "", time.time() - t0),
        "%-12s %-6s %-7s %s" % ("mutant", "attacks", "caught", "first divergences (rom: frame/fields)")]
    worst = 0
    for m in ms:
        hits = []
        for r in rlist:
            bmeta, brows, _ = run_status("base", r["slug"])
            if bmeta is None or bmeta.get("rc") != 0:
                continue
            txt, ok = cell(brows, m, r["slug"], o.frames)
            if ok is False:
                hits.append("%s: %s" % (r["slug"].split("-")[0][:18], txt.replace("FAIL ", "")))
        caught = "%d/%d" % (len(hits), len(rlist))
        if not hits:
            worst = 1
        lines.append("%-12s %-7s %-7s %s" % (m, MUTANTS[m][2], caught, MUTANTS[m][3]))
        for h in hits:
            lines.append("%28s %s" % ("", h))
    lines.append("RESULT: %s" % ("every mutant CAUGHT" if worst == 0 else
                                 "SOME MUTANT SURVIVED -- the corpus does not reach that code"))
    txt = "\n".join(lines)
    print(txt)
    if not o.only and not given:
        open(os.path.join(HOME, "runs", "MUTANTS.txt"), "w").write(txt + "\n")
    return worst


def cmd_report(o):
    try:
        last = json.load(open(os.path.join(HOME, "runs", "last.json")))
    except OSError:
        sys.exit("no previous run")
    worst, _ = report(o.variants or last["variants"], o.frames or last["frames"])
    return worst


def cmd_diff(o):
    slug = [r["slug"] for r in roms() if o.rom in r["slug"]]
    if len(slug) != 1:
        sys.exit("ROM must match exactly one slug: %s" % slug)
    s = slug[0]
    A = parse(os.path.join(run_dir(o.a, s), "hash.txt"))
    B = parse(os.path.join(run_dir(o.b, s), "hash.txt"))
    ok, short, i = compare(A, B)
    print("%s: %s vs %s -> %s" % (s, o.a, o.b, short))
    if ok:
        return 0
    lo = max(0, i - o.context)
    for k in range(lo, min(i + o.context + 1, max(len(A), len(B)))):
        a = A[k] if k < len(A) else {}
        b = B[k] if k < len(B) else {}
        d = [f for f in FIELDS if a.get(f) != b.get(f)]
        mark = ">>" if k == i else "  "
        print("%s f=%s  %s" % (mark, a.get("f", b.get("f")), "same" if not d else
                               "  ".join("%s:%s/%s" % (f, a.get(f), b.get(f)) for f in d)))
    # how long does the divergence last: does it heal (a transient) or persist?
    n = min(len(A), len(B))
    same_after = sum(1 for k in range(i, n) if A[k] == B[k])
    print("frames from first divergence: %d, identical among them: %d" % (n - i, same_after))
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage")
    s.add_argument("--lib", action="append")
    s.add_argument("--bios")
    for name in ("build", "full"):
        b = sub.add_parser(name)
        b.add_argument("variants", nargs="*")
        b.add_argument("--build-jobs", type=int, default=4)
        if name == "full":
            b.add_argument("--frames", type=int, default=6000)
            b.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
            b.add_argument("--only", default="")
            b.add_argument("--skip", default="")
            b.add_argument("--force", action="store_true")
            b.add_argument("--dumps", default="600,2000,4000,6000")
            b.add_argument("-v", "--verbose", action="store_true")
    m = sub.add_parser("mutants")
    m.add_argument("variants", nargs="*")
    m.add_argument("--frames", type=int, default=6000)
    m.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    m.add_argument("--build-jobs", type=int, default=4)
    m.add_argument("--only", default="")
    m.add_argument("--skip", default="")
    m.add_argument("--force", action="store_true")
    m.add_argument("--no-build", action="store_true")
    m.add_argument("-v", "--verbose", action="store_true")
    r = sub.add_parser("run")
    r.add_argument("--variants", type=lambda x: x.split(","))
    r.add_argument("--skip", default="", help="ROM slugs (substrings) to leave out")
    r.add_argument("--frames", type=int, default=6000)
    r.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    r.add_argument("--only", default="")
    r.add_argument("--force", action="store_true")
    r.add_argument("--dumps", default="600,2000,4000,6000",
                   help="base-run frames to save as BMP (eyeball that the game is alive)")
    r.add_argument("-v", "--verbose", action="store_true")
    p = sub.add_parser("report")
    p.add_argument("--variants", type=lambda x: x.split(","))
    p.add_argument("--frames", type=int, default=0)
    d = sub.add_parser("diff")
    d.add_argument("rom")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--context", type=int, default=3)
    o = ap.parse_args()
    if o.cmd == "stage":
        return cmd_stage(o)
    if o.cmd == "build":
        return cmd_build(o)
    if o.cmd == "full":
        cmd_build(o)
        o.variants = o.variants or ORDER
        return cmd_run(o)
    if o.cmd == "run":
        return cmd_run(o)
    if o.cmd == "report":
        return cmd_report(o)
    if o.cmd == "mutants":
        return cmd_mutants(o)
    if o.cmd == "diff":
        return cmd_diff(o)


if __name__ == "__main__":
    sys.exit(main() or 0)
