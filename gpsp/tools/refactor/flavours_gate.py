#!/usr/bin/env python3
"""flavours_gate.py -- helpers of flavours.sh --against (T15).

    flavours_gate.py [--any] [--expect-code-change] CHECKS.txt
                                         print the commit gate line generated
                                         from the '== <flavour> <gate> PASS|DIFF|FAIL'
                                         lines of flavours.sh's checks.txt; every
                                         gate of REQUIRED must be there with all
                                         its flavours (--any: no required set)
    flavours_gate.py jsoneq A.json B.json  equal JSON reports (path keys ignored)
    flavours_gate.py ratchet [GEN_DIR [RATCHET]]  T14's ratchet (tier C, C6-1): per flavour, the
                                         core copies (asserted + UNCHECKED) and the UNCHECKED
                                         copies that generated/<flavour>/fe_abi_asserts.h states
                                         may not exceed abi_asserts.ratchet's maxima (they may
                                         only fall; PLATFORM rows are not counted).  Defaults:
                                         this tool's generated/ and abi_asserts.ratchet
    flavours_gate.py --identity            print the identity gates (flavours.sh reads them)
    flavours_gate.py --selftest

The gate line is never typed (PLAN-P-beta5 §4 common): one token per gate, e.g.
    G1 ok rel/lab/har G2 ok rel/lab/har HO ok ... CI ok
and on a failure  G1 FAIL lab (det_check) ...   Exit 0 when every verdict is
PASS, 1 when any is FAIL or when checks.txt holds no verdict at all, 2 usage.

IDENTITY gates (G0 G0var G1 G2 HO ST BF DS) prove that a step changed nothing:
built code, preprocessed source, stack, hot order, build files, diff scope.
flavours.sh writes DIFF when such a check ran and found a difference (its
tool's exit 1), FAIL when it could not run or broke.  The other gates (SS LL
CF CI FP) are invariants every commit must keep; they are only PASS or FAIL.
  default               a pure refactor: a DIFF is a failure, printed FAIL
  --expect-code-change  a commit meant to change code: an identity gate whose
                        verdicts are all PASS or DIFF prints
                        `G1 CHANGED rel/lab/har` and passes; a FAIL, a MISSING
                        gate or flavour, a DIFF on an invariant gate and an
                        invariant FAIL still fail
"""
import json
import os
import re
import sys

ABBR = {'release': 'rel', 'lab': 'lab', 'harness': 'har', '-': ''}
# release-stable (RFU_SCHED=0) went with the switch in code-health C-4
FLAVOURS = ('release', 'lab', 'harness')
# every gate a P step needs, and the flavours each must cover (adversarial review
# 2026-11-07: G0/G0var/LL/CF and the build-file diff were typed by hand, and a gate
# with one flavour was accepted).  A missing gate or flavour is a FAIL.
REQUIRED = {'G0': FLAVOURS, 'G0var': ('-',), 'G1': FLAVOURS, 'G2': FLAVOURS, 'HO': FLAVOURS, 'SS': FLAVOURS,
            'ST': FLAVOURS, 'LL': ('release', 'lab'), 'CF': ('-',), 'BF': ('-',), 'DS': ('-',),
            'CI': ('-',), 'FP': ('-',)}
ORDER = ['G0', 'G0var', 'G1', 'G2', 'HO', 'SS', 'ST', 'LL', 'CF', 'BF', 'DS', 'CI', 'FP']
IDENTITY = ('G0', 'G0var', 'G1', 'G2', 'HO', 'ST', 'BF', 'DS')
VERDICT = re.compile(r'^== (\S+) (\S+) (PASS|DIFF|FAIL)(?: \((.*)\))?\s*$')


def gate_line(lines, required=None, expect_change=False):
    """-> (line, ok).  A gate is ok only if every one of its verdicts passed and,
    with `required` ({gate: flavours}), every required gate has a verdict for
    each of its flavours.  With expect_change, an IDENTITY gate's DIFF verdicts
    are expected (CHANGED) instead of failures."""
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
        expected = expect_change and g in IDENTITY
        fails = [(f, lab) for f, v, lab in per[g] if v == 'FAIL' or (v == 'DIFF' and not expected)]
        fls, changed = [], []
        for f, v, _ in per[g]:
            a = ABBR.get(f, f)
            if a and a not in fls:
                fls.append(a)
            if v == 'DIFF' and (a or '-') not in changed:
                changed.append(a or '-')
        if fails:
            ok = False
            out.append('%s FAIL %s' % (g, ','.join('%s(%s)' % (ABBR.get(f, f) or '-', lab) for f, lab in fails)))
        elif changed:
            out.append('%s CHANGED %s' % (g, '/'.join(changed)))
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
    try:
        a, b = (norm(json.load(open(p))) for p in (pa, pb))
    except (OSError, ValueError) as e:   # a report that is not there is no difference: exit 2
        print('jsoneq: cannot read: %s' % e)
        return 2
    if a == b:
        print('jsoneq: identical (%s, %s)' % (os.path.basename(pa), os.path.basename(pb)))
        return 0
    ka = set(a) if isinstance(a, dict) else set()
    kb = set(b) if isinstance(b, dict) else set()
    diff = sorted(k for k in ka | kb if not (isinstance(a, dict) and isinstance(b, dict)) or a.get(k) != b.get(k))
    print('jsoneq: DIFFERENT in %s' % ', '.join(diff[:20]))
    return 1


HERE = os.path.dirname(os.path.abspath(__file__))
RATCHET = os.path.join(HERE, 'abi_asserts.ratchet')
T14_COUNTS = re.compile(r'checked: (\d+) declarations, \d+ constants; unchecked: (\d+)(?:; platform: (\d+))?')


def load_ratchet(path):
    """{flavour: (max core copies, max unchecked)} of abi_asserts.ratchet."""
    out = {}
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        p = line.split()
        out[p[0]] = (int(p[1]), int(p[2]))
    return out


def ratchet_check(headers, maxima):
    """headers {flavour: header text}; maxima {flavour: (copies, unchecked)} -> (ok, lines).
    A flavour of the ratchet with no header, or a header without the counts line, fails."""
    ok, out = True, []
    for fl in sorted(maxima):
        mc, mu = maxima[fl]
        txt = headers.get(fl)
        m = T14_COUNTS.search(txt or '')
        if not m:
            ok = False
            out.append('  FAIL  %s: %s' % (fl, 'no header' if txt is None else 'no "checked: ... unchecked:" line'))
            continue
        a, u, pl = int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)
        c = a + u
        bad = c > mc or u > mu
        ok = ok and not bad
        out.append('  %s  %s: core copies %d (max %d), unchecked %d (max %d), platform %d%s' % (
            'FAIL' if bad else 'ok  ', fl, c, mc, u, mu, pl,
            '' if bad or (c, u) == (mc, mu) else '; below the ratchet: lower abi_asserts.ratchet to %d %d' % (c, u)))
    return ok, out


def ratchet(gen_dir, path):
    maxima = load_ratchet(path)
    hs = {}
    for fl in maxima:
        h = os.path.join(gen_dir, fl, 'fe_abi_asserts.h')
        if os.path.exists(h):
            hs[fl] = open(h, encoding='utf-8').read()
    ok, out = ratchet_check(hs, maxima)
    print('T14 ratchet (%s):' % os.path.basename(path))
    print('\n'.join(out))
    print('T14 ratchet: %s' % ('ok' if ok else 'FAIL'))
    return 0 if ok else 1


def selftest():
    bad = 0

    def t(cond, msg):
        nonlocal bad
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        bad += 0 if cond else 1
    good = ['== release G1 PASS (release det_check)',
            '== lab G1 PASS (x)', '== harness G1 PASS (x)', '== release SS PASS (x)', '== - CI PASS (ci)']
    line, ok = gate_line(good)
    t(ok and line == 'G1 ok rel/lab/har SS ok rel CI ok', 'all pass -> %r' % line)
    line, ok = gate_line(good + ['== lab ST FAIL (lab stack_depth)'])
    t(not ok and 'ST FAIL lab(lab stack_depth)' in line, 'one FAIL turns the gate red -> %r' % line)
    line, ok = gate_line(['-- noise', 'PASS'])
    t(not ok, 'no verdict lines is not a pass -> %r' % line)
    line, ok = gate_line(good[:1] + ['== - CI FAIL (ci_greps rc absent)'])
    t(not ok and 'CI FAIL' in line, 'an absent tool (failchk) is a FAIL -> %r' % line)
    full = ['== %s %s PASS (x)' % (f, g) for g in ('G0', 'G1', 'G2', 'HO', 'SS', 'ST') for f in FLAVOURS]
    full += ['== release LL PASS (x)', '== lab LL PASS (x)'] + ['== - %s PASS (x)' % g for g in ('G0var', 'CF', 'BF', 'DS', 'CI', 'FP')]
    line, ok = gate_line(full, REQUIRED)
    t(ok and line.startswith('G0 ok rel/lab/har G0var ok G1 ok') and 'LL ok rel/lab' in line and line.endswith('DS ok CI ok FP ok'),
      'the whole required set -> %r' % line)
    line, ok = gate_line([x for x in full if 'G0var' not in x], REQUIRED)
    t(not ok and 'G0var MISSING' in line, 'a required gate missing (G0var) is a FAIL -> %r' % line)
    line, ok = gate_line([x for x in full if ' FP ' not in x], REQUIRED)
    t(not ok and 'FP MISSING' in line, 'the frame-parity gate missing is a FAIL -> %r' % line)
    line, ok = gate_line([x for x in full if ' FP ' not in x] + ['== - FP FAIL (frame_parity release vs lab)'], REQUIRED)
    t(not ok and 'FP FAIL' in line, 'a frame-parity FAIL turns the gate red -> %r' % line)
    line, ok = gate_line([x for x in full if not x.startswith('== harness G1')], REQUIRED)
    t(not ok and 'G1 MISSING har' in line, 'a gate covering 2 of 3 flavours is a FAIL -> %r' % line)
    line, ok = gate_line(good, REQUIRED)
    t(not ok, 'the old six-gate set is not a full gate -> %r' % line[:80])
    hdr = ' * checked: %d declarations, 6 constants; unchecked: %d; platform: %d (listed at the end). */'
    ok, out = ratchet_check({'release': hdr % (52, 34, 5)}, {'release': (86, 34)})
    t(ok, 'ratchet: at the maxima -> %s' % out[0].strip())
    ok, out = ratchet_check({'release': hdr % (53, 34, 5)}, {'release': (86, 34)})
    t(not ok, 'ratchet: one more core copy fails -> %s' % out[0].strip())
    ok, out = ratchet_check({'release': hdr % (86, 0, 5)}, {'release': (86, 34)})
    t(ok and 'lower' in out[0], 'ratchet: copies moved from UNCHECKED to asserted pass, with a hint -> %s' % out[0].strip())
    ok, out = ratchet_check({'release': hdr % (52, 35, 4)}, {'release': (87, 34)})
    t(not ok, 'ratchet: a platform name turned UNCHECKED fails -> %s' % out[0].strip())
    ok, out = ratchet_check({}, {'release': (86, 34)})
    t(not ok, 'ratchet: a missing header fails -> %s' % out[0].strip())
    ok, out = ratchet_check({'release': ' * checked: 52 declarations, 6 constants; unchecked: 39 (listed at the end). */'},
                            {'release': (91, 39)})
    t(ok, 'ratchet: a pre-C6-1 header (no platform count) is read -> %s' % out[0].strip())
    # --expect-code-change: `full` with every identity gate DIFF in every flavour
    chg = [x.replace(' PASS ', ' DIFF ') if x.split()[2] in IDENTITY else x for x in full]
    line, ok = gate_line(chg, REQUIRED)
    t(not ok and 'G1 FAIL' in line and 'CHANGED' not in line, 'default mode: a DIFF is a FAIL -> %r' % line[:60])
    line, ok = gate_line(chg, REQUIRED, expect_change=True)
    t(ok and 'G1 CHANGED rel/lab/har' in line and 'G0var CHANGED -' in line and 'SS ok rel/lab/har' in line
      and 'FAIL' not in line, 'expect-change: identity DIFFs are CHANGED, the gate passes -> %r' % line)
    line, ok = gate_line([x for x in chg if not x.startswith('== lab G1')] + ['== lab G1 PASS (x)'], REQUIRED, True)
    t(ok and 'G1 CHANGED rel/har' in line, 'expect-change: CHANGED names only the flavours that differ -> %r' % line[:40])
    line, ok = gate_line(chg + ['== lab ST FAIL (lab stack_depth; rc 2)'], REQUIRED, True)
    t(not ok and 'ST FAIL lab(lab stack_depth; rc 2)' in line,
      'expect-change: a FAIL in an identity gate (the tool broke) still fails -> %r' % line)
    line, ok = gate_line([x for x in chg if x != '== lab SS PASS (x)'] + ['== lab SS FAIL (x)'], REQUIRED, True)
    t(not ok and 'SS FAIL lab' in line, 'expect-change: an invariant FAIL (SS) still fails -> %r' % line)
    line, ok = gate_line([x for x in chg if ' FP ' not in x] + ['== - FP DIFF (x)'], REQUIRED, True)
    t(not ok and 'FP FAIL' in line, 'expect-change: a DIFF on an invariant gate (FP) is a FAIL -> %r' % line)
    line, ok = gate_line([x for x in chg if not x.startswith('== harness G2')], REQUIRED, True)
    t(not ok and 'G2 MISSING har' in line, 'expect-change: a missing flavour is still a FAIL -> %r' % line)
    line, ok = gate_line(full, REQUIRED, True)
    t(ok and 'CHANGED' not in line, 'expect-change on a commit that changed nothing: all ok -> %r' % line[:40])
    print('flavours_gate selftest: %s' % ('PASS' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


def main(argv):
    if argv[:1] == ['--selftest']:
        return selftest()
    if argv == ['--identity']:          # flavours.sh reads the identity gates from here
        print(' '.join(IDENTITY))
        return 0
    if argv[:1] == ['ratchet'] and len(argv) <= 3:
        return ratchet(argv[1] if len(argv) > 1 else os.path.join(HERE, 'generated'),
                       argv[2] if len(argv) > 2 else RATCHET)
    if len(argv) == 3 and argv[0] == 'jsoneq':
        return jsoneq(argv[1], argv[2])
    req, expect = REQUIRED, False
    while argv[:1] in (['--any'], ['--expect-code-change']):
        if argv[0] == '--any':         # no required set (ad-hoc checks files)
            req = None
        else:
            expect = True
        argv = argv[1:]
    if len(argv) != 1:
        print(__doc__)
        return 2
    with open(argv[0], encoding='utf-8', errors='replace') as f:
        line, ok = gate_line(f, req, expect)
    print(line)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
