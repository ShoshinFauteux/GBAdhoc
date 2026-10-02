#!/usr/bin/env python3
"""gen_layout.py -- the pinned hot-path layout of the PSP EBOOT
(docs/LAYOUT-PINNING.md).

  gen_layout.py report   PROFILE_ARGS
      where the I-cache traffic is: per fixture, the static hot footprint
      (PSP bytes covering 50/90/95/99 % of placed static fetches, by line and
      by whole function), the emitted stub area's, and the translated code's.

  gen_layout.py weights  PROFILE_ARGS --out tools/layout/profile_weights.txt
      the committed profile: per-PSP-function weights from the twin runs.

  gen_layout.py generate --weights tools/layout/profile_weights.txt
                         --psp-nm ELF.nm --asm-nm mips_stub.nm
                         --out-order psp/layout/hot_order.txt
                         --out-ld psp/layout/hot.ord [--hot-budget BYTES]
      choose the order from the weights and THIS build's function sizes:
        hot_order.txt  one function per line with weight and size (committed:
                       the reviewable record of the choice)
        hot.ord        the --section-ordering-file psp/Makefile links with
                       (committed: generated from hot_order.txt, never edited)
      tools/layout/regen.sh does the whole thing from a source tree.

  gen_layout.py ord --order psp/layout/hot_order.txt --out-ld psp/layout/hot.ord
      regenerate hot.ord from a (possibly hand-reviewed) hot_order.txt.

PROFILE_ARGS: --twin-nm/--twin-members (the twin binary's nm and archive
member listing, tools/layout/twin_build.sh), --runs DIR... (twin_sim.sh run
directories: counts.txt + run.info), --psp-nm (psp-nm -n -S of a
LAYOUT_PIN=1 ELF -- the sizes of the functions as -ffunction-sections builds
them), --frame-path (tools/layout/psp_frame_path.txt).

WHY A WHOLE-FUNCTION ORDER AND NOT A LINE ORDER.  The linker moves sections;
-ffunction-sections makes every function one.  Hot code inside a giant
function (translate_block_arm is ~360 KiB: the whole ARM emitter inlined)
cannot be pulled out without changing the code, so such functions are left
where they fall; translation is the cold path of every scene that matters
except H&S's SMC storm, and that one is capacity-bound anyway.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp_map  # noqa: E402

LINE = 64
WAY = 8192

# Assembly with no per-function sections: placed as whole input sections by
# object name (archive:member).  The dispatcher, update_gba's entry, the
# mode-switch and SMC write paths all live here; it is one section of a few
# KiB, all of it per-frame code.
ASM_OBJECTS = [('mips_stub.o', 'gpsp_libretro_psp1.a:mips_stub.o(.text)')]


def profile_args(ap):
    ap.add_argument('--twin-nm', required=True)
    ap.add_argument('--twin-members')
    ap.add_argument('--runs', nargs='+', required=True)
    ap.add_argument('--psp-nm', required=True)
    ap.add_argument('--frame-path')


def load(a):
    runs = [(os.path.join(r, 'counts.txt'), os.path.join(r, 'run.info'))
            for r in a.runs]
    w, stats, psyms = lp_map.weights(a.twin_nm, a.twin_members, runs,
                                     a.psp_nm, a.frame_path)
    return w, stats, psyms


def footprint(sorted_weights, total, sizes, fracs=(0.5, 0.9, 0.95, 0.99)):
    out = {}
    acc = 0.0
    size = 0
    it = iter(sorted_weights)
    for f in fracs:
        while acc < f * total:
            try:
                k, v = next(it)
            except StopIteration:
                break
            acc += v
            size += sizes(k)
        out[f] = size
    return out


def cmd_report(a):
    w, stats, psyms = load(a)
    for s in stats:
        name = os.path.basename(os.path.dirname(s['counts']))
        fr = s['frames'] or 1
        placed, static = s['placed'], s['static']
        lines = collections.Counter()
        for ad, pa in placed.items():
            lines[pa // LINE] += static[ad]
        tot = sum(lines.values())
        fp_line = footprint(lines.most_common(), tot, lambda k: LINE)
        fns = collections.Counter()
        for ad, pa in placed.items():
            fns[psyms.at(pa)[2]] += static[ad]
        dens = sorted(fns.items(),
                      key=lambda kv: -kv[1] / max(psyms.unique(kv[0])[1], 4))
        fp_fn = footprint(dens, tot, lambda k: psyms.unique(k)[1])
        c, _ = lp_map.read_counts(s['counts'])
        jr = lp_map.jit_range(os.path.join(os.path.dirname(s['counts']),
                                           'run.info'))
        stub = collections.Counter()
        xl = collections.Counter()
        for ad, n in c.items():
            if jr and jr[0] <= ad < jr[1]:
                off = ad - jr[0]
                (stub if off < a.stub_bytes else xl)[off // LINE] += n
        st, xt = sum(stub.values()), sum(xl.values())
        fp_stub = footprint(stub.most_common(), st, lambda k: LINE)
        fp_xl = footprint(xl.most_common(), xt, lambda k: LINE)
        print('== %s: %d frames; per frame: static %.0fk placed (%.0fk lost),'
              ' stub area %.0fk, translated %.0fk instructions'
              % (name, fr, tot / fr / 1e3, s['static_lost'] / fr / 1e3,
                 st / fr / 1e3, xt / fr / 1e3))
        for lab, fp in (('static, by line', fp_line),
                        ('static, whole functions by density', fp_fn),
                        ('emitted stub area, by line', fp_stub),
                        ('translated code, by line', fp_xl)):
            print('   %-36s ' % lab + '  '.join(
                '%d%%=%.1fK' % (f * 100, v / 1024) for f, v in fp.items()))
        print('   top lost: ' + ', '.join('%s=%.0fk' % (k, v / fr / 1e3)
                                         for k, v in s['top_lost'][:5]))
    print('== combined weights (each fixture = 1.0), top %d:' % a.top)
    for k, v in w.most_common(a.top):
        r = psyms.unique(k)
        print('   %-44s %8.4f %7d  dens %.2e' % (k, v, r[1] if r else 0,
                                                v / max(r[1] if r else 4, 4)))


def cmd_weights(a):
    """Write the committed profile: per-PSP-function weights from the twin."""
    w, stats, psyms = load(a)
    with open(a.out, 'w', newline='\n') as fh:
        fh.write('# profile_weights.txt -- GENERATED by tools/layout/gen_layout.py '
                 'weights\n# (docs/LAYOUT-PINNING.md).  Per PSP function: its share of '
                 'the placed\n# static instruction fetches, each fixture normalised '
                 'to 1.0 and summed,\n# plus the PSP-only per-frame path '
                 '(psp_frame_path.txt).  Fixtures:\n')
        for st in stats:
            fh.write('#   %s (%s frames)\n' % (
                os.path.basename(os.path.dirname(st['counts'])), st['frames']))
        for k, v in sorted(w.items(), key=lambda kv: (-kv[1], kv[0])):
            if v >= a.min_weight:
                fh.write('%s %.7f\n' % (k, v))
    print('wrote %s (%d functions >= %g)' % (
        a.out, sum(1 for v in w.values() if v >= a.min_weight), a.min_weight))


def read_weights(path):
    w = {}
    for l in open(path):
        l = l.split('#', 1)[0].split()
        if len(l) == 2:
            w[l[0]] = float(l[1])
    return w


def choose(w, psyms, asm_names, hot_budget, giant, heads=None):
    """HOT: weight-dense functions, densest first, until hot_budget bytes
    (the asm object is placed before them and is not counted).  WARM: every
    other function with any weight, densest first, functions above `giant`
    bytes last -- pinned too, so nothing on the per-frame path floats.

    heads {name: bytes}: functions of which only the first `bytes` run per
    frame (psp_frame_path.txt's third column -- update_scanline's ME-capture
    head).  Density and budget count the head only, and the densest such
    function that makes the hot set is placed LAST in it, so its head is hot
    and its body spills into the warm tail.  At most one: a second would sit
    whole inside the hot core."""
    heads = heads or {}
    rows = []
    for k, v in w.items():
        r = psyms.unique(k)
        if not r or k in asm_names:
            continue
        eff = min(r[1], heads[k]) if k in heads else r[1]
        rows.append((v / max(eff, 4), v, r[1], r[2], eff))
    rows.sort(key=lambda x: (-x[0], x[3]))
    hot, warm, big = [], [], []
    tailfn = None
    used = 0
    for d, v, size, k, eff in rows:
        # a knapsack by density: a function that does not fit goes to the
        # warm tail and the scan continues with the smaller ones
        if eff < size and tailfn is None and used + eff <= hot_budget:
            tailfn = (k, v, size)
            used += eff
        elif size <= giant and eff == size and used + size <= hot_budget:
            hot.append((k, v, size))
            used += size
        elif size > giant:
            big.append((k, v, size))
        else:
            warm.append((k, v, size))
    if tailfn:
        hot.append(tailfn)
    return hot, warm + big, used


def read_lib_nm(path):
    """name -> 'lib.a:member.o' from psp-nm -A --defined-only of the
    toolchain archives (libc, libgcc, ...).  Those are prebuilt without
    -ffunction-sections, so a library routine is placed by its archive
    member's .text, not by a .text.<name> section that does not exist."""
    out = {}
    if not path:
        return out
    for l in open(path, errors='replace'):
        p = l.split()
        if len(p) >= 3 and p[-2] in 'TW':
            loc = p[0].rsplit(':', 2)
            if len(loc) >= 3:
                out.setdefault(p[-1], '%s:%s' % (os.path.basename(loc[0]),
                                                 loc[1]))
    return out


def write_order(path, hot, warm, used, a, asm_bytes, lib=None):
    lib = lib or {}
    with open(path, 'w', newline='\n') as fh:
        fh.write('# hot_order.txt -- GENERATED by tools/layout/gen_layout.py generate;\n'
                 '# the pinned per-frame path of the PSP EBOOT (docs/LAYOUT-PINNING.md).\n'
                 '# Linked FIRST in .text, in this order, from 0x08804040: the asm\n'
                 '# stubs (@), the HOT core, then the WARM tail.  Do not hand-edit:\n'
                 '# regenerate (tools/layout/regen.sh), which rewrites hot.ord too.\n')
        fh.write('# weights: %s; hot budget %d bytes (used %d) after %d bytes of asm;\n'
                 '# functions over %d bytes go last\n'
                 % (os.path.basename(a.weights), a.hot_budget, used, asm_bytes,
                    a.giant))
        fh.write('# name                                            weight     size\n')
        for o, _ in ASM_OBJECTS:
            fh.write('@%s\n' % o)
        for k, v, size in hot:
            fh.write(('%-48s %9.6f %7d %s' % (k, v, size, lib.get(k, '')))
                     .rstrip() + '\n')
        fh.write('# ---- warm tail\n')
        for k, v, size in warm:
            fh.write(('%-48s %9.6f %7d %s' % (k, v, size, lib.get(k, '')))
                     .rstrip() + '\n')


def write_ord(order_path, out):
    names = []
    for l in open(order_path):
        l = l.split('#', 1)[0].split()
        if l:
            names.append(l[3] if len(l) > 3 else l[0])
    with open(out, 'w', newline='\n') as fh:
        fh.write('/* hot.ord -- GENERATED from hot_order.txt by '
                 'tools/layout/gen_layout.py.\n'
                 ' * psp/Makefile links with -Wl,--section-ordering-file when '
                 'LAYOUT_PIN=1\n'
                 ' * (docs/LAYOUT-PINNING.md).  A name that no longer exists '
                 'matches nothing\n * and costs nothing. */\n')
        fh.write('.text : {\n')
        seen = set()
        for n in names:
            if n in seen:
                continue
            seen.add(n)
            if n.startswith('@'):
                fh.write('  *%s\n' % dict(ASM_OBJECTS)[n[1:]])
            elif ':' in n:          # a prebuilt library member (read_lib_nm)
                # matched in ANY archive: psp-gcc links newlib as libg.a,
                # the same members as the libc.a they were read from
                fh.write('  *:%s(.text .text.*)\n' % n.split(':', 1)[1])
            else:
                fh.write('  *(.text.%s)\n' % n)
        fh.write('}\n')
    print('wrote %s (%d entries)' % (out, len(names)))


def cmd_generate(a):
    w = read_weights(a.weights)
    psyms = lp_map.Syms(lp_map.read_nm(a.psp_nm))
    asm = lp_map.read_nm(a.asm_nm)
    asm_names = {r[2] for r in asm}
    asm_bytes = (max(r[0] + r[1] for r in asm) - min(r[0] for r in asm)) if asm else 0
    heads = {}
    if a.frame_path:
        for name, calls, nbytes, grp in lp_map.read_frame_path(a.frame_path):
            if nbytes:
                heads[name] = nbytes
    hot, warm, used = choose(w, psyms, asm_names, a.hot_budget, a.giant, heads)
    write_order(a.out_order, hot, warm, used, a, asm_bytes,
                read_lib_nm(a.lib_nm))
    print('hot: %d functions, %d bytes (+%d asm); warm: %d functions, %d bytes'
          % (len(hot), used, asm_bytes, len(warm), sum(x[2] for x in warm)))
    write_ord(a.out_order, a.out_ld)


def cmd_ord(a):
    write_ord(a.order, a.out_ld)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('report')
    profile_args(r)
    r.add_argument('--stub-bytes', type=lambda x: int(x, 0), default=0x3000)
    r.add_argument('--top', type=int, default=60)
    wp = sub.add_parser('weights')
    profile_args(wp)
    wp.add_argument('--min-weight', type=float, default=1e-6)
    wp.add_argument('--out', required=True)
    g = sub.add_parser('generate')
    g.add_argument('--weights', required=True)
    g.add_argument('--psp-nm', required=True,
                   help='psp-nm -n -S of a LAYOUT_PIN=1 ELF (function sizes)')
    g.add_argument('--asm-nm', required=True,
                   help='psp-nm -S of the core archive member mips_stub.o')
    g.add_argument('--frame-path', help='psp_frame_path.txt (head-hot functions)')
    g.add_argument('--lib-nm', help='psp-nm -A --defined-only of libc.a, '
                   'libgcc.a, ... (places library routines by member)')
    g.add_argument('--hot-budget', type=int, default=6 * 1024)
    g.add_argument('--giant', type=int, default=16 * 1024)
    g.add_argument('--out-order', required=True)
    g.add_argument('--out-ld', required=True)
    o = sub.add_parser('ord')
    o.add_argument('--order', required=True)
    o.add_argument('--out-ld', required=True)
    a = ap.parse_args()
    {'report': cmd_report, 'weights': cmd_weights, 'generate': cmd_generate,
     'ord': cmd_ord}[a.cmd](a)


if __name__ == '__main__':
    main()
