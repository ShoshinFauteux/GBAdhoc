"""runs.py -- the flavours of a flavours.sh run dir, discovered (never a fixed list).

A flavour is any subdirectory holding gpsp_adhoc.elf.  The adversarial review of
2026-11-07 found det_check.py --runs, sect_cmp.py --runs and strong_syms.py checkrun
hard-coding release/lab/harness, so a broken release-stable passed all three.
"""
import json
import os

CANON = ('release', 'release-stable', 'lab', 'harness')


def flavours_in(run):
    if not os.path.isdir(run):
        return []
    fl = [d for d in os.listdir(run) if os.path.isfile(os.path.join(run, d, 'gpsp_adhoc.elf'))]
    return sorted(fl, key=lambda f: (CANON.index(f) if f in CANON else len(CANON), f))


def pair(r1, r2):
    """-> (in both, only in one).  A flavour built in one run only is never 'identical'."""
    a, b = flavours_in(r1), flavours_in(r2)
    both = [f for f in a if f in b]
    one = [f for f in a + b if f not in both]
    return both, sorted(set(one), key=one.index)


def profile_of(run, fl):
    """the build.sh profile a flavour dir was built with (flavour.json), else its name."""
    p = os.path.join(run, fl, 'flavour.json')
    try:
        return json.load(open(p)).get('buildShProfile') or fl
    except (OSError, ValueError):
        return {'release-stable': 'release'}.get(fl, fl)
