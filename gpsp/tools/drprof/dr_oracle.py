#!/usr/bin/env python3
"""dr_oracle.py -- the DIFFERENTIAL ORACLE for dynarec changes.

    dr_oracle.py A B [--only f1,f2] [--frames N] [--keep]

Runs every fixture of the library (dr_suite.sh's SUITE, or --only) on twin
variant A and twin variant B (both built with dr_build.sh; same savestate, same
save, same input script) and requires the per-frame guest-state hash lines to
be IDENTICAL, every frame, every field:

    a/n  rolling FNV of every audio sample the core produced / sample count
    r    r0-r15 + CPSR        pc  r15
    i    IWRAM                e   EWRAM          io  I/O registers
    p    palette              o   OAM            v   VRAM

Exit status 0 = every fixture identical; 1 = at least one divergence (the
first differing frame and fields are printed); 2 = a run failed.

WHY THIS IS THE GATE.  A translator change that alters guest behaviour shows
up here as a diverged frame; one that does not, cannot.  Audio is included on
purpose: the 2026-08 SMC_PARTIAL work produced byte-identical VIDEO and
corrupted AUDIO (smc-partial-invalidation), so a video-only oracle is known to
be insufficient.  The twin runs the PSP's own MIPS emitter (mips_emit.h,
mips_stub.S) under qemu, and matched the PSP-harness golden (PPSSPP, itself
proven equal to PSP Go hardware) for 3,614 consecutive AW2 frames -- see
docs/DYNAREC-PROFILE.md "fidelity of the twin".

Timing changes are divergences.  Anything that moves cycle accounting (block
boundaries, where update_gba runs) legitimately changes the hashes; such a
change cannot be validated by this oracle and must be argued and A/B'd
separately (SMC gates were exactly such a change).  Pure-speed changes -- the
same guest instructions retired at the same cycle counts by faster host code
-- must pass it bit-exactly.

Also used for its own controls:
    dr_oracle.py base base            determinism (same binary twice)
    dr_oracle.py base base-notwin     the twin's bookkeeping perturbs nothing

PPSSPP / hardware: the same hash lines come from a PSP harness EBOOT with
`shash = 1` (tools/linkbench/ppsspp_run.sh); compare two such runs with
tools/linkbench/cmp_hash.py --mark ... (fields a,n there are the PSP audio
path's and must be compared within one platform only).
"""
import argparse
import concurrent.futures as cf
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.environ.get("DRPROF_HOME", os.path.expanduser("~/drprof"))
FIELDS = ["a", "n", "r", "pc", "i", "e", "io", "p", "o", "v"]


def suite():
    """parse dr_suite.sh's SUITE block (single source of truth)"""
    txt = open(os.path.join(HERE, "dr_suite.sh")).read()
    body = txt.split('SUITE=${SUITE:-"', 1)[1].split('"}', 1)[0]
    out = []
    for ln in body.strip().splitlines():
        f = ln.split()
        if len(f) == 6:
            out.append(f)
    return out


def run(variant, fx, frames, tag):
    name, rom, st, sav, scr, fr = fx
    fr = str(frames or fr)
    outdir = "oracle/%s/%s" % (tag, name)
    env = dict(os.environ, PROF="0", SAV=sav)
    p = subprocess.run(["bash", os.path.join(HERE, "dr_profile.sh"), outdir, rom, st,
                        scr, fr, variant], env=env, capture_output=True, text=True)
    h = os.path.join(HOME, "runs", outdir, "hash.txt")
    if p.returncode != 0 or not os.path.exists(h):
        return None, p.stdout + p.stderr
    return h, p.stdout


def parse(path):
    rows = []
    for ln in open(path):
        if ln.startswith("f="):
            rows.append(dict(kv.split("=", 1) for kv in ln.split()))
    return rows


def compare(ha, hb):
    A, B = parse(ha), parse(hb)
    n = min(len(A), len(B))
    for i in range(n):
        if A[i] != B[i]:
            diff = [f for f in FIELDS if A[i].get(f) != B[i].get(f)]
            return False, "DIVERGED at frame %s (seg %s): %s" % (
                A[i]["f"], A[i].get("s"), ",".join(diff)), n
    if len(A) != len(B):
        return False, "LENGTH differs: %d vs %d frames" % (len(A), len(B)), n
    return True, "IDENTICAL %d frames" % n, n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--only", default="")
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=7)
    o = ap.parse_args()
    fxs = [f for f in suite() if not o.only or f[0] in o.only.split(",")]
    if not fxs:
        print("no fixtures")
        return 2
    tag_a, tag_b = "A-" + o.a, "B-" + o.b
    res = {}
    with cf.ThreadPoolExecutor(o.jobs) as ex:
        futs = {}
        for fx in fxs:
            futs[ex.submit(run, o.a, fx, o.frames, tag_a)] = (fx[0], "a")
            futs[ex.submit(run, o.b, fx, o.frames, tag_b)] = (fx[0], "b")
        for f in cf.as_completed(futs):
            name, side = futs[f]
            res[(name, side)] = f.result()
    worst = 0
    print("oracle: %s vs %s" % (o.a, o.b))
    for fx in fxs:
        ha, la = res[(fx[0], "a")]
        hb, lb = res[(fx[0], "b")]
        if not ha or not hb:
            print("  %-10s RUN FAILED\n%s" % (fx[0], (la if not ha else lb)[-800:]))
            worst = max(worst, 2)
            continue
        ok, msg, n = compare(ha, hb)
        print("  %-10s %s" % (fx[0], msg))
        if not ok:
            worst = max(worst, 1)
    print("RESULT: %s" % ("PASS" if worst == 0 else "FAIL" if worst == 1 else "ERROR"))
    return worst


if __name__ == "__main__":
    sys.exit(main())
