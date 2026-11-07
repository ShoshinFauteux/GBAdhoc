#!/usr/bin/env python3
"""loop_lpsim.py (T16, gate LP) -- main()'s per-frame loop window in the
I-cache simulation (PLAN.md 0.2-9, 5/T16).

The layout twin (tools/layout: lp_map.py + lpsim_plugin.c, docs/LAYOUT-PINNING.md)
replays the dynarec twin's fetches at PSP addresses, but the twin cannot run
main_psp.c's loop: it is neither placed nor in psp_frame_path.txt.  This tool
adds it, WITHOUT changing the existing layout tooling (lp_map.py is imported
read-only; the plugin is a copy with one extra group, loop_lpsim_plugin.c).

Subcommands
  window ELF [--src auto|LO:HI] [--main-c SRC] [--json]
        main's loop window from DWARF .debug_line: by LINE (rows of main_psp.c
        lines in --src inside `main`, by default the window found by anchor,
        loop_anchor.py; x4's 220 lines / 115 sets) and by ADDRESS
        (every byte of main between the first and last such row, i.e. inlined
        helper code included).  Prints main's start mod 8192 and the set map.
  loopfile ELF --out F [--model window|bylines] [--rates R] [--shift N]
        the loop's per-frame PSP line fetches for the simulation (see RATES).
  shift-nm NM --by N --out OUT [--at main]
        a never-linked candidate: every text symbol at or after `main` moved by
        N bytes (the PLAN's "seeded 160 B shift before main").
  setmap ELF_A ELF_B [--shift-b N] [--nm-a NM --nm-b NM --weights W]
        static (no simulation): how many loop lines change I-cache set, the set
        histogram, and the loop lines sharing a set with the weighted hot path.
  layouts --lp DIR --set NAME --fixture FX --twin VARIANT --prof runs/prof/rc45f
          --elf name=ELF[+SHIFT][@JITHEX][#window|bylines] ... [--frame-path F] [--model ...]
        two layouts files for one twin run: FX.lay (old format, for
        lpsim_plugin.c) and FX.loop.lay (the same rows with loop=FILE, plus
        a `<name>-noloop` row per ELF without it).
  report SIM_OLD SIM_NEW [--accepted NAME]
        reproduction check (old plugin vs new plugin's -noloop rows: J/S/F must
        be identical) and the loop miss delta of every candidate against the
        accepted ELF, per frame, with the 215.3 ns/miss cost.
  --selftest ELF   (no docker): window figures, shift, setmap and report
        parsing, with mutants.

RATES (the TODO): PLAN wants the loop fetched at rates from a MEASURED trace
(a PPSSPP block trace in phase T; DRPROF_HW samples later).  This run was not
allowed a PPSSPP trace, so the default is a PLACEHOLDER: every 64 B line of the
window fetched once per frame (--model window, an upper bound on footprint;
x4 notes the window includes rarely run branches), or only the by-line rows
(--model bylines).  --rates R takes the measured input when it exists:
    # frames N
    <psp_hex_address> <executions>        (one row per executed address)
the same format as the twin's counts.txt (tools/cachemap/icount_plugin.c).
Each 64 B line is fetched max(executions in it)/N times per frame.  See
docs/stack-cache.md "T16 measured rates: TODO" for the command to produce it.
"""
import argparse
import bisect
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'layout'))
from elf32 import Elf  # noqa: E402
import dwline  # noqa: E402

LINE = 64
WAY = 8192
NSETS = WAY // LINE
MISS_NS = 215.3            # docs/CACHE-MAP.md, as sim_report.py
SRC_DEFAULT = None   # by anchor (loop_anchor.py; 16368-18090 on 01067e74), checked vs DW_AT_decl_line
import loop_anchor  # noqa: E402
JIT_DEFAULT = 0x09001500   # LAYOUT_PIN: both tiers at JIT_PIN_OFFSET 0x1500 mod 8 KiB


# ------------------------------------------------------------------ window
def main_sym(e, name='main'):
    fs = [s for s in e.funcs() if s['name'] == name]
    if len(fs) != 1:
        raise SystemExit('%s: %d symbols named %s' % (e.path, len(fs), name))
    return fs[0]['value'], fs[0]['size']


def window(elf_path, src=SRC_DEFAULT, fn='main', main_c=None):
    e = Elf(elf_path)
    mlo, msz = main_sym(e, fn)
    mhi = mlo + msz
    rws = dwline.rows(e, 'main_psp.c')
    if src is None or main_c:
        r = loop_anchor.for_elf(elf_path, main_c)
        if src is not None:
            loop_anchor.check_explicit(r, src[0], src[1], 'loop_lpsim')
        src = (r['lo'], r['hi'])
        sys.stderr.write(loop_anchor.describe(r) + '\n')
    lo, hi = src
    bylines = set()
    amin, amax = 1 << 40, 0
    for i in range(len(rws) - 1):
        a, f, ln = rws[i]
        nx = rws[i + 1][0]
        if a is None or nx is None or not f.endswith('main_psp.c') or not lo <= ln <= hi:
            continue
        if not mlo <= a < mhi:
            continue
        for x in range(a, nx, 4):
            bylines.add(x >> 6)
        amin, amax = min(amin, a), max(amax, nx)
    if not bylines:
        raise SystemExit('no main_psp.c rows %d..%d inside %s' % (lo, hi, fn))
    # bytes of the window by (file, in-loop?) -- inlined helpers show up as
    # main_psp.c rows outside --src or as other files
    acc = dwline.attribute([r for r in rws], amin, amax)
    inl = collections.Counter()
    for (f, ln), n in acc.items():
        if f.endswith('main_psp.c') and lo <= ln <= hi:
            inl['loop rows'] += n
        elif f.endswith('main_psp.c') or f.endswith('.inc.c'):
            # by compile unit: a P4 fragment is main_psp.o's own text (review 2026-11-07)
            inl['main_psp.c rows outside the loop (inlined helpers)'] += n
        else:
            inl['other files (%s)' % os.path.basename(f)] += n
    wlines = list(range(amin >> 6, (amax + LINE - 1) >> 6))
    return dict(elf=elf_path, main=mlo, main_size=msz, main_mod=mlo % WAY,
                lo=amin, hi=amax, off_lo=amin - mlo, off_hi=amax - mlo,
                bylines=sorted(bylines), bylines_sets=len({l % NSETS for l in bylines}),
                window_lines=wlines, window_sets=len({l % NSETS for l in wlines}),
                attrib=dict(inl), src=src)


def print_window(w, out=sys.stdout):
    out.write('loop window of %s\n' % w['elf'])
    out.write('  main 0x%08x size %d, main mod 8192 = %d (0x%x)\n'
              % (w['main'], w['main_size'], w['main_mod'], w['main_mod']))
    out.write('  by line (main_psp.c:%d-%d in main): %d lines over %d sets (x4: 220 / 115)\n'
              % (w['src'] + (len(w['bylines']), w['bylines_sets'])))
    out.write('  by address: 0x%08x..0x%08x = main+%d..+%d (%d B), %d lines over %d sets\n'
              % (w['lo'], w['hi'], w['off_lo'], w['off_hi'], w['hi'] - w['lo'],
                 len(w['window_lines']), w['window_sets']))
    for k, v in sorted(w['attrib'].items(), key=lambda kv: -kv[1]):
        out.write('    %6d B  %s\n' % (v, k))
    hist = collections.Counter(l % NSETS for l in w['window_lines'])
    out.write('  window lines per set: %s\n' % dict(sorted(collections.Counter(hist.values()).items())))


# ------------------------------------------------------------------ rates
def read_rates(path):
    frames = None
    c = {}
    for l in open(path):
        if l.startswith('# frames'):
            frames = int(l.split()[2])
            continue
        p = l.split()
        if len(p) == 2 and not l.startswith('#'):
            c[int(p[0], 16)] = int(p[1])
    if not frames:
        raise SystemExit('%s: no "# frames N" header' % path)
    return c, frames


def loop_fetches(w, model='window', rates=None, shift=0):
    """PSP line addresses fetched per frame, in address order.  The shift is
    applied to BYTE addresses before they are bucketed into 64 B lines, so a
    shift that is not a multiple of 64 (the PLAN's 160 B) makes a source line
    straddle two PSP lines and both are fetched (it used to fetch only the
    line holding the shifted start, dropping the tail of every line)."""
    if rates:
        c, frames = rates
        per = collections.defaultdict(int)
        for a, n in c.items():
            if w['lo'] <= a < w['hi']:
                l = (a + shift) >> 6
                per[l] = max(per[l], n)
        out = []
        for l in sorted(per):
            r = per[l] / frames
            k = int(r) + (1 if r - int(r) >= 0.5 else 0)
            out.extend([l << 6] * k)
        return out
    lines = w['window_lines'] if model == 'window' else w['bylines']
    got = set()
    for l in lines:
        got.add(((l << 6) + shift) >> 6)
        got.add(((l << 6) + shift + LINE - 1) >> 6)
    return [l << 6 for l in sorted(got)]


# ------------------------------------------------------------------ shift
def shift_nm(nm_in, nm_out, by, at='main'):
    rows = [l.rstrip('\n') for l in open(nm_in, errors='replace')]
    pivot = None
    for l in rows:
        p = l.split()
        if len(p) >= 3 and p[-1] == at and p[-2] in 'tT':
            pivot = int(p[0], 16)
    if pivot is None:
        raise SystemExit('%s: no text symbol %s' % (nm_in, at))
    n = 0
    with open(nm_out, 'w') as fh:
        for l in rows:
            p = l.split()
            if len(p) in (3, 4) and p[-2] in 'tTwW':
                a = int(p[0], 16)
                if a >= pivot:
                    p[0] = '%08x' % (a + by)
                    n += 1
                    fh.write(' '.join(p) + '\n')
                    continue
            fh.write(l + '\n')
    return pivot, n


# ------------------------------------------------------------------ setmap
def hot_lines(nm, weights_path, top=120):
    """64 B lines of the top-weighted static functions (profile_weights.txt)
    at their addresses in nm -- the competing per-frame static path."""
    import lp_map
    psyms = lp_map.Syms(lp_map.read_nm(nm))
    out = set()
    k = 0
    for l in open(weights_path):
        if l.startswith('#'):
            continue
        p = l.split()
        if not p:
            continue
        r = psyms.unique(p[0])
        if not r:
            continue
        a, s, _ = r
        out.update(range(a >> 6, (a + min(s, 4096) + 63) >> 6))
        k += 1
        if k >= top:
            break
    return out


def setmap(wa, wb, shift_b=0, hot_a=None, hot_b=None):
    la = wa['window_lines']
    lb = [l + (shift_b >> 6) for l in wb['window_lines']] if shift_b % LINE == 0 else None
    if lb is None:
        lb = sorted({((l << 6) + shift_b) >> 6 for l in wb['window_lines']} |
                    {((l << 6) + shift_b + LINE - 1) >> 6 for l in wb['window_lines']})
    sa = collections.Counter(l % NSETS for l in la)
    sb = collections.Counter(l % NSETS for l in lb)
    moved = (wb['main'] + shift_b) % WAY - wa['main'] % WAY
    res = dict(main_mod_a=wa['main'] % WAY, main_mod_b=(wb['main'] + shift_b) % WAY,
               delta_mod=moved, lines_a=len(la), lines_b=len(lb),
               set_hist_equal=(sa == sb),
               sets_changed=sum(1 for s in range(NSETS) if sa[s] != sb[s]))
    if hot_a is not None and hot_b is not None:
        ha = collections.Counter(l % NSETS for l in hot_a)
        hb = collections.Counter(l % NSETS for l in hot_b)
        # 2-way: a set with > 2 distinct per-frame lines must evict
        res['over2_a'] = sum(1 for s in range(NSETS) if sa[s] + ha[s] > 2)
        res['over2_b'] = sum(1 for s in range(NSETS) if sb[s] + hb[s] > 2)
        res['excess_a'] = sum(max(0, sa[s] + ha[s] - 2) for s in range(NSETS))
        res['excess_b'] = sum(max(0, sb[s] + hb[s] - 2) for s in range(NSETS))
    return res


# ------------------------------------------------------------------ layouts
def cmd_layouts(a):
    import lp_map
    lp = os.path.expanduser(a.lp)
    out = os.path.join(lp, 'loop', 'sim', a.set)
    os.makedirs(out, exist_ok=True)
    prof = os.path.join(lp, a.prof, a.fixture)
    jr = lp_map.jit_range(os.path.join(prof, 'run.info'))
    tsyms = lp_map.Syms(lp_map.read_nm(os.path.join(lp, 'bin', 'dr_host_%s.nm' % a.twin)))
    excluded = lp_map.members_of(os.path.join(lp, 'bin', 'dr_host_%s.members' % a.twin),
                                 lp_map.RENDERER_OBJS)
    counts, _ = lp_map.read_counts(os.path.join(prof, 'counts.txt'))
    static = sorted(x for x in counts if not jr[0] <= x < jr[1])
    rates = read_rates(a.rates) if a.rates else None
    old, new = [], []
    cw = lambda p: p.replace(lp, '/w')   # noqa: E731  container path
    for spec in a.elf:
        name, rest = spec.split('=', 1)
        rest, _, model = rest.partition("#")
        model = model or a.model
        rest, _, jit = rest.partition("@")
        path, _, sh = rest.partition('+')
        shift = int(sh) if sh else 0
        jb = int(jit, 16) if jit else JIT_DEFAULT
        nm = os.path.join(os.path.dirname(path), 'elf.nm')
        if shift:
            nm2 = os.path.join(out, '%s.nm' % name)
            shift_nm(nm, nm2, shift)
            nm = nm2
        psyms = lp_map.Syms(lp_map.read_nm(nm))
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
        w = window(path)
        lf = os.path.join(out, '%s.%s.loop' % (a.fixture, name))
        fetch = loop_fetches(w, model, rates, shift)
        with open(lf, 'w') as fh:
            fh.write('# %s model=%s rates=%s shift=%d main=0x%x\n' % (
                path, model, a.rates or 'PLACEHOLDER', shift, w['main'] + shift))
            for x in fetch:
                fh.write('%x\n' % x)
        row = '%s %s %x %s' % (name, cw(mp), jb & ~63, cw(fp) if fp != '-' else '-')
        old.append(row)
        new.append(row.replace(name + ' ', name + '-noloop ', 1))
        new.append(row + ' loop=%s' % cw(lf))
        print('%s: %d twin instr placed, %d fn lost; loop %d fetches/frame; main mod 8192 = %d'
              % (name, len(placed), len(lost), len(fetch), (w['main'] + shift) % WAY), file=sys.stderr)
    for fn, rows in (('%s.lay' % a.fixture, old), ('%s.loop.lay' % a.fixture, new)):
        with open(os.path.join(out, fn), 'w') as fh:
            fh.write('\n'.join(rows) + '\n')
    print(cw(os.path.join(out, '%s.lay' % a.fixture)))
    print(cw(os.path.join(out, '%s.loop.lay' % a.fixture)))


# ------------------------------------------------------------------ report
def read_sim(path):
    frames, rows = 1, {}
    for l in open(path):
        p = l.split()
        if not p:
            continue
        if p[0] == 'frames':
            frames = max(int(p[1]), 1)
        elif p[0] == 'layout':
            g = {}
            for t in p[6:]:
                k, v = t.split('=')
                m, acc = v.split('/')
                g[k] = (int(m), int(acc))
            rows[p[1]] = dict(access=int(p[3]), miss=int(p[5]), g=g)
    return frames, rows


def report(old_path, new_path, accepted, out=sys.stdout, status=None):
    """returns True when the reproduction holds.  `status` (a dict, optional)
    gets 'informative': False when the loop group cannot rank layouts: the
    accepted row's L misses are >= 99 % of its L accesses, i.e. every loop line
    misses every frame wherever it is placed (a loop fetched once per frame
    never survives a frame of cache traffic), so a dL of 0 is NOT a pass."""
    fo, ro = read_sim(old_path)
    fn, rn = read_sim(new_path)
    ok = fo == fn
    out.write('reproduction (old lpsim_plugin vs loop_lpsim -noloop rows, same trace): frames %d / %d\n' % (fo, fn))
    if not ro:
        ok = False
        out.write('  no layout rows in %s\n' % old_path)
    for name, r in sorted(ro.items()):
        n = rn.get(name + '-noloop')
        same = n is not None and all(r['g'].get(k) == n['g'].get(k) for k in ('J', 'S', 'F')) \
            and n['g'].get('L', (0, 0)) == (0, 0) and n['access'] == r['access'] and n['miss'] == r['miss']
        ok &= same
        out.write('  %-12s %s  J=%s S=%s F=%s\n' % (name, 'IDENTICAL' if same else 'DIFFERENT',
                  '%d/%d' % r['g']['J'], '%d/%d' % r['g']['S'], '%d/%d' % r['g']['F']))
    out.write('loop group L (misses/frame [ms/frame at %.1f ns]):\n' % MISS_NS)
    acc = rn.get(accepted)
    if acc is None:
        ok = False
        out.write('  ACCEPTED layout %r not in %s: no delta can be computed\n' % (accepted, new_path))
    for name, r in sorted(rn.items()):
        if name.endswith('-noloop'):
            continue
        if name + '-noloop' not in rn or name not in ro:
            ok = False
            out.write('  %-12s has no -noloop twin / no old-plugin row\n' % name)
        lm, la = r['g'].get('L', (0, 0))
        tot = r['miss'] / fn
        line = '  %-12s L %8.1f/%-8.1f [%.3f]  total %9.1f [%.3f]' % (
            name, lm / fn, la / fn, lm / fn * MISS_NS / 1e6, tot, tot * MISS_NS / 1e6)
        if acc and name != accepted:
            dl = (lm - acc['g']['L'][0]) / fn
            dt = (r['miss'] - acc['miss']) / fn
            line += '   vs %s: dL %+.1f (%+.4f ms)  dTotal %+.1f (%+.4f ms)' % (
                accepted, dl, dl * MISS_NS / 1e6, dt, dt * MISS_NS / 1e6)
        out.write(line + '\n')
    informative = True
    if acc:
        lm, la = acc['g'].get('L', (0, 0))
        if la == 0 or lm >= 0.99 * la:
            informative = False
            out.write('LP: NOT INFORMATIVE -- the accepted layout misses %d of %d loop fetches (%.1f %%): '
                      'every loop line misses every frame wherever it sits, so dL cannot rank a '
                      'candidate (fetch the loop at measured, interleaved rates; docs/stack-cache.md)\n'
                      % (lm, la, 100.0 * lm / la if la else 0))
    if status is not None:
        status['informative'] = informative
    return ok


# ------------------------------------------------------------------ selftest
def selftest(elf_path):
    import tempfile
    fails = 0

    def chk(c, label):
        nonlocal fails
        print('  %-5s %s' % ('ok' if c else 'FAIL', label))
        fails += 0 if c else 1

    w = window(elf_path)
    print_window(w)
    flav = None
    fj = os.path.join(os.path.dirname(elf_path), 'flavour.json')
    if os.path.exists(fj):
        flav = json.load(open(fj)).get('flavour')
    if flav == 'release':
        # x4's figure is a release-ELF figure; lab/harness have their own
        chk(len(w['bylines']) == 220 and w['bylines_sets'] == 115,
            'by-line figure reproduces x4 on release (220 lines / 115 sets): got %d / %d'
            % (len(w['bylines']), w['bylines_sets']))
    else:
        print('        (flavour %s: x4 220/115 is a release figure; recorded %d / %d)'
              % (flav, len(w['bylines']), w['bylines_sets']))
    chk(w['lo'] >> 6 <= min(w['bylines']) and max(w['bylines']) <= (w['hi'] - 1) >> 6,
        'by-address window contains every by-line row')
    chk(len(w['window_lines']) >= len(w['bylines']), 'window has at least the by-line lines')
    # M1: a seeded 160 B shift before main changes main mod 8192 and the set map
    w2 = dict(w)
    sm = setmap(w, w2, 160)
    chk(sm['delta_mod'] == 160 and not sm['set_hist_equal'] and sm['sets_changed'] > 0,
        'M1 seeded +160 B before main: main mod 8192 %d -> %d, %d sets change occupancy'
        % (sm['main_mod_a'], sm['main_mod_b'], sm['sets_changed']))
    # control: shift by a whole way (8192) restores the set map exactly
    sm0 = setmap(w, w2, 8192)
    chk(sm0['set_hist_equal'] and sm0['delta_mod'] == 0, 'C1 +8192 B (one way) restores the set map')
    # M2: shift-nm moves main and later symbols only
    td = tempfile.mkdtemp(prefix='looplp.')
    nm = os.path.join(os.path.dirname(elf_path), 'elf.nm')
    if os.path.exists(nm):
        out = os.path.join(td, 's.nm')
        pivot, n = shift_nm(nm, out, 160)
        before = {l.split()[-1]: int(l.split()[0], 16) for l in open(nm) if len(l.split()) in (3, 4) and l.split()[-2] in 'tT'}
        after = {l.split()[-1]: int(l.split()[0], 16) for l in open(out) if len(l.split()) in (3, 4) and l.split()[-2] in 'tT'}
        moved = [k for k in before if after.get(k) != before[k]]
        chk(all(before[k] >= pivot for k in moved) and after['main'] == before['main'] + 160 and n > 0,
            'M2 shift-nm: %d text symbols at/after main moved +160, none before it' % n)
    # M3: rates model -- a synthetic measured trace at 3 fetches/frame on two lines
    rp = os.path.join(td, 'r.txt')
    with open(rp, 'w') as fh:
        fh.write('# frames 10\n%x 30\n%x 30\n%x 99\n' % (w['lo'], w['lo'] + 64, w['hi'] + 4096))
    f = loop_fetches(w, rates=read_rates(rp))
    chk(len(f) == 6 and f[0] == (w['lo'] >> 6) << 6, 'M3 rates file: 2 lines x 3/frame inside the window, outside ignored (%d fetches)' % len(f))
    # M4: report() catches a J/S/F difference between old and new plugin output
    so, sn = os.path.join(td, 'o.txt'), os.path.join(td, 'n.txt')
    open(so, 'w').write('frames 10\nlayout acc access 100 miss 10 J=1/50 S=4/40 F=5/10\n'
                        'layout cand access 100 miss 10 J=1/50 S=4/40 F=5/10\n')
    open(sn, 'w').write('frames 10\nlayout acc-noloop access 100 miss 10 J=1/50 S=4/40 F=5/10 L=0/0\n'
                        'layout acc access 120 miss 14 J=1/50 S=5/40 F=5/10 L=3/20\n'
                        'layout cand-noloop access 100 miss 10 J=1/50 S=4/40 F=5/10 L=0/0\n'
                        'layout cand access 120 miss 19 J=1/50 S=6/40 F=5/10 L=7/20\n')
    import io
    buf = io.StringIO()
    okr = report(so, sn, 'acc', buf)
    chk(okr and 'dL +0.4' in buf.getvalue(), 'C2 report: identical rows pass, candidate loop delta +0.4/frame computed')
    open(sn, 'w').write('frames 10\nlayout acc-noloop access 100 miss 11 J=1/50 S=5/40 F=5/10 L=0/0\n')
    chk(not report(so, sn, 'acc', io.StringIO()), 'M4 report: an S-group difference fails reproduction')
    # M5 (verifier): the accepted name is absent -> not a pass (it used to print no deltas and exit 0)
    open(sn, 'w').write('frames 10\nlayout acc-noloop access 100 miss 10 J=1/50 S=4/40 F=5/10 L=0/0\n'
                        'layout acc access 120 miss 14 J=1/50 S=5/40 F=5/10 L=3/20\n')
    chk(not report(so, sn, 'accX', io.StringIO()), 'M5 report: a missing accepted layout fails')
    # M6 (verifier): L misses == L accesses -> NOT INFORMATIVE (the t16base runs: 376/376 every frame)
    open(sn, 'w').write('frames 10\nlayout acc-noloop access 100 miss 10 J=1/50 S=4/40 F=5/10 L=0/0\n'
                        'layout acc access 120 miss 30 J=1/50 S=4/40 F=5/10 L=20/20\n'
                        'layout cand-noloop access 100 miss 10 J=1/50 S=4/40 F=5/10 L=0/0\n'
                        'layout cand access 120 miss 30 J=1/50 S=4/40 F=5/10 L=20/20\n')
    open(so, 'w').write('frames 10\nlayout acc access 100 miss 10 J=1/50 S=4/40 F=5/10\n'
                        'layout cand access 100 miss 10 J=1/50 S=4/40 F=5/10\n')
    st = {}
    buf = io.StringIO()
    okr = report(so, sn, 'acc', buf, st)
    chk(okr and st.get('informative') is False and 'NOT INFORMATIVE' in buf.getvalue(),
        'M6 report: L misses == accesses is flagged NOT INFORMATIVE (dL 0 is not a pass)')
    # M7 (verifier): an unaligned shift fetches the straddled lines too
    f0 = loop_fetches(w, 'window', None, 0)
    f160 = loop_fetches(w, 'window', None, 160)
    want = len({((l << 6) + 160) >> 6 for l in w['window_lines']} |
               {((l << 6) + 160 + 63) >> 6 for l in w['window_lines']})
    chk(len(f160) == want and len(f160) >= len(f0) and min(f160) == ((w['window_lines'][0] << 6) + 160) & ~63,
        'M7 +160 B shift: %d window lines -> %d PSP lines fetched (straddles counted)' % (len(f0), len(f160)))
    import shutil
    shutil.rmtree(td, ignore_errors=True)
    print('selftest: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


def main(argv):
    if len(argv) == 2 and argv[0] == '--selftest':
        return selftest(argv[1])
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('window')
    p.add_argument('elf')
    p.add_argument('--src', default='auto')
    p.add_argument('--main-c')
    p.add_argument('--json')
    p = sub.add_parser('loopfile')
    p.add_argument('elf')
    p.add_argument('--out', required=True)
    p.add_argument('--model', default='window', choices=('window', 'bylines'))
    p.add_argument('--rates')
    p.add_argument('--shift', type=int, default=0)
    p = sub.add_parser('shift-nm')
    p.add_argument('nm')
    p.add_argument('--by', type=int, required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--at', default='main')
    p = sub.add_parser('setmap')
    p.add_argument('elf_a')
    p.add_argument('elf_b')
    p.add_argument('--shift-b', type=int, default=0)
    p.add_argument('--weights', default=os.path.join(os.path.dirname(os.path.dirname(HERE)), 'layout', 'profile_weights.txt'))
    p = sub.add_parser('layouts')
    p.add_argument('--lp', required=True)
    p.add_argument('--set', required=True)
    p.add_argument('--fixture', required=True)
    p.add_argument('--twin', required=True)
    p.add_argument('--prof', required=True)
    p.add_argument('--elf', action='append', required=True)
    p.add_argument('--frame-path')
    p.add_argument('--model', default='window', choices=('window', 'bylines'))
    p.add_argument('--rates')
    p = sub.add_parser('report')
    p.add_argument('sim_old')
    p.add_argument('sim_new')
    p.add_argument('--accepted', default='acc')
    a = ap.parse_args(argv)
    if a.cmd == 'window':
        if a.src == 'auto':
            w = window(a.elf, None, main_c=a.main_c)
        else:
            lo, hi = (int(x) for x in a.src.split(':'))
            # an explicit range is held to the anchored window of the ELF's source
            r = loop_anchor.for_elf(a.elf, a.main_c)
            loop_anchor.check_explicit(r, lo, hi, 'loop_lpsim')
            w = window(a.elf, (lo, hi))
        print_window(w)
        if a.json:
            json.dump({k: v for k, v in w.items() if k not in ('bylines', 'window_lines')},
                      open(a.json, 'w'), indent=1)
    elif a.cmd == 'loopfile':
        w = window(a.elf)
        f = loop_fetches(w, a.model, read_rates(a.rates) if a.rates else None, a.shift)
        with open(a.out, 'w') as fh:
            fh.write('# model=%s rates=%s\n' % (a.model, a.rates or 'PLACEHOLDER'))
            fh.writelines('%x\n' % x for x in f)
        print('%s: %d fetches per frame' % (a.out, len(f)))
    elif a.cmd == 'shift-nm':
        pivot, n = shift_nm(a.nm, a.out, a.by, a.at)
        print('%s: %d text symbols at/after 0x%08x moved by %d' % (a.out, n, pivot, a.by))
    elif a.cmd == 'setmap':
        wa, wb = window(a.elf_a), window(a.elf_b)
        ha = hb = None
        nma = os.path.join(os.path.dirname(a.elf_a), 'elf.nm')
        nmb = os.path.join(os.path.dirname(a.elf_b), 'elf.nm')
        if os.path.exists(nma) and os.path.exists(nmb) and os.path.exists(a.weights):
            import tempfile
            ha = hot_lines(nma, a.weights)
            if a.shift_b:
                t = tempfile.NamedTemporaryFile('w', suffix='.nm', delete=False).name
                shift_nm(nmb, t, a.shift_b)
                nmb = t
            hb = hot_lines(nmb, a.weights)
        r = setmap(wa, wb, a.shift_b, ha, hb)
        for k, v in r.items():
            print('  %-16s %s' % (k, v))
    elif a.cmd == 'layouts':
        cmd_layouts(a)
    elif a.cmd == 'report':
        # 0 reproduction holds and L can rank layouts; 1 reproduction broken;
        # 3 reproduction holds but L is placement-blind (NOT a pass)
        st = {}
        if not report(a.sim_old, a.sim_new, a.accepted, status=st):
            return 1
        return 0 if st.get('informative') else 3
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
