#!/usr/bin/env python3
"""loop_diff.py -- T1b / gate G2L (builds/mainpsp-map/out/PLAN.md §0.1, §5).

While the per-frame loop still lives inside `main`, prove it unchanged BY
ADDRESS, not by source line.  A line-range diff cannot see an edited inlined
helper (critique R4): a helper inlined into the loop carries ITS OWN line
numbers (audio_status_frame L:850, rfu_link_down_drain 7431, exit_assist_frame
7490, rfu_trace_drain 7646, turbo_frame, ...), so its bytes sit inside the
loop's address window but outside the loop's line range.

For each ELF it computes
  * the by-line footprint (x4's method, scratch/x4v_loopsets.py): code rows of
    `main` whose main_psp.c line is in --loop-lines; distinct 64 B I-cache
    lines, sets used (128 sets per 8 KiB way), max lines per set;
  * the BY-ADDRESS loop window: from the first to the last address of
      - the loop-line rows of `main`, and
      - every DW_TAG_inlined_subroutine directly in `main` whose
        DW_AT_call_line is in --loop-lines (its DW_AT_ranges / low-high pc);
    every byte of `main` between those bounds is in the window, whatever line
    or helper the DWARF gives it (GCC interleaves cold boot rows into it too);
  * main's start mod 8192 and the window's set map;
  * the inlined-helper total: bytes of `main` whose line row is in main_psp.c
    before main's own definition line (x4:90 figure, 9,912 B), and the DWARF
    total of inlined_subroutine ranges in `main` whose origin is a main_psp.c
    function (a cross-check; it also counts header helpers and nesting once).
With two ELFs it compares (ef_mips.compare_words, the func_diff normaliser):
  pass 1  the loop window, word by word, every differing word attributed to
          its source line and to the inlined helper whose range holds it;
  pass 2  main's whole body;
  place   main's start mod 8192, the window's offset in main and its set map.

usage:
  loop_diff.py ELF                      report one ELF
  loop_diff.py BASE.elf CAND.elf        compare
      [--func main] [--file main_psp.c] [--loop-lines 16368-18090]
      [--body-strict] [--json OUT] [-v] [--any-flavour]
      --selftest                        run tests/elf-func_test_loop_diff.py
exit 0  loop window same (bytes or modulo address) and placed the same
     1  loop window code changed (or, with --body-strict, any byte of main)
     2  usage / flavour mismatch / DWARF missing
     3  loop code same but its placement moved (main mod 8192, the window's
        offset, or a set's occupancy changed)
What it does NOT prove: anything outside `main` (func_diff.py, G2); the
per-frame miss cost (the window is an upper bound on fetched lines: rarely
run branches are in it; LP/T16 models the cost); behaviour of data.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from elf32 import Elf                       # noqa: E402
import ef_dwarf as D                        # noqa: E402
import ef_mips as M                         # noqa: E402

DEFAULT_LOOP = '16368-18090'                # x4's range; the old 18067 bound is dropped


def _sets(lines):
    sets = {}
    for ln in lines:
        s = ln & 127
        sets[s] = sets.get(s, 0) + 1
    return sets


class Loop:
    """Everything loop_diff needs from one ELF."""

    def __init__(self, path, func='main', srcfile='main_psp.c', loop=(16368, 18090)):
        self.path = path
        self.elf = e = Elf(path)
        syms = [s for s in e.funcs() if s['name'] == func]
        if len(syms) != 1:
            raise LookupError('%s: %d symbols named %s' % (path, len(syms), func))
        self.func = func
        self.lo, self.size = syms[0]['value'], syms[0]['size']
        self.hi = self.lo + self.size
        self.code = e.read(self.lo, self.size)
        self.loop = loop
        info = D.Info(e)
        cu = info.cu_named(srcfile)
        self.cu = cu
        sp = [s for s in cu.subprograms() if s['name'] == func and s['concrete']]
        self.decl_line = sp[0]['decl_line'] if sp else None
        rows = D.line_rows(e, srcfile)
        self.spans = [(a, b, f, l) for a, b, f, l in D.row_spans(rows)
                      if self.lo <= a < self.hi and f is not None]
        isrc = lambda f: f == srcfile or f.endswith('/' + srcfile)
        self.isrc = isrc
        # ---- by line (x4v_loopsets.py)
        lines = set(); amin, amax = None, None
        for a, b, f, l in self.spans:
            if not (isrc(f) and loop[0] <= l <= loop[1]):
                continue
            b = min(b, self.hi)
            for x in range(a, b, 4):
                lines.add(x >> 6)
            amin = a if amin is None else min(amin, a)
            amax = b if amax is None else max(amax, b)
        self.byline = dict(lines=len(lines), sets=len(_sets(lines)),
                           max_per_set=max(_sets(lines).values()) if lines else 0,
                           off_lo=amin - self.lo if amin is not None else None,
                           off_hi=amax - self.lo if amax is not None else None,
                           line_addrs=sorted(lines))
        # ---- inlined subroutines directly in main
        inl = [x for x in cu.inlined()
               if x['container_name'] == func and any(self.lo <= a < self.hi for a, _ in x['ranges'])]
        self.inlined = inl
        self.loop_inl = [x for x in inl if x['depth'] == 0 and x['call_file'] and isrc(x['call_file'])
                         and x['call_line'] is not None and loop[0] <= x['call_line'] <= loop[1]]
        # ---- by-address window
        lo_c = [amin] if amin is not None else []
        hi_c = [amax] if amax is not None else []
        for x in self.loop_inl:
            for a, b in x['ranges']:
                lo_c.append(a); hi_c.append(b)
        self.wlo, self.whi = min(lo_c), max(hi_c)
        wl = set(range(self.wlo >> 6, ((self.whi - 1) >> 6) + 1))
        self.wsets = _sets(wl)
        self.wlines = sorted(wl)
        # composition of the window
        comp = dict(loop_lines=0, inlined_loop_helpers=0, other_main_lines=0, other_files=0, unattributed=0)
        helper_bytes = {}
        cover = 0
        for a, b, f, l in self.spans:
            s, t = max(a, self.wlo), min(b, self.whi)
            if t <= s:
                continue
            cover += t - s
            h = self.helper_at(s)
            if h is not None:
                comp['inlined_loop_helpers'] += t - s
                helper_bytes[h['name']] = helper_bytes.get(h['name'], 0) + t - s
            elif isrc(f) and loop[0] <= l <= loop[1]:
                comp['loop_lines'] += t - s
            elif isrc(f):
                comp['other_main_lines'] += t - s
            else:
                comp['other_files'] += t - s
        comp['unattributed'] = (self.whi - self.wlo) - cover
        self.comp = comp
        self.helper_bytes = helper_bytes
        # ---- x4:90 inlined-helper totals
        self.pre_main_bytes = sum(min(b, self.hi) - a for a, b, f, l in self.spans
                                  if isrc(f) and self.decl_line and l < self.decl_line)
        top = [x for x in inl if x['depth'] == 0]
        self.dwarf_inl_src_bytes = sum(b - a for x in top if x['decl_file'] and isrc(x['decl_file'])
                                       and self.decl_line and (x['decl_line'] or 0) < self.decl_line
                                       for a, b in x['ranges'])
        self.dwarf_inl_all_bytes = sum(b - a for x in top for a, b in x['ranges'])

    def helper_at(self, addr, loop_only=True):
        """innermost-first: the loop-called top-level inlined helper whose
        range holds addr (None if none)."""
        for x in (self.loop_inl if loop_only else [y for y in self.inlined if y['depth'] == 0]):
            for a, b in x['ranges']:
                if a <= addr < b:
                    return x
        return None

    def klass(self, addr):
        """who owns a byte of main: loop-line, loop-helper (inlined from a
        loop call), other-main-line (a boot/session/teardown row GCC placed
        inside the window, e.g. the cold tail +0x7620..+0x82b4), other-file."""
        if self.helper_at(addr) is not None:
            return 'loop-helper'
        for a, b, f, l in self.spans:
            if a <= addr < b:
                if self.isrc(f):
                    return 'loop-line' if self.loop[0] <= l <= self.loop[1] else 'other-main-line'
                return 'other-file'
        return 'unattributed'

    def where(self, addr):
        """'main_psp.c:LINE [in helper (inlined, called at L:n)]'."""
        line = None
        for a, b, f, l in self.spans:
            if a <= addr < b:
                line = '%s:%d' % (os.path.basename(f), l)
                break
        chain = []
        for x in self.inlined:
            if any(a <= addr < b for a, b in x['ranges']):
                chain.append((x['depth'], '%s@L:%s' % (x['name'], x['call_line'])))
        chain = [c for _, c in sorted(chain)]
        return '%s%s' % (line or '?', (' inlined ' + ' > '.join(chain)) if chain else '')

    def summary(self):
        return dict(
            elf=self.path, func=self.func, start='%08x' % self.lo, size=self.size,
            start_mod_8192=self.lo & 8191, decl_line=self.decl_line,
            loop_lines='%d-%d' % self.loop,
            by_line=dict((k, v) for k, v in self.byline.items() if k != 'line_addrs'),
            window=dict(off_lo=self.wlo - self.lo, off_hi=self.whi - self.lo, bytes=self.whi - self.wlo,
                        lines=len(self.wlines), sets=len(self.wsets),
                        max_per_set=max(self.wsets.values()),
                        start_mod_8192=self.wlo & 8191, composition=self.comp,
                        loop_inlined_instances=len(self.loop_inl),
                        loop_inlined_helpers=dict(sorted(self.helper_bytes.items(), key=lambda kv: -kv[1]))),
            inlined_helper_bytes=dict(line_rows_before_main=self.pre_main_bytes,
                                      dwarf_top_level_src_helpers=self.dwarf_inl_src_bytes,
                                      dwarf_top_level_all=self.dwarf_inl_all_bytes),
            set_map={str(k): v for k, v in sorted(self.wsets.items())})


def print_one(L, v=False):
    s = L.summary()
    print('%s: %s @%s size %d, start mod 8192 = %d (0x%04x)' % (
        os.path.basename(os.path.dirname(L.path)) or L.path, L.func, s['start'], L.size,
        s['start_mod_8192'], s['start_mod_8192']))
    b = s['by_line']
    print('  by line %s: %d lines of 64 B over %d sets (max %d per set), off %s..%s' % (
        s['loop_lines'], b['lines'], b['sets'], b['max_per_set'], b['off_lo'], b['off_hi']))
    w = s['window']
    print('  by address: window off %d..%d (%d B), %d lines over %d sets (max %d per set), '
          'window start mod 8192 = %d' % (w['off_lo'], w['off_hi'], w['bytes'], w['lines'], w['sets'],
                                          w['max_per_set'], w['start_mod_8192']))
    print('    composition: %s' % w['composition'])
    print('    %d inlined instances called from the loop; helpers by bytes: %s' % (
        w['loop_inlined_instances'],
        ', '.join('%s %d' % kv for kv in list(w['loop_inlined_helpers'].items())[:12 if not v else None])))
    ih = s['inlined_helper_bytes']
    print('  inlined helpers in %s: %d B by line rows before L:%s (x4:90 says 9,912); DWARF top-level '
          'instances: %d B from main_psp.c functions, %d B all' % (
              L.func, ih['line_rows_before_main'], L.decl_line, ih['dwarf_top_level_src_helpers'],
              ih['dwarf_top_level_all']))
    return s


def compare(A, B, body_strict=False, v=False):
    SA, SB = M.Space(A.elf), M.Space(B.elf)
    res = dict()
    # pass 2 first when sizes allow: one joint scan of main, partitioned later
    same_size = A.size == B.size
    body = None
    if A.code == B.code and A.lo == B.lo:
        body = dict(ok=True, bad=[], weak=[], naddr=0, same_bytes=True)
    elif same_size:
        body = M.compare_words(SA, SB, A.lo, B.lo, A.code, B.code, stop_first=False)
        body['same_bytes'] = A.code == B.code
    # pass 1: the window
    woA, woB = A.wlo - A.lo, B.wlo - B.lo
    wsA, wsB = A.whi - A.wlo, B.whi - B.wlo
    if body is not None and woA == woB and wsA == wsB:
        wbad = [(i, why) for i, why in body['bad'] if woA <= 4 * i < woA + wsA]
        wweak = [i for i in body['weak'] if woA <= 4 * i < woA + wsA]
        wsame = A.code[woA:woA + wsA] == B.code[woB:woB + wsB] and A.wlo == B.wlo
        win = dict(ok=not wbad, bad=wbad, weak=wweak, same_bytes=wsame)
    elif wsA == wsB:
        ca, cb = A.code[woA:woA + wsA], B.code[woB:woB + wsB]
        r = M.compare_words(SA, SB, A.wlo, B.wlo, ca, cb, stop_first=False)
        win = dict(ok=r['ok'], bad=[(i + woA // 4, w) for i, w in r['bad']],
                   weak=[i + woA // 4 for i in r['weak']], same_bytes=ca == cb and A.wlo == B.wlo)
    else:
        win = dict(ok=False, bad=[(woA // 4, 'window size %d vs %d' % (wsA, wsB))], weak=[], same_bytes=False)

    def describe(bad):
        out = []
        for i, why in bad:
            a = A.lo + 4 * i
            out.append(dict(off=4 * i, addr='%08x' % a, where=A.where(a), why=why, klass=A.klass(a)))
        return out
    kl = {}
    for i, _ in win['bad']:
        k = A.klass(A.lo + 4 * i); kl[k] = kl.get(k, 0) + 1
    res['window'] = dict(classes=kl, state='same-bytes' if win['same_bytes'] else ('same-mod-address' if win['ok'] else 'CHANGED'),
                         diffs=describe(win['bad']), weak=len(win['weak']))
    if body is None:
        res['body'] = dict(state='CHANGED', diffs=[dict(off=0, addr='%08x' % A.lo, where='-',
                                                         why='size %d vs %d' % (A.size, B.size))], weak=0)
    else:
        res['body'] = dict(state='same-bytes' if body['same_bytes'] and A.lo == B.lo else
                           ('same-mod-address' if body['ok'] else 'CHANGED'),
                           diffs=describe(body['bad']), weak=len(body['weak']))
        res['body']['outside_window'] = sum(1 for i, _ in body['bad'] if not (woA <= 4 * i < woA + wsA))
    # placement
    moved_sets = sum(1 for s in set(A.wsets) | set(B.wsets) if A.wsets.get(s, 0) != B.wsets.get(s, 0))
    res['placement'] = dict(main_mod_8192=[A.lo & 8191, B.lo & 8191],
                            window_off=[woA, woB], window_bytes=[wsA, wsB],
                            window_mod_8192=[A.wlo & 8191, B.wlo & 8191],
                            sets_with_changed_occupancy=moved_sets,
                            # every window line maps to another set unless the
                            # window moved by a multiple of 8 KiB
                            lines_changed_set=(len(A.wlines) if (A.wlo >> 6) % 128 != (B.wlo >> 6) % 128
                                               else 0) if wsA == wsB else None,
                            byline=[(A.byline['lines'], A.byline['sets']), (B.byline['lines'], B.byline['sets'])])
    placed_same = (A.lo & 8191) == (B.lo & 8191) and woA == woB and wsA == wsB and moved_sets == 0
    rc = 0
    if res['window']['state'] == 'CHANGED' or (body_strict and res['body']['state'] == 'CHANGED'):
        rc = 1
    elif not placed_same:
        rc = 3
    res['rc'] = rc
    # report
    print('compare:')
    print('  pass 1 loop window: %s%s' % (res['window']['state'],
                                          ' (%d differing words)' % len(win['bad']) if win['bad'] else ''))
    for d in res['window']['diffs'][:None if v else 12]:
        print('     +%-6d %s  %-15s %-58s %s' % (d['off'], d['addr'], d['klass'], d['where'], d['why']))
    if kl:
        print('     differing words by owner: %s' % kl)
    print('  pass 2 main body:   %s%s' % (res['body']['state'], ' (%d differing words, %d outside the window)' % (
        len(res['body']['diffs']), res['body'].get('outside_window', 0)) if res['body']['diffs'] else ''))
    if res['body']['state'] == 'CHANGED' and v:
        for d in res['body']['diffs'][:40]:
            print('     +%-6d %s  %-58s %s' % (d['off'], d['addr'], d['where'], d['why']))
    p = res['placement']
    print('  placement: main mod 8192 %d -> %d; window off %d -> %d, %d -> %d B, window mod 8192 %d -> %d; '
          '%d sets changed occupancy, %s window lines changed set' % (
              p['main_mod_8192'][0], p['main_mod_8192'][1], woA, woB, wsA, wsB,
              p['window_mod_8192'][0], p['window_mod_8192'][1], moved_sets, p['lines_changed_set']))
    print('loop_diff: %s' % {0: 'LOOP SAME', 1: 'LOOP CHANGED', 3: 'LOOP SAME CODE, PLACEMENT MOVED'}[rc])
    return res


def parse_range(s):
    a, b = s.split('-')
    return int(a), int(b)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--selftest' in argv:
        import subprocess
        return subprocess.call([sys.executable, os.path.join(HERE, 'tests', 'elf-func_test_loop_diff.py')])
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('elfs', nargs='+')
    ap.add_argument('--func', default='main'); ap.add_argument('--file', default='main_psp.c')
    ap.add_argument('--loop-lines', default=DEFAULT_LOOP)
    ap.add_argument('--body-strict', action='store_true')
    ap.add_argument('--any-flavour', action='store_true')
    ap.add_argument('--json'); ap.add_argument('-v', action='store_true')
    o = ap.parse_args(argv)
    if len(o.elfs) > 2:
        ap.error('one or two ELFs')
    for p in o.elfs:
        if not os.path.exists(p):
            print('loop_diff: no such ELF: %s' % p); return 2
    if len(o.elfs) == 2 and not o.any_flavour:
        sys.path.insert(0, HERE)
        from func_diff import manifest
        fa, fb = manifest(o.elfs[0]), manifest(o.elfs[1])
        if fa and fb and fa != fb:
            print('loop_diff: REFUSED: flavour %s vs %s (--any-flavour to override)' % (fa, fb))
            return 2
    try:
        Ls = [Loop(p, o.func, o.file, parse_range(o.loop_lines)) for p in o.elfs]
    except (LookupError, ValueError, KeyError) as ex:
        print('loop_diff: %s' % ex); return 2
    out = dict(elfs=[print_one(L, o.v) for L in Ls])
    rc = 0
    if len(Ls) == 2:
        r = compare(Ls[0], Ls[1], o.body_strict, o.v)
        out['compare'] = r; rc = r['rc']
    if o.json:
        with open(o.json, 'w') as fh:
            json.dump(out, fh, indent=1)
    return rc


if __name__ == '__main__':
    sys.exit(main())
