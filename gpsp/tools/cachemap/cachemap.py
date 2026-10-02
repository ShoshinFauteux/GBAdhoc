#!/usr/bin/env python3
"""cachemap.py -- which code shares which instruction-cache sets, and how hot
it is (docs/CACHE-MAP.md).

Inputs (all text; tools/cachemap/cachemap.sh makes them with docker):
  --elf-nm / --elf-lines      psp-nm -n -S and psp-objdump --dwarf=decodedline
                              of the linked PSP ELF (psp/Makefile builds -g;
                              the EBOOT is packed stripped, so -g never changes
                              the shipped bytes)
  --twin-nm / --twin-lines    the same for the qemu twin (twin_profile.sh)
  --icount                    icount_plugin.so output: executed addresses of
                              the twin with their counts
  --frames N                  profiled frames (to report per-frame numbers)
  --frame-path FILE           PSP-only per-frame code the twin cannot run
                              (frontend loop, video, audio, netdrv): lines
                              "symbol executions_per_frame [group]"; every
                              instruction of the symbol gets the rate

How the twin's counts reach the PSP binary.  The twin is the same C compiled
for mipsel-linux; its instruction addresses mean nothing on the PSP, but its
SOURCE LINES do.  Each executed twin address is charged to (copy, file, line)
through the twin's DWARF line table; each (copy, file, line) is spread over
the PSP instructions the PSP line table gives that line, at the twin's mean
executions per instruction of the line.  Addresses without a line (libc) fall
back to symbol names (memcpy -> memcpy).  "copy" is which of the two
instances: a function belongs to copy A when the same name exists exactly
DELTA bytes further on (DELTA = gbcoreb_power_on - gbcore_power_on), and to
copy B at that distance back.

Cache: --size 16384 --ways 2 --line 64 (Allegrex I-cache; see the doc).
Output: groups' footprints, per-set occupancy, the conflict score, and with
--check a non-zero exit when the score exceeds --max-score.
"""
import argparse
import bisect
import collections
import json
import os
import re
import sys

ROW = re.compile(r'^(\S+)\s+(\d+|-)\s+(0x[0-9a-f]+)')


def read_nm(path):
    """[(addr, size, name)] for text symbols, sorted."""
    out = []
    for l in open(path, errors='replace'):
        p = l.split()
        if len(p) == 4 and p[2] in 'tTwW':
            out.append((int(p[0], 16), int(p[1], 16), p[3]))
    out.sort()
    return out


def read_lines(path):
    """sorted [(addr, file, line)] from objdump --dwarf=decodedline."""
    rows = []
    for l in open(path, errors='replace'):
        m = ROW.match(l)
        if not m or m.group(2) == '-':
            continue
        rows.append((int(m.group(3), 16), os.path.basename(m.group(1)),
                     int(m.group(2))))
    rows.sort()
    # keep the LAST row at an address (the one that owns the code after it)
    dedup = {}
    for a, f, n in rows:
        dedup[a] = (f, n)
    return sorted((a, f, n) for a, (f, n) in dedup.items())


def copies(nm):
    """addr -> copy for functions of the two GB core instances."""
    by = {n: a for a, s, n in nm}
    if 'gbcore_power_on' not in by or 'gbcoreb_power_on' not in by:
        sys.exit('no gbcore_power_on / gbcoreb_power_on: not a two-copy binary')
    delta = by['gbcoreb_power_on'] - by['gbcore_power_on']
    at = {}
    for a, s, n in nm:
        at.setdefault(a, set()).add(n.replace('gbcoreb_', 'gbcore_'))
    cp = {}
    for a, s, n in nm:
        k = n.replace('gbcoreb_', 'gbcore_')
        if k in at.get(a + delta, ()):
            cp[a] = 'A'
        elif k in at.get(a - delta, ()):
            cp[a] = 'B'
    return cp, delta


class Sym:
    def __init__(self, nm):
        self.nm = nm
        self.addrs = [a for a, s, n in nm]

    def at(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return None
        a, s, n = self.nm[i]
        return (a, s, n) if addr < a + max(s, 4) else None


def line_ranges(rows, sym):
    """(copy-agnostic) {(file, line): [(start, end), ...]} over function
    bodies; a row's range ends at the next row or the function's end."""
    out = collections.defaultdict(list)
    for i, (a, f, n) in enumerate(rows):
        s = sym.at(a)
        if not s:
            continue
        end = rows[i + 1][0] if i + 1 < len(rows) else a + 4
        end = min(end, s[0] + s[1])
        if end > a:
            out[(f, n)].append((a, end))
    return out


def insns(ranges):
    out = []
    for s, e in sorted(ranges):
        out.extend(range(s, e, 4))
    return out


def emit_map(a, tnm, tsym, tcp, trows, tranges, pnm, psym, pcp, pranges, pby):
    """twin instruction -> PSP instruction, through (copy, file, line); the
    i-th twin instruction of a line goes to the proportional PSP one."""
    reloc = {}
    if a.relocate:
        for l in open(a.relocate):
            p = l.split()
            if len(p) >= 2:
                reloc[int(p[0], 16)] = int(p[1], 16)

    def place(addr):
        s = psym.at(addr)
        if s and s[0] in reloc:
            return reloc[s[0]] + (addr - s[0])
        return addr

    def gl(addr):
        s = psym.at(addr)
        c = pcp.get(s[0]) if s else None
        return c or ('S' if s else 'O')

    taddr = [r[0] for r in trows]
    executed = []
    for l in open(a.icount):
        if not l.startswith('#'):
            executed.append(int(l.split()[0], 16))
    keyof = {}
    for ad in executed:
        s = tsym.at(ad)
        i = bisect.bisect_right(taddr, ad) - 1
        if s and i >= 0 and trows[i][0] >= s[0]:
            keyof[ad] = (tcp.get(s[0], 'S'), trows[i][1], trows[i][2])
        elif s:
            keyof[ad] = ('SYM', s[2], s[0])
    tl = {}
    pl = {}
    n = 0
    with open(a.emit_map, 'w') as fh:
        for ad in executed:
            k = keyof.get(ad)
            if not k:
                continue
            if k[0] == 'SYM':
                loc = pby.get(k[1])
                if not loc or len(loc) != 1:
                    continue
                ts = tsym.at(ad)
                p0, psz = loc[0]
                off = (ad - k[2]) * psz // max(ts[1], 4)
                pa = p0 + off - off % 4
            else:
                cp, f, ln = k
                if k not in tl:
                    tl[k] = insns(tranges.get((f, ln), []))
                    rng = [(s0, e0) for s0, e0 in pranges.get((f, ln), [])
                           if (pcp.get((psym.at(s0) or (0,))[0]) or 'S') ==
                           (cp if cp in ('A', 'B') else 'S')]
                    pl[k] = insns(rng)
                T, P = tl[k], pl[k]
                if not P or not T:
                    continue
                i = bisect.bisect_left(T, ad)
                pa = P[min(len(P) - 1, i * len(P) // len(T))]
            if a.only and gl(pa) != a.only:
                continue
            fh.write('%x %x %s' % (ad, place(pa), gl(pa)) + chr(10))
            n += 1
    print('map: %d twin instructions placed (%s)' % (n, a.emit_map))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--elf-nm', required=True)
    ap.add_argument('--elf-lines', required=True)
    ap.add_argument('--twin-nm')
    ap.add_argument('--twin-lines')
    ap.add_argument('--icount')
    ap.add_argument('--frames', type=float, default=1.0)
    ap.add_argument('--frame-path')
    ap.add_argument('--size', type=int, default=16384)
    ap.add_argument('--ways', type=int, default=2)
    ap.add_argument('--line', type=int, default=64)
    ap.add_argument('--hot', type=float, default=0.99,
                    help='a group\'s hot set = its lines covering this share '
                         'of its executions (default 0.99)')
    ap.add_argument('--json')
    ap.add_argument('--csv', help='per-set table')
    ap.add_argument('--lines-out', help='per-line heat: addr group heat symbol')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--max-score', type=float, default=0.0)
    ap.add_argument('--label', default='')
    ap.add_argument('--emit-map', help='write "twin_addr psp_addr group" for '
                    'every executed twin instruction (icsim_plugin map=)')
    ap.add_argument('--only', help='emit only this group (A or B): the '
                    'other core then costs the cache nothing -- SOLO-like')
    ap.add_argument('--relocate', help='a layout to simulate before linking '
                    'it: lines "old_hex new_hex" moving PSP functions '
                    '(layout.py writes these)')
    a = ap.parse_args()

    L = a.line
    nsets = a.size // (a.ways * L)
    pnm = read_nm(a.elf_nm)
    psym = Sym(pnm)
    pcp, pdelta = copies(pnm)
    prows = read_lines(a.elf_lines)
    pranges = line_ranges(prows, psym)
    pby = collections.defaultdict(list)
    for ad, s, n in pnm:
        pby[n].append((ad, s))

    heat = collections.Counter()        # (psp line addr) -> executions
    group_of = {}                        # psp line addr -> group

    def group(addr):
        s = psym.at(addr)
        if not s:
            return 'other'
        c = pcp.get(s[0])
        return c or 'shared'

    def charge(start, end, per_insn):
        for ad in range(start, end, 4):
            la = ad - ad % L
            heat[la] += per_insn
            group_of.setdefault(la, group(ad))

    unmapped = 0.0
    total = 0.0
    if a.icount:
        tnm = read_nm(a.twin_nm)
        tsym = Sym(tnm)
        tcp, _ = copies(tnm)
        trows = read_lines(a.twin_lines)
        taddr = [r[0] for r in trows]
        tranges = line_ranges(trows, tsym)
        tbytes = {k: sum(e - s for s, e in v) for k, v in tranges.items()}
        per_key = collections.Counter()      # (copy, file, line) -> execs
        per_sym = collections.Counter()      # (name) -> execs (no line)
        for l in open(a.icount):
            if l.startswith('#'):
                continue
            h, c = l.split()
            ad, c = int(h, 16), int(c)
            total += c
            s = tsym.at(ad)
            i = bisect.bisect_right(taddr, ad) - 1
            if s and i >= 0 and trows[i][0] >= s[0]:
                cp = tcp.get(s[0], 'shared')
                per_key[(cp, trows[i][1], trows[i][2])] += c
            elif s:
                per_sym[s[2]] += c
            else:
                unmapped += c
        # spread onto the PSP binary
        for (cp, f, n), c in per_key.items():
            tb = tbytes.get((f, n), 0)
            rng = pranges.get((f, n), [])
            if cp in ('A', 'B'):
                rng = [(s, e) for s, e in rng if pcp.get((psym.at(s) or (0,))[0]) == cp]
            else:
                rng = [(s, e) for s, e in rng if pcp.get((psym.at(s) or (0,))[0]) is None]
            if not rng or not tb:
                per_sym['?%s:%d' % (f, n)] += c
                continue
            per = c / (tb / 4.0)        # executions per twin instruction
            for s, e in rng:
                charge(s, e, per)
        for name, c in per_sym.items():
            loc = pby.get(name)
            if not loc or len(loc) != 1:
                unmapped += c
                continue
            s0, sz = loc[0]
            if sz:
                charge(s0, s0 + sz, c / (sz / 4.0))
    if a.emit_map:
        emit_map(a, tnm, tsym, tcp, trows, tranges, pnm, psym, pcp, pranges, pby)
    fp_rate = {}
    if a.frame_path:
        for l in open(a.frame_path):
            l = l.split('#')[0].split()
            if len(l) < 2:
                continue
            name, rate = l[0], float(l[1])
            g = l[2] if len(l) > 2 else 'FE'
            for s0, sz in pby.get(name, []):
                for ad in range(s0, s0 + sz, 4):
                    la = ad - ad % L
                    heat[la] += rate * a.frames
                    group_of[la] = g
                fp_rate[name] = rate

    # ---- per group hot sets --------------------------------------------
    groups = collections.defaultdict(list)
    for la, h in heat.items():
        groups[group_of[la]].append((h, la))
    hot = {}
    summary = {}
    for g, v in groups.items():
        v.sort(reverse=True)
        tot = sum(h for h, _ in v)
        acc, keep = 0.0, []
        for h, la in v:
            if acc >= a.hot * tot:
                break
            acc += h
            keep.append(la)
        hot[g] = set(keep)
        summary[g] = {'lines_executed': len(v), 'hot_lines': len(keep),
                      'hot_bytes': len(keep) * L,
                      'execs_per_frame': tot / a.frames}
    # ---- sets --------------------------------------------------------------
    occ = [collections.Counter() for _ in range(nsets)]
    sheat = [collections.Counter() for _ in range(nsets)]
    for g, s in hot.items():
        for la in s:
            k = (la // L) % nsets
            occ[k][g] += 1
            sheat[k][g] += heat[la]
    # Conflict score: in each set, the lines beyond the ways that must be
    # refetched; the cost of a set is the heat of its (n - ways) COOLEST hot
    # lines -- the ones LRU keeps evicting -- per frame.
    score = 0.0
    bad = []
    for k in range(nsets):
        n = sum(occ[k].values())
        if n > a.ways:
            lines = sorted(heat[la] for g, s in hot.items() for la in s
                           if (la // L) % nsets == k)
            cost = sum(lines[:n - a.ways]) / a.frames
            score += cost
            bad.append((cost, k, dict(occ[k])))
    bad.sort(reverse=True)

    # ---- report ------------------------------------------------------------
    print('cachemap %s: %d B %d-way %d B lines = %d sets; hot = %.1f%% of each '
          'group\'s executions' % (a.label, a.size, a.ways, L, nsets, a.hot * 100))
    if a.icount:
        print('twin: %.0f instructions profiled, %.3f%% unmapped, %d frames'
              % (total, 100 * unmapped / max(total, 1), a.frames))
    print('%-8s %10s %10s %10s %14s' % ('group', 'lines', 'hot lines',
                                        'hot KiB', 'execs/frame'))
    for g in sorted(summary):
        s = summary[g]
        print('%-8s %10d %10d %10.1f %14.0f' % (g, s['lines_executed'],
              s['hot_lines'], s['hot_bytes'] / 1024, s['execs_per_frame']))
    tot_hot = sum(len(s) for s in hot.values())
    print('hot lines in all groups: %d of %d the cache holds (%.0f%%)'
          % (tot_hot, nsets * a.ways, 100.0 * tot_hot / (nsets * a.ways)))
    print('sets over %d ways: %d of %d; conflict score %.0f refetch-weighted '
          'executions/frame' % (a.ways, len(bad), nsets, score))
    for cost, k, o in bad[:12]:
        print('  set %3d  %s  cost %.0f' % (k, ' '.join('%s=%d' % x for x in
                                                       sorted(o.items())), cost))
    if a.csv:
        with open(a.csv, 'w') as fh:
            gs = sorted(hot)
            fh.write('set,' + ','.join(gs) + ',' +
                     ','.join('heat_' + g for g in gs) + '\n')
            for k in range(nsets):
                fh.write('%d,%s,%s\n' % (k, ','.join(str(occ[k][g]) for g in gs),
                                         ','.join('%.0f' % (sheat[k][g] / a.frames)
                                                  for g in gs)))
    if a.lines_out:
        with open(a.lines_out, 'w') as fh:
            for la in sorted(heat):
                s = psym.at(la) or (0, 0, '?')
                fh.write('%08x %s %.1f %s%s\n' % (la, group_of[la],
                         heat[la] / a.frames, s[2],
                         ' HOT' if la in hot.get(group_of[la], ()) else ''))
    if a.json:
        with open(a.json, 'w') as fh:
            json.dump({'geometry': [a.size, a.ways, L], 'summary': summary,
                       'score': score, 'sets_over': len(bad),
                       'delta_copies': pdelta, 'frame_path': fp_rate}, fh,
                      indent=1)
    if a.check and score > a.max_score:
        print('CACHEMAP WARNING: conflict score %.0f > %.0f -- hot code '
              'shares cache sets again (docs/CACHE-MAP.md)' % (score, a.max_score))
        return 3
    return 0


if __name__ == '__main__':
    sys.exit(main())
