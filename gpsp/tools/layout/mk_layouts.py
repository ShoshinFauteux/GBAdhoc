#!/usr/bin/env python3
"""mk_layouts.py -- write one lpsim_plugin layouts file for a fixture: every
listed PSP ELF crossed with every listed translation-cache base
(docs/LAYOUT-PINNING.md).

  mk_layouts.py --lp ~/lp --set NAME --fixture aw2 --twin VARIANT
                --elf name=path/elf.nm[@jit_hex,jit_hex,...] ...
                [--sweep STEP] [--frame-path tools/layout/psp_frame_path.txt]
                [--prof runs/prof]

For each ELF: a static map (twin address -> PSP address, lp_map.py) built
from the fixture's profile run ($LP/runs/prof/FIXTURE: counts.txt, run.info),
a per-frame PSP path file, and one layout per JIT base.  The bases are the
listed ones (e.g. what the hardware log printed) plus, with --sweep STEP,
0x09000000 + k*STEP for k*STEP < 8 KiB: only base mod 8 KiB matters to a
16 KiB 2-way cache, so the sweep covers every placement the heap could give.
Writes $LP/sim/NAME/FIXTURE.lay (paths as the container sees them, /w = $LP).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp_map  # noqa: E402

WAY = 8192


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--lp', required=True)
    ap.add_argument('--set', required=True)
    ap.add_argument('--fixture', required=True)
    ap.add_argument('--twin', default='base')
    ap.add_argument('--prof', default='runs/prof',
                    help='profile runs under $LP (PROF/FIXTURE/counts.txt)')
    ap.add_argument('--elf', action='append', required=True)
    ap.add_argument('--sweep', type=int, default=0)
    ap.add_argument('--sweep-elfs', default='',
                    help='comma list: only these ELFs get the sweep (default all)')
    ap.add_argument('--frame-path')
    a = ap.parse_args()
    lp = os.path.expanduser(a.lp)
    out = os.path.join(lp, 'sim', a.set)
    os.makedirs(out, exist_ok=True)
    prof = os.path.join(lp, a.prof, a.fixture)
    jr = lp_map.jit_range(os.path.join(prof, 'run.info'))
    tnm = os.path.join(lp, 'bin', 'dr_host_%s.nm' % a.twin)
    tmem = os.path.join(lp, 'bin', 'dr_host_%s.members' % a.twin)
    tsyms = lp_map.Syms(lp_map.read_nm(tnm))
    excluded = lp_map.members_of(tmem, lp_map.RENDERER_OBJS)
    counts, _ = lp_map.read_counts(os.path.join(prof, 'counts.txt'))
    static = sorted(x for x in counts if not jr[0] <= x < jr[1])
    rows = []
    for spec in a.elf:
        name, rest = spec.split('=', 1)
        path, _, bases = rest.partition('@')
        psyms = lp_map.Syms(lp_map.read_nm(path))
        placed, pairs, lost = lp_map.place_static(tsyms, psyms, excluded, static)
        mp = os.path.join(out, '%s.%s.map' % (a.fixture, name))
        with open(mp, 'w') as fh:
            for t in sorted(placed):
                fh.write('%x %x\n' % (t, placed[t]))
        fp = '-'
        if a.frame_path:
            fp = os.path.join(out, '%s.%s.frame' % (a.fixture, name))
            with open(fp, 'w') as fh:
                for x in lp_map.frame_lines(psyms, lp_map.read_frame_path(a.frame_path)):
                    fh.write('%x\n' % x)
        jbs = [int(b, 16) for b in bases.split(',') if b]
        tags = ['hw'] * len(jbs)
        sweep_ok = not a.sweep_elfs or name in a.sweep_elfs.split(',')
        if a.sweep and sweep_ok:
            for k in range(0, WAY, a.sweep):
                jbs.append(0x09000000 + k)
                tags.append('%04x' % k)
        for jb, tag in zip(jbs, tags):
            rows.append('%s@%s %s %x %s' % (
                name, tag if tag != 'hw' else 'hw%04x' % (jb % WAY),
                mp.replace(lp, '/w'), jb & ~63,
                fp.replace(lp, '/w') if fp != '-' else '-'))
        print('%s: %d twin instructions placed, %d functions not placed'
              % (name, len(placed), len(lost)), file=sys.stderr)
    lay = os.path.join(out, '%s.lay' % a.fixture)
    with open(lay, 'w') as fh:
        fh.write('\n'.join(rows) + '\n')
    print(lay.replace(lp, '/w'))
    if len(rows) > 160:
        sys.exit('too many layouts (%d > 160, lpsim_plugin MAXL)' % len(rows))


if __name__ == '__main__':
    main()
