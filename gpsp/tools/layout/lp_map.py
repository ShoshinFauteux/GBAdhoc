#!/usr/bin/env python3
"""lp_map.py -- place the dynarec twin's execution on a PSP binary
(docs/LAYOUT-PINNING.md).

The twin (tools/drprof, tools/layout/twin_build.sh) is the PSP's core built
for mipsel-linux; lpsim_plugin.c replays its instruction fetches through the
Allegrex I-cache, but only after every twin instruction is given the address
the SAME instruction has in a PSP layout.  This module does that placement,
and turns the twin's execution counts into per-PSP-function weights for the
order generator (gen_layout.py).

Placement is by FUNCTION: a twin instruction at offset o of function f lands
at the same fraction of f in the PSP binary (psp_start + o * psp_size /
twin_size).  Names, not line tables, because the twin carries the drprof hooks
(line numbers shift) and the two compilers (gcc 12 / psp-gcc 15) agree on
which functions exist far better than on which lines.  A twin function with no
unique PSP namesake is not placed (reported as lost weight): the host harness,
glibc internals, and the RENDERER (video.o), which runs on the Media Engine on
a PSP.  What the PSP's CPU still does in video.cc per line (update_scanline's
capture head) comes in through the per-frame PSP path instead.

Subcommands:
  map     --twin-nm --twin-members --counts C [C...] --psp-nm N --out MAP
  frame   --psp-nm N --frame-path F --out FRAMEFILE
  weights --twin-nm --twin-members --counts C [C...] --psp-nm N [--frame-path]
          (prints per-function weights; gen_layout.py imports weights())
"""
import argparse
import bisect
import collections
import sys

TEXT = set('tTwW')


def read_nm(path):
    """[(addr, size, name)] of text symbols, sorted; a zero/absent size (asm
    labels) becomes the distance to the next text symbol."""
    rows = []
    for l in open(path, errors='replace'):
        p = l.split()
        if len(p) == 4 and p[2] in TEXT:
            rows.append([int(p[0], 16), int(p[1], 16), p[3]])
        elif len(p) == 3 and p[1] in TEXT:
            rows.append([int(p[0], 16), 0, p[2]])
    rows.sort()
    out = []
    for i, (a, s, n) in enumerate(rows):
        if s == 0:
            nxt = next((r[0] for r in rows[i + 1:] if r[0] > a), a + 4)
            s = min(nxt - a, 0x10000)
        out.append((a, s, n))
    # drop exact duplicates at one address (aliases): keep the first name
    seen = set()
    ded = []
    for a, s, n in out:
        if (a, n) not in seen:
            seen.add((a, n))
            ded.append((a, s, n))
    return ded


def base_name(n):
    return n.split('.', 1)[0]


class Syms:
    def __init__(self, rows):
        self.rows = rows
        self.addrs = [r[0] for r in rows]
        self.by = collections.defaultdict(list)
        self.bybase = collections.defaultdict(list)
        for r in rows:
            self.by[r[2]].append(r)
            self.bybase[base_name(r[2])].append(r)

    def at(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        while i >= 0:
            a, s, n = self.rows[i]
            if addr < a + s:
                return self.rows[i]
            if a < addr - 0x10000:
                return None
            i -= 1
        return None

    def unique(self, name):
        r = self.by.get(name, [])
        if len(r) == 1:
            return r[0]
        if not r:
            b = self.bybase.get(base_name(name), [])
            if len(b) == 1:
                return b[0]
        return None


def members_of(path, obj_suffixes):
    """names defined in archive members whose name ends with one of
    obj_suffixes (nm -A output: lib.a:member.o:addr T name)."""
    names = set()
    if not path:
        return names
    for l in open(path, errors='replace'):
        p = l.split()
        if len(p) >= 3:
            mem = p[0].split(':')
            if len(mem) >= 2 and any(mem[1].startswith(s) or mem[1] == s
                                     for s in obj_suffixes):
                names.add(p[-1])
    return names


RENDERER_OBJS = ('video.o', 'gba_cc_lut.o')
# The twin's own harness (tools/drprof/dr_host.c: main, the per-frame state
# hash, the drprof markers) has PSP namesakes it must not be charged to.
HOST_ONLY = ('main', 'fe_crc32')  # fe_crc32: the twin's per-frame state hash
HOST_PREFIXES = ('drprof_', 'dr_')


def host_only(name):
    return name in HOST_ONLY or name.startswith(HOST_PREFIXES)


def read_counts(path):
    c = {}
    frames = None
    for l in open(path):
        if l.startswith('# frames'):
            frames = int(l.split()[2])
            continue
        p = l.split()
        if len(p) == 2:
            c[int(p[0], 16)] = int(p[1])
    return c, frames


def jit_range(info_path):
    for l in open(info_path):
        for tok in l.split():
            if tok.startswith('jit='):
                lo, hi = tok[4:].split(':')
                return int(lo, 16), int(hi, 16)
    return None


def place_static(tsyms, psyms, excluded, addrs):
    """{twin_addr: psp_addr} for twin addresses inside placeable functions,
    plus {twin_fn_name: psp_row} and the lost names."""
    out = {}
    pairs = {}
    lost = set()
    for ad in addrs:
        t = tsyms.at(ad)
        if not t:
            continue
        name = t[2]
        if name in excluded or host_only(name):
            lost.add(name)
            continue
        if name not in pairs:
            pairs[name] = psyms.unique(name)
        p = pairs[name]
        if not p:
            lost.add(name)
            continue
        off = (ad - t[0]) * p[1] // max(t[1], 4)
        out[ad] = p[0] + off - off % 4
    return out, pairs, lost


def read_frame_path(path):
    rows = []
    for l in open(path):
        l = l.split('#', 1)[0].split()
        if len(l) >= 2:
            rows.append((l[0], float(l[1]), int(l[2]) if len(l) > 2 else None,
                         l[3] if len(l) > 3 else 'FE'))
    return rows


def frame_lines(psyms, frame_rows):
    """PSP line addresses fetched per emulated frame by the PSP-only path."""
    out = []
    for name, calls, nbytes, grp in frame_rows:
        r = psyms.unique(name)
        if not r:
            continue
        a, s, n = r
        span = s if nbytes is None else min(s, nbytes)
        lines = list(range(a & ~63, a + span, 64))
        whole = int(calls)
        for _ in range(whole):
            out.extend(lines)
        if calls - whole >= 0.5:
            out.extend(lines)
    return out


def weights(twin_nm, twin_members, counts_list, psp_nm, frame_path=None,
            exclude_extra=()):
    """{psp_function_name: weight}, each fixture normalised to 1.0 of its
    placed static executions, summed; and per-fixture coverage stats."""
    tsyms = Syms(read_nm(twin_nm))
    psyms = Syms(read_nm(psp_nm))
    excluded = members_of(twin_members, RENDERER_OBJS) | set(exclude_extra)
    total = collections.Counter()
    stats = []
    for cpath, info in counts_list:
        c, frames = read_counts(cpath)
        jr = jit_range(info) if info else None
        static = {a: n for a, n in c.items()
                  if not (jr and jr[0] <= a < jr[1])}
        jit_n = sum(n for a, n in c.items() if jr and jr[0] <= a < jr[1])
        placed, pairs, lost = place_static(tsyms, psyms, excluded, static)
        per = collections.Counter()
        for ad, pa in placed.items():
            per[pairs[tsyms.at(ad)[2]][2]] += static[ad]
        tot = sum(per.values()) or 1
        lost_n = sum(n for a, n in static.items() if a not in placed)
        lostby = collections.Counter()
        for a, n in static.items():
            if a not in placed:
                t = tsyms.at(a)
                lostby['video.o' if t and t[2] in excluded else
                       (t[2] if t else '?')] += n
        for k, v in per.items():
            total[k] += v / tot
        stats.append(dict(counts=cpath, frames=frames, jit=jit_n,
                          static_placed=tot, static_lost=lost_n,
                          top_lost=lostby.most_common(8),
                          placed=placed, static=static))
    if frame_path:
        for name, calls, nbytes, grp in read_frame_path(frame_path):
            r = psyms.unique(name)
            if r:
                # a per-frame PSP function: weight it like a static function
                # executed calls * span/4 times per frame, against the
                # fixture-normalised scale (~1e6 instructions a frame)
                span = r[1] if nbytes is None else min(r[1], nbytes)
                total[r[2]] += len(counts_list) * calls * span / 4 / 1e6
    return total, stats, psyms


def cmd_map(a):
    tsyms = Syms(read_nm(a.twin_nm))
    psyms = Syms(read_nm(a.psp_nm))
    excluded = members_of(a.twin_members, RENDERER_OBJS)
    addrs = set()
    for c in a.counts:
        cc, _ = read_counts(c)
        addrs.update(cc)
    if a.jit:
        lo, hi = (int(x, 16) for x in a.jit.split(':'))
        addrs = {x for x in addrs if not lo <= x < hi}
    placed, pairs, lost = place_static(tsyms, psyms, excluded, sorted(addrs))
    with open(a.out, 'w') as fh:
        for t in sorted(placed):
            fh.write('%x %x\n' % (t, placed[t]))
    print('map %s: %d twin instructions placed, %d functions, %d not placed'
          % (a.out, len(placed), sum(1 for p in pairs.values() if p),
             len(lost)))


def cmd_frame(a):
    psyms = Syms(read_nm(a.psp_nm))
    rows = read_frame_path(a.frame_path)
    lines = frame_lines(psyms, rows)
    missing = [r[0] for r in rows if not psyms.unique(r[0])]
    with open(a.out, 'w') as fh:
        for x in lines:
            fh.write('%x\n' % x)
    print('frame %s: %d line fetches per frame%s' % (
        a.out, len(lines), (' (not found: %s)' % ', '.join(missing))
        if missing else ''))


def cmd_weights(a):
    infos = a.info or [None] * len(a.counts)
    w, stats, psyms = weights(a.twin_nm, a.twin_members,
                              list(zip(a.counts, infos)), a.psp_nm,
                              a.frame_path)
    for s in stats:
        print('# %(counts)s frames=%(frames)s jit=%(jit)d static_placed=%(static_placed)d '
              'static_lost=%(static_lost)d' % s)
        print('#   top lost: %s' % ', '.join('%s=%d' % kv for kv in s['top_lost']))
    for k, v in w.most_common(a.top):
        r = psyms.unique(k)
        print('%-48s %10.6f %6d' % (k, v, r[1] if r else 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('map')
    m.add_argument('--twin-nm', required=True)
    m.add_argument('--twin-members')
    m.add_argument('--counts', nargs='+', required=True)
    m.add_argument('--psp-nm', required=True)
    m.add_argument('--jit', help='twin JIT range lo:hi (hex), not placed here')
    m.add_argument('--out', required=True)
    f = sub.add_parser('frame')
    f.add_argument('--psp-nm', required=True)
    f.add_argument('--frame-path', required=True)
    f.add_argument('--out', required=True)
    w = sub.add_parser('weights')
    w.add_argument('--twin-nm', required=True)
    w.add_argument('--twin-members')
    w.add_argument('--counts', nargs='+', required=True)
    w.add_argument('--info', nargs='*')
    w.add_argument('--psp-nm', required=True)
    w.add_argument('--frame-path')
    w.add_argument('--top', type=int, default=80)
    a = ap.parse_args()
    {'map': cmd_map, 'frame': cmd_frame, 'weights': cmd_weights}[a.cmd](a)


if __name__ == '__main__':
    main()
