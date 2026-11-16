#!/usr/bin/env python3
"""The in-process return's reset: what every game starts from (Epoch C C3-1, tier C C-1).

Before the in-process return goes back to the game list, main() sets again the
per-game state a fresh boot starts from (docs/INPROC-RETURN.md).  Epoch C C3-1
moved that list into session_reset() (main()'s locals by address, the boot's
values by value); tier C C-1 commit 5 made it loop_reset(&L, &bc,
&autoload_state_slot), beside the loop's state (loop_t, psp/main_psp.c): `L->f`
is the loop's field f (main()'s local f before C-1), `bc->x` the boot's value x.

Asserted on the production text (main_psp.c with its fragments expanded):
  * the list is exactly the start tree's (C0b, f9243b87): the same 29 statements
    in the same order, read with `L->f`, `bc->x` and `*name` as `f`, `x`, `name`;
  * main() calls it once, with its own L, bc and autoload_state_slot in their
    parameters' places (a swapped pair would compile and reset the wrong thing);
  * every loop_t field (C-1.md commit 5) is set in loop_reset exactly once, or
    tagged `sticky` in loop_t (on its line, or on a comment line right above a
    declaration line) and then NOT set there: a new field needs a decision;
  * every boot value is read once.
Then each property is broken in the text (a mutant) and must be caught.  On a tree
before C-1 commit 5 the session_reset() rules apply; before C3-1 the list is read
from main() itself.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import extract  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# the start tree's list (C0b f9243b87, main() after `psp_clock_apply("inproc_return")`)
EXPECTED = [
    'g_running = !g_exit_req;', 'g_relaunch = 0;', 'exit_code = 0;', 'exit_reason = NULL;',
    'ff_engaged = 0;', 'ff_toggled = 0;', 'ff_last_mode = -1;', 'ff_last_me = -1;',
    'prev_pad = 0;', 'g_pad_swallow = 0;', 'chord_frames = 0;', 'g_dump_chord_pending = 0;',
    'ctl_turbo_stop(&g_turbo);', 'ui_set_turbo_locked(0);', 'inproc_menu_armed = 0;',
    'g_ff_uncapped = g_ff_mult = g_ff_user = g_ff_gate = 0;', 'g_blit_suppress = 0;',
    'g_drew = 0;', 'memset(&g_ff_gate_st, 0, sizeof(g_ff_gate_st));', 'g_wake_have = 0;',
    'g_wake_menu = 0;', 'g_last_pix = NULL;', 'g_me_rend_cfg = me_rend_cfg_boot;',
    'g_me_video_cfg = 0;', 'g_me_video = 0;', 'g_pcfg.sram_thread = sram_thread_boot;',
    'g_autoload_state = autoload_boot;', 'autoload_state_slot = 1;', 'g_autoload_from_shelf = 0;',
]
BOOT_VALUES = ('me_rend_cfg_boot', 'sram_thread_boot', 'autoload_boot')
DEF = re.compile(r'^static [^\n]*\bvoid (session_reset|loop_reset)\(([^)]*)\)\s*\{\n(.*?)\n\}\n',
                 re.M | re.S)
LOOP_T = re.compile(r'^typedef struct loop_t\n\{\n(.*?)\n\} loop_t;', re.M | re.S)


def statements(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return [re.sub(r'\s+', ' ', s.strip()) + ';' for s in text.split(';') if s.strip()]


def loop_fields(source):
    """[(field, sticky)] of loop_t, in order; None without loop_t."""
    m = LOOP_T.search(source)
    if not m:
        return None
    out, above = [], False
    for ln in m.group(1).split('\n'):
        s = ln.strip()
        if not s:
            continue
        if s.startswith('/*'):
            above = bool(re.match(r'/\*\s*sticky\b', s))
            continue
        decl = re.sub(r'/\*.*?\*/', '', s).strip().rstrip(';')
        sticky = above or bool(re.search(r'/\*\s*sticky\b', s))
        above = False
        decl = re.sub(r'^(?:const\s+)?(?:unsigned|int|long|char|short)\b(?:\s+(?:int|long))?\s*', '', decl)
        decl = re.sub(r'^(?:const\s+)?\w+\s*\*?\s*', '', decl) if not re.match(r'^[\w\s,*]+$', decl) else decl
        for name in decl.split(','):
            name = name.strip().lstrip('*').strip()
            if name:
                out.append((name, sticky))
    return out


def check(source):
    """-> list of problems (empty: the reset is the start tree's)."""
    bad = []
    m = DEF.search(source)
    if not m:
        a = source.find('         g_running = !g_exit_req;\n')
        b = source.find('         g_autoload_from_shelf = 0;\n', a)
        if a < 0 or b < 0:
            return ['no loop_reset() / session_reset() and no reset list in main()']
        got = statements(source[a:b + len('         g_autoload_from_shelf = 0;\n')])
        return [] if got == EXPECTED else ['main()\'s reset list differs: %s' % _first_diff(got)]
    name, plist, body = m.group(1), m.group(2), m.group(3)
    params = [p.strip() for p in plist.split(',')]
    ptr = [re.search(r'\*\s*(\w+)$', p).group(1) for p in params if '*' in p]
    val = [re.search(r'(\w+)$', p).group(1) for p in params if '*' not in p]
    text = body
    if name == 'loop_reset':
        text = re.sub(r'\b(?:L|bc)->', '', text)
    text = re.sub(r'\*(%s)\b' % '|'.join(ptr), r'\1', text) if ptr else text
    got = statements(text)
    if got != EXPECTED:
        bad.append('%s differs from the start tree\'s list: %s' % (name, _first_diff(got)))
    calls = re.findall(r'\b%s\(([^;{]*)\);' % name, source[m.end():])
    if len(calls) != 1:
        bad.append('%s called %d times (once: the in-process return)' % (name, len(calls)))
        return bad
    args = [re.sub(r'\s+', '', x) for x in calls[0].split(',')]
    if name == 'loop_reset':
        if params != ['loop_t *L', 'const boot_ctx *bc', 'int *autoload_state_slot']:
            bad.append('loop_reset takes %s, not (loop_t *L, const boot_ctx *bc, '
                       'int *autoload_state_slot)' % params)
        if args != ['&L', '&bc', '&autoload_state_slot']:
            bad.append('main() passes %s, the parameters are L, bc, autoload_state_slot' % args)
        for v in BOOT_VALUES:
            r = len(re.findall(r'\bbc->%s\b' % v, body))
            if r != 1:
                bad.append('boot value %s read %d times' % (v, r))
        fields = loop_fields(source)
        if fields is None:
            bad.append('no `typedef struct loop_t { .. } loop_t;`')
            fields = []
        for f, sticky in fields:
            w = len(re.findall(r'\bL->%s\s*=[^=]' % f, body))
            if sticky and w:
                bad.append('loop_t.%s is tagged sticky but loop_reset sets it' % f)
            elif not sticky and w != 1:
                bad.append('loop_t.%s set %d times in loop_reset and not tagged sticky' % (f, w))
        return bad
    for n in ptr:
        w = len(re.findall(r'\*%s\s*=[^=]' % n, body))
        if w != 1:
            bad.append('by-address %s written %d times' % (n, w))
    for n in val:
        r = len(re.findall(r'\b%s\b' % n, body))
        if r != 1:
            bad.append('by-value %s read %d times' % (n, r))
    want = ['&' + n for n in ptr] + val
    if args != want:
        bad.append('main() passes %s, the parameters are %s' % (args, want))
    return bad


def _first_diff(got):
    for k, (a, b) in enumerate(zip(got, EXPECTED)):
        if a != b:
            return 'statement %d is %r, the start tree has %r' % (k + 1, a, b)
    return '%d statements, the start tree has %d' % (len(got), len(EXPECTED))


def main():
    source = extract.read_source(str(ROOT / 'psp/main_psp.c'))
    fails = 0

    def expect(cond, what):
        nonlocal fails
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', what))
        fails += not cond

    probs = check(source)
    expect(not probs, 'the production reset is the start tree\'s list, called with matching '
           'arguments%s' % ('' if not probs else ': ' + '; '.join(probs)))
    m = DEF.search(source)
    print('  (%s)' % (m.group(1) + '()' if m else 'the list in main()'))
    if m and m.group(1) == 'loop_reset':
        fields = loop_fields(source)
        print('  loop_t: %d fields, %d set by loop_reset, %d sticky (%s)'
              % (len(fields), sum(1 for f, s in fields if not s), sum(1 for f, s in fields if s),
                 ', '.join(f for f, s in fields if s)))
        k = source.rindex('loop_reset(')
        muts = [
            ('a statement dropped', '   L->chord_frames = 0;\n', ''),
            ('two statements swapped', '   g_wake_have = 0;\n   g_wake_menu = 0;\n',
             '   g_wake_menu = 0;\n   g_wake_have = 0;\n'),
            ('a value changed', '   L->ff_last_mode = -1;\n', '   L->ff_last_mode = 0;\n'),
            ('a boot value read from the loop', 'g_pcfg.sram_thread = bc->sram_thread_boot;',
             'g_pcfg.sram_thread = L->sram_thread_boot;'),
            ('a sticky field reset', '   L->inproc_menu_armed = 0;\n',
             '   L->inproc_menu_armed = 0;\n   L->rom_fail_paused = 0;\n'),
            ('a loop_t field with no decision', '   int rom_fail_paused;',
             '   int rom_fail_paused;\n   int new_field;\n   int rom_fail_paused2;'),
        ]
        for what, a, b in muts:
            if source.count(a) < 1:
                expect(False, 'mutant %s: anchor not found' % what)
                continue
            mut = source.replace(a, b, 1)
            expect(mut != source and bool(check(mut)), 'mutant caught: %s' % what)
        mut = source[:k] + source[k:].replace('loop_reset(&L, &bc,', 'loop_reset(&bc, &L,', 1)
        expect(mut != source and bool(check(mut)), 'mutant caught: two arguments swapped')
    print('session_reset: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
