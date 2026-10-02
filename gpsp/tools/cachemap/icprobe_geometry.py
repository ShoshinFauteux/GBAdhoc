#!/usr/bin/env python3
"""icprobe_geometry.py -- the instruction-cache geometry from a console's
`icache_probe = 1` log (psp/icprobe.c, docs/CACHE-MAP.md).

    icprobe_geometry.py frontend.log [--json out.json]

The probe times chains of K one-line blocks at stride S (warm, ns per block)
and single cold passes over 2048 blocks at small strides.  From them:

  ref      the S = 64 row (consecutive lines never conflict): the same code
           with the same per-pass overhead, so every stride is read as its
           ratio to this row at the same K
  step(S)  the smallest K from which every longer chain costs > STEP x the
           reference -- the chain no longer fits its sets
  way      the smallest stride whose step is the lowest of all strides and
           equal for every larger stride (all blocks in one set)
  ways     step(way) - 1, checked against step(way / 2) = 2 * ways + 1
  line     cold cost per block is proportional to s / line below the line
           and flat from it: the smallest s with cold(s) >= FLAT x cold(2s)
  miss     step cost - hit cost (ns per miss, warm)

Exit 0 with the geometry, 2 when the log shows no step at any stride (an
emulator without a cache model -- PPSSPP: the analyser's negative control),
3 when the rows contradict each other (no conclusion, never a guess).
"""
import argparse
import json
import re
import statistics
import sys

STEP = 2.5     # a miss chain costs at least this many times a hit chain
FLAT = 0.8     # cold(s) / cold(2s) at or above this: s >= the line size


def parse(path):
    warm, cold = {}, {}
    for l in open(path, errors='replace'):
        m = re.match(r'EVT icprobe_(warm|cold) s=(\d+) k=(\d+) ns10=(\d+)', l)
        if not m:
            continue
        s, k, v = int(m.group(2)), int(m.group(3)), int(m.group(4)) / 10.0
        if m.group(1) == 'warm':
            warm.setdefault(s, {})[k] = v
        else:
            cold[s] = v
    return warm, cold


def analyse(warm, cold):
    out = {'verdict': None, 'notes': []}
    if 64 not in warm or len(warm[64]) < 4:
        out['verdict'] = 'NO_DATA'
        return out
    # Every chain pays a fixed loop-back cost per pass, so ns per block falls
    # as 1/K on its own (PPSSPP: 12.0 ns at K=1, 3.7 at K=12).  The S = 64
    # row -- the same instructions, K consecutive lines that can never
    # conflict -- carries exactly that overhead: each stride is read as its
    # RATIO to it at the same K.  Hits ~1, a set that overflowed several x.
    ref = warm[64]
    hit = min(ref.values())
    out['hit_ns'] = hit
    ratio = {s: {k: v / ref[k] for k, v in row.items() if k in ref}
             for s, row in warm.items()}
    out['ratio'] = {s: {k: round(v, 2) for k, v in r.items()}
                    for s, r in ratio.items()}
    steps = {}
    for s, row in sorted(ratio.items()):
        ks = sorted(row)
        step = None
        for i, k in enumerate(ks):
            if all(row[j] > STEP for j in ks[i:]):
                step = k
                break
        # every chain shorter than the step must read as a hit
        if step is not None and any(row[j] > STEP for j in ks if j < step):
            out['notes'].append('stride %d: a miss below the step %s' % (s, row))
            out['verdict'] = 'CONTRADICTORY'
            return out
        if step is None and s != 64 and any(row[j] > STEP for j in ks):
            out['notes'].append('stride %d: misses that do not persist %s'
                                % (s, row))
            out['verdict'] = 'CONTRADICTORY'
            return out
        steps[s] = step
    out['steps'] = steps
    found = [s for s in sorted(steps) if steps[s] is not None]
    if not found:
        out['verdict'] = 'NO_GEOMETRY'
        out['notes'].append('no stride shows a miss step: no cache is being '
                            'modelled (an emulator), or the chains all fit')
        return out
    low = min(steps[s] for s in found)
    way = None
    for s in sorted(steps):
        if steps[s] == low and all(steps[t] == low for t in steps if t >= s):
            way = s
            break
    if way is None:
        out['verdict'] = 'CONTRADICTORY'
        out['notes'].append('the lowest step is not shared by every larger '
                            'stride: %s' % steps)
        return out
    ways = low - 1
    half = steps.get(way // 2)
    if way // 2 in steps and 2 * ways + 1 <= max(warm[way // 2]):
        if half != 2 * ways + 1:
            out['verdict'] = 'CONTRADICTORY'
            out['notes'].append('stride %d steps at %s, expected %d (2 x %d '
                                'ways + 1)' % (way // 2, half, 2 * ways + 1, ways))
            return out
        out['notes'].append('stride %d steps at %d = 2 x ways + 1: consistent'
                            % (way // 2, half))
    miss = statistics.median(warm[way][k] - ref[k] for k in warm[way]
                             if k >= low and k in ref)
    out.update(way_bytes=way, ways=ways, size_bytes=way * ways, miss_ns=miss)
    line = None
    for s in sorted(cold):
        if 2 * s in cold and cold[s] >= FLAT * cold[2 * s]:
            line = s
            break
    if line is None:
        out['notes'].append('line size not resolved by the cold rows %s' % cold)
    else:
        out['line_bytes'] = line
        out['sets'] = way // line
    out['verdict'] = 'GEOMETRY'
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--json')
    a = ap.parse_args()
    warm, cold = parse(a.log)
    r = analyse(warm, cold)
    if a.json:
        with open(a.json, 'w') as fh:
            json.dump(r, fh, indent=1, sort_keys=True)
    if r['verdict'] == 'GEOMETRY':
        print('I-cache: %d B = %d-way x %d B per way, %s B lines%s; hit %.1f ns, '
              'miss +%.1f ns per line' % (
                  r['size_bytes'], r['ways'], r['way_bytes'],
                  r.get('line_bytes', '?'),
                  ' (%d sets)' % r['sets'] if 'sets' in r else '',
                  r['hit_ns'], r['miss_ns']))
    else:
        print('I-cache: %s' % r['verdict'])
    for n in r['notes']:
        print('  ' + n)
    print('  steps (first K that misses, per stride): %s' % r.get('steps'))
    return {'GEOMETRY': 0, 'NO_GEOMETRY': 2}.get(r['verdict'], 3)


if __name__ == '__main__':
    sys.exit(main())
