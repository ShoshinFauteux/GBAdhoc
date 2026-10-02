#!/usr/bin/env python3
"""sharp_compare.py <runs dir> -- verdicts for tools/display-rig/sharp.sh.

Byte-compares the GE drawbuffer dumps (ge_NNNNNN.bmp) of run pairs, frame by
frame, from frame MIN_FRAME on (earlier frames precede the state load, where
the wall-clock RTC makes runs differ).  A pair passes only if both runs exited
cleanly, both have the same set of dumped frames, there are at least MIN_N of
them, and every one is identical.  Pairs that MUST differ (sharp vs bilinear)
are checked too, so a comparison that cannot see a difference fails.
"""
import hashlib
import os
import re
import sys

MIN_FRAME = 60
MIN_N = 5

runs = sys.argv[1]


def dumps(tag, pat=r"ge_(\d+)\.bmp$"):
    d = os.path.join(runs, tag)
    out = {}
    if not os.path.isdir(d):
        return None
    for f in os.listdir(d):
        m = re.match(pat, f)
        if m and int(m.group(1)) >= MIN_FRAME:
            out[int(m.group(1))] = hashlib.md5(
                open(os.path.join(d, f), "rb").read()).hexdigest()
    return out


def clean_exit(tag):
    try:
        log = open(os.path.join(runs, tag, "frontend.log"),
                   errors="replace").read()
    except OSError:
        return False
    return re.search(r"^EVT exit", log, re.M) is not None


def pair(a, b, want_same=True, pat=r"ge_(\d+)\.bmp$"):
    da, db = dumps(a, pat), dumps(b, pat)
    if da is None or db is None:
        return False, "%s vs %s: MISSING run" % (a, b)
    if not (clean_exit(a) and clean_exit(b)):
        return False, "%s vs %s: no clean EVT exit" % (a, b)
    if set(da) != set(db) or len(da) < (MIN_N if pat.startswith("ge") else 1):
        return False, "%s vs %s: frame sets differ or too few (%d/%d)" % (
            a, b, len(da), len(db))
    same = sum(1 for k in da if da[k] == db[k])
    n = len(da)
    if want_same:
        ok = same == n
        return ok, "%-4s %-22s == %-22s %d/%d frames identical" % (
            "PASS" if ok else "FAIL", a, b, same, n)
    ok = same == 0
    return ok, "%-4s %-22s != %-22s %d/%d frames differ" % (
        "PASS" if ok else "FAIL", a, b, n - same, n)


checks = []
# OFF: base vs this build, every existing arm.
for t in sorted(os.listdir(runs)):
    if t.startswith("B-") and os.path.isdir(os.path.join(runs, "N-" + t[2:])):
        checks.append(("off", "B-" + t[2:], "N-" + t[2:], True, None))
checks.append(("ctrl", "B-g-s1-f1", "B2-g-s1-f1", True, None))
# ON: integer scales are nearest, exactly.
checks += [
    ("on", "S-g-s0", "N-g-s0-f0", True, None),
    ("on", "S-g-s3", "N-g-s3-f1", True, None),
    ("on", "S-g-s0-amb", "N-g-s0-f0-amb", True, None),
    ("on", "S-c-s0", "N-c-s0-f0", True, None),
    ("on", "S-c-s3", "N-c-s3-f0", True, None),
    # ...and the non-integer ones really are different pictures.
    ("on", "S-g-s1", "N-g-s1-f1", False, None),
    ("on", "S-g-s1", "N-g-s1-f0", False, None),
    ("on", "S-g-s2", "N-g-s2-f1", False, None),
    ("on", "S-c-s1", "N-c-s1-f1", False, None),
    ("on", "S-c-s1", "N-c-s1-f0", False, None),
    ("on", "S-k-s1", "N-k-s1-f1", False, None),
    # Screenshots (the core frame) do not depend on the filter.
    ("shot", "S-g-s1", "N-g-s1-f1-shot", True, r"frame_(\d+)\.bmp$"),
]

fails = 0
for kind, a, b, same, pat in checks:
    if not os.path.isdir(os.path.join(runs, a)) or \
       not os.path.isdir(os.path.join(runs, b)):
        continue
    ok, line = pair(a, b, same, pat or r"ge_(\d+)\.bmp$")
    fails += not ok
    print("[%s] %s" % (kind, line))
print("RESULT: %s (%d failing)" % ("PASS" if not fails else "FAIL", fails))
sys.exit(1 if fails else 0)
