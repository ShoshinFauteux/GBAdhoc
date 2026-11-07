#!/usr/bin/env python3
"""Self-test + mutant tests for inline_census.py (T13, gate IC).

Acceptance (PLAN §5 T13): reproduces x4's counts on the clean release base:
335 definitions, 280 live, 55 compiled out, 127 live with no symbol, 159
emitted symbols, 22 clones, 25 global (x4 b.2), 104,380 B of .text (x4_map).
Mutants: a clone renamed in .strtab, an emitted function renamed away, an
inline instance re-pointed in .debug_info (turbo_frame's only instance now
claims to be audio_status_frame), and the scanner's continuation-line fix.
"""
import json
import os
import struct
import sys
import tempfile

T = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, T)
sys.path.insert(0, os.path.dirname(T))
from elf_func_lib import Checks, Mutant, run_tool, elf, FLAVOURS, PAD144, SRC  # noqa: E402
import ef_dwarf as D  # noqa: E402
import ef_srcscan  # noqa: E402
import inline_census as IC  # noqa: E402

C = Checks('inline_census selftest')
TOOL = 'inline_census.py'
REL = elf('run1', 'release')


def jrun(*args):
    j = os.path.join(tempfile.mkdtemp(prefix='elf-func-ic-'), 'o.json')
    rc, out = run_tool(TOOL, *(list(args) + ['--src', SRC, '--json', j]))
    return rc, out, (json.load(open(j)) if os.path.exists(j) else None)


# 1. x4's counts on the clean release ELF
rc, out, j = jrun(REL)
s = j['census']['summary']
got = (s['definitions'], s['live'], s['compiled_out'], s['live_without_symbol'], s['symbols'],
       s['clone_symbols'], s['global_symbols'], s['text_bytes'])
C.check(rc == 0 and got == (335, 280, 55, 127, 159, 22, 25, 104380),
        'release: %d defs / %d live / %d out / %d no symbol / %d symbols / %d clones / %d global / %d B '
        '(x4: 335/280/55/127/159/22/25/104,380)' % got)
C.check(s['live_without_symbol_inlined'] + s['live_without_symbol_dead'] == s['live_without_symbol'],
        'the 127 split by DWARF: %d inlined, %d dead (new: x4 could not tell them apart)' % (
            s['live_without_symbol_inlined'], s['live_without_symbol_dead']))
fr = {r['name']: r for r in j['census']['functions']}
C.check(fr['audio_status_frame']['status'] == 'inlined' and
        list(fr['audio_status_frame']['inline']['callers']) == ['main'],
        'audio_status_frame: inlined into %s' % list(fr['audio_status_frame']['inline']['callers']))
C.check(fr['power_request_poll']['status'] == 'clone' and fr['power_request_poll']['clone_kinds'] == ['part'],
        'power_request_poll: emitted only as %s (x4 refuted 2) + inlined into %s' % (
            fr['power_request_poll']['symbols'], sorted(fr['power_request_poll']['inline']['callers'])))
C.check('lat_swap.isra.0' in fr['us_to_vblank']['inline']['callers'],
        'callers are named by symbol: us_to_vblank -> %s' % sorted(fr['us_to_vblank']['inline']['callers']))

# 2. lab / harness: recorded, and the scanner agrees with the ELF everywhere
for fl in FLAVOURS:
    p = elf('run1', fl)
    if not os.path.exists(p):
        continue
    rc, out, jj = jrun(p)
    ss = jj['census']['summary']
    C.check(rc == 0 and not ss['scan_disagreements'],
            '%s: %d live / %d out, %d no symbol (%d inlined, %d dead), %d symbols (%d clones, %d global), '
            '%d inline instances; 0 scan disagreements' % (
                fl, ss['live'], ss['compiled_out'], ss['live_without_symbol'], ss['live_without_symbol_inlined'],
                ss['live_without_symbol_dead'], ss['symbols'], ss['clone_symbols'], ss['global_symbols'],
                ss['inline_instances']))

# 2b. the scanner fix is what makes lab agree (x4_src.py behaviour = mutant)
ef_srcscan.JOIN_CONTINUATIONS = False
try:
    old = IC.census(elf('run1', 'lab'), SRC, 'lab')['summary']
finally:
    ef_srcscan.JOIN_CONTINUATIONS = True
C.check(len(old['scan_disagreements']) == 7 and 'stall_thread' in old['scan_disagreements'],
        'x4_src.py first-line-only #if: lab has %d disagreements (%s): caught' % (
            len(old['scan_disagreements']), ', '.join(old['scan_disagreements'])))

# 3. no change on identical builds and on the pad pair
for fl in FLAVOURS:
    a, b = elf('run1', fl), elf('run2', fl)
    if os.path.exists(a) and os.path.exists(b):
        rc, out, jj = jrun(b, '--against', a)
        C.check(rc == 0 and not jj['changes'], '%s run2 --against run1: %d changes' % (fl, len(jj['changes'])))
if os.path.exists(PAD144):
    rc, out, jj = jrun(PAD144, '--against', REL)
    C.check(rc == 0 and not jj['changes'], 'pad144 --against pad304: %d changes' % len(jj['changes']))


# 4. mutants
def strtab_rename(m, old, new):
    assert len(old) == len(new)
    st = m.e.by_name['.strtab']
    blob = bytes(m.d[st['offset']:st['offset'] + st['size']])
    k = blob.find(b'\0' + old.encode() + b'\0')
    assert k >= 0, old
    o = st['offset'] + k + 1
    m.d[o:o + len(new)] = new.encode()
    m.notes.append('strtab %s -> %s' % (old, new))


def mutant_run(m, label, expect_names, allow=None):
    p = m.save()
    args = [p, '--flavour', 'release', '--against', REL] + (['--allow', allow] if allow else [])
    rc, out, jj = jrun(*args)
    names = sorted(x['name'] for x in jj['changes'] if not x['allowed'])
    ok = (rc == 1 and names == sorted(expect_names)) if not allow else rc == 0
    C.check(ok, '%s: exit %d, changes %s' % (label, rc, [(x['name'], x['why']) for x in jj['changes']][:4]))
    m.cleanup()


m = Mutant(REL); strtab_rename(m, 'lat_swap.isra.0', 'lat_swap.part.0')
mutant_run(m, 'clone kind renamed (lat_swap.isra.0 -> .part.0; us_to_vblank is inlined INTO it)',
           ['lat_swap', 'us_to_vblank'])

m = Mutant(REL); strtab_rename(m, 'exit_cb', 'exit_cX')
mutant_run(m, 'emitted function renamed away (exit_cb)', ['exit_cb', 'exit_cX'])

m = Mutant(REL); strtab_rename(m, 'exit_cb', 'exit_cX')
mutant_run(m, '... same, with --allow exit_cb,exit_cX', [], allow='exit_cb,exit_cX')


def attr_pos(info, die, at):
    """byte offset in the file of attribute `at`'s value in `die`."""
    d = info.elf.data
    cu = die.cu
    tab = info._abbrevs(cu.abbrev_off)
    code, p = D.uleb(d, die.offset)
    tag, kids, specs = tab[code]
    for a, fm, imp in specs:
        if a == at:
            return p, fm
        _, p = D._form_value(info.elf, d, p, fm, cu.addr_size, cu, cu.version, imp)
    raise LookupError(at)


m = Mutant(REL)
info = D.Info(m.e)
cu = info.cu_named('main_psp.c')
inl = cu.inlined()
t = [x for x in inl if x['name'] == 'turbo_frame']
a = [x for x in inl if x['name'] == 'audio_status_frame']
pos, fm = attr_pos(info, t[0]['die'], D.AT_abstract_origin)
target = a[0]['die'].get(D.AT_abstract_origin)[1] - cu.offset
assert fm == 0x13, fm                       # ref4
struct.pack_into('<I', m.d, pos, target)
m.notes.append('turbo_frame instance -> audio_status_frame')
mutant_run(m, 'DWARF: turbo_frame\'s only inline instance re-pointed to audio_status_frame (%d instance)' % len(t),
           ['audio_status_frame', 'turbo_frame'])


# 5. move functions across fragments (PLAN §1.1: psp/fe/<name>.inc.c included
#    into main_psp.c, same CU).  The ELF is unchanged, so the census of the
#    split source must equal the census of the original: 0 changes.  The
#    first version scanned main_psp.c only: 7 false changes, and an
#    inlined-only function moved into a fragment had no row at all (a change
#    in its inlining between two split trees was invisible).
def split_source(dst, names):
    ef_srcscan.INCLUDE_FRAGMENTS = False
    try:
        rows = {f['name']: f for f in ef_srcscan.scan(SRC, ('release',))['functions']}
    finally:
        ef_srcscan.INCLUDE_FRAGMENTS = True
    lines = open(SRC, encoding='utf-8', errors='replace').read().split('\n')
    os.makedirs(os.path.join(dst, 'fe'), exist_ok=True)
    for s, e, n in sorted(((rows[n]['start'], rows[n]['end'], n) for n in names), reverse=True):
        open(os.path.join(dst, 'fe', n + '.inc.c'), 'w', encoding='utf-8').write('\n'.join(lines[s - 1:e]) + '\n')
        lines[s - 1:e] = ['#include "fe/%s.inc.c"' % n]
    p = os.path.join(dst, 'main_psp.c')
    open(p, 'w', encoding='utf-8').write('\n'.join(lines))
    return p


MOVED = ['audio_status_frame', 'vm_lock', 'exit_cb', 'heap_census', 'online_build_id']
d = tempfile.mkdtemp(prefix='elf-func-frag-')
sp = split_source(d, MOVED)
j = os.path.join(d, 'o.json')
rc, out = run_tool(TOOL, REL, '--src', sp, '--src-base', SRC, '--against', elf('run2', 'release'), '--json', j)
jj = json.load(open(j))
C.check(rc == 0 and not jj['changes'] and jj['census']['summary']['definitions'] == 335,
        'split source (%s moved to fe/*.inc.c) vs original: exit %d, %d changes, %d definitions' % (
            ', '.join(MOVED), rc, len(jj['changes']), jj['census']['summary']['definitions']))
fr2 = {r['name']: r for r in jj['census']['functions']}
C.check(fr2['audio_status_frame']['status'] == 'inlined' and fr2['audio_status_frame']['src_file'].endswith(
    'audio_status_frame.inc.c'), 'audio_status_frame found in its fragment, still inlined into main')
ef_srcscan.INCLUDE_FRAGMENTS = False
try:
    old = IC.census(REL, sp, 'release')
finally:
    ef_srcscan.INCLUDE_FRAGMENTS = True
C.check('audio_status_frame' not in {r['name'] for r in old['functions']},
        'main_psp.c-only scan (first version) loses audio_status_frame entirely: caught')

sys.exit(C.done())
