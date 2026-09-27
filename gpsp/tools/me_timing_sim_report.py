#!/usr/bin/env python3
"""Summarise an ME_TIMING_SIM log (see tools/e2e/run_me_timing_sim.sh).

For each simulated engine -- "post" (render posted at the end of retro_run)
and "vis" (posted at vcount 160) -- classify every frame N by which CPU frame
its image equals: N itself, a neighbour within +-8, or none ("unmatched").
"unmatched" is the figure docs/ME-RENDERER-DIVERGENCE.md measured on hardware.

    me_timing_sim_report.py MTS_LOG [FIRST LAST]
"""
import collections
import re
import sys

LINE = re.compile(r'^MTS f=(\d+) cpu=(\w+) post=(\w+) vis=(\w+)', re.M)


def spans(frames):
    out = []
    for f in frames:
        if out and f == out[-1][1] + 1:
            out[-1][1] = f
        else:
            out.append([f, f])
    return ' '.join(str(a) if a == b else '%d-%d' % (a, b) for a, b in out)


def main():
    log = sys.argv[1]
    lo, hi = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (0, 1 << 30)
    cpu, arms = {}, {'post': {}, 'vis': {}}
    with open(log, errors='replace') as fh:
        for m in LINE.finditer(fh.read()):
            f = int(m.group(1))
            cpu[f] = m.group(2)
            arms['post'][f], arms['vis'][f] = m.group(3), m.group(4)
    if not cpu:
        sys.exit('no MTS lines in %s -- was video.o built with -DME_TIMING_SIM=1?' % log)
    for name, arm in arms.items():
        count, bad = collections.Counter(), []
        for f in sorted(arm):
            if not lo <= f <= hi:
                continue
            if arm[f] == cpu[f]:
                count['exact'] += 1
                continue
            for d in (1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8, -8):
                if cpu.get(f + d) == arm[f]:
                    count['cpu%+d' % d] += 1
                    break
            else:
                count['unmatched'] += 1
                bad.append(f)
        print('%-4s %s' % (name, ' '.join('%s=%d' % kv for kv in sorted(count.items()))))
        if bad:
            print('     unmatched: %s' % spans(bad))


if __name__ == '__main__':
    main()
