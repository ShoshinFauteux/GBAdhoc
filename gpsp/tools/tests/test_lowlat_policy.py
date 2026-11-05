#!/usr/bin/env python3
"""The low-latency tier-1 policy of psp/main_psp.c, run on the host.

Extracts, unchanged, the low-latency globals, mer_lowlat_active,
us_to_vblank, me_rend_present, me_rend_retire_if_idle, lat_swap and the
vblank wait's begin/step/end (TIER 1 POLICY at g_flip_early), and drives
them from tools/test_lowlat_policy.c: a model of the PSP's clock, vblank,
NEXTFRAME flip, GE and ME around the paced branch of the main loop.

Checked per scenario (light .. sub-60), tier 0 against tier 1 from the same
random stream: no frame is ever skipped or shown out of order; every input
poll lands on the same vblank (the guest is untouched); tier 1 repeats no
more frames than tier 0 (+1); it engages and reaches ~1 vblank where there
is room, and stays out at no cost where there is none.

Then mutants of the production text, each of which must fail.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")
HARNESS = ROOT / "tools/test_lowlat_policy.c"

RANGES = (
    ("#ifndef PLAY_ME_LOWLAT", "static int mer_rewind_busy(void);"),
    ("static int mer_retire_early_active(void)", "/* VISIBLE-END POST"),
    ("static void me_rend_present(void)", "/* SAME-FRAME RETIREMENT."),
    ("static void me_rend_retire_if_idle(void)", "/* Called from plat_video_frame"),
    ("static int lat_swap(int early)", "static void lat_census(void)"),
    ("/* LATE PRESENT (tier 0b).", "int main(int argc"),
)


# Fragments of main()'s game loop, included into the model's loop body.
FRAGMENTS = (
    ("lowlat_top.inc", "\n      g_ll_iter_h = 0;\n",
     "#ifdef REWIND\n      if (g_drew && (g_rw_on || g_rwh.denied))", False),
    ("lowlat_want.inc",
     "         if (!g_ff_mult && ring_w - ring_r > RING_HIGH_WATER &&",
     "         audio_ring_note();", False),
    ("lowlat_wait.inc", "         lat_wait_begin(want);",
     "         g_lw_swapped = 0;\n", True),
)


def production(source):
    parts = []
    for a, b in RANGES:
        i = source.index(a)
        j = source.index(b, i)
        parts.append(source[i:j])
    files = {"lowlat.inc": "\n".join(parts)}
    for name, a, b, incl in FRAGMENTS:
        assert source.count(a) == 1, name
        i = source.index(a)
        j = source.index(b, i) + (len(b) if incl else 0)
        files[name] = source[i:j]
    return files


def build_and_run(files, tmp, quiet=False):
    for name, text in files.items():
        (tmp / name).write_text(text, encoding="utf-8")
    exe = tmp / "t_lowlat"
    r = subprocess.run(["gcc", "-std=gnu99", "-O2", "-Wall", "-Wno-unused-function",
                        "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                        "-I" + str(tmp), str(HARNESS), "-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return None
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
    if not quiet:
        print(r.stdout, end="")
    return r.returncode


MUTANTS = (
    # the prototype's entry: from any loop top once eligible, superseding
    # the frame that top retired
    ("enter anywhere",
     "         if (g_ll_mode_h &&\n"
     "             (!mer_lowlat_active() ||\n"
     "              (g_mer_ready >= 0 && g_mer_ready != g_mer_last_presented)))\n"
     "            ll_to_b();",
     "         if (!g_ll_mode_h && mer_lowlat_active() && ll_eligible())\n"
     "         {\n"
     "            g_ll_mode_h = 1;\n"
     "            g_mer_last_presented = -1;\n"
     "         }"),
    # flip the entry frame before the first frame latched
    ("no latch check",
     "      if ((int)(g_vc_target - sceDisplayGetVcount()) > 1 ||\n"
     "          !vid_flip_latched())",
     "      if (0)"),
    # no deadline at all
    ("no deadline",
     "   if (n < 800u)\n      n = 800u;",
     "   n = 0u;\n   if (0)\n      n = 800u;"),
    # stay in tier 1 after a miss
    ("no exit",
     "static void ll_to_b(void)\n{\n   if (g_ll_mode_h)",
     "static void ll_to_b(void)\n{\n   if (0)"),
    # tier 0b without the late present: a render that misses its loop top
    # repeats the previous frame and is then skipped (FireRed at 222 MHz)
    ("no late present",
     "   g_dfr = 1;\n   g_dfr_n++;\n   return 1;",
     "   return 0;"),
    # an early nudge with the ring nowhere near the high-water mark
    ("pull anytime",
     "level + in_rate / 60u > RING_HIGH_WATER",
     "level + 100000u > RING_HIGH_WATER"),
)


def main():
    prod = production(SOURCE)
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        rc = build_and_run(prod, tmp)
        if rc != 0:
            print("FAIL: production policy")
            return 1
        bad = 0
        for name, old, new in MUTANTS:
            hits = [k for k, v in prod.items() if v.count(old) == 1]
            if len(hits) != 1:
                print("FAIL: mutant %r no longer applies" % name)
                bad += 1
                continue
            mut = dict(prod)
            mut[hits[0]] = prod[hits[0]].replace(old, new)
            rc = build_and_run(mut, tmp, quiet=True)
            if rc == 0:
                print("FAIL: mutant %r survived" % name)
                bad += 1
            else:
                print("mutant %-16s killed" % name)
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
