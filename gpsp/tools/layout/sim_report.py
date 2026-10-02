#!/usr/bin/env python3
"""sim_report.py -- read lpsim_plugin results (docs/LAYOUT-PINNING.md).

  sim_report.py RUN_DIR [RUN_DIR...] [--pick] [--detail ELF]

Per run (one fixture) and per ELF: I-cache misses per frame and their cost in
ms per frame at the measured 215.3 ns a miss (docs/CACHE-MAP.md), over the
sweep of translation-cache bases (min / median / max: the spread an unrelated
change can move a build across), and the named bases (hw*: what a console's
log printed).  --pick: the (ELF, base offset) with the lowest mean of
per-fixture misses normalised to each fixture's median -- how JIT_PIN_OFFSET
and the hot budget were chosen.
"""
import argparse
import collections
import os
import statistics

MISS_NS = 215.3


def read(path):
    frames = 1
    rows = {}
    for l in open(path):
        p = l.split()
        if p[0] == 'frames':
            frames = max(int(p[1]), 1)
        elif p[0] == 'layout':
            name = p[1]
            miss = int(p[5])
            groups = {}
            for g in p[6:]:
                k, v = g.split('=')
                m, a = v.split('/')
                groups[k] = (int(m), int(a))
            rows[name] = (miss, groups)
    return frames, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--pick', action='store_true')
    ap.add_argument('--detail')
    a = ap.parse_args()
    norm = collections.defaultdict(list)
    for r in a.runs:
        frames, rows = read(os.path.join(r, 'sim.txt'))
        fx = os.path.basename(r.rstrip('/'))
        by = collections.defaultdict(dict)
        for name, (miss, g) in rows.items():
            elf, tag = name.split('@')
            by[elf][tag] = (miss / frames, g)
        allmed = statistics.median(v[0] for e in by.values() for v in e.values())
        print('== %s (%d frames)   misses/frame [ms/frame]; J translated, S static, F PSP-only'
              % (fx, frames))
        print('   %-8s %22s %22s %22s   %s' % ('elf', 'sweep min', 'median', 'max', 'named bases'))
        for elf, tags in by.items():
            sw = sorted(v[0] for t, v in tags.items() if not t.startswith('hw'))
            named = ['%s %.0f [%.2f]' % (t, v[0], v[0] * MISS_NS / 1e6)
                     for t, v in sorted(tags.items()) if t.startswith('hw')]
            if sw:
                print('   %-8s %13.0f [%5.2f] %13.0f [%5.2f] %13.0f [%5.2f]   %s' % (
                    elf, sw[0], sw[0] * MISS_NS / 1e6,
                    statistics.median(sw), statistics.median(sw) * MISS_NS / 1e6,
                    sw[-1], sw[-1] * MISS_NS / 1e6, '  '.join(named)))
            else:
                print('   %-8s %s' % (elf, '  '.join(named)))
            for t, v in tags.items():
                norm[(elf, t)].append(v[0] / allmed)
            if a.detail == elf:
                for t, (m, g) in sorted(tags.items()):
                    print('      %s %8.0f  ' % (t, m) + '  '.join(
                        '%s=%.0f/%.0f' % (k, mm / frames, aa / frames)
                        for k, (mm, aa) in g.items()))
    if a.pick:
        n = len(a.runs)
        cand = [(sum(v) / n, k) for k, v in norm.items() if len(v) == n]
        cand.sort()
        print('== best (elf, JIT base offset) by mean normalised misses over %d fixtures:' % n)
        for s, (elf, t) in cand[:12]:
            print('   %-8s %s  %.4f' % (elf, t, s))
        per = collections.defaultdict(list)
        for s, (elf, t) in cand:
            per[elf].append(s)
        print('== per elf over the sweep: min / median / max of the normalised mean')
        for elf, v in per.items():
            v.sort()
            print('   %-8s %.4f / %.4f / %.4f' % (elf, v[0], statistics.median(v), v[-1]))


if __name__ == '__main__':
    main()
