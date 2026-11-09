#!/usr/bin/env python3
"""strong_syms.py -- T6 (PLAN §5, gate SS): the weak-override hooks and the exported ABI
of psp/main_psp.c are still STRONG, GLOBAL and defined by main_psp.o in the linked ELF.

Why: the core declares its frontend hooks as weak no-ops (cut base 3bc947d0: rfu.c:96/113/351/411/452,
main.c:1068, 958 on fbe9da63; strong_syms.spec cites each).  If an override in main_psp.c is renamed, made static, or #if'ed away, the
weak no-op links silently and the feature dies with no build error (x1 H6).  The same
goes for globals other TUs use (x1 H5): a `static` added during the split breaks the
link (good), but a renamed definition plus a stale extern elsewhere does not always.

Per entry and flavour it requires, from the ELF symtab and the link map:
  1. exactly one non-local definition of the name, binding GLOBAL (not WEAK), defined;
  2. no LOCAL symbol of the same name (a static shadow beside the weak winner);
  3. the map's input section that holds the symbol's address comes from the owner
     object (main_psp.o), i.e. the override won, not the weak stub;
  4. for hooks: the core's weak stub is still in the link (an input section
     .text.<name> from a core object), so the core still calls this name.
Entries absent from a flavour by design (gpsp_net_wait_beat in release) are listed with
their flavours; a name present where the spec says absent is reported, not failed.

The spec is strong_syms.spec (hooks + x1 H5 + PLAN's gpsp_net_wait_beat) plus a DERIVED
list: every global main_psp.o defines (from the map) that another linked TU declares
`extern`, or that a header declares.  `golden` writes both, per flavour, from a known-good
ELF set; `check` holds a candidate to the golden.

Usage:
  strong_syms.py check  --elf X.elf --map X.map --flavour F [--golden G.tsv] [--spec S]
  strong_syms.py golden --run RUNDIR --src GPSP_ROOT --out G.tsv     (3 flavours)
  strong_syms.py checkrun --run RUNDIR --golden G.tsv                 (every flavour in RUNDIR)
  strong_syms.py --selftest
Exit: 0 green, 1 a violation, 2 usage.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from elf32 import Elf  # noqa: E402

FLAVOURS = ('release', 'lab', 'harness')
SPEC = os.path.join(HERE, 'strong_syms.spec')
STB = {0: 'LOCAL', 1: 'GLOBAL', 2: 'WEAK'}


# ------------------------------------------------------------- map
def obj_base(owner):
    """'../gpsp_libretro_psp1.a(rfu.o)' -> 'rfu.o'; '../frontend-common/fe_host.o' -> 'fe_host.o'."""
    m = re.search(r'\(([^)]+)\)\s*$', owner)
    return os.path.basename(m.group(1) if m else owner.strip())


def parse_map(path):
    """-> (sections, symbols): sections = [(addr, size, name, owner)], symbols = {name: addr}
    for the 'Linker script and memory map' part of a GNU ld map."""
    secs, syms = [], {}
    started, pend = False, None
    rx_full = re.compile(r'^ (\.\S+|COMMON)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$')
    rx_name = re.compile(r'^ (\.\S+|COMMON)\s*$')
    rx_cont = re.compile(r'^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$')
    rx_sym = re.compile(r'^\s+0x([0-9a-fA-F]+)\s+([A-Za-z_.$][\w.$]*)\s*$')
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if not started:
                started = line.startswith('Linker script and memory map')
                continue
            if pend is not None:
                m = rx_cont.match(line)
                if m:
                    secs.append((int(m.group(1), 16), int(m.group(2), 16), pend, m.group(3).strip()))
                    pend = None
                    continue
                pend = None
            m = rx_full.match(line)
            if m:
                secs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1), m.group(4).strip()))
                continue
            m = rx_name.match(line)
            if m:
                pend = m.group(1)
                continue
            m = rx_sym.match(line)
            if m and secs:
                syms.setdefault(m.group(2), []).append((int(m.group(1), 16), len(secs) - 1))
    return secs, syms


class MapIndex(object):
    def __init__(self, path):
        self.secs, self.syms = parse_map(path)
        self.by_name = {}
        for k, s in enumerate(self.secs):
            self.by_name.setdefault(s[2], []).append(k)

    def owner_of_symbol(self, name, addr):
        for a, k in self.syms.get(name, []):
            if a == addr:
                return obj_base(self.secs[k][3]), self.secs[k][2]
        # fall back to the input section that covers the address
        for a, sz, nm, ow in self.secs:
            if sz and a <= addr < a + sz:
                return obj_base(ow), nm
        return None, None

    def objects_with_section(self, secname):
        return sorted(set(obj_base(self.secs[k][3]) for k in self.by_name.get(secname, [])))

    def objects(self):
        return set(obj_base(s[3]) for s in self.secs)

    def globals_of(self, obj):
        out = {}
        for name, lst in self.syms.items():
            for a, k in lst:
                if obj_base(self.secs[k][3]) == obj and self.secs[k][1] > 0:
                    out[name] = (a, self.secs[k][2])
        return out


# ------------------------------------------------------------- spec
def load_spec(path):
    """lines: name  kind  flavours(comma|all)  owner  [stub objects comma]"""
    out = []
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        p = line.split()
        fl = FLAVOURS if p[2] == 'all' else tuple(p[2].split(','))
        out.append(dict(name=p[0], kind=p[1], flavours=fl, owner=p[3],
                        stubs=p[4].split(',') if len(p) > 4 and p[4] != '-' else []))
    return out


# ------------------------------------------------------------- derive exported ABI
_CMT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)


def derive(mapidx, src, owner='main_psp.o', owner_src='psp/main_psp.c'):
    """Globals `owner` defines that another linked TU extern-declares, or a header declares."""
    g = mapidx.globals_of(owner)
    linked = mapidx.objects()
    texts = {}
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ('.git', 'tools', 'tests', 'sdl', 'build', 'out')]
        for fn in files:
            if fn.endswith(('.c', '.h', '.inc', '.inc.c')):
                p = os.path.join(root, fn)
                rel = os.path.relpath(p, src).replace('\\', '/')
                if rel == owner_src:
                    continue
                try:
                    texts[rel] = _CMT.sub(' ', open(p, encoding='utf-8', errors='replace').read())
                except OSError:
                    pass
    words = {rel: set(re.findall(r'[A-Za-z_]\w*', t)) for rel, t in texts.items()}
    res = []
    for name in sorted(g):
        if name.startswith(('_', '.')):
            continue
        refs = []
        rx = re.compile(r'\bextern\b[^;{}]*\b%s\b[^;{}]*;' % re.escape(name))
        rxp = re.compile(r'\b%s\s*\([^;{}]*\)\s*(?:__attribute__\s*\(\([^;{}]*\)\))?\s*;' % re.escape(name))
        for rel, t in texts.items():
            if name not in words[rel]:
                continue
            base = os.path.basename(rel)
            is_h = rel.endswith('.h')
            linked_c = (base[:-2] + '.o') in linked if base.endswith('.c') else False
            if is_h and (rx.search(t) or rxp.search(t)):
                refs.append('h:' + rel)
            elif linked_c and (rx.search(t) or rxp.search(t)):
                refs.append('c:' + rel)
        if refs:
            res.append((name, g[name][1], refs))
    return res


# ------------------------------------------------------------- checking
def check_one(elf, mapidx, entries, flavour, out):
    fails = 0
    by = {}
    for s in elf.symbols:
        by.setdefault(s['name'], []).append(s)
    for e in entries:
        name = e['name']
        want = flavour in e['flavours']
        syms = by.get(name, [])
        nonloc = [s for s in syms if s['bind'] != 0 and s['shndx'] != 0]
        locs = [s for s in syms if s['bind'] == 0 and s['type'] in (1, 2)]
        if not want:
            out.append('  --    %-28s %-5s absent by design in %s%s' % (
                name, e['kind'], flavour, ' (PRESENT: review)' if nonloc else ''))
            continue
        why = []
        if len(nonloc) != 1:
            why.append('%d non-local definitions' % len(nonloc))
        s = nonloc[0] if nonloc else None
        if s is not None and s['bind'] != 1:
            why.append('binding %s' % STB.get(s['bind'], s['bind']))
        if locs:
            why.append('LOCAL shadow in %s' % ','.join(sorted(set(str(l['file']) for l in locs))))
        own = sec = None
        if s is not None:
            own, sec = mapidx.owner_of_symbol(name, s['value'])
            if own != e['owner']:
                why.append('map owner %s (want %s)' % (own, e['owner']))
        if e['stubs']:
            have = mapidx.objects_with_section('.text.' + name)
            miss = [x for x in e['stubs'] if x not in have]
            if miss:
                why.append('weak stub gone from %s (core no longer calls it?)' % ','.join(miss))
        if why:
            fails += 1
            out.append('  FAIL  %-28s %-5s %s' % (name, e['kind'], '; '.join(why)))
        else:
            out.append('  ok    %-28s %-5s %s 0x%08x %s%s' % (
                name, e['kind'], STB[s['bind']], s['value'], own,
                (' (stub: %s)' % ','.join(e['stubs'])) if e['stubs'] else ''))
    return fails


def entries_for(golden, flavour, spec):
    ents = list(spec)
    names = set(e['name'] for e in ents)
    if golden and os.path.exists(golden):
        for line in open(golden, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            fl, name, kind, owner, stubs = line.rstrip('\n').split('\t')[:5]
            if fl == flavour and kind == 'abi-derived' and name not in names:
                ents.append(dict(name=name, kind='derv', flavours=(flavour,), owner=owner,
                                 stubs=[]))
    return ents


def cmd_check(elfp, mapp, flavour, golden, spec, quiet=False):
    if golden and not os.path.exists(golden):
        # a mistyped path used to drop every derived-ABI entry and still report green
        raise SystemExit('strong_syms: golden %s does not exist' % golden)
    out = []
    n = check_one(Elf(elfp), MapIndex(mapp), entries_for(golden, flavour, load_spec(spec)), flavour, out)
    if not quiet:
        print('strong_syms %s: %s' % (flavour, os.path.basename(elfp)))
        print('\n'.join(out))
        print('SS %s: %s (%d entries)' % (flavour, 'ok' if not n else '%d FAIL' % n,
                                          sum(1 for l in out if not l.startswith('  --'))))
    return n, out


def cmd_golden(run, src, outp, spec):
    sp = load_spec(spec)
    with open(outp, 'w', newline='\n') as f:
        f.write('# strong_syms golden: flavour name kind owner stubs refs\n')
        for fl in FLAVOURS:
            mi = MapIndex(os.path.join(run, fl, 'gpsp_adhoc.map'))
            for e in sp:
                if fl in e['flavours']:
                    f.write('\t'.join([fl, e['name'], e['kind'], e['owner'],
                                       ','.join(e['stubs']) or '-', 'spec']) + '\n')
            for name, sec, refs in derive(mi, src):
                f.write('\t'.join([fl, name, 'abi-derived', 'main_psp.o', '-', ' '.join(refs)]) + '\n')
    print('wrote %s' % outp)


def cmd_checkrun(run, golden, spec):
    """every flavour IN the run (discovered: release-stable is held to the rows of its
    build.sh profile, release), not a fixed list (adversarial review 2026-11-07)."""
    import runs
    fls = runs.flavours_in(run)
    if not fls:
        print('SS: no <flavour>/gpsp_adhoc.elf under %s' % run)
        return 2
    have = set(l.split('\t')[0] for l in open(golden, encoding='utf-8') if l.strip() and not l.startswith('#'))
    tot = 0
    for fl in fls:
        gf = runs.profile_of(run, fl)
        if gf not in have:
            print('[%s] FAIL: the golden has no rows for %s' % (fl, gf))
            tot += 1
            continue
        if not os.path.isfile(os.path.join(run, fl, 'gpsp_adhoc.map')):
            print('[%s] FAIL: no gpsp_adhoc.map' % fl)
            tot += 1
            continue
        print('[%s] held to the %s rows' % (fl, gf))
        n, _ = cmd_check(os.path.join(run, fl, 'gpsp_adhoc.elf'), os.path.join(run, fl, 'gpsp_adhoc.map'),
                         gf, golden, spec)
        tot += n
    print('SS: %s (%s)' % ('green in every flavour' if not tot else '%d FAIL' % tot, ', '.join(fls)))
    return 1 if tot else 0


def selftest():
    """Unit cases of the map parser; tests/abi-exit-ci_test_strong.sh does ELF/map mutants."""
    import tempfile
    d = tempfile.mkdtemp(prefix='ss')
    p = os.path.join(d, 'x.map')
    open(p, 'w').write('''Archive member included\n\nLinker script and memory map\n
LOAD main_psp.o
 *(.text.foo)
 .text.foo      0x00001000       0x10 main_psp.o
                0x00001000                foo
 .text.foo
                0x00001020       0x20 ../lib.a(rfu.o)
 .bss           0x00002000       0x40 main_psp.o
                0x00002000                g_x
''')
    mi = MapIndex(p)
    fails = 0
    for cond, msg in [
        (mi.owner_of_symbol('foo', 0x1000) == ('main_psp.o', '.text.foo'), 'owner of foo'),
        (mi.objects_with_section('.text.foo') == ['main_psp.o', 'rfu.o'], 'both definitions seen'),
        (mi.owner_of_symbol('g_x', 0x2000)[0] == 'main_psp.o', 'bss global owner'),
        (set(mi.globals_of('main_psp.o')) == {'foo', 'g_x'}, 'globals_of'),
    ]:
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        fails += not cond
    print('selftest: %s' % ('ok' if not fails else 'FAIL'))
    return 1 if fails else 0


def main():
    if '--selftest' in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    c = sub.add_parser('check')
    c.add_argument('--elf', required=True)
    c.add_argument('--map', required=True)
    c.add_argument('--flavour', required=True)
    c.add_argument('--golden')
    c.add_argument('--spec', default=SPEC)
    g = sub.add_parser('golden')
    g.add_argument('--run', required=True)
    g.add_argument('--src', required=True)
    g.add_argument('--out', required=True)
    g.add_argument('--spec', default=SPEC)
    r = sub.add_parser('checkrun')
    r.add_argument('--run', required=True)
    r.add_argument('--golden', required=True)
    r.add_argument('--spec', default=SPEC)
    o = ap.parse_args()
    if o.cmd == 'check':
        n, _ = cmd_check(o.elf, o.map, o.flavour, o.golden, o.spec)
        return 1 if n else 0
    if o.cmd == 'golden':
        cmd_golden(o.run, o.src, o.out, o.spec)
        return 0
    if o.cmd == 'checkrun':
        return cmd_checkrun(o.run, o.golden, o.spec)
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
