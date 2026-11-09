#!/usr/bin/env python3
"""flavours_gate.py -- helpers of flavours.sh --against (T15).

    flavours_gate.py [--any] CHECKS.txt  print the commit gate line generated
                                         from the '== <flavour> <gate> PASS|FAIL'
                                         lines of flavours.sh's checks.txt; every
                                         gate of REQUIRED must be there with all
                                         its flavours (--any: no required set)
    flavours_gate.py jsoneq A.json B.json  equal JSON reports (path keys ignored)
    flavours_gate.py --selftest

The gate line is never typed (PLAN-P-beta5 §4 common): one token per gate, e.g.
    G1 ok rel/stb/lab/har G2 ok rel/stb/lab/har HO ok ... CI ok
and on a failure  G1 FAIL lab (det_check) ...   Exit 0 when every verdict is
PASS, 1 when any is FAIL or when checks.txt holds no verdict at all, 2 usage.
"""
import json
import os
import re
import sys

ABBR = {'release': 'rel', 'release-stable': 'stb', 'lab': 'lab', 'harness': 'har', '-': ''}
FOUR = ('release', 'release-stable', 'lab', 'harness')
# every gate a P step needs, and the flavours each must cover (adversarial review
# 2026-11-07: G0/G0var/LL/CF and the build-file diff were typed by hand, and a gate
# with one flavour was accepted).  A missing gate or flavour is a FAIL.
REQUIRED = {'G0': FOUR, 'G0var': ('-',), 'G1': FOUR, 'G2': FOUR, 'HO': FOUR, 'SS': FOUR,
            'ST': FOUR, 'LL': ('release', 'lab'), 'CF': ('-',), 'BF': ('-',), 'DS': ('-',),
            'CI': ('-',)}
ORDER = ['G0', 'G0var', 'G1', 'G2', 'HO', 'SS', 'ST', 'LL', 'CF', 'BF', 'DS', 'CI']
VERDICT = re.compile(r'^== (\S+) (\S+) (PASS|FAIL)(?: \((.*)\))?\s*$')


def gate_line(lines, required=None):
    """-> (line, ok).  A gate is ok only if every one of its verdicts passed and,
    with `required` ({gate: flavours}), every required gate has a PASS verdict
    for each of its flavours."""
    per = {}   # gate -> list of (flavour, verdict, label)
    for ln in lines:
        m = VERDICT.match(ln.rstrip('\r\n'))
        if m:
            per.setdefault(m.group(2), []).append((m.group(1), m.group(3), m.group(4) or ''))
    if not per:
        return 'NO VERDICTS (checks.txt empty: nothing was checked)', False
    out, ok = [], True
    for g in (required or {}):
        if g not in per:
            per[g] = []
    for g in ORDER + sorted(set(per) - set(ORDER)):
        if g not in per:
            continue
        missing = [f for f in (required or {}).get(g, ()) if f not in {x for x, _, _ in per[g]}]
        if missing:
            ok = False
            out.append('%s MISSING %s' % (g, ','.join(ABBR.get(f, f) or '-' for f in missing)))
            continue
        fails = [(f, lab) for f, v, lab in per[g] if v != 'PASS']
        fls = []
        for f, _, _ in per[g]:
            a = ABBR.get(f, f)
            if a and a not in fls:
                fls.append(a)
        if fails:
            ok = False
            out.append('%s FAIL %s' % (g, ','.join('%s(%s)' % (ABBR.get(f, f) or '-', lab) for f, lab in fails)))
        else:
            out.append('%s ok%s' % (g, (' ' + '/'.join(fls)) if fls else ''))
    return ' '.join(out), ok


def jsoneq(pa, pb):
    def norm(o):
        if isinstance(o, dict):
            return {k: norm(v) for k, v in o.items()
                    if k not in ('elf', 'map', 'build', 'dir', 'path', 'ord', 'order', 'json')}
        if isinstance(o, list):
            return [norm(v) for v in o]
        return o
    a, b = (norm(json.load(open(p))) for p in (pa, pb))
    if a == b:
        print('jsoneq: identical (%s, %s)' % (os.path.basename(pa), os.path.basename(pb)))
        return 0
    ka = set(a) if isinstance(a, dict) else set()
    kb = set(b) if isinstance(b, dict) else set()
    diff = sorted(k for k in ka | kb if not (isinstance(a, dict) and isinstance(b, dict)) or a.get(k) != b.get(k))
    print('jsoneq: DIFFERENT in %s' % ', '.join(diff[:20]))
    return 1


def selftest():
    bad = 0

    def t(cond, msg):
        nonlocal bad
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        bad += 0 if cond else 1
    good = ['== release G1 PASS (release det_check)', '== release-stable G1 PASS (x)',
            '== lab G1 PASS (x)', '== harness G1 PASS (x)', '== release SS PASS (x)', '== - CI PASS (ci)']
    line, ok = gate_line(good)
    t(ok and line == 'G1 ok rel/stb/lab/har SS ok rel CI ok', 'all pass -> %r' % line)
    line, ok = gate_line(good + ['== lab ST FAIL (lab stack_depth)'])
    t(not ok and 'ST FAIL lab(lab stack_depth)' in line, 'one FAIL turns the gate red -> %r' % line)
    line, ok = gate_line(['-- noise', 'PASS'])
    t(not ok, 'no verdict lines is not a pass -> %r' % line)
    line, ok = gate_line(good[:1] + ['== - CI FAIL (ci_greps rc absent)'])
    t(not ok and 'CI FAIL' in line, 'an absent tool (failchk) is a FAIL -> %r' % line)
    full = ['== %s %s PASS (x)' % (f, g) for g in ('G0', 'G1', 'G2', 'HO', 'SS', 'ST') for f in FOUR]
    full += ['== release LL PASS (x)', '== lab LL PASS (x)'] + ['== - %s PASS (x)' % g for g in ('G0var', 'CF', 'BF', 'DS', 'CI')]
    line, ok = gate_line(full, REQUIRED)
    t(ok and line.startswith('G0 ok rel/stb/lab/har G0var ok G1 ok') and 'LL ok rel/lab' in line and line.endswith('DS ok CI ok'),
      'the whole required set -> %r' % line)
    line, ok = gate_line([x for x in full if 'G0var' not in x], REQUIRED)
    t(not ok and 'G0var MISSING' in line, 'a required gate missing (G0var) is a FAIL -> %r' % line)
    line, ok = gate_line([x for x in full if not x.startswith('== harness G1')], REQUIRED)
    t(not ok and 'G1 MISSING har' in line, 'a gate covering 3 of 4 flavours is a FAIL -> %r' % line)
    line, ok = gate_line(good, REQUIRED)
    t(not ok, 'the old six-gate set is not a full gate -> %r' % line[:80])
    print('flavours_gate selftest: %s' % ('PASS' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


def main(argv):
    if argv[:1] == ['--selftest']:
        return selftest()
    if len(argv) == 3 and argv[0] == 'jsoneq':
        return jsoneq(argv[1], argv[2])
    req = REQUIRED
    if argv[:1] == ['--any']:          # no required set (ad-hoc checks files)
        req, argv = None, argv[1:]
    if len(argv) != 1:
        print(__doc__)
        return 2
    with open(argv[0], encoding='utf-8', errors='replace') as f:
        line, ok = gate_line(f, req)
    print(line)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
