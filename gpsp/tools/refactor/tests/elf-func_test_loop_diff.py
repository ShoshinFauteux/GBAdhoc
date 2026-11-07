#!/usr/bin/env python3
"""Self-test + mutant tests for loop_diff.py (T1b, gate G2L).

Acceptance (PLAN §5 T1b): 220 lines / 115 sets by line on the clean base;
the by-address window recorded; a seeded edit inside audio_status_frame
(L:850, inlined into the loop) is reported as a loop change.  Also: the
9,912 B inlined-helper figure (x4:90), determinism pairs, the pad 304/144
pair (main does not move: the pad is after it), an edit outside the window
(body pass only), an edit on a boot row GCC placed INSIDE the window, and a
synthetic placement move (exit 3).
"""
import contextlib
import copy
import io
import struct
import os
import sys
import tempfile
import json

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from elf_func_lib import Checks, Mutant, run_tool, elf, FLAVOURS, PAD144  # noqa: E402
import loop_diff as LD  # noqa: E402
import ef_mips as M  # noqa: E402

C = Checks('loop_diff selftest')
TOOL = 'loop_diff.py'
REL = elf('run1', 'release')


def jrun(*args):
    j = os.path.join(tempfile.mkdtemp(prefix='elf-func-ld-'), 'o.json')
    rc, out = run_tool(TOOL, *(list(args) + ['--json', j]))
    return rc, out, (json.load(open(j)) if os.path.exists(j) else None)


# 1. the clean base, release: x4's figures
rc, out, j = jrun(REL)
s = j['elfs'][0]
b = s['by_line']
C.check((b['lines'], b['sets'], b['max_per_set'], b['off_lo'], b['off_hi']) == (220, 115, 3, 10372, 34432),
        'release by line: %d lines / %d sets / max %d, off %d..%d (x4: 220/115/3, 10,372..34,432)' % (
            b['lines'], b['sets'], b['max_per_set'], b['off_lo'], b['off_hi']))
ih = s['inlined_helper_bytes']['line_rows_before_main']
C.check(ih == 9912, 'inlined helpers before L:13837 in main: %d B (x4:90: 9,912)' % ih)
C.check(s['start_mod_8192'] == 6912 and s['start'] == '0893bb00',
        'main @%s, start mod 8192 = %d' % (s['start'], s['start_mod_8192']))
w = s['window']
print('     by-address window (new baseline): off %d..%d, %d B, %d lines / %d sets, composition %s' % (
    w['off_lo'], w['off_hi'], w['bytes'], w['lines'], w['sets'], w['composition']))
C.check(w['composition']['unattributed'] == 0 and sum(w['composition'].values()) == w['bytes'],
        'every window byte is attributed (%s)' % w['composition'])
helpers = w['loop_inlined_helpers']
want = ['audio_status_frame', 'rfu_link_down_drain', 'exit_assist_frame', 'rfu_trace_drain', 'turbo_frame']
C.check(all(h in helpers for h in want),
        'PLAN-named loop helpers are in the window: %s' % {h: helpers.get(h) for h in want})

# lab / harness reports (recorded; no PLAN figure exists for them)
for fl in ('lab', 'harness'):
    p = elf('run1', fl)
    if os.path.exists(p):
        rc, out, jj = jrun(p)
        ss = jj['elfs'][0]
        C.check(rc == 0, '%s report: main @%s mod %d, by line %d/%d, window %d B %d lines/%d sets' % (
            fl, ss['start'], ss['start_mod_8192'], ss['by_line']['lines'], ss['by_line']['sets'],
            ss['window']['bytes'], ss['window']['lines'], ss['window']['sets']))

# 2. determinism pairs
for fl in FLAVOURS:
    a, bb = elf('run1', fl), elf('run2', fl)
    if os.path.exists(a) and os.path.exists(bb):
        rc, out, jj = jrun(a, bb)
        c = jj['compare']
        C.check(rc == 0 and c['window']['state'] == 'same-bytes' and c['body']['state'] == 'same-bytes',
                '%s run1 vs run2: exit %d, window %s, body %s' % (fl, rc, c['window']['state'], c['body']['state']))

# 3. pad 304/144: main precedes the pad -> same place, same-mod-address
if os.path.exists(PAD144):
    rc, out, jj = jrun(REL, PAD144)
    c = jj['compare']
    C.check(rc == 0 and c['window']['state'] == 'same-mod-address',
            'pad 304 vs 144: exit %d, window %s, main mod 8192 %s' % (
                rc, c['window']['state'], c['placement']['main_mod_8192']))

# 4. mutants
L = LD.Loop(REL)


def inl_addr(Lx, name):
    for x in Lx.loop_inl:
        if x['name'] == name:
            a, bnd = x['ranges'][0]
            return a + 4 * ((bnd - a) // 8), x     # a word in the middle of its first range
    raise LookupError(name)


def flip(src, off_in_main, note):
    m = Mutant(src)
    lo = [s for s in m.e.funcs() if s['name'] == 'main'][0]['value']
    a = lo + off_in_main
    m.put(a, m.word(a) ^ 1, note)
    return m, m.save()


# 4a. THE acceptance mutant: a word of audio_status_frame inlined into the loop
a, inst = inl_addr(L, 'audio_status_frame')
off = a - L.lo
row = [(f, l) for s0, s1, f, l in L.spans if s0 <= a < s1][0]
C.check(not (16368 <= row[1] <= 18090),
        'audio_status_frame word +%d carries line %s:%d, OUTSIDE 16368-18090 (a line-range diff would miss it); '
        'called from L:%s' % (off, os.path.basename(row[0]), row[1], inst['call_line']))
for base, src in ((REL, REL), (REL, PAD144)):
    if not os.path.exists(src):
        continue
    m, p = flip(src, off, 'audio_status_frame bit')
    try:
        rc, out, jj = jrun(base, p, '--any-flavour')
        c = jj['compare']
        d = c['window']['diffs']
        C.check(rc == 1 and c['window']['state'] == 'CHANGED' and len(d) == 1 and
                'audio_status_frame' in d[0]['where'] and d[0]['klass'] == 'loop-helper',
                'seeded edit in inlined audio_status_frame (%s): exit %d, %s' % (
                    'A vs mut(A)' if src == REL else 'pad304 vs mut(pad144)', rc,
                    d[0]['where'] + ' / ' + d[0]['why'] if d else '-'))
    finally:
        m.cleanup()

# 4b. an edit OUTSIDE the window (a boot row before offset 10,372): loop same, body changed
boot = [(s0, f, l) for s0, s1, f, l in L.spans if s0 < L.wlo and L.isrc(f) and 13837 < l < 15251
        and not L.helper_at(s0, loop_only=False)]
s0 = boot[len(boot) // 2][0]
m, p = flip(REL, s0 - L.lo, 'boot row bit')
try:
    rc, out, jj = jrun(REL, p)
    c = jj['compare']
    C.check(rc == 0 and c['window']['state'] == 'same-bytes' and c['body']['state'] == 'CHANGED'
            and c['body']['outside_window'] == 1,
            'edit on boot row L:%d (+%d, before the window): exit %d, window %s, body %s' % (
                boot[len(boot) // 2][2], s0 - L.lo, rc, c['window']['state'], c['body']['state']))
    rc2, _ = run_tool(TOOL, REL, p, '--body-strict')
    C.check(rc2 == 1, '... and --body-strict fails it (exit %d)' % rc2)
finally:
    m.cleanup()

# 4c. a boot/session row that GCC placed INSIDE the window is a window change
inside = [(s0, l) for s0, s1, f, l in L.spans if L.wlo <= s0 < L.whi and L.isrc(f)
          and not (16368 <= l <= 18090) and l >= 13837 and L.helper_at(s0) is None]
s0, ln = inside[len(inside) // 2]
m, p = flip(REL, s0 - L.lo, 'foreign row in window')
try:
    rc, out, jj = jrun(REL, p)
    d = jj['compare']['window']['diffs']
    C.check(rc == 1 and d and d[0]['klass'] == 'other-main-line',
            'edit on L:%d (+%d, a non-loop row inside the window, %d such rows): exit %d, class %s' % (
                ln, s0 - L.lo, len(inside), rc, d[0]['klass'] if d else '-'))
finally:
    m.cleanup()

# 4d. synthetic placement move: same bytes, main 64 B later -> exit 3
B = copy.copy(L)
shift = 64
B.lo, B.hi, B.wlo, B.whi = L.lo + shift, L.hi + shift, L.wlo + shift, L.whi + shift
B.wsets = LD._sets(set(range(B.wlo >> 6, ((B.whi - 1) >> 6) + 1)))
r = LD.compare(L, B)
C.check(r['rc'] == 3 and r['window']['state'] == 'same-mod-address',
        'synthetic: main +64 B, same code -> exit %d (%s, %d sets changed occupancy)' % (
            r['rc'], r['window']['state'], r['placement']['sets_with_changed_occupancy']))

# 4e. verify pass: %lo/offset +4 mutants inside the window, in the pad-144 ELF.
#     The first version (ef_mips content-only `ro:` tokens and colliding
#     canonical names) let 683 of 2,673 such in-window mutants pass, e.g.
#     `lw v0,0xf8(v1)` -> `0xfc` at +10376 (L:16368).  Sampled here: every
#     8th IMM-user word of the window must be caught.
if os.path.exists(PAD144):
    LB = LD.Loop(PAD144)
    n = len(LB.code) // 4
    ws = list(struct.unpack('<%dI' % n, LB.code))
    tried = missed = 0
    first_miss = None
    for i in range((LB.wlo - LB.lo) // 4, (LB.whi - LB.lo) // 4):
        if ws[i] >> 26 not in M.IMM_USERS:
            continue
        tried += 1
        if tried % 8:
            continue
        w2 = ws[:]; w2[i] = (ws[i] & 0xFFFF0000) | ((ws[i] + 4) & 0xFFFF)
        B = copy.copy(LB); B.code = struct.pack('<%dI' % n, *w2)
        with contextlib.redirect_stdout(io.StringIO()):
            r = LD.compare(L, B)
        if r['rc'] != 1:
            missed += 1; first_miss = first_miss or 4 * i
    C.check(missed == 0, 'window %%lo+4 mutants (every 8th of %d IMM users, pad304 vs mut(pad144)): %d missed%s' % (
        tried, missed, ' (first +%d)' % first_miss if first_miss else ''))

sys.exit(C.done())
