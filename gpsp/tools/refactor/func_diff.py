#!/usr/bin/env python3
"""func_diff.py -- T1 / gate G2 (gpsp/docs/design/RESTRUCTURE.md §0.1, §5).

Prove that two linked PSP ELFs run the same code, function by function,
modulo WHERE things were linked.  No relocations survive in the EBOOT's ELF,
so address operands are rebuilt from the instruction stream (ef_mips.py):
j/jal targets, %hi/%lo pairs (incl. VFPU lv/sv), pointer words in data, and
clone/static number suffixes in names.  Everything else (opcodes, registers,
PC-relative branch offsets, other immediates) must match bit for bit.

Every function is one of
  same-bytes        identical bytes at the identical address
  same-mod-address  identical once address operands are resolved to symbols
  different         anything else (first differing word and why are printed)
  only-in-A / only-in-B   the symbol vanished / appeared (inlining, rename)
Functions are keyed by (canonical name, STT_FILE for locals, occurrence).

usage:
  func_diff.py BASE.elf CAND.elf
      [--file main_psp.c[,f2]] [--names a,b]   scope (default: every function)
      [--allow fn,...] [--allow-file F]        functions allowed to differ /
                                               vanish / appear (one per line,
                                               '#' comments; a reason is welcome)
      [--hot psp/layout/hot_order.txt|hot.ord] I-cache set report of every hot
                                               function (G2's second half)
      [--hot-strict]                           a hot set change fails (exit 3)
      [--data]                                 also compare data (sized + anonymous)
      [--strict-weak]                          a WEAK hi/lo pairing fails
      [--any-flavour]                          allow release vs lab (refused
                                               by default when manifests exist)
      [--json OUT] [-v]
      --selftest                               run tests/elf-func_test_func_diff.py

exit 0: equivalent (all differences allowed)  1: a difference not allowed
     2: usage / flavour mismatch                3: --hot-strict and a hot
                                                   function changed I-cache set
What it does NOT prove: that the code BEHAVES the same when data layout moved
(cache/timing effects: G3/LP/G5 cover those); data is compared only with
--data: sized STT_OBJECT symbols, plus (unscoped runs) every anonymous gap of
a non-code PROGBITS section (string literals, switch tables, float constants;
without it an edited literal behind byte-identical code was EQUIVALENT); a
WEAK pairing could in
theory accept an immediate change that equals a layout shift (each weak site
is listed; --strict-weak refuses them).
"""
import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from elf32 import Elf                       # noqa: E402
import ef_mips as M                         # noqa: E402


def keyed(elf, kinds, filt):
    """canonical key -> symbol (duplicates get #n by address order)."""
    d = {}
    for s in sorted(elf.symbols, key=lambda s: (s['value'], s['name'])):
        if s['type'] not in kinds or s['size'] == 0 or s['shndx'] in (0, 0xfff1):
            continue
        if filt and not filt(s):
            continue
        base = M.canon(s['name']) + ('@' + s['file'] if s['file'] else '')
        k = base; n = 1
        while k in d:
            n += 1; k = '%s#%d' % (base, n)
        d[k] = s
    return d


def read_hot(path):
    """names from hot_order.txt (first column, '@obj' lines skipped) or
    hot.ord (`*(.text.NAME)` patterns)."""
    names = []
    txt = open(path).read()
    if '.text' in txt and ('*(' in txt or 'KEEP' in txt):
        for m in re.finditer(r'\.text\.([A-Za-z_][\w.$]*)', txt):
            if m.group(1) not in names:
                names.append(m.group(1))
        return names
    for ln in txt.splitlines():
        ln = ln.strip()
        if not ln or ln.startswith('#') or ln.startswith('@'):
            continue
        names.append(ln.split()[0])
    return names


def read_allow(args):
    allow = {}
    for x in (args.allow or '').split(','):
        if x.strip():
            allow[x.strip()] = 'cli'
    if args.allow_file:
        for ln in open(args.allow_file):
            ln = ln.split('#', 1)[0].strip()
            if ln:
                nm = ln.split()[0]
                allow[nm] = ln[len(nm):].strip() or 'allow-file'
    return allow


def allowed(key, allow):
    """an allow entry matches the key, its canonical name, or its base name
    (`foo` allows `foo.isra@main_psp.c#2`)."""
    nm = key.split('@', 1)[0].split('#', 1)[0]
    for cand in (key, nm, M.base_name(nm)):
        if cand in allow:
            return cand
    return None


def manifest(elf_path):
    d = os.path.dirname(os.path.abspath(elf_path))
    for fn in ('flavour.json', 'build-manifest.json'):
        p = os.path.join(d, fn)
        if os.path.exists(p):
            try:
                j = json.load(open(p))
            except Exception:
                continue
            return j.get('flavour') or j.get('profile')
    return None


def run(o):
    t0 = time.time()
    fa_m, fb_m = manifest(o.a), manifest(o.b)
    if fa_m and fb_m and fa_m != fb_m and not o.any_flavour:
        print('func_diff: REFUSED: flavour %s vs %s (release is compared with release, '
              'Lab with Lab; --any-flavour to override)' % (fa_m, fb_m))
        return 2, None
    A, B = Elf(o.a), Elf(o.b)
    SA, SB = M.Space(A), M.Space(B)
    files = set(x for x in (o.file or '').split(',') if x)
    names = set(x for x in (o.names or '').split(',') if x)
    scoped = bool(files or names)

    def filt(s):
        if not scoped:
            return True
        if files and s['file'] in files:
            return True
        return M.canon(s['name']) in names or M.base_name(s['name']) in names
    allow = read_allow(o)
    fa, fb = keyed(A, (2,), filt), keyed(B, (2,), filt)
    stat = dict(functions=0, same_bytes=0, same_mod_addr=0, different=0,
                only_a=0, only_b=0, allowed=0, addr_words=0, weak_sites=0)
    moved = {}
    findings = []          # dict(kind, key, detail, allowed)
    weak_sites = []
    for k in sorted(set(fa) | set(fb)):
        stat['functions'] += 1
        if k not in fb or k not in fa:
            kind = 'only_in_A' if k not in fb else 'only_in_B'
            stat['only_a' if k not in fb else 'only_b'] += 1
            s = fa.get(k) or fb.get(k)
            findings.append(dict(kind=kind, key=k, detail='%08x size %d' % (s['value'], s['size']),
                                 allowed=allowed(k, allow)))
            continue
        sa, sb = fa[k], fb[k]
        ca, cb = A.read(sa['value'], sa['size']), B.read(sb['value'], sb['size'])
        d = sb['value'] - sa['value']
        moved[d] = moved.get(d, 0) + 1
        if ca == cb and d == 0:
            stat['same_bytes'] += 1
            continue
        if ca is None or cb is None:
            r = dict(ok=False, bad=[(0, 'not in a PROGBITS section')], weak=[], naddr=0)
        else:
            r = M.compare_words(SA, SB, sa['value'], sb['value'], ca, cb, stop_first=True)
        stat['addr_words'] += r['naddr']
        for w in r['weak']:
            weak_sites.append('%s+%d' % (k, 4 * w))
        if r['ok'] and not (o.strict_weak and r['weak']):
            stat['same_mod_addr'] += 1
            continue
        stat['different'] += 1
        if r['ok']:
            j, why = r['weak'][0], 'WEAK hi/lo pairing (--strict-weak)'
        else:
            j, why = r['bad'][0]
        findings.append(dict(kind='different', key=k,
                             detail='size %d->%d first diff at +%d (%08x/%08x): %s' % (
                                 sa['size'], sb['size'], 4 * j, sa['value'] + 4 * j,
                                 sb['value'] + 4 * j, why),
                             allowed=allowed(k, allow)))
    stat['weak_sites'] = len(weak_sites)
    data_stat = None
    if o.data:
        da, db = keyed(A, (1,), filt), keyed(B, (1,), filt)
        data_stat = dict(objects=0, same=0, different=0, bss_same=0, bss_changed=0, only=0)
        for k in sorted(set(da) | set(db)):
            data_stat['objects'] += 1
            if k not in da or k not in db:
                data_stat['only'] += 1
                findings.append(dict(kind='data_only_in_' + ('B' if k in db else 'A'), key=k,
                                     detail='', allowed=allowed(k, allow)))
                continue
            sa, sb = da[k], db[k]
            seca, secb = A.section_of(sa['value']), B.section_of(sb['value'])
            if seca is None or secb is None:
                continue
            if seca['type'] == 8 or secb['type'] == 8:
                if sa['size'] == sb['size'] and seca['name'] == secb['name']:
                    data_stat['bss_same'] += 1
                else:
                    data_stat['bss_changed'] += 1
                    findings.append(dict(kind='bss', key=k, detail='%s/%d -> %s/%d' % (
                        seca['name'], sa['size'], secb['name'], sb['size']), allowed=allowed(k, allow)))
                continue
            ra, rb = A.read(sa['value'], sa['size']), B.read(sb['value'], sb['size'])
            if ra == rb or M.compare_data(SA, SB, ra, rb):
                data_stat['same'] += 1
            else:
                data_stat['different'] += 1
                findings.append(dict(kind='data', key=k, detail='in %s' % seca['name'],
                                     allowed=allowed(k, allow)))
        # Anonymous data: string literals, switch tables and float constants
        # have no sized symbol, and code that references them at an unchanged
        # address is byte-identical, so neither pass above sees an edited
        # literal (verify mutant: a .rodata string byte flip was EQUIVALENT
        # even with --data).  Each gap between sized symbols in a non-code
        # PROGBITS section is keyed by its anchor and compared modulo pointers.
        if not scoped:
            ga, gb = M.anon_gaps(SA), M.anon_gaps(SB)
            data_stat.update(anon_gaps=0, anon_same=0, anon_different=0, anon_only=0)
            for k in sorted(set(ga) | set(gb)):
                data_stat['anon_gaps'] += 1
                if k not in ga or k not in gb:
                    data_stat['anon_only'] += 1
                    findings.append(dict(kind='anon_data_only_in_' + ('B' if k in gb else 'A'), key=k,
                                         detail='%d B' % len((gb.get(k) or ga.get(k))[1]),
                                         allowed=allowed(k, allow)))
                    continue
                (aa, ra), (ab, rb) = ga[k], gb[k]
                if ra == rb or M.compare_anon(SA, SB, aa, ra, ab, rb):
                    data_stat['anon_same'] += 1
                else:
                    data_stat['anon_different'] += 1
                    j = next(q for q in range(min(len(ra), len(rb)) + 1)
                             if q == min(len(ra), len(rb)) or ra[q] != rb[q])
                    findings.append(dict(kind='anon_data', key=k, detail='%d/%d B, first diff +%d (%08x/%08x)' % (
                        len(ra), len(rb), j, aa + j, ab + j), allowed=allowed(k, allow)))
    hot = None
    if o.hot:
        names_h = read_hot(o.hot)
        ga, gb = {}, {}
        for s in A.funcs():
            ga.setdefault(M.canon(s['name']), s); ga.setdefault(s['name'], s)
        for s in B.funcs():
            gb.setdefault(M.canon(s['name']), s); gb.setdefault(s['name'], s)
        rows = []
        missing = []
        for h in names_h:
            a, b = ga.get(h) or ga.get(M.canon(h)), gb.get(h) or gb.get(M.canon(h))
            if a is None or b is None:
                missing.append(h); continue
            rows.append(dict(name=h, a=a['value'], b=b['value'],
                             set_a=M.icache_set(a['value']), set_b=M.icache_set(b['value']),
                             mod_a=a['value'] & 0x1fff, mod_b=b['value'] & 0x1fff,
                             size_a=a['size'], size_b=b['size']))
        changed = [r for r in rows if r['mod_a'] != r['mod_b'] or r['size_a'] != r['size_b']]
        hot = dict(listed=len(names_h), found=len(rows), missing=missing,
                   set_changed=[r['name'] for r in changed], rows=rows)
    bad = [f for f in findings if not f['allowed']]
    rc = 1 if bad else 0
    if hot and o.hot_strict and hot['set_changed'] and rc == 0:
        rc = 3
    out = dict(a=o.a, b=o.b, functions=stat, data=data_stat, findings=findings,
               weak_sites=weak_sites, hot=hot,
               deltas=sorted(moved.items(), key=lambda kv: -kv[1])[:16],
               seconds=round(time.time() - t0, 2), rc=rc)
    # ---- report
    print('func_diff: %s  vs  %s' % (o.a, o.b))
    print('functions: %d  same-bytes %d  same-mod-address %d  different %d  only-in-A %d  '
          'only-in-B %d  (address words resolved %d, weak sites %d)' % (
              stat['functions'], stat['same_bytes'], stat['same_mod_addr'], stat['different'],
              stat['only_a'], stat['only_b'], stat['addr_words'], stat['weak_sites']))
    print('address deltas (B-A:count):', ', '.join('%+d:%d' % kv for kv in out['deltas'][:8]))
    if data_stat:
        print('data objects:', data_stat)
    if hot:
        print('hot functions: %d listed, %d found, %d changed I-cache set (mod 8 KiB) or size'
              % (hot['listed'], hot['found'], len(hot['set_changed'])))
        lim = None if o.v else 20
        for r in [r for r in hot['rows'] if r['name'] in hot['set_changed']][:lim]:
            print('   %-36s %08x set %3d size %5d  ->  %08x set %3d size %5d' % (
                r['name'], r['a'], r['set_a'], r['size_a'], r['b'], r['set_b'], r['size_b']))
        if hot['missing']:
            print('   not found in both ELFs: %d (%s)' % (len(hot['missing']), ', '.join(hot['missing'][:8])))
    lim = None if o.v else 20
    for f in findings[:lim]:
        print('  %s%s: %s %s' % ('[allowed: %s] ' % f['allowed'] if f['allowed'] else '',
                                  f['kind'], f['key'], f['detail']))
    if lim and len(findings) > lim:
        print('  ... %d more (-v)' % (len(findings) - lim))
    if o.v and weak_sites:
        print('weak sites:', ', '.join(weak_sites))
    print('func_diff: %s (%d not allowed, %d allowed) in %.1f s' % (
        {0: 'EQUIVALENT', 1: 'DIFFERENT', 3: 'HOT SET MOVED'}[rc], len(bad),
        len(findings) - len(bad), out['seconds']))
    if o.json:
        with open(o.json, 'w') as fh:
            json.dump(out, fh, indent=1)
    return rc, out


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if '--selftest' in argv:
        import subprocess
        t = os.path.join(HERE, 'tests', 'elf-func_test_func_diff.py')
        return subprocess.call([sys.executable, t] + [x for x in argv if x != '--selftest'])
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('a'); ap.add_argument('b')
    ap.add_argument('--file'); ap.add_argument('--names')
    ap.add_argument('--allow'); ap.add_argument('--allow-file')
    ap.add_argument('--hot'); ap.add_argument('--hot-strict', action='store_true')
    ap.add_argument('--data', action='store_true')
    ap.add_argument('--strict-weak', action='store_true')
    ap.add_argument('--any-flavour', action='store_true')
    ap.add_argument('--json'); ap.add_argument('-v', action='store_true')
    o = ap.parse_args(argv)
    for p in (o.a, o.b):
        if not os.path.exists(p):
            print('func_diff: no such ELF: %s' % p); return 2
    rc, _ = run(o)
    return rc


if __name__ == '__main__':
    sys.exit(main())
