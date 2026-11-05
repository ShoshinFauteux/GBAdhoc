#!/usr/bin/env python3
"""cap_map.py -- layouts for the I+D lpsim run, ME capture mode twin.

  cap_map.py --lp ~/lpcf --set S --fixture em_battle --twin b3cap \
     --frame-path F --elf name=path.nm[@jit_hex] ... [--lines name=decodedline.txt]

Differences from mk_layouts.py:
  * video.o is NOT excluded: the twin runs capture mode 1 (DR_MECAP), so what
    video.o executes on the twin is what the PSP CPU executes;
  * C++ names are matched on their unmangled base name (the twin's u32 is
    'unsigned int' (j), psp-gcc's is 'unsigned long' (m));
  * optional line-table placement for named functions whose code differs
    between the layouts (--line-fns, with --twin-lines and per-ELF line
    tables): a twin instruction goes to the PSP instructions of the same
    source line (by rank), instead of the same fraction of the function;
  * a data map per ELF (D=), every named data object by name.
"""
import argparse, bisect, collections, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp_map

DATA = set('dDbBrRvVsSgG')


def norm(name):
    m = re.match(r'^_ZL?(\d+)(.*)$', name)
    if m:
        n = int(m.group(1))
        rest = m.group(2)
        base = rest[:n]
        sfx = re.search(r'(\.(isra|part|constprop|cold|lto_priv)\.\d+)+$', rest)
        return 'C++:' + base + (sfx.group(0) if sfx else '')
    return name


class NSyms(lp_map.Syms):
    def __init__(self, rows):
        lp_map.Syms.__init__(self, rows)
        self.bynorm = collections.defaultdict(list)
        for r in rows:
            self.bynorm[norm(r[2])].append(r)

    def unique(self, name):
        u = lp_map.Syms.unique(self, name)
        if u:
            return u
        r = self.bynorm.get(norm(name), [])
        if len(r) == 1:
            return r[0]
        if len(r) == 2 and self.gaddrs and not name.startswith(lp_map.B_PREFIX):
            a = [x for x in r if not self._in_b(x)]
            if len(a) == 1:
                return a[0]
        return None


def read_data_nm(path):
    rows = []
    for l in open(path, errors='replace'):
        p = l.split()
        if len(p) == 4 and p[2] in DATA:
            rows.append(lp_map._Row(int(p[0], 16), int(p[1], 16), p[3], p[2].isupper()))
    rows.sort()
    return rows


def read_lines(path):
    """psp-objdump/objdump --dwarf=decodedline -> sorted [(addr, file, line)]."""
    out = []
    cur = None
    for l in open(path, errors='replace'):
        p = l.split()
        if len(p) >= 3 and re.match(r'^0x[0-9a-f]+$', p[2]) and p[1].isdigit():
            out.append((int(p[2], 16), os.path.basename(p[0]), int(p[1])))
    out.sort()
    return out


def line_index(lines):
    addrs = [a for a, _, _ in lines]
    return addrs, lines


def line_at(idx, a):
    addrs, lines = idx
    i = bisect.bisect_right(addrs, a) - 1
    return lines[i][1:] if i >= 0 else None


def place(tsyms, psyms, static, line_fns=(), tl=None, pl=None, xlate=None):
    out, pairs, lost = {}, {}, set()
    # line placement tables, per function
    lt = {}
    if line_fns and tl and pl:
        for fn in line_fns:
            t = tsyms.unique(fn); p = psyms.unique(fn)
            if not t or not p:
                continue
            # psp instructions of p, grouped by (file, line)
            grp = collections.defaultdict(list)
            for a in range(p[0], p[0] + p[1], 4):
                k = line_at(pl, a)
                if k:
                    grp[k].append(a)
            lt[t[2]] = (t, p, grp)
    tgrp = collections.defaultdict(list)
    for ad in sorted(static):
        t = tsyms.at(ad)
        if t and t[2] in lt:
            k = line_at(tl, ad)
            tgrp[(t[2], k)].append(ad)
    for ad in static:
        t = tsyms.at(ad)
        if not t:
            continue
        name = t[2]
        if lp_map.host_only(name):
            lost.add(name)
            continue
        if name not in pairs:
            pairs[name] = psyms.unique(name)
        p = pairs[name]
        if not p:
            lost.add(name)
            continue
        if name in lt:
            k = line_at(tl, ad)
            kk = (k[0], xlate(k[0], k[1])) if (k and xlate) else k
            cands = lt[name][2].get(kk)
            if cands:
                tl_list = tgrp[(name, k)]
                r = tl_list.index(ad)
                out[ad] = cands[r * len(cands) // len(tl_list)]
                continue
        off = (ad - t[0]) * p[1] // max(t[1], 4)
        out[ad] = p[0] + off - off % 4
    return out, pairs, lost


def dmap(tdata, pdata, window, jit=None):
    ps = NSyms(pdata)
    rows = []
    hit = miss = 0
    for a, s, n in tdata:
        if s == 0 or a < window[0] or a >= window[1]:
            continue
        p = ps.unique(n)
        if not p or p[1] == 0:
            miss += 1
            continue
        hit += 1
        rows.append((a, a + min(s, p[1]), p[0]))
    return rows, hit, miss


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lp', required=True)
    ap.add_argument('--set', required=True)
    ap.add_argument('--fixture', required=True)
    ap.add_argument('--twin', required=True)
    ap.add_argument('--prof', default='runs/prof')
    ap.add_argument('--elf', action='append', required=True)
    ap.add_argument('--frame-path')
    ap.add_argument('--twin-lines')
    ap.add_argument('--lines', action='append', default=[], help='name=decodedline.txt')
    ap.add_argument('--xlate', action='append', default=[],
                    help='name=file.map (b3 line -> this source line: "file b3line line")')
    ap.add_argument('--line-fns', default='')
    ap.add_argument('--nodata', action='store_true')
    ap.add_argument('--path', action='append', default=[])
    ap.add_argument('--hot-min', type=float, default=100.0)
    a = ap.parse_args()
    lp = os.path.expanduser(a.lp)
    out = os.path.join(lp, 'sim', a.set)
    os.makedirs(out, exist_ok=True)
    prof = os.path.join(lp, a.prof, a.fixture)
    jr = lp_map.jit_range(os.path.join(prof, 'run.info'))
    tnm = os.path.join(lp, 'bin', 'dr_host_%s.nm' % a.twin)
    tsyms = NSyms(lp_map.read_nm(tnm))
    tdata = read_data_nm(tnm)
    counts, frames_n = lp_map.read_counts(os.path.join(prof, 'counts.txt'))
    static = sorted(x for x in counts if not jr[0] <= x < jr[1])
    win = (min(r[0] for r in tdata if r[0] > 0x400000), max(r[0] + r[1] for r in tdata))
    win = (win[0], min(win[1], jr[0]))
    tl = line_index(read_lines(a.twin_lines)) if a.twin_lines else None
    lines = dict(x.split('=', 1) for x in a.lines)
    xl = {}
    for x in a.xlate:
        nm_, f = x.split('=', 1)
        d = {}
        for l in open(f):
            p = l.split()
            if len(p) == 3:
                d[(p[0], int(p[1]))] = int(p[2])
        xl[nm_] = d
    line_fns = [x for x in a.line_fns.split(',') if x]
    rows = []
    for spec in a.elf:
        name, rest = spec.split('=', 1)
        path, _, bases = rest.partition('@')
        psyms = NSyms(lp_map.read_nm(path))
        pl = line_index(read_lines(lines[name])) if name in lines else None
        xmap = xl.get(name)
        xlate = (lambda f, l, xmap=xmap: xmap.get((f, l), l)) if xmap is not None else None
        placed, pairs, lost = place(tsyms, psyms, static, line_fns, tl, pl, xlate)
        # --path name=fn:file: the PSP's own per-line path through fn (uspath.py):
        # the twin's per-line instructions of fn (executed >= hot_min times a
        # frame) go, by rank, onto the PSP path's instructions (>= 4 vcounts)
        for spec2 in a.path:
            nm2, rest2 = spec2.split('=', 1)
            if nm2 != name:
                continue
            fn, pf = rest2.split(':', 1)
            t = tsyms.unique(fn)
            psp_path = sorted(int(l.split()[0], 16) for l in open(pf)
                              if int(l.split()[1]) >= 4)
            tw = sorted(x for x in static if t[0] <= x < t[0] + t[1]
                        and counts[x] >= a.hot_min * frames_n)
            for i, x in enumerate(tw):
                placed[x] = psp_path[i * len(psp_path) // len(tw)]
            print('%s: %s path %d PSP insns <- %d twin insns' % (name, fn, len(psp_path), len(tw)), file=sys.stderr)
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
        dtok = ''
        if not a.nodata:
            drows, hit, miss = dmap(tdata, read_data_nm(path), win)
            dp = os.path.join(out, '%s.%s.dmap' % (a.fixture, name))
            with open(dp, 'w') as fh:
                fh.write('window %x %x\n' % win)
                for lo, hi, to in drows:
                    fh.write('%x %x %x\n' % (lo, hi, to))
            dtok = ' D=' + dp.replace(lp, '/w')
            print('%s: data %d objects placed, %d not' % (name, hit, miss), file=sys.stderr)
        jb = int(bases, 16) if bases else 0x09001500
        rows.append('%s %s %x %s%s' % (name, mp.replace(lp, '/w'), jb & ~63,
                                       fp.replace(lp, '/w') if fp != '-' else '-', dtok))
        lostw = collections.Counter()
        for ad in static:
            if ad not in placed:
                t = tsyms.at(ad)
                lostw[t[2] if t else '?'] += counts[ad]
        print('%s: %d twin instructions placed; lost top: %s' % (
            name, len(placed), ', '.join('%s=%d' % kv for kv in lostw.most_common(6))), file=sys.stderr)
    lay = os.path.join(out, '%s.lay' % a.fixture)
    with open(lay, 'w') as fh:
        fh.write('\n'.join(rows) + '\n')
    print(lay.replace(lp, '/w'))


if __name__ == '__main__':
    main()
