#!/usr/bin/env python3
"""test_frame_parity.py -- tools/layout/frame_parity.py (the frame-parity gate, owner decision
2026-11-17), with no PSP build:

* the committed spec parses, names the frame loop as a root and the autopilot as rig-only;
* the committed baseline parses and records the 624af868 release/lab pair, frame_loop in it;
* the ratchet (check) on synthetic rows, each a control that passes or a mutant that fails:
  a release-only edit inside a function (one more hunk) fails; the same hunks pass; fewer hunks
  pass with a note; a held I-cache set that moves fails, a never-shared one outside the hot
  block is a note; same -> code difference fails; layout -> code difference fails; a member
  that drops out of one build fails; a new member must be the same code; rig-only is skipped;
* multiset_sub, layout_pair, the name globs, hunks() (the pure-Python patience diff: small cases, a 4000-word mutant, determinism).
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'layout'))
import frame_parity as F  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails += 1


# ---- the committed inputs
spec = F.read_spec(F.SPEC)
check(any(p == 'frame_loop' for p, _ in spec['root']), 'spec: frame_loop is a root')
check(any(p == 'fe_autopilot_frame' for p, _ in spec['rigonly']), 'spec: fe_autopilot_frame is rig-only')
check(any(p == 'netpacket_iface' for p, _ in spec['data']), 'spec: netpacket_iface is per-frame data')
check(os.path.exists(F.BASE), 'the baseline file is committed')
if os.path.exists(F.BASE):
    base = F.read_base(F.BASE)
    hdr = open(F.BASE).read()
    check('624af868' in hdr, 'baseline: recorded from the 624af868 pair')
    check(base.get('frame_loop', {}).get('state') == 'diff', 'baseline: frame_loop differed at 624af868')
    check(base.get('update_gba', {}).get('d') == 0, 'baseline: update_gba shared its I-cache set')


def row(state, d=0, chunks=(), hot=False, rig=None):
    return dict(state=state, d=d, chunks=[dict(h=h) for h in chunks], hot=hot, rig=rig, kind='fn')


def b(state, d=0, c=()):
    return dict(kind='fn', state=state, d=d, c=list(c))


def run(rows, base):
    return F.check(rows, base)


# ---- the ratchet
f, n = run({'x': row('diff', 0x17ec, ['a', 'b'])}, {'x': b('diff', 0x17ec, ['a', 'b'])})
check(not f, 'control: the baseline\'s own hunks pass')
f, n = run({'x': row('diff', 0x17ec, ['a', 'b', 'c'])}, {'x': b('diff', 0x17ec, ['a', 'b'])})
check(len(f) == 1 and 'did not have' in f[0], 'mutant: one more release/lab hunk fails -> %s' % f[:1])
f, n = run({'x': row('diff', 0x17ec, ['a', 'a'])}, {'x': b('diff', 0x17ec, ['a', 'b'])})
check(len(f) == 1, 'mutant: a hunk twice where the baseline had it once fails (a multiset)')
f, n = run({'x': row('diff', 0x17ec, ['a'])}, {'x': b('diff', 0x17ec, ['a', 'b'])})
check(not f and any('fewer' in x for x in n), 'control: fewer hunks pass, with a re-record note')
f, n = run({'x': row('same', 0)}, {'x': b('same', 0)})
check(not f and not n, 'control: same and d 0 hold')
f, n = run({'x': row('same', 0x20)}, {'x': b('same', 0)})
check(len(f) == 1 and 'I-cache' in f[0], 'mutant: a shared I-cache set lost fails')
f, n = run({'x': row('same', 0x1848)}, {'x': b('same', 0x1808)})
check(not f and len(n) == 1, 'control: a never-shared set outside the hot block moves with a note')
f, n = run({'x': row('same', 0x1848, hot=True)}, {'x': b('same', 0x1808)})
check(len(f) == 1, 'mutant: a hot-block member\'s offset is held even where it was not 0')
f, n = run({'x': row('diff', 0, ['q'])}, {'x': b('same', 0)})
check(len(f) == 1 and 'identical at the baseline' in f[0], 'mutant: same -> a code difference fails')
f, n = run({'x': row('layout', 0)}, {'x': b('same', 0)})
check(len(f) == 1, 'mutant: same -> layout (a changed immediate) fails')
f, n = run({'x': row('diff', 0, ['q'])}, {'x': b('layout', 0)})
check(len(f) == 1 and 'by code too' in f[0], 'mutant: layout -> a code difference fails')
f, n = run({'x': row('layout', 0)}, {'x': b('layout', 0)})
check(not f, 'control: layout stays layout')
f, n = run({'x': row('lab-only')}, {'x': b('layout', 0x1d00)})
check(len(f) == 1 and 'now lab-only' in f[0], 'mutant: a per-frame callee that left the release fails')
f, n = run({'x': row('lab-only')}, {'x': dict(kind='fn', state='lab-only')})
check(not f, 'control: lab-only stays lab-only')
f, n = run({'x': row('same', 0)}, {'x': dict(kind='fn', state='lab-only')})
check(not f and n, 'control: lab-only -> in both and identical passes, with a note')
f, n = run({'y': row('same', 0)}, {})
check(not f, 'control: a new member, the same code at the same set')
f, n = run({'y': row('diff', 0, ['q'])}, {})
check(len(f) == 1 and 'NEW' in f[0], 'mutant: a new member with release/lab code differences fails')
f, n = run({'y': row('same', 0x40)}, {})
check(not f and n, 'control: a new member outside the hot block at another set: a note')
f, n = run({'y': row('same', 0x40, hot=True)}, {})
check(len(f) == 1, 'mutant: a new hot-block member at another set fails')
f, n = run({'y': row('lab-only')}, {})
check(len(f) == 1, 'mutant: a new lab-only per-frame function fails')
f, n = run({'z': row('diff', 0x40, ['q'], rig=('rigonly', 'x'))}, {'z': b('same', 0)})
check(not f, 'control: a rig-only member is not compared')
f, n = run({}, {'gone': b('same', 0)})
check(not f and n, 'control: a member that left the set is a note')

# ---- helpers
extra, fewer = F.multiset_sub(['a', 'b', 'b'], ['a', 'b', 'c'])
check(extra == ['b'] and fewer == 1, 'multiset_sub: one extra b, one c left')
lw = 0x8C820010            # lw v0, 16(a0)
check(F.layout_pair(lw, (lw & 0xFFFF0000) | 0x0014), 'layout_pair: another immediate of one load')
check(not F.layout_pair(lw, 0x8C830010), 'layout_pair: another target register is code')
check(not F.layout_pair(0x0C000010, 0x0C000020), 'layout_pair: another call is code')
pats = [('lat_swap*', ''), ('frame_loop', '')]
check(F.matches('lat_swap.isra.0', pats) is not None, 'matches: a GCC clone by its base name')
check(F.matches('frame_loop_helper', pats) is None, 'matches: no prefix match without a glob')
h = F.hunks(['a', 'b', 'c'], ['a', 'x', 'c'])
check(h == [(1, 2, 1, 2)], 'hunks: one replaced word -> %r' % h)
h = F.hunks(['p', 'n', 'q', 'n', 'r'], ['p', 'n', 'q', 'n', 'n', 'r'])
check(len(h) == 1 and h[0][1] - h[0][0] == 0 and h[0][3] - h[0][2] == 1,
      'hunks: one inserted word among repeats -> %r' % h)
check(F.hunks(list('abc'), list('abc')) == [], 'hunks: equal lists have none')
import random  # noqa: E402
rnd = random.Random(7)
xa = [rnd.choice('abcdefgh') + str(rnd.randrange(40)) for _ in range(4000)]
xb = list(xa)
for k in (100, 1700, 3999):
    xb[k] = 'MUT'
xb.insert(2500, 'INS')
h = F.hunks(xa, xb)
check(len(h) == 4 and h == F.hunks(xa, xb), 'hunks: 3 replaced and 1 inserted word in 4000 -> %d hunks, '
      'the same on a second run' % len(h))
# every hunk reproduces b from a
rebuilt, last = [], 0
for i1, i2, j1, j2 in h:
    rebuilt += xa[last:i1] + xb[j1:j2]
    last = i2
rebuilt += xa[last:]
check(rebuilt == xb, 'hunks: the regions patch a into b')

print('test_frame_parity: %s' % ('PASS' if not fails else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
