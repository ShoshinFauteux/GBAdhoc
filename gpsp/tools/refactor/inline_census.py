#!/usr/bin/env python3
"""inline_census.py -- T13 / gate IC (builds/mainpsp-map/out/PLAN.md §0.1, §5).

Per function DEFINED in a source file (main_psp.c by default): is it compiled
in this flavour, emitted out of line (and under which symbols: plain, or
.isra/.part/.constprop clones), and inlined into which callers?  Sources:
  * the source scan (ef_srcscan.py, the x4_src.py mini preprocessor) for the
    definitions and whether each is live in the flavour;
  * the ELF symtab (nm): a local whose STT_FILE is the source file, or a global
    whose base name is a source function, is an emitted symbol (x4_map.py's
    selection, so x4's numbers reproduce);
  * DWARF DW_TAG_inlined_subroutine in the file's CU: every inline instance,
    its abstract origin (the inlined function), the concrete function whose
    code holds it (the "caller" an ELF symbol names), the immediate inline
    parent when nested, its call line and its bytes.
Status per live function:
  emitted          a plain symbol, no inline instance
  emitted+inlined  a plain symbol AND inline instances (partly inlined)
  clone            only clone symbols (foo.part.0 ...), perhaps instances too
  inlined          no symbol, inline instances only
  dead             no symbol and no instance (removed as unreferenced)
and `compiled-out` when the definition is not live in the flavour.

usage:
  inline_census.py ELF --src main_psp.c [--flavour release|lab|harness]
                   [--file main_psp.c] [--json OUT] [-v] [--list]
  inline_census.py ELF --src ... --against BASE.elf [--src-base OTHER.c]
                   [--allow fn,...]     report every function whose status,
                                        symbols or caller set changed (IC gate)
  inline_census.py --selftest
exit 0 ok / no change, 1 a change not allowed, 2 usage
--flavour defaults to the ELF's flavour.json / build-manifest.json.
What it does NOT prove: that code is the same (func_diff.py does); inlining
in OTHER translation units (only the source file's CU is read, with its #included
.c/.inc fragments, PLAN §1.1; a global
inlined elsewhere by LTO would not show); the source scan does not read
headers, so a #if on a header-only macro is taken as undefined (§ ef_srcscan).
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from elf32 import Elf                       # noqa: E402
import ef_dwarf as D                        # noqa: E402
import ef_srcscan as S                      # noqa: E402

CLONE = re.compile(r'\.(isra|part|constprop|cold|lto_priv)\.\d+.*$')
CLONE_X4 = re.compile(r'\.(isra|part|constprop)\.\d+$')


def base_of(name):
    b = CLONE.sub('', name)
    return CLONE_X4.sub('', b)


def clone_kind(name):
    m = re.search(r'\.(isra|part|constprop|cold|lto_priv)\b', name)
    return m.group(1) if m else None


def own_file_test(srcfile):
    """f (a DWARF file name of srcfile's CU) is the CU's own text: srcfile itself or a
    *.inc.c fragment (they exist only as part of one TU, PLAN-P-beta5 §5)."""
    return lambda f: f is not None and (f == srcfile or f.endswith('/' + srcfile) or f.endswith('.inc.c'))


def census(elf_path, src_path, flavour, srcfile='main_psp.c'):
    e = Elf(elf_path)
    src = S.scan(src_path, (flavour,))
    funcs = src['functions']
    srcf = {}
    for f in funcs:
        srcf.setdefault(f['name'], f)          # a name defined twice (#if/#else) counts once for lookup
    live_names = {f['name'] for f in funcs if f[flavour]['active_at_open']}
    # ---- symbols (x4_map.py selection)
    syms = []
    for s in e.funcs():
        b = base_of(s['name'])
        if s['file'] == srcfile or (s['bind'] != 0 and b in srcf):
            syms.append(dict(name=s['name'], base=b, addr=s['value'], size=s['size'],
                             bind='L' if s['bind'] == 0 else 'G', clone=clone_kind(s['name'])))
    by_base = {}
    for s in syms:
        by_base.setdefault(s['base'], []).append(s)
    fsyms = sorted((s['value'], s['value'] + s['size'], s['name']) for s in e.funcs())

    def sym_at(a):
        import bisect
        k = bisect.bisect_right(fsyms, (a, 1 << 40, '')) - 1
        while k >= 0 and fsyms[k][0] <= a:
            if a < fsyms[k][1]:
                return fsyms[k][2]
            k -= 1
        return None
    # ---- DWARF inline instances in the file's CU
    info = D.Info(e)
    cu = info.cu_named(srcfile)
    # attributed by COMPILE UNIT (adversarial review 2026-11-07): a function of
    # main_psp.o's own text is declared in main_psp.c OR in a *.inc.c fragment that
    # main_psp.c #includes (P4); matching decl_file to main_psp.c alone dropped T13
    # on a real split from 120 inlined / 7 dead to 1 / 126.  Headers stay excluded.
    isrc = own_file_test(srcfile)
    inst = {}
    for x in cu.inlined():
        if not x['name']:
            continue
        nb = sum(b - a for a, b in x['ranges'])
        rec = inst.setdefault(x['name'], dict(instances=0, bytes=0, callers={}, nested=0, from_src=False))
        rec['instances'] += 1
        rec['bytes'] += nb
        rec['from_src'] = rec['from_src'] or isrc(x['decl_file'])
        if x['depth'] > 0:
            rec['nested'] += 1
        # the caller is named by the ELF symbol that holds the container's
        # code (lat_swap.isra.0, not the DWARF name lat_swap)
        c = x['container_name'] or '?'
        if x['container_ranges']:
            c = sym_at(x['container_ranges'][0][0]) or c
        cr = rec['callers'].setdefault(c, dict(instances=0, bytes=0, via=set()))
        cr['instances'] += 1; cr['bytes'] += nb
        if x['inline_parent_name']:
            cr['via'].add(x['inline_parent_name'])
    for rec in inst.values():
        for cr in rec['callers'].values():
            cr['via'] = sorted(cr['via'])
    # concrete function DIE -> symbol name (the container name is the DWARF
    # name; a clone's concrete DIE carries the base name, so say which symbol)
    rows = []
    seen = set()
    for f in funcs:
        nm = f['name']
        if nm in seen:
            continue                            # second #if-alternative definition: one row per name
        seen.add(nm)
        live = nm in live_names
        ss = by_base.get(nm, [])
        ins = inst.get(nm) if nm in inst and inst[nm]['from_src'] else None
        plain = [s for s in ss if not s['clone']]
        clones = [s for s in ss if s['clone']]
        if not live and not ss and not ins:
            status = 'compiled-out'
        elif plain:
            status = 'emitted+inlined' if ins else 'emitted'
        elif clones:
            status = 'clone'
        elif ins:
            status = 'inlined'
        else:
            status = 'dead'
        rows.append(dict(name=nm, line=f.get('src_line', f['start']), src_file=f.get('src_file'), live=live, status=status,
                         symbols=[s['name'] for s in ss], clone_kinds=sorted({s['clone'] for s in clones}),
                         global_=any(s['bind'] == 'G' for s in ss), text_bytes=sum(s['size'] for s in ss),
                         inline=dict(instances=ins['instances'], bytes=ins['bytes'], nested=ins['nested'],
                                     callers=ins['callers']) if ins else None))
    orphans = [s['name'] for s in syms if s['base'] not in srcf]
    live_rows = [r for r in rows if r['live']]
    nos = [r for r in live_rows if not r['symbols']]
    summ = dict(
        flavour=flavour, definitions=len(funcs), live=sum(1 for f in funcs if f[flavour]['active_at_open']),
        compiled_out=sum(1 for f in funcs if not f[flavour]['active_at_open']),
        live_without_symbol=sum(1 for f in funcs if f[flavour]['active_at_open'] and f['name'] not in by_base),
        live_without_symbol_inlined=sum(1 for r in nos if r['inline']),
        live_without_symbol_dead=sum(1 for r in nos if not r['inline']),
        symbols=len(syms), clone_symbols=sum(1 for s in syms if s['clone']),
        global_symbols=sum(1 for s in syms if s['bind'] == 'G'),
        text_bytes=sum(s['size'] for s in syms),
        functions_with_symbol=len(by_base),
        status={k: sum(1 for r in rows if r['status'] == k) for k in
                ('emitted', 'emitted+inlined', 'clone', 'inlined', 'dead', 'compiled-out')},
        inline_instances=sum(r['inline']['instances'] for r in rows if r['inline']),
        inline_bytes_top_level=sum(cr['bytes'] for r in rows if r['inline']
                                   for cr in r['inline']['callers'].values()),
        orphan_symbols=orphans,
        # the source scan says "not live" but the ELF has code for it: the
        # scanner is wrong there (it does not read headers), never the ELF
        scan_disagreements=[r['name'] for r in rows if not r['live'] and r['status'] != 'compiled-out'],
        duplicate_definitions=sorted({f['name'] for f in funcs if sum(1 for g in funcs if g['name'] == f['name']) > 1}))
    return dict(elf=elf_path, src=src_path, summary=summ, functions=rows)


def compare(base, cand, allow):
    bb = {r['name']: r for r in base['functions']}
    cc = {r['name']: r for r in cand['functions']}
    changes = []
    for nm in sorted(set(bb) | set(cc)):
        a, b = bb.get(nm), cc.get(nm)
        why = []
        if a is None or b is None:
            why.append('definition %s' % ('added' if a is None else 'removed'))
        else:
            if a['status'] != b['status']:
                why.append('status %s -> %s' % (a['status'], b['status']))
            sa = sorted(re.sub(r'\.\d+$', '', s) for s in a['symbols'])
            sb = sorted(re.sub(r'\.\d+$', '', s) for s in b['symbols'])
            if sa != sb:
                why.append('symbols %s -> %s' % (sa, sb))
            ca = set(a['inline']['callers']) if a['inline'] else set()
            cb = set(b['inline']['callers']) if b['inline'] else set()
            if ca != cb:
                why.append('callers -%s +%s' % (sorted(ca - cb), sorted(cb - ca)))
            ia = a['inline']['instances'] if a['inline'] else 0
            ib = b['inline']['instances'] if b['inline'] else 0
            if ia != ib:
                why.append('inline instances %d -> %d' % (ia, ib))
        if why:
            changes.append(dict(name=nm, why=why, allowed=nm in allow))
    oa, ob = set(base['summary']['orphan_symbols']), set(cand['summary']['orphan_symbols'])
    for o in sorted(oa ^ ob):
        changes.append(dict(name=o, why=['orphan symbol %s' % ('added' if o in ob else 'removed')],
                            allowed=o in allow))
    return changes


def manifest_flavour(p):
    d = os.path.dirname(os.path.abspath(p))
    for fn in ('flavour.json', 'build-manifest.json'):
        q = os.path.join(d, fn)
        if os.path.exists(q):
            try:
                j = json.load(open(q))
                fl = j.get('flavour') or j.get('profile')
                if fl in S.CONFIGS:
                    return fl
            except Exception:
                pass
    return None


def print_summary(c):
    s = c['summary']
    print('inline_census: %s (%s)' % (c['elf'], s['flavour']))
    print('  %d definitions: %d live, %d compiled out' % (s['definitions'], s['live'], s['compiled_out']))
    print('  %d live with no symbol: %d inlined (DWARF instances), %d dead (no instance)' % (
        s['live_without_symbol'], s['live_without_symbol_inlined'], s['live_without_symbol_dead']))
    print('  %d emitted symbols (%d clones, %d global), %d B of .text, for %d functions' % (
        s['symbols'], s['clone_symbols'], s['global_symbols'], s['text_bytes'], s['functions_with_symbol']))
    print('  status: %s' % s['status'])
    print('  %d inline instances, %d B in their containers' % (s['inline_instances'], s['inline_bytes_top_level']))
    if s['orphan_symbols']:
        print('  symbols with no definition in the scanned file (header functions): %s' % ', '.join(
            s['orphan_symbols'][:12]))
    if s['duplicate_definitions']:
        print('  names defined more than once (#if alternatives, one row each): %s' % ', '.join(
            s['duplicate_definitions']))
    print('  source-scan disagreements (not live in the scan, code in the ELF): %d%s' % (
        len(s['scan_disagreements']), (' ' + ', '.join(s['scan_disagreements'])) if s['scan_disagreements'] else ''))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--selftest' in argv:
        import subprocess
        return subprocess.call([sys.executable, os.path.join(HERE, 'tests', 'elf-func_test_inline_census.py')])
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('elf')
    ap.add_argument('--src', required=True, help='the source file the ELF was built from')
    ap.add_argument('--file', default=None, help='CU / STT_FILE name (default: basename of --src)')
    ap.add_argument('--flavour', choices=sorted(S.CONFIGS))
    ap.add_argument('--against'); ap.add_argument('--src-base')
    ap.add_argument('--allow', default='')
    ap.add_argument('--list', action='store_true', help='one line per function')
    ap.add_argument('--json'); ap.add_argument('-v', action='store_true')
    o = ap.parse_args(argv)
    for p in [o.elf, o.src] + ([o.against] if o.against else []):
        if not os.path.exists(p):
            print('inline_census: no such file: %s' % p); return 2
    fl = o.flavour or manifest_flavour(o.elf)
    if not fl:
        print('inline_census: --flavour needed (no manifest next to the ELF)'); return 2
    fname = o.file or os.path.basename(o.src)
    c = census(o.elf, o.src, fl, fname)
    print_summary(c)
    if o.list:
        for r in c['functions']:
            inl = r['inline']
            print('  %-34s L:%-5d %-15s %-40s %s' % (
                r['name'], r['line'], r['status'], ','.join(r['symbols'])[:40],
                '' if not inl else 'inlined x%d (%d B) into %s' % (
                    inl['instances'], inl['bytes'],
                    ', '.join('%s x%d' % (k, v['instances']) for k, v in sorted(inl['callers'].items())))))
    out = dict(census=c)
    rc = 0
    if o.against:
        flb = manifest_flavour(o.against) or fl
        if flb != fl:
            print('inline_census: REFUSED: flavour %s vs %s' % (flb, fl)); return 2
        b = census(o.against, o.src_base or o.src, fl, fname)
        allow = set(x for x in o.allow.split(',') if x)
        ch = compare(b, c, allow)
        out['against'] = o.against; out['changes'] = ch
        bad = [x for x in ch if not x['allowed']]
        for x in ch[:None if o.v else 30]:
            print('  %s%s: %s' % ('[allowed] ' if x['allowed'] else '', x['name'], '; '.join(x['why'])))
        print('inline_census: %d changes against %s (%d not allowed)' % (len(ch), o.against, len(bad)))
        rc = 1 if bad else 0
    if o.json:
        with open(o.json, 'w') as fh:
            json.dump(out, fh, indent=1)
    return rc


if __name__ == '__main__':
    sys.exit(main())
