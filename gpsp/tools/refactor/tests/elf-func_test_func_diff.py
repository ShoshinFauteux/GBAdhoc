#!/usr/bin/env python3
"""Self-test + mutant tests for func_diff.py (T1, gate G2).

Acceptance (PLAN §5 T1, phase T Verify): on the clean 01067e74 release pair
pad 304 (baseline/run1/release) vs pad 144 (scratch-elf-func/pad144, built by
flavours.sh with FLAV_RELEASE_ENV=LAYOUT_PAD_BYTES=144): 5,163 functions, 140
identical, 5,022 identical modulo address, 1 differing (layout_pad_cold);
x4's 3 mutants caught.  Extra mutants cover what the production tool added
(VFPU %lo users, --data, --strict-weak, --hot, --allow, flavour refusal) and
a mutant in a function that MOVED (so detection is not just a byte compare).

usage: elf-func_test_func_diff.py   (env EF_BASELINE / EF_PAD144 override paths)
"""
import json
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf_func_lib import (Checks, Mutant, run_tool, elf, FLAVOURS, PAD144,  # noqa: E402
                          HOT_ORDER)
from ef_mips import canon, compare_words  # noqa: E402

C = Checks('func_diff selftest')
TOOL = 'func_diff.py'
REL = elf('run1', 'release')


def js(rc_out, path):
    return json.load(open(path))


def summary(*args):
    j = os.path.join(tempfile.mkdtemp(prefix='elf-func-fd-'), 'o.json')
    rc, out = run_tool(TOOL, *(list(args) + ['--json', j]))
    return rc, out, (json.load(open(j)) if os.path.exists(j) else None)


# 1. determinism pairs: run1 vs run2 in every flavour, with data
for fl in FLAVOURS:
    a, b = elf('run1', fl), elf('run2', fl)
    if not (os.path.exists(a) and os.path.exists(b)):
        C.skipped('%s run1/run2 missing' % fl); continue
    rc, out, j = summary(a, b, '--data')
    f = j['functions']
    C.check(rc == 0 and f['same_bytes'] == f['functions'] and f['functions'] > 5000,
            '%s run1 vs run2: rc %d, %d/%d same bytes, data %s' % (
                fl, rc, f['same_bytes'], f['functions'], j['data']))

# 2. flavour refusal: release vs lab has manifests -> exit 2
rc, out = run_tool(TOOL, REL, elf('run1', 'lab'))
C.check(rc == 2 and 'REFUSED' in out, 'release vs lab refused (rc %d)' % rc)

# 3. x4 acceptance on the clean 304/144 pair
if os.path.exists(PAD144):
    rc, out, j = summary(REL, PAD144, '--hot', HOT_ORDER)
    f = j['functions']
    diffs = [x['key'] for x in j['findings']]
    C.check((f['functions'], f['same_bytes'], f['same_mod_addr'], f['different']) == (5163, 140, 5022, 1)
            and diffs == ['layout_pad_cold'] and rc == 1,
            'pad 304/144: %d functions, %d same, %d same-mod-address, %d different %s (x4: 5163/140/5022/1 layout_pad_cold)'
            % (f['functions'], f['same_bytes'], f['same_mod_addr'], f['different'], diffs))
    C.check(dict(j['deltas']).get(-160) == 4905 and dict(j['deltas']).get(0) == 258,
            'address deltas %s (x4: -160 x4905, 0 x258)' % j['deltas'][:3])
    C.check(j['hot']['found'] == 151 and not j['hot']['set_changed'],
            'hot_order.txt: %d hot functions found, %d changed set (x4: 0/151)' % (
                j['hot']['found'], len(j['hot']['set_changed'])))
    rc, out = run_tool(TOOL, REL, PAD144, '--allow', 'layout_pad_cold')
    C.check(rc == 0, '--allow layout_pad_cold -> exit %d' % rc)
    # --hot reports a set move: exit_cb comes after the pad and moved -160 B
    hp = os.path.join(tempfile.mkdtemp(prefix='elf-func-hot-'), 'hot.txt')
    open(hp, 'w').write('# test\n@mips_stub.o\nexit_cb 0.1 16\nupdate_gba 1 100\n')
    rc, out, j = summary(REL, PAD144, '--hot', hp, '--hot-strict', '--allow', 'layout_pad_cold')
    C.check(rc == 3 and j['hot']['set_changed'] == ['exit_cb'],
            '--hot-strict: exit_cb (moved -160 B) flagged, exit %d %s' % (rc, j['hot']['set_changed']))
else:
    C.skipped('pad144 ELF absent (%s): x4 acceptance not run' % PAD144)


# 4. mutants.  Each one is tested against its own unmutated ELF (A vs A')
#    and, where the pad-144 ELF exists, mutated INTO the pad-144 ELF and
#    compared with pad 304 (so the mutant hides among 5,022 shifted functions).
def first_op(m, fn, pred, start=0):
    s = m.func(fn)
    for k in range(start, s['size'] // 4):
        a = s['value'] + 4 * k
        if pred(m.word(a)):
            return a
    raise LookupError('%s: no matching word' % fn)


def mut_x4_1(m):     # x4 #1: lat_wait_step word 40, flip immediate bit 0
    a = m.func('lat_wait_step')['value'] + 4 * 40
    m.put(a, m.word(a) ^ 1, 'imm bit'); return 'lat_wait_step'


def mut_x4_2(m):     # x4 #2: audio_thread word 20, change the rt field
    a = m.func('audio_thread')['value'] + 4 * 20
    m.put(a, m.word(a) ^ (1 << 16), 'rt field'); return 'audio_thread'


def mut_x4_3(m):     # x4 #3: plat_video_frame, first jal retargeted by +8
    a = first_op(m, 'plat_video_frame', lambda w: w >> 26 == 3)
    m.put(a, m.word(a) + 2, 'jal +8'); return 'plat_video_frame'


def mut_lo(m):       # an address %lo (addiu after lui) moved by +4 inside its object
    s = m.func('audio_thread')
    luis = {}
    for k in range(s['size'] // 4):
        a = s['value'] + 4 * k; w = m.word(a)
        if w >> 26 == 0x0F:
            luis[(w >> 16) & 31] = w
        elif w >> 26 in (0x09, 0x23) and ((w >> 21) & 31) in luis:
            m.put(a, (w & 0xffff0000) | ((w + 4) & 0xffff), '%lo +4'); return 'audio_thread'
    raise LookupError('no lo')


def mut_moved(m):    # a function AFTER the pad (moved -160 B in pad 144): flip a jal
    pad = m.func('layout_pad_cold')['value']
    for f in sorted((f for f in m.e.funcs() if f['file'] == 'main_psp.c' and f['value'] > pad),
                    key=lambda f: f['value']):
        try:
            a = first_op(m, f['name'], lambda w: w >> 26 == 3)
        except LookupError:
            continue
        m.put(a, m.word(a) + 1, 'jal +4'); return f['name']
    raise LookupError('no moved function with a jal')


MUTANTS = [('x4#1 imm bit lat_wait_step', mut_x4_1), ('x4#2 rt audio_thread', mut_x4_2),
           ('x4#3 jal +8 plat_video_frame', mut_x4_3), ('%lo +4 audio_thread', mut_lo),
           ('moved fn (after pad) jal +4', mut_moved)]

caught = 0
for label, fn in MUTANTS:
    for base, cand_src, allow in ((REL, REL, None), (REL, PAD144, 'layout_pad_cold')):
        if not os.path.exists(cand_src):
            continue
        m = Mutant(cand_src)
        try:
            who = fn(m)
            p = m.save()
            args = [base, p, '--any-flavour'] + (['--allow', allow] if allow else [])
            rc, out, j = summary(*args)
            keys = [x['key'] for x in j['findings'] if not x['allowed']]
            hit = rc == 1 and bool(keys) and keys[0].split('@')[0].split('#')[0] == canon(who)
            tag = 'A vs mut(A)' if cand_src == REL else 'pad304 vs mut(pad144)'
            if C.check(hit and len(keys) == 1,
                       'mutant %-30s %-22s exit %d, reported %s (%s)' % (
                           label, tag, rc, keys,
                           [x['detail'].split(': ', 1)[-1] for x in j['findings'] if not x['allowed']][:1])):
                caught += 1
        finally:
            m.cleanup()

# 5. --data catches a changed data word
m = Mutant(REL)
try:
    objs = [s for s in m.e.objects() if s['size'] >= 4 and m.e.section_of(s['value'])
            and m.e.section_of(s['value'])['type'] == 1 and m.e.section_of(s['value'])['name'] == '.data'
            and s['file'] == 'main_psp.c']
    o = objs[0]
    m.put(o['value'], m.word(o['value']) ^ 0x100, 'data bit')
    p = m.save()
    rc, out, j = summary(REL, p, '--data')
    rc2, _ = run_tool(TOOL, REL, p)
    C.check(rc == 1 and any(x['kind'] == 'data' and x['key'].startswith(o['name']) for x in j['findings'])
            and rc2 == 0,
            '--data catches %s word flip (exit %d), code-only run ignores it (exit %d)' % (o['name'], rc, rc2))
finally:
    m.cleanup()

# 6. --strict-weak turns the 35 listed weak sites into failures
if os.path.exists(PAD144):
    rc, out, j = summary(REL, PAD144, '--allow', 'layout_pad_cold', '--strict-weak')
    C.check(rc == 1 and j['functions']['weak_sites'] == 35 and
            all('WEAK' in x['detail'] for x in j['findings'] if not x['allowed']),
            '--strict-weak: %d weak sites listed, exit %d (each weak site is now reviewable)' % (
                j['functions']['weak_sites'], rc))


# 7. VFPU %lo users (lv.s/sv.s/lv.q/sv.q).  The clean ELFs contain NO VFPU
#    memory op at all (0 words with opcode 0x32/0x35/0x36/0x3a/0x3d/0x3e in
#    .text), so this is a synthetic unit test of the decoder.
class FakeSpace:
    def __init__(self, base):            # one 64 B object 'obj' at base
        self.base = base
    def in_range(self, a):
        return 0x08800000 <= a < 0x0a000000
    def token(self, a):
        return 'obj+%d' % (a - self.base) if self.base <= a < self.base + 64 else 'other'


def blob(*ws):
    return struct.pack('<%dI' % len(ws), *ws)


LUI = 0x3c040890                          # lui a0,0x0890
def lvs(off): return (0x32 << 26) | (4 << 21) | (off & 0xfffc)       # lv.s S000, off(a0)
def svq(off): return (0x3e << 26) | (4 << 21) | (off & 0xfffc) | 2   # sv.q, write-back bit set
SA, SB = FakeSpace(0x08900010), FakeSpace(0x08900030)                # obj moved +0x20
r = compare_words(SA, SB, 0, 0, blob(LUI, lvs(0x18), svq(0x20)), blob(LUI, lvs(0x38), svq(0x40)))
C.check(r['ok'] and r['naddr'] == 2, 'VFPU lv.s/sv.q under a +32 B layout shift: same-mod-address (%s)' % r)
r = compare_words(SA, SA, 0, 0, blob(LUI, lvs(0x18)), blob(LUI, lvs(0x1c)))
C.check(not r['ok'], 'VFPU lv.s offset +4 with no layout shift: different (%s)' % r['bad'])
r = compare_words(SA, SB, 0, 0, blob(LUI, svq(0x20)), blob(LUI, svq(0x40) ^ 2))
C.check(not r['ok'], 'VFPU sv.q write-back bit flipped under a shift: different (%s)' % r['bad'])


# 8. verify-pass mutants (false PASSes of the first version, now caught).
#    Each patches one known word; the expected original word is checked first
#    so a rebuilt baseline skips instead of testing the wrong instruction.
def site(m, name, idx, inst=0):
    hits = sorted((s for s in m.e.funcs() if s['name'] == name), key=lambda s: s['value'])
    return hits[inst]['value'] + 4 * idx


VERIFY = [
    # tier-1 lui v0 into a zero-filled gap: `addiu v0,v0,1` -> `+5` (2nd instance)
    ('tier-1 ro: gap, addiu +4', PAD144, '_Z20render_scanline_objsImL8rendtype1EEvmmmPvPKt.isra.0',
     1, 173, 0x24420001, 0x24420005),
    # canonical-name collision: the address moves to a sibling static
    ('sibling static, addiu +4', REL, 'sh_wait_frame.constprop.0', 0, 27, 0x2484ee04, 0x2484ee08),
    # WEAK pairing through two different ")" strings
    ('weak short string, lw +64', PAD144, 'gbab_load_gamepak', 0, 3, 0x8c52230c, 0x8c52234c),
]
for label, src, name, inst, idx, old, new in VERIFY:
    if not os.path.exists(src):
        C.skipped('%s: %s absent' % (label, src)); continue
    m = Mutant(src)
    try:
        a = site(m, name, idx, inst)
        if m.word(a) != old:
            C.skipped('%s: word %08x is not %08x (baseline rebuilt?)' % (label, m.word(a), old)); continue
        m.put(a, new, label)
        p = m.save()
        rc, out, j = summary(REL, p, '--any-flavour', '--allow', 'layout_pad_cold')
        keys = [x['key'] for x in j['findings'] if not x['allowed']]
        C.check(rc == 1 and len(keys) == 1 and keys[0].split('@')[0].split('#')[0] == canon(name),
                'verify mutant %-28s exit %d, reported %s' % (label, rc, keys))
    finally:
        m.cleanup()

# 9. --data sees an edited string literal behind byte-identical code
m = Mutant(REL)
try:
    ro = m.e.by_name['.rodata']
    k = bytes(m.d).find(b'GBAdhoc', ro['offset'], ro['offset'] + ro['size'])
    m.d[k] = ord('X')
    p = m.save()
    rc, out, j = summary(REL, p, '--data')
    rc2, _ = run_tool(TOOL, REL, p)
    C.check(rc == 1 and [x['kind'] for x in j['findings']] == ['anon_data'] and rc2 == 0,
            '--data: .rodata literal byte flip -> anon_data (exit %d); code-only run exit %d' % (rc, rc2))
finally:
    m.cleanup()
if os.path.exists(PAD144):
    rc, out, j = summary(REL, PAD144, '--data', '--allow', 'layout_pad_cold,module_info')
    C.check(rc == 0 and j['data']['anon_different'] == 0 and j['data']['anon_gaps'] > 50,
            '--data on pad 304/144: %d anonymous gaps, %d differ (module_info is sized and allowed)' % (
                j['data']['anon_gaps'], j['data']['anon_different']))

print('mutants caught: %d of %d runs' % (caught, sum(
    1 + os.path.exists(PAD144) for _ in MUTANTS)))
sys.exit(C.done())
