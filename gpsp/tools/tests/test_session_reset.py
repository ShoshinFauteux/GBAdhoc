#!/usr/bin/env python3
"""The in-process return's reset: what every game starts from (Epoch C C3-1).

Before the in-process return goes back to the game list, main() sets again the
per-game state a fresh boot starts from (docs/INPROC-RETURN.md).  Epoch C C3-1
moved that list into session_reset() (psp/fe/session.inc.c, always_inline):
main()'s locals by address, the boot's latched values by value.

Asserted on the production text (main_psp.c with its fragments expanded):
  * the list is exactly the start tree's (C0b, f9243b87): the same 29 statements
    in the same order, read with `*name` as `name`;
  * main() calls it with each local's own address in its parameter's place and the
    boot values in theirs (a swapped pair would compile and reset the wrong thing);
  * every by-address parameter is written once and every by-value one read once.
Then each property is broken in the text (a mutant) and must be caught.  On a tree
before C3-1 the list is read from main() itself, so the test also holds there.
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
DEF = re.compile(r'^static [^\n]*\bvoid session_reset\(([^)]*)\)\s*\{\n(.*?)\n\}\n', re.M | re.S)
CALL = re.compile(r'\bsession_reset\(([^;{]*)\);')


def statements(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return [re.sub(r'\s+', ' ', s.strip()) + ';' for s in text.split(';') if s.strip()]


def check(source):
    """-> list of problems (empty: the reset is the start tree's)."""
    bad = []
    m = DEF.search(source)
    if not m:
        a = source.find('         g_running = !g_exit_req;\n')
        b = source.find('         g_autoload_from_shelf = 0;\n', a)
        if a < 0 or b < 0:
            return ['no session_reset() and no reset list in main()']
        got = statements(source[a:b + len('         g_autoload_from_shelf = 0;\n')])
        return [] if got == EXPECTED else ['main()\'s reset list differs: %s' % _first_diff(got)]
    params = [p.strip() for p in m.group(1).split(',')]
    ptr = [re.search(r'\*\s*(\w+)$', p).group(1) for p in params if '*' in p]
    val = [re.search(r'(\w+)$', p).group(1) for p in params if '*' not in p]
    body = m.group(2)
    got = statements(re.sub(r'\*(%s)\b' % '|'.join(ptr), r'\1', body)) if ptr else statements(body)
    if got != EXPECTED:
        bad.append('session_reset differs from the start tree\'s list: %s' % _first_diff(got))
    for n in ptr:
        w = len(re.findall(r'\*%s\s*=[^=]' % n, body))
        if w != 1:
            bad.append('by-address %s written %d times' % (n, w))
    for n in val:
        r = len(re.findall(r'\b%s\b' % n, body))
        if r != 1:
            bad.append('by-value %s read %d times' % (n, r))
    calls = CALL.findall(source[m.end():])
    if len(calls) != 1:
        bad.append('session_reset called %d times (once: the in-process return)' % len(calls))
    else:
        args = [re.sub(r'\s+', '', x) for x in calls[0].split(',')]
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
    have = DEF.search(source) is not None
    print('  (%s)' % ('session_reset() in psp/fe/session.inc.c' if have else 'the list in main()'))
    if have:
        muts = [
            ('a statement dropped', '   *chord_frames = 0;\n', ''),
            ('two statements swapped', '   g_wake_have = 0;\n   g_wake_menu = 0;\n',
             '   g_wake_menu = 0;\n   g_wake_have = 0;\n'),
            ('a value changed', '   *ff_last_mode = -1;\n', '   *ff_last_mode = 0;\n'),
            ('a pointer write turned into a parameter write', '   *prev_pad = 0;\n', '   prev_pad = 0;\n'),
            ('two arguments swapped', '&ff_engaged, &ff_toggled', '&ff_toggled, &ff_engaged'),
            ('two boot values swapped', 'sram_thread_boot,\n', 'SWAPTMP,\n'),
        ]
        for what, a, b in muts:
            if source.count(a) < 1:
                expect(False, 'mutant %s: anchor not found' % what)
                continue
            if b == 'SWAPTMP,\n':
                k = source.rindex('session_reset(')
                tail = source[k:].replace('me_rend_cfg_boot, sram_thread_boot', 'sram_thread_boot, me_rend_cfg_boot', 1)
                mut = source[:k] + tail
            elif what == 'two arguments swapped':
                k = source.rindex('session_reset(')
                mut = source[:k] + source[k:].replace(a, b, 1)
            else:
                mut = source.replace(a, b, 1)
            expect(mut != source and bool(check(mut)), 'mutant caught: %s' % what)
    print('session_reset: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
