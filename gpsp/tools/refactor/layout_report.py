#!/usr/bin/env python3
"""layout_report.py -- T10, gate G3 (builds/mainpsp-map/out/PLAN.md §0.1, §5).

    layout_report.py CAND [--ref REF] [--plan FILE] [--objects RE] [--json OUT]
    layout_report.py --selftest DIR [--ref REF]      seeded mutants, no build

CAND / REF: a flavours.sh flavour dir (gpsp_adhoc.elf, gpsp_adhoc.map,
build-manifest.json), or an ELF with --map / --ref-map.  Release is compared
with release and Lab with Lab only: their pads differ.

THE REPORT (one candidate; with --ref, every figure also as a delta):
  1. pads in use: LAYOUT_PAD_BYTES (manifest, else layout_pad_cold's size) and
     LAYOUT_PAD_RO_BYTES (the map's .rodata.layout_pad_ro, else the manifest)
  2. main's start, mod 8192 and its I-cache set (64 B lines, 128 sets per 8 KiB way);
     main's size; layout_pad_cold; _end; rom_translation_cache_static & 8191 (A and B)
  3. .data.pin: size and headroom (bytes the text+rodata before it may still grow
     before .data -- and with it .bss and the heap -- moves a whole 8 KiB page)
  4. the first 20 .bss symbols of main_psp.o and the watched ones (out.0, ring_r,
     ring_w, audio_ring) with their D-cache sets
  5. main's loop set map BY LINE (DWARF rows of main_psp.c's loop window, found by
     anchor: loop_anchor.py; 16368-18090 on 01067e74
     inside main; x4's 220 lines / 115 sets): lines, sets, max lines per set.
     T1b loop_diff.py owns the by-address window with inlined ranges.
  6. the INPUT-SECTION AUDIT: the input-section name of every function of the
     audited objects (default main_psp.o), from the link map.  FAILS on any
     .text.unlikely*, .text.hot*, .text.startup*, .text.exit* section the plan
     does not list (default plan: .text.startup.main).  Also counts .cold symbols.
  7. with --ref: PAD SUGGESTIONS.  Instance A's cold text (functions of
     gpsp_libretro_psp1.a linked after layout_pad_cold) and A's tables
     (gpsp_libretro_psp1.a .rodata objects) are matched by name between REF and
     CAND; the commonest move mod 8 KiB is what the pad must take back:
         text pad = (CAND pad - move) mod 8192,   ro pad likewise
     with the share of anchors that suggestion re-pins.  Then
     tools/layout/pad_check.py is run on the two ELFs (hot core / warm tail
     moves, hot-core set changes, JIT mod 8 KiB, d(_etext), d(_end)).

Exit: 0 ok, 1 the input-section audit failed (or --expect-pins not met), 2 usage.
"""
import argparse
import collections
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GPSP = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
from elf32 import Elf  # noqa: E402
import sl_ldmap  # noqa: E402

WAY = 8192
LINE = 64
SETS = WAY // LINE                  # 128 sets per way (16 KiB 2-way I and D caches)
LOOP_LINES = None   # by anchor (loop_anchor.py), checked against main's DW_AT_decl_line
WATCH_BSS = ('out.0', 'ring_r', 'ring_w', 'audio_ring')
SPECIAL = re.compile(r'^\.text\.(unlikely|hot|startup|exit)(\.|$)')
DEFAULT_PLAN = ['.text.startup.main']
CORE_ARCHIVE = 'gpsp_libretro_psp1.a('


def setno(a):
    return (a // LINE) % SETS


# --------------------------------------------------------------------------- inputs

class Build:
    def __init__(self, path, map_path=None):
        if os.path.isdir(path):
            self.dir = path
            self.elf_path = os.path.join(path, 'gpsp_adhoc.elf')
            mp = os.path.join(path, 'gpsp_adhoc.map')
            map_path = map_path or (mp if os.path.isfile(mp) else None)
            mf = os.path.join(path, 'build-manifest.json')
            self.manifest = json.load(open(mf)) if os.path.isfile(mf) else {}
        else:
            self.dir = None
            self.elf_path = path
            self.manifest = {}
        self.e = Elf(self.elf_path)
        self.map = sl_ldmap.LdMap(map_path) if map_path else None
        self.sym = {}
        self.count = collections.Counter()
        for s in self.e.symbols:
            self.count[s['name']] += 1
            self.sym.setdefault(s['name'], s)

    def addr(self, name):
        s = self.sym.get(name)
        return s['value'] if s else None

    def unique(self, name):
        return self.count[name] == 1

    def text_pad(self):
        if 'layoutPadBytes' in self.manifest:
            return int(self.manifest['layoutPadBytes']), 'manifest'
        s = self.sym.get('layout_pad_cold')
        if s is None:
            return 0, 'no layout_pad_cold'
        return s['size'] - 4, 'layout_pad_cold size - 4 (estimate)'

    def ro_pad(self):
        if self.map is not None:
            ins = [i for i in self.map.inputs if i.name == '.rodata.layout_pad_ro']
            return (ins[0].size if ins else 0), 'map'
        m = re.search(r'LAYOUT_PAD_RO_BYTES=(\d+)', json.dumps(self.manifest))
        if m:
            return int(m.group(1)), 'manifest'
        return 0, 'default (no map, not in the manifest)'


# --------------------------------------------------------------------------- report parts

def key_addresses(b):
    r = collections.OrderedDict()
    m = b.sym.get('main')
    if m:
        r['main'] = {'addr': m['value'], 'size': m['size'], 'mod8192': m['value'] % WAY,
                     'set': setno(m['value'])}
    for n in ('layout_pad_cold', '_end', 'rom_translation_cache_static', 'gbab_rom_translation_cache_static',
              'gbahot_start', '_gp'):
        a = b.addr(n)
        if a is not None:
            r[n] = {'addr': a, 'mod8192': a % WAY}
    for sname in ('.text', '.rodata', '.data.pin', '.data', '.data.gbahot', '.sdata', '.sbss', '.bss'):
        s = b.e.by_name.get(sname)
        if s:
            r[sname] = {'addr': s['addr'], 'size': s['size'], 'mod8192': s['addr'] % WAY}
    pin = b.e.by_name.get('.data.pin')
    if pin:
        # hot_data_guard.ld: BYTE(0) then ALIGN(8192): the section is 1 B of content
        # plus the pad; whatever precedes it may grow by size-1 before .data moves
        r['data_pin_headroom'] = pin['size'] - 1
    return r


def bss_symbols(b, obj='main_psp.o', n=20):
    """The first n symbols of obj's .bss input section (needs the map), + watched ones."""
    out = {'first': [], 'watch': []}
    rng = None
    if b.map is not None:
        ins = [i for i in b.map.inputs if i.out == '.bss' and i.name == '.bss' and i.file.endswith(obj) and i.size]
        if ins:
            rng = (ins[0].addr, ins[0].addr + ins[0].size)
    if rng:
        syms = sorted((s for s in b.e.symbols if s['type'] in (0, 1) and s['section'] == '.bss'
                       and rng[0] <= s['value'] < rng[1] and s['name']), key=lambda s: (s['value'], s['name']))
        out['range'] = rng
    else:
        # no map: main_psp.c's LOCAL .bss objects (STT_FILE owner); globals have no
        # owner in the symtab and are missed -- the report says so
        src = obj[:-2] + '.c'
        syms = sorted((s for s in b.e.symbols if s['type'] == 1 and s['section'] == '.bss'
                       and s['file'] == src), key=lambda s: (s['value'], s['name']))
        out['locals_only'] = True
    out['first'] = [{'name': s['name'], 'addr': s['value'], 'size': s['size'], 'mod8192': s['value'] % WAY,
                     'set': setno(s['value'])} for s in syms[:n]]
    for w in WATCH_BSS:
        # a function-static's number (out.0 in release, out.1 in Lab) is GCC's, not ours:
        # match the base name among main_psp.c's own .bss symbols
        base = w.split('.')[0]
        cands = [x for x in b.e.symbols if x['type'] == 1 and x['section'] in ('.bss', '.sbss')
                 and re.match(r'^%s(\.\d+)?$' % re.escape(base), x['name'])
                 and (x['file'] == obj[:-2] + '.c' or x['bind'] != 0)]
        s = cands[0] if len(cands) == 1 else b.sym.get(w)
        if s:
            w = s['name']
            out['watch'].append({'name': w, 'addr': s['value'], 'size': s['size'], 'mod8192': s['value'] % WAY,
                                 'set': setno(s['value']), 'last_set': setno(s['value'] + max(s['size'], 1) - 1),
                                 'unique': b.unique(w)})
    return out


def loop_sets(b, lines=LOOP_LINES):
    """main's loop body by source line (x4's method): cache lines touched, sets used."""
    if '.debug_line' not in b.e.by_name or 'main' not in b.sym:
        return None
    import sl_dwline
    m = b.sym['main']
    import loop_anchor
    r = loop_anchor.for_elf(b.e.path)
    if lines is not None:     # an explicit window must be this ELF's anchored one
        loop_anchor.check_explicit(r, lines[0], lines[1], 'layout_report')
    lines = (r['lo'], r['hi'])
    print('  ' + loop_anchor.describe(r))
    lo, hi = m['value'], m['value'] + m['size']
    rws = sl_dwline.rows(b.e, 'main_psp.c')
    cl = set()
    amin, amax = 1 << 40, 0
    for i in range(len(rws) - 1):
        a, f, ln = rws[i]
        nx = rws[i + 1][0]
        if a is None or nx is None or not f.endswith('main_psp.c') or not (lines[0] <= ln <= lines[1]):
            continue
        if not (lo <= a < hi):
            continue
        for x in range(a, nx, 4):
            cl.add(x // LINE)
        amin, amax = min(amin, a), max(amax, nx)
    per = collections.Counter(c % SETS for c in cl)
    return {'lines': len(cl), 'sets': len(per), 'max_per_set': max(per.values()) if per else 0,
            'window': (amin - lo, amax - lo) if cl else None, 'set_counts': dict(sorted(per.items())),
            'src_lines': lines}


def audit_sections(b, objects_re, plan):
    """Every input section of the audited objects in executable output sections."""
    res = {'objects': objects_re, 'plan': plan, 'sections': [], 'special': [], 'unplanned': [],
           'cold_symbols': sum(1 for s in b.e.symbols if '.cold' in s['name'])}
    if b.map is None:
        res['error'] = 'no link map: the input-section audit needs gpsp_adhoc.map (flavours_sidecar.sh)'
        return res
    r = re.compile(objects_re)
    for i in b.map.inputs:
        if not r.search(i.file) or not i.out.startswith('.text') and not i.name.startswith('.text'):
            continue
        if not i.name.startswith('.text'):
            continue
        rec = {'name': i.name, 'out': i.out, 'addr': i.addr, 'size': i.size, 'file': i.file}
        res['sections'].append(rec)
        if SPECIAL.match(i.name):
            res['special'].append(rec)
            if not any(fnmatch.fnmatchcase(i.name, p) for p in plan):
                res['unplanned'].append(rec)
    if not res['sections']:
        # an audit of nothing is not a pass (wrong --objects, or an LTO/renamed object)
        res['error'] = 'no .text input section of %s in the map' % objects_re
    return res


def read_plan(path):
    if not path:
        return list(DEFAULT_PLAN)
    out = []
    for l in open(path):
        l = l.split('#', 1)[0].strip()
        if l:
            out.append(l)
    return out


# --------------------------------------------------------------------------- pads

def anchors(ref, cand, kind):
    """(name, ref addr, cand addr) for instance A's cold text ('text') or tables ('ro').
    Classified with whichever build has a map (REF preferred)."""
    src = ref if ref.map is not None else cand
    if src.map is None:
        return None
    if kind == 'text':
        pad = src.addr('layout_pad_cold') or 0
        iv = [(i.addr, i.addr + i.size) for i in src.map.inputs
              if i.out == '.text' and i.size and CORE_ARCHIVE in i.file and i.addr > pad]
        want_type = 2
    else:
        iv = [(i.addr, i.addr + i.size) for i in src.map.inputs
              if i.out == '.rodata' and i.size and CORE_ARCHIVE in i.file]
        want_type = 1
    iv.sort()
    import bisect
    starts = [a for a, _ in iv]

    def inside(a):
        k = bisect.bisect_right(starts, a) - 1
        return k >= 0 and iv[k][0] <= a < iv[k][1]
    other = cand if src is ref else ref
    out = []
    for s in src.e.symbols:
        if s['type'] != want_type or not s['size'] or not inside(s['value']):
            continue
        n = s['name']
        if not (src.unique(n) and other.unique(n)):
            continue
        o = other.sym.get(n)
        if o is None:
            continue
        ra, ca = (s['value'], o['value']) if src is ref else (o['value'], s['value'])
        out.append((n, ra, ca))
    return out


def suggest(ref, cand, kind):
    an = anchors(ref, cand, kind)
    if an is None:
        return {'error': 'needs a link map of REF or CAND'}
    if not an:
        return {'error': 'no anchors'}
    cnt = collections.Counter(((c - r) % WAY) for _, r, c in an)
    move, hits = cnt.most_common(1)[0]
    cur, how = cand.text_pad() if kind == 'text' else cand.ro_pad()
    refpad, _ = ref.text_pad() if kind == 'text' else ref.ro_pad()
    sug = (cur - move) % WAY
    note = ''
    if kind == 'text' and 0 < sug < 8:
        sug += WAY
        note = 'a text pad is at least 8 B (jr ra; nop): +8192'
    if kind == 'ro' and sug % 16:
        note = '.rodata moves in 16 B steps: check the neighbours'
    sign = move if move < WAY // 2 else move - WAY
    return {'anchors': len(an), 'move_mod8192': sign, 'share': hits / len(an), 'cand_pad': cur,
            'cand_pad_from': how, 'ref_pad': refpad, 'suggested': sug, 'note': note,
            'distinct_moves': len(cnt)}


def write_nm(b, path):
    """psp-nm -n -S style lines from the ELF symtab (what pad_check.py/lp_map read)."""
    rows = []
    for s in b.e.symbols:
        if not s['name'] or s['section'] is None and s['type'] != 0:
            continue
        if s['type'] == 2:
            letter = 'T' if s['bind'] else 't'
        elif s['type'] == 1:
            nob = s['section'] in ('.bss', '.sbss')
            letter = ('B' if nob else 'D') if s['bind'] else ('b' if nob else 'd')
        elif s['type'] == 0 and s['bind']:
            letter = 'A' if s['shndx'] == 0xfff1 else 'T'
            if s['name'] in ('_end', '_etext', '_gp', 'end', 'etext', 'edata', '_edata'):
                letter = 'A'
        else:
            continue
        rows.append((s['value'], s['size'], letter, s['name']))
    rows.sort()
    with open(path, 'w', newline='\n') as f:
        for a, sz, l, n in rows:
            if sz:
                f.write('%08x %08x %s %s\n' % (a, sz, l, n))
            else:
                f.write('%08x %s %s\n' % (a, l, n))


def asm_nm(b, path):
    """mips_stub.o's text symbols (instance A), object-relative, for pad_check --asm-nm."""
    src = b.map
    iv = [(i.addr, i.addr + i.size) for i in src.inputs
          if i.file.endswith('gpsp_libretro_psp1.a(mips_stub.o)') and i.out == '.text' and i.size]
    with open(path, 'w', newline='\n') as f:
        for s in sorted(b.e.symbols, key=lambda s: s['value']):
            if s['type'] in (0, 2) and s['section'] == '.text' and s['name'] and \
                    any(lo <= s['value'] < hi for lo, hi in iv):
                f.write('%08x %s %s\n' % (s['value'] - 0x08804040, 'T' if s['bind'] else 't', s['name']))


def run_pad_check(ref, cand, order):
    src = ref if ref.map is not None else cand
    if src.map is None:
        return 'pad_check not run: needs a map of REF or CAND for mips_stub.o\'s symbols'
    t = tempfile.mkdtemp(prefix='layoutrep_')
    try:
        rn, cn, an = (os.path.join(t, x) for x in ('ref.nm', 'cand.nm', 'asm.nm'))
        write_nm(ref, rn)
        write_nm(cand, cn)
        asm_nm(src, an)
        cmd = [sys.executable, os.path.join(GPSP, 'tools', 'layout', 'pad_check.py'),
               '--order', order, '--asm-nm', an, 'REF=' + rn, 'CAND=' + cn]
        p = subprocess.run(cmd, capture_output=True, text=True)
        return (p.stdout + p.stderr).rstrip() + ('\n(pad_check exit %d)' % p.returncode)
    finally:
        import shutil
        shutil.rmtree(t, ignore_errors=True)


# --------------------------------------------------------------------------- report

def report(cand, ref=None, plan=None, objects=r'(^|/)main_psp\.o$', order=None, out=print, pad_check=True):
    plan = plan or list(DEFAULT_PLAN)
    res = collections.OrderedDict()
    res['cand'] = cand.elf_path
    res['ref'] = ref.elf_path if ref else None
    tp, tph = cand.text_pad()
    rp, rph = cand.ro_pad()
    res['pads'] = {'text': tp, 'text_from': tph, 'ro': rp, 'ro_from': rph}
    out('layout_report: %s%s' % (cand.elf_path, '\n          ref: %s' % ref.elf_path if ref else ''))
    out('pads: LAYOUT_PAD_BYTES=%d (%s), LAYOUT_PAD_RO_BYTES=%d (%s)' % (tp, tph, rp, rph))
    ka = key_addresses(cand)
    kr = key_addresses(ref) if ref else {}
    res['key'] = ka
    out('\n-- key addresses (mod 8192 = position in an 8 KiB cache way; set = 64 B line index 0..127)')
    for k, v in ka.items():
        if not isinstance(v, dict):
            continue
        d = ''
        if k in kr:
            dd = v['addr'] - kr[k]['addr']
            d = '  d=%+d%s' % (dd, '' if dd % WAY == 0 else ' (mod 8K %+d)' % (((dd + WAY // 2) % WAY) - WAY // 2))
        extra = ''
        if 'size' in v:
            extra += ' size %d' % v['size']
        if 'set' in v:
            extra += ' set %d' % v['set']
        out('  %-34s %#010x  mod8192 %#06x%s%s' % (k, v['addr'], v['mod8192'], extra, d))
    if 'data_pin_headroom' in ka:
        out('  .data.pin headroom: %d B before .data moves a page%s' % (
            ka['data_pin_headroom'], '' if 'data_pin_headroom' not in kr else
            ' (ref %d)' % kr['data_pin_headroom']))
    bs = bss_symbols(cand)
    res['bss'] = bs
    out('\n-- main_psp.o .bss: first %d symbols%s' % (len(bs['first']), '' if bs.get('range') else
                                                     ' (NO MAP: main_psp.c locals only, globals missed)'))
    rs = {x['name']: x for x in bss_symbols(ref)['first']} if ref else {}
    for x in bs['first']:
        d = ''
        if x['name'] in rs:
            d = '  d=%+d' % (x['addr'] - rs[x['name']]['addr'])
        out('  %#010x %7d  mod8192 %#06x set %3d  %s%s' % (x['addr'], x['size'], x['mod8192'], x['set'], x['name'], d))
    out('  watched:')
    for x in bs['watch']:
        span = 'all %d sets' % SETS if x['size'] >= WAY else 'sets %d..%d' % (x['set'], x['last_set'])
        out('  %#010x %7d  mod8192 %#06x %-12s %s%s' % (x['addr'], x['size'], x['mod8192'], span, x['name'],
                                                      '' if x['unique'] else ' (name not unique)'))
    lp = loop_sets(cand, LOOP_LINES)
    res['loop'] = lp
    if lp:
        out('\n-- main loop by line (main_psp.c %d-%d inside main): %d cache lines over %d sets, max %d per set,'
            ' window main+%d..+%d' % (lp['src_lines'][0], lp['src_lines'][1], lp['lines'], lp['sets'],
                                     lp['max_per_set'], lp['window'][0], lp['window'][1]))
        if ref:
            lr = loop_sets(ref)
            if lr:
                gone = sorted(set(lr['set_counts']) - set(lp['set_counts']))
                new = sorted(set(lp['set_counts']) - set(lr['set_counts']))
                same = lr['set_counts'] == lp['set_counts']
                out('   vs ref: %d lines / %d sets; set map %s%s' % (
                    lr['lines'], lr['sets'], 'IDENTICAL' if same else 'CHANGED',
                    '' if same else ' (sets only in ref %s, only in cand %s)' % (gone[:12], new[:12])))
                res['loop_vs_ref'] = {'same': same, 'only_ref': gone, 'only_cand': new}
    au = audit_sections(cand, objects, plan)
    res['audit'] = au
    out('\n-- input-section audit (%s; plan %s)' % (objects, plan))
    if 'error' in au:
        out('  ' + au['error'])
    else:
        kinds = collections.Counter(SPECIAL.match(s['name']).group(0).rstrip('.') if SPECIAL.match(s['name'])
                                    else '.text.<name>' if s['name'] != '.text' else '.text'
                                    for s in au['sections'])
        out('  %d input sections: %s' % (len(au['sections']), ', '.join('%s %d' % kv for kv in sorted(kinds.items()))))
        for s in au['special']:
            planned = s not in au['unplanned']
            out('  %-9s %-40s %#010x %6d %s' % ('planned' if planned else 'UNPLANNED', s['name'], s['addr'],
                                                s['size'], s['file']))
    out('  .cold symbols in the ELF: %d' % au['cold_symbols'])
    fail = 1 if au.get('unplanned') or 'error' in au else 0
    if ref:
        out('\n-- pad suggestions (re-pin CAND to REF)')
        res['suggest'] = {}
        for kind, var in (('text', 'LAYOUT_PAD_BYTES'), ('ro', 'LAYOUT_PAD_RO_BYTES')):
            sg = suggest(ref, cand, kind)
            res['suggest'][kind] = sg
            if 'error' in sg:
                out('  %-20s %s' % (var, sg['error']))
                continue
            if kind == 'ro' and res['suggest'].get('text', {}).get('suggested') not in (None, tp):
                sg['note'] = (sg['note'] + '; ' if sg['note'] else '') + (
                    'valid for an UNCHANGED text pad only: a new text pad moves .rodata too (rounded to '
                    'its alignment), so set LAYOUT_PAD_BYTES first, rebuild and re-run')
                sg['depends_on_text'] = True
            out('  %-20s %d (cand %d, ref %d): A\'s %s moved %+d mod 8 KiB in %d of %d anchors (%.1f%%, %d distinct '
                'moves)%s' % (var, sg['suggested'], sg['cand_pad'], sg['ref_pad'],
                              'cold text' if kind == 'text' else 'tables', sg['move_mod8192'],
                              round(sg['share'] * sg['anchors']), sg['anchors'], 100 * sg['share'],
                              sg['distinct_moves'], '; ' + sg['note'] if sg['note'] else ''))
        if pad_check:
            pc = run_pad_check(ref, cand, order or os.path.join(GPSP, 'psp', 'layout', 'hot_order.txt'))
            res['pad_check'] = pc
            out('\n-- tools/layout/pad_check.py')
            for l in pc.splitlines():
                out('  ' + l)
    out('\nG3 %s' % ('ok' if not fail else 'FAIL (unplanned .text.unlikely/.hot/.startup/.exit input section)'
                     if au.get('unplanned') else 'FAIL (%s)' % au.get('error')))
    res['fail'] = fail
    return fail, res


# --------------------------------------------------------------------------- selftest

def selftest(d, ref_dir=None):
    """Controls and mutants on a real flavour dir (map required); with --ref (another
    build of the same flavour and tree, different pad) the pad suggestion is checked."""
    cand = Build(d)
    if cand.map is None:
        print('selftest needs %s/gpsp_adhoc.map' % d)
        return 2
    quiet = lambda *_: None
    fails = [0]

    def expect(label, cond):
        print('  %-72s %s' % (label, 'ok' if cond else 'FAILED'))
        fails[0] += not cond

    rc, r = report(cand, out=quiet, pad_check=False)
    expect('control: G3 passes on the baseline (only .text.startup.main special; got %s)'
           % [s['name'] for s in r['audit']['special']], rc == 0)
    expect('control: main is in .text.startup.main', any(s['name'] == '.text.startup.main'
                                                         for s in r['audit']['special']))
    expect('control: 0 .cold symbols (PLAN: grep -c "\\.cold" elf.nm = 0)', r['audit']['cold_symbols'] == 0)
    t = tempfile.mkdtemp(prefix='layoutrep_')
    try:
        txt = open(cand.map.path, errors='replace').read()

        def with_map(new_txt, tag):
            p = os.path.join(t, tag + '.map')
            open(p, 'w').write(new_txt)
            return Build(cand.elf_path, p)

        # M1: a cold function in main_psp.o without a section (GCC: .text.unlikely.<fn>,
        # which the default script places BEFORE .text.startup, i.e. before main)
        anchor = ' *(.text.unlikely .text.*_unlikely .text.unlikely.*)\n'
        m1 = with_map(txt.replace(anchor, anchor + ' .text.unlikely.fe_seeded_cold\n'
                                  '                0x0893bb00       0x40 main_psp.o\n', 1), 'm1')
        rc, r = report(m1, out=quiet, pad_check=False)
        expect('mutant: cold fn -> .text.unlikely.fe_seeded_cold in main_psp.o', rc == 1 and any(
            s['name'] == '.text.unlikely.fe_seeded_cold' for s in r['audit']['unplanned']))
        # M2: the same in a plain .text.unlikely (no -ffunction-sections name)
        m2 = with_map(txt.replace(anchor, anchor + ' .text.unlikely\n'
                                  '                0x0893bb00       0x40 main_psp.o\n', 1), 'm2')
        rc, r = report(m2, out=quiet, pad_check=False)
        expect('mutant: a plain .text.unlikely input section of main_psp.o', rc == 1)
        # M3: .text.hot.* and a second .text.startup.* (an unplanned boot phase)
        for nm_ in ('.text.hot.fe_seeded', '.text.startup.fe_boot_run'):
            m3 = with_map(txt.replace(anchor, anchor + ' %s\n                0x0893bb00       0x40 main_psp.o\n'
                                      % nm_, 1), 'm3')
            rc, r = report(m3, out=quiet, pad_check=False)
            expect('mutant: unplanned %s' % nm_, rc == 1)
        # C: the same .text.startup.fe_boot_run passes once the plan lists it
        rc, r = report(m3, plan=['.text.startup.main', '.text.startup.fe_*'], out=quiet, pad_check=False)
        expect('control: .text.startup.fe_boot_run with plan .text.startup.fe_* passes', rc == 0)
        # C: an explicit section (what PLAN §0.2-5 requires) is not special
        m4 = with_map(txt.replace(' .text.exit_cb  ', ' .text.fe_seeded_cold\n                0x08944700       0x10 main_psp.o\n .text.exit_cb  ', 1), 'm4')
        rc, r = report(m4, out=quiet, pad_check=False)
        expect('control: an explicitly sectioned .text.fe_seeded_cold passes', rc == 0)
        # C: another object's .text.unlikely is not main_psp.o's (default --objects)
        m5 = with_map(txt.replace(anchor, anchor + ' .text.unlikely.x\n                0x0893bb00       0x40 ui_psp.o\n', 1),
                      'm5')
        rc, r = report(m5, out=quiet, pad_check=False)
        expect('control: ui_psp.o .text.unlikely ignored with the default --objects', rc == 0)
        rc, r = report(m5, objects=r'(^|/)(main_psp|ui_psp)\.o$', out=quiet, pad_check=False)
        expect('mutant: ...but caught with --objects main_psp|ui_psp', rc == 1)
        # no map at all: the audit cannot run -> fail, never a silent pass
        rc, r = report(Build(cand.elf_path), out=quiet, pad_check=False)
        expect('mutant: no map -> the audit fails (never a silent pass)', rc == 1)
        rc, r = report(cand, objects=r'(^|/)fe_nonexistent\.o$', out=quiet, pad_check=False)
        expect('mutant: --objects matching no object -> fails (an empty audit is no pass)', rc == 1)
    finally:
        import shutil
        shutil.rmtree(t, ignore_errors=True)
    # pads: CAND vs itself suggests its own pads
    for kind in ('text', 'ro'):
        sg = suggest(cand, cand, kind)
        cur = cand.text_pad()[0] if kind == 'text' else cand.ro_pad()[0]
        expect('control: %s pad vs itself suggests the current %d' % (kind, cur),
               sg.get('suggested') == cur and sg.get('share') == 1.0)
    if ref_dir:
        ref = Build(ref_dir)
        for a, b in ((ref, cand), (cand, ref)):
            sg = suggest(a, b, 'text')
            want = a.text_pad()[0]
            expect('pair: REF pad %d, CAND pad %d -> suggests %s (want %d, %.1f%% of %d anchors)'
                   % (a.text_pad()[0], b.text_pad()[0], sg.get('suggested'), want, 100 * sg.get('share', 0),
                      sg.get('anchors', 0)), sg.get('suggested') == want and sg.get('share', 0) >= 0.99)
            # the ro suggestion holds for an unchanged text pad; the report must say so
            _, r_ = report(b, a, out=quiet, pad_check=False)
            so = r_['suggest']['ro']
            expect('pair: ro suggestion %s flagged as depending on the text pad' % so.get('suggested'),
                   so.get('depends_on_text', False) == (sg.get('suggested') != b.text_pad()[0]))
    print('selftest: %s' % ('PASS' if not fails[0] else '%d FAILED' % fails[0]))
    return 1 if fails[0] else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cand', nargs='?')
    ap.add_argument('--map')
    ap.add_argument('--ref')
    ap.add_argument('--ref-map')
    ap.add_argument('--plan', help='file of planned special input sections (globs), one per line')
    ap.add_argument('--objects', default=r'(^|/)main_psp\.o$', help='regex of objects to audit')
    ap.add_argument('--order', default=os.path.join(GPSP, 'psp', 'layout', 'hot_order.txt'))
    ap.add_argument('--no-pad-check', action='store_true')
    ap.add_argument('--loop-lines', help='LO-HI: held to the anchored window (default: by anchor)')
    ap.add_argument('--json')
    ap.add_argument('--selftest', metavar='DIR')
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.selftest, a.ref)
    if not a.cand:
        ap.error('CAND is required')
    global LOOP_LINES
    if a.loop_lines:
        LOOP_LINES = tuple(int(x) for x in a.loop_lines.split('-'))
    cand = Build(a.cand, a.map)
    ref = Build(a.ref, a.ref_map) if a.ref else None
    rc, res = report(cand, ref, read_plan(a.plan), a.objects, a.order, pad_check=not a.no_pad_check)
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1, default=str)
    return rc


if __name__ == '__main__':
    sys.exit(main())
