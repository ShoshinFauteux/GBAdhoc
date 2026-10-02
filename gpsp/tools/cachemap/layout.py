#!/usr/bin/env python3
"""layout.py -- candidate code layouts for the two GB cores, as relocations
the cache simulator can try before anything is linked (docs/CACHE-MAP.md).

  layout.py --elf-nm elf.nm --lines lines.txt --out DIR

lines.txt is cachemap.py --lines-out (per-line heat).  Writes DIR/<name>.reloc
("old_hex new_hex" per moved function) for:
  shiftB<k>     copy B moved k cache lines (k*64 B) relative to where it is
  hotpack<k>    each copy's functions reordered hottest-density first, in an
                imaginary region past the end of .text; copy B's region
                starts k sets after copy A's (mod the way size), so their
                hottest code lands on different sets
  color<a>_<p>  CACHE COLOURING: each copy's hottest functions (covering p %
                of its executions, hot-density first) packed into its own
                colour -- copy A into sets [0, a) of every way-sized window,
                copy B into sets [a, nsets) -- so the two cores' hottest code
                can never evict each other; the rest of each copy follows,
                uncoloured.  a = sets for A (--colors a1,a2,...; nsets/2 is an
                even split), p = --color-cover (default 95).
  soloA / soloB are not relocations: cachemap.py --only A|B writes those maps.
The same order, as a linker section order, comes from --emit-order.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cachemap import read_nm, copies  # noqa: E402


def color_layout(order, split, cover, nsets, line, way, base):
    """(old, new) per function: the hottest functions of copy A packed into
    sets [0, split) of successive way-sized windows, copy B's into
    [split, nsets); both copies' remaining functions after them, packed
    with no colour.  A function that does not fit the room left in its
    colour starts at the next window; one larger than its colour runs on
    (its tail is uncoloured -- the report counts those)."""
    out = []
    ranges = {'A': (0, split * line), 'B': (split * line, nsets * line)}
    win = {'A': 0, 'B': 0}
    cur = {'A': base, 'B': base + ranges['B'][0]}
    rest = []
    for c in ('A', 'B'):
        tot = sum(f[1] for f in order[c])
        acc = 0.0
        lo, hi = ranges[c]
        for dens, h, ad, sz, n in order[c]:
            if acc >= cover * tot or hi <= lo:
                rest.append((c, ad, sz))
                continue
            acc += h
            at = (cur[c] + 7) & ~7
            off = (at - base) % way
            if off < lo or off + min(sz, hi - lo) > hi:
                win[c] += 1
                at = base + win[c] * way + lo
            else:
                win[c] = (at - base) // way
            out.append((ad, at))
            cur[c] = at + sz
            win[c] = (cur[c] - base) // way
    end = max([base] + [n + 0 for _o, n in out]) + 0x10000
    at = (end + 0xFFFF) & ~0xFFFF
    for c, ad, sz in rest:
        at = (at + 7) & ~7
        out.append((ad, at))
        at += sz
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf-nm', required=True)
    ap.add_argument('--lines', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--line', type=int, default=64)
    ap.add_argument('--way', type=int, default=8192, help='bytes per way')
    ap.add_argument('--shifts', default='16,32,48,64,96')
    ap.add_argument('--packs', default='0,32,64')
    ap.add_argument('--colors', default='',
                    help='colour splits: sets given to copy A, e.g. 64,80,96')
    ap.add_argument('--color-cover', default='95',
                    help='percent of each copy\'s executions coloured '
                         '(comma list allowed)')
    ap.add_argument('--emit-order', help='write the hotpack order (function '
                    'names, hottest first) for the linker')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    nm = read_nm(a.elf_nm)
    cp, delta = copies(nm)
    heat = collections.Counter()
    for l in open(a.lines):
        p = l.split()
        heat[int(p[0], 16)] = float(p[2])
    funcs = {'A': [], 'B': []}
    for ad, sz, n in nm:
        c = cp.get(ad)
        if c in funcs and sz:
            h = sum(heat.get(x, 0.0) for x in range(ad - ad % a.line, ad + sz, a.line))
            funcs[c].append((h / sz, h, ad, sz, n))
    for k in [int(x) for x in a.shifts.split(',') if x]:
        with open(os.path.join(a.out, 'shiftB%d.reloc' % k), 'w') as fh:
            for _, _, ad, sz, n in funcs['B']:
                fh.write('%x %x\n' % (ad, ad + k * a.line))
    end = max(ad + sz for ad, sz, n in nm)
    base = (end + 0xFFFF) & ~0xFFFF
    order = {}
    for c in funcs:
        order[c] = sorted(funcs[c], key=lambda f: -f[0])
    for k in [int(x) for x in a.packs.split(',') if x]:
        with open(os.path.join(a.out, 'hotpack%d.reloc' % k), 'w') as fh:
            for c, start in (('A', base), ('B', base + 0x100000 + k * a.line)):
                at = start
                for dens, h, ad, sz, n in order[c]:
                    at = (at + 7) & ~7
                    fh.write('%x %x\n' % (ad, at))
                    at += sz
    nsets = a.way // a.line
    for split in [int(x) for x in a.colors.split(',') if x]:
        for cover in [float(x) for x in a.color_cover.split(',') if x]:
            name = 'color%d_%g' % (split, cover)
            relocs = color_layout(order, split, cover / 100.0, nsets, a.line,
                                  a.way, base)
            with open(os.path.join(a.out, name + '.reloc'), 'w') as fh:
                for old, new in relocs:
                    fh.write('%x %x\n' % (old, new))
    if a.emit_order:
        with open(a.emit_order, 'w') as fh:
            for dens, h, ad, sz, n in order['A']:
                fh.write('%s %.1f %d\n' % (n, h, sz))
    tot = {c: sum(f[1] for f in funcs[c]) for c in funcs}
    for c in funcs:
        acc, kb = 0.0, 0
        for dens, h, ad, sz, n in order[c]:
            acc += h
            kb += sz
            if acc >= 0.9 * tot[c]:
                break
        print('copy %s: %d functions, 90%% of executions in the first %.1f KiB '
              'of the hot-first order' % (c, len(funcs[c]), kb / 1024.0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
