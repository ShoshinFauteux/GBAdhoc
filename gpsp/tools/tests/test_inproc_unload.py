#!/usr/bin/env python3
"""The in-process return's unload unit (Epoch C C3-2a: inproc_unload).

Before the census compares the heap with the one this game's boot started from, the
in-process return gives back what the exit path leaves to the process ending: the
unit U1-U12 of docs/INPROC-RETURN.md.  Epoch C C3-2a moved it, unchanged, into
inproc_unload() (psp/fe/teardown.inc.c, noinline in .text.fe_cold.inproc_unload).

Asserted on the production text (main_psp.c with its fragments expanded):
  * the unit is exactly the start tree's (C0b, f9243b87): the same statements in the
    same order, under the same #ifdef (comments ignored);
  * it is called exactly once, from main()'s in-process return: after the second
    inproc_refusal() said yes (`if (!why)`) and after the audio_hash line, and right
    before `heap_snap(&after)` -- the census reads the heap it leaves;
  * it is a void(void) function in its own .text.fe_cold section, so nothing of
    main()'s escapes into it.
Then each property is broken in the text (a mutant) and must be caught.  On a tree
before C3-2a the unit is read from main() itself, so the test also holds there.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import extract  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# the start tree's unit (C0b f9243b87, main() after `if (!why)`), comments dropped
EXPECTED = [
    'dynarec_release_translation_caches();',
    'if (dynarec_heap_tables_release) dynarec_heap_tables_release();',
    '#ifdef GBA_LINK',
    'if (g_cable_reserve) { free(g_cable_reserve);', 'g_cable_reserve = NULL;', '}',
    'g_gba_cable = 0;',
    '#endif',
    'me_rend_suspend();',
    'sceKernelDcacheWritebackInvalidateAll();', 'sceKernelIcacheInvalidateAll();',
    'home_host_forget();', 'fe_host_session_reset();', 'osd_reset();', 'ui_close();',
    'vid_ambient_drop();', 'audio_stop();', 'audio_ring_reset();', 'audio_start();',
]
DEF = re.compile(r'^static __attribute__\(\(noinline, section\("\.text\.fe_cold\.inproc_unload"\)\)\) '
                 r'void inproc_unload\(void\)\n\{\n(.*?)\n\}\n', re.M | re.S)
CALL = '         inproc_unload();\n'


def statements(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    out = []
    for ln in text.split('\n'):
        if ln.strip().startswith('#'):
            out.append('\x00' + ln.strip() + '\x01')
        else:
            out.append(ln)
    flat = re.sub(r'[ \t\n]+', ' ', ' '.join(out))
    res = []
    for piece in re.split(r'(\x00[^\x01]*\x01)', flat):
        if piece.startswith('\x00'):
            res.append(re.sub(r'\s+', ' ', piece[1:-1]))
            continue
        for s in re.split(r'(?<=;)|(?<=\})', piece):
            s = s.strip()
            if s:
                res.append(s)
    return res


def check(source):
    """-> list of problems (empty: the unit is the start tree's, called in its place)."""
    m = DEF.search(source)
    # Epoch C C3-2b: the decision is inproc_try_return(); read main() with it in place
    main = getattr(extract, 'epochc_unloop', lambda m, s: m)(extract.extract(source, 'fn:main'), source)
    if not m:
        a = main.find('         dynarec_release_translation_caches();\n')
        b = main.find('         audio_start();\n', a)
        if a < 0 or b < 0:
            return ['no inproc_unload() and no unload unit in main()']
        got = statements(main[a:b + len('         audio_start();\n')])
        return [] if got == EXPECTED else ['main()\'s unload unit differs: %s' % _first_diff(got)]
    bad = []
    got = statements(m.group(1))
    if got != EXPECTED:
        bad.append('inproc_unload differs from the start tree\'s unit: %s' % _first_diff(got))
    n = len(re.findall(r'\binproc_unload\s*\(', source[m.end():]))
    if n != 1 or main.count(CALL) != 1:
        bad.append('inproc_unload called %d times, %d of them main()\'s statement (once: the '
                   'in-process return)' % (n, main.count(CALL)))
        return bad
    k = main.index(CALL)
    before, after = main[:k], main[k + len(CALL):]
    if not after.startswith('         heap_snap(&after);\n'):
        bad.append('the census\'s heap_snap(&after) does not follow the unload')
    i_why = before.rfind('      if (!why)\n')
    i_ref = before.rfind('inproc_refusal(have_variant)')
    i_hash = before.rfind('fe_evt("audio_hash %08x samples=%u"')
    if not (0 <= i_ref < i_hash < i_why):
        bad.append('the unload is not under `if (!why)` after the second inproc_refusal and the '
                   'audio_hash line')
    elif re.search(r'\n      \}\n', before[i_why:]):
        bad.append('the unload is outside the `if (!why)` block')
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
    expect(not probs, 'the production unload is the start tree\'s unit, called in its place%s'
           % ('' if not probs else ': ' + '; '.join(probs)))
    have = DEF.search(source) is not None
    print('  (%s)' % ('inproc_unload() in psp/fe/teardown.inc.c' if have else 'the unit in main()'))
    if have:
        def swap_call(src):
            # the call moved below the census's heap_snap(&after)
            return src.replace(CALL + '         heap_snap(&after);\n',
                               '         heap_snap(&after);\n' + CALL, 1)

        def call_before_refusal(src):
            a = src.index(CALL)
            src = src[:a] + src[a + len(CALL):]
            k = src.index('      const char *why = inproc_refusal(have_variant);\n')
            return src[:k] + CALL + src[k:]
        muts = [
            ('a statement dropped', lambda s: s.replace(
                '   sceKernelIcacheInvalidateAll();\n   home_host_forget();\n',
                '   sceKernelIcacheInvalidateAll();\n', 1)),
            ('two statements swapped', lambda s: s.replace(
                '   audio_stop();\n   audio_ring_reset();\n',
                '   audio_ring_reset();\n   audio_stop();\n', 1)),
            ('the cable reserve taken out of #ifdef GBA_LINK', lambda s: s.replace(
                '#ifdef GBA_LINK\n   /* The GBA link\'s cable reserve', '#if 1\n   /* The GBA link\'s cable reserve', 1)),
            ('the call after the census\'s heap_snap', swap_call),
            ('the call ahead of the second inproc_refusal', call_before_refusal),
            ('the call dropped', lambda s: s.replace(CALL, '', 1)),
            ('a second call', lambda s: s.replace(CALL, CALL + CALL, 1)),
        ]
        for what, f in muts:
            try:
                mut = f(source)
            except ValueError:
                expect(False, 'mutant %s: anchor not found' % what)
                continue
            expect(mut != source and bool(check(mut)), 'mutant caught: %s' % what)
    print('inproc_unload: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
