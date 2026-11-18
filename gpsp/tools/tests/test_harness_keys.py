#!/usr/bin/env python3
"""test_harness_keys.py -- the harness-ini key table (tools/refactor/harness_keys.py, tier C C2-0).

* harness_keys.py selftest: reads found through p_harness, an alias, the online and lab readers;
  a default with a call / assignment / ++ fails (a); a default that differs between flavours
  fails (b); an unclassified use of p_harness, an unlisted alias key, a non-literal key and an
  unknown lab_* literal fail.
* harness_keys.py check on this tree: the committed generated/harness_keys.tsv still matches the
  source (every site holds its key; every raw-grep key is in the table) and the key and site
  counts are PINNED below.  Adding or removing a harness key changes them: regenerate the table
  (harness_keys.py gen, needs a flavours.sh run) and re-pin here, in the same commit.
* harness_keys.py selftest also reads opt-in arms written in fe_priv.h's call form
  (HARNESS_INT/HARNESS_STR, C2-1).
* ci_greps.py R7 (check c) on a scratch tree: warn mode counts and never fails; fail mode (the
  default since C2-1) fails each raw read unless a KNOWN row names its file and key; fe_priv.h
  and comments are not counted.  On this tree R7 in fail mode is clean: the one raw read is the
  frame loop's rewind_gedump, KNOWN (frame parity: raw in every build).
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HK = os.path.join(ROOT, "tools", "refactor", "harness_keys.py")
CG = os.path.join(ROOT, "tools", "refactor", "ci_greps.py")
# pinned at C2-0 (3790851f's source): 344 keys on 370 read sites (release 203, lab 341, harness 319
# keys; 3 keys only in opt-in arms: drprof_us, jit_pub_inline, xlat_scan_fast).  C4-2 (code-health
# C-4): 339 keys on 365 sites, the five pre-ticks adapter rule keys gone (rfu_frame_pace, rfu_hold,
# rfu_pace_max_hold, rfu_shed_keep, rfu_supersede; release 198, lab 336, harness 314).  C2-4'
# (code-health C-2): 323 keys on 349 sites, the 16 rejected-experiment keys gone (release 182,
# lab 320, harness 298).  health-bios: 324 keys on 351 sites, lab_bios (the Lab's bundled-BIOS switch,
# lab_harness.c lab_audit_keys: two sites, lab only; lab 321).
EXPECT_KEYS = 324
EXPECT_SITES = 351

fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1


def run(*args):
    p = subprocess.run([sys.executable, "-I"] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


rc, out = run(HK, "selftest")
check(rc == 0, "harness_keys.py selftest")
if rc:
    print(out)

rc, out = run(HK, "check", "--src", ROOT, "--expect-keys", str(EXPECT_KEYS), "--expect-sites", str(EXPECT_SITES))
check(rc == 0, "harness_keys.py check: table matches the tree, %d keys / %d sites pinned" % (EXPECT_KEYS, EXPECT_SITES))
if rc:
    print(out)

rc, out = run(HK, "check", "--src", ROOT, "--expect-keys", str(EXPECT_KEYS + 1))
check(rc == 1 and "!= pinned" in out, "M-pin: a different key count fails")

d = tempfile.mkdtemp(prefix="hk_r7_")
try:
    os.makedirs(os.path.join(d, "psp", "fe"))
    os.makedirs(os.path.join(d, "frontend-common"))
    with open(os.path.join(d, "psp", "fe", "fe_priv.h"), "w") as f:
        f.write("#define HARNESS_INT(k, d) fe_ini_get_int(HARNESS_INI, k, d)\n")
    with open(os.path.join(d, "psp", "fe", "x.inc.c"), "w") as f:
        f.write("/* fe_ini_get_int(HARNESS_INI, \"in_comment\", 0) */\n"
                "long a = HARNESS_INT(\"ok\", 0);\n"
                "long b = fe_ini_get_int(\n   HARNESS_INI, \"raw\", 0);\n"
                "#define RW_KEY(f, k) g.f = fe_ini_get_int(HARNESS_INI, k, g.f)\n")
    rc, out = run(CG, "--src", d, "--rules", "R7", "--r7", "warn")
    check(rc == 0 and "R7 raw harness reads (mode warn): 2 " in out, "R7 warn: 2 raw reads counted, exit 0")
    rc, out = run(CG, "--src", d, "--rules", "R7")
    check(rc == 1 and "(mode fail)" in out and "psp/fe/x.inc.c:3" in out and "psp/fe/x.inc.c:5" in out
          and "in_comment" not in out,
          "R7 fail (default): the multi-line call and the macro definition fail; fe_priv.h and the comment do not")
    kn = os.path.join(d, "known")
    with open(kn, "w") as f:
        f.write("R7\tpsp/fe/x.inc.c\traw\tscratch exception\n")
    rc, out = run(CG, "--src", d, "--rules", "R7", "--known", kn)
    check(rc == 1 and "KNOWN R7 psp/fe/x.inc.c:3" in out and "FAIL  R7 psp/fe/x.inc.c:5" in out,
          "R7 fail: a KNOWN row (file + key) excuses its read only")
finally:
    shutil.rmtree(d, ignore_errors=True)

rc, out = run(CG, "--src", ROOT, "--rules", "R7")
check(rc == 0 and "KNOWN R7 psp/fe/loop.inc.c:" in out and "(rewind_gedump)" in out and "FAIL" not in out,
      "R7 fail on this tree: every harness read uses HARNESS_INT/HARNESS_STR but the KNOWN rewind_gedump")
if rc:
    print(out)

print("test_harness_keys: %s" % ("PASS" if not fails else "FAILED (%d)" % fails))
sys.exit(1 if fails else 0)
