#!/usr/bin/env python3
"""pad_check.py -- does unrelated code move the hot path?  (docs/LAYOUT-PINNING.md)

  pad_check.py --order psp/layout/hot_order.txt --asm-nm mips_stub.nm
               REF=ref/elf.nm NAME=pad/elf.nm [NAME=...]

The hot path is every function hot_order.txt lists (the HOT core and the
warm tail) plus every symbol of mips_stub.o.  For each variant against REF:
how many hot-path functions moved, by how much, and how many of the hot
core's 64-byte lines changed I-cache set (address bits 6..12); where the
SMALL translation-cache tier starts mod 8 KiB; and how far .text's end and
.bss's end (where the heap -- and with it the LARGE tier -- begins) moved.
Exit 1 if --expect-pinned and any hot-core function or the JIT set moved.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp_map  # noqa: E402

WAY = 8192


def all_syms(path):
    d = {}
    for l in open(path, errors='replace'):
        p = l.split()
        if len(p) >= 3:
            d.setdefault(p[-1], int(p[0], 16))
    return d


def order_names(path):
    hot, warm, cur = [], [], None
    cur = hot
    for l in open(path):
        if l.startswith('# ---- warm'):
            cur = warm
            continue
        l = l.split('#', 1)[0].split()
        if l and not l[0].startswith('@'):
            cur.append(l[0])
    return hot, warm


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--order', required=True)
    ap.add_argument('--asm-nm', required=True)
    ap.add_argument('--expect-pinned', action='store_true')
    ap.add_argument('elfs', nargs='+')
    a = ap.parse_args()
    hot, warm = order_names(a.order)
    asm = [r[2] for r in lp_map.read_nm(a.asm_nm)]
    core = asm + hot
    path = core + warm
    pairs = [e.split('=', 1) for e in a.elfs]
    ref_name, ref_path = pairs[0]
    ref = all_syms(ref_path)
    rsz = {r[2]: r for r in lp_map.read_nm(ref_path)}
    core_lines = 0
    for n in core:
        if n in rsz:
            core_lines += (rsz[n][1] + 63) // 64
    print('hot core: %d functions (%d asm), %d lines; warm tail: %d functions'
          % (len(core), len(asm), core_lines, len(warm)))
    print('%-14s %8s %8s %10s %10s %9s %9s %9s'
          % ('variant', 'core mv', 'warm mv', 'core sets', 'jit mod8K',
             'd(etext)', 'd(end)', 'max mv'))
    bad = 0
    for name, p in pairs:
        s = all_syms(p)
        mv_core = [n for n in core if n in s and n in ref and s[n] != ref[n]]
        mv_warm = [n for n in warm if n in s and n in ref and s[n] != ref[n]]
        set_ch = 0
        for n in core:
            if n in s and n in ref and n in rsz:
                for k in range((rsz[n][1] + 63) // 64):
                    if ((s[n] + 64 * k) // 64) % 128 != ((ref[n] + 64 * k) // 64) % 128:
                        set_ch += 1
        jit = s.get('rom_translation_cache_static')
        dmax = max([abs(s[n] - ref[n]) for n in path if n in s and n in ref] or [0])
        print('%-14s %8d %8d %6d/%-3d %10s %+9d %+9d %9d' % (
            name, len(mv_core), len(mv_warm), set_ch, core_lines,
            '0x%04x' % (jit % WAY) if jit else '-',
            s.get('_etext', 0) - ref.get('_etext', 0),
            s.get('_end', 0) - ref.get('_end', 0), dmax))
        if a.expect_pinned and (mv_core or set_ch or
                                (jit and jit % WAY != ref['rom_translation_cache_static'] % WAY)):
            bad = 1
    sys.exit(bad)


if __name__ == '__main__':
    main()
