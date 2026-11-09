#!/usr/bin/env python3
"""source-split_test_extract.py -- self-test of gpsp/tools/tests/extract.py (T7).

    python source-split_test_extract.py [TRACE.json] [MAIN_PSP_C]

TRACE.json is extract_trace.py's record of the byte ranges every host test
slices out of main_psp.c today (default: builds/mainpsp-tools-out/
scratch-source-split/trace-01067e74.json; regenerate with extract_trace.py).

  C1 enumerate: the readers found by grep (12 on 01067e74; PLAN: 12 at x1)
  C2 every traced region of every test, planted as markers all at once in one
     file (overlapping and nested regions included), extracts to EXACTLY the
     text the old slicing took
  C3 strip_markers(planted) == the original; --check finds no error
  C4 own-line markers: a region whose bounds are line starts is planted with
     markers on their own lines, and still extracts identically
  M1 a missing end marker          -> ExtractError
  M2 a duplicated begin marker     -> ExtractError
  M3 end before begin              -> ExtractError
  M4 an unknown name               -> ExtractError
  M5 a marker inside the region of another is not leaked into its text
  M6 the trace is stale (the source changed under it) -> detected (hash/size)
  (adversarial review 2026-11-05; each was a false pass before the fix)
  M7 plant() at a position inside a block comment   -> ExtractError (the marker's */ would end it)
  M8 plant() inside a string literal                -> ExtractError (.rodata change)
  M9 a region crossing a fragment cut, read from a split tree: read_source() gives the old
     slice; plain concatenation (shell + fe/*.inc.c sorted) would not
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GPSP = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(GPSP, 'tools', 'tests'))
import extract as X  # noqa: E402

FAILS = []
DEF_TRACE = os.path.normpath(os.path.join(GPSP, '..', '..', 'mainpsp-tools-out', 'scratch-source-split',
                                          'trace-01067e74.json'))


def ok(m):
    print('  ok    ' + m)


def bad(m):
    print('  FAIL  ' + m)
    FAILS.append(m)


def raises(label, fn):
    try:
        fn()
    except X.ExtractError as e:
        ok('%s: ExtractError (%s)' % (label, e))
        return
    bad('%s: no error' % label)


def run(trace=DEF_TRACE, src_path=None):
    src_path = src_path or os.path.join(GPSP, 'psp', 'main_psp.c')
    print('extract.py self-test on %s' % src_path)
    src = open(src_path, encoding='utf-8', newline='').read()
    rd = X.enumerate_readers(GPSP)
    (ok if len(rd) == 12 else bad)('C1 enumerate: %d host tests read main_psp.c as text' % len(rd))
    if not os.path.exists(trace):
        bad('no trace %s (run extract_trace.py)' % trace)
        return 1
    tr = json.load(open(trace))
    if tr['size'] != len(src):
        bad('M6/C2 the trace is stale: it was taken on %d chars, the source has %d' % (tr['size'], len(src)))
        return 1
    ok('M6 trace matches the source (%d chars); a changed source is detected by size' % len(src))
    regions = {}
    per = {}
    for t, v in sorted(tr['tests'].items()):
        tag = os.path.basename(t)[:-3]
        for k, (a, b) in enumerate(v['regions']):
            regions['%s.%d' % (tag, k)] = (a, b)
        per[t] = len(v['regions'])
    planted = X.plant(src, regions)
    bad_n = [n for n, (a, b) in regions.items() if X.extract(planted, n) != src[a:b]]
    if not bad_n:
        ok('C2 %d traced regions from %d tests (%d distinct spans) planted together: every extraction identical '
           'to the old slicing' % (len(regions), len(per), len(set(regions.values()))))
    else:
        bad('C2 %d region(s) differ: %s' % (len(bad_n), bad_n[:5]))
    if X.strip_markers(planted) == src and not X.check(planted):
        ok('C3 strip_markers(planted) == source; --check clean (%d markers)' % (2 * len(regions)))
    else:
        bad('C3 strip/check')
    own = sum(1 for (a, b) in regions.values() if (a == 0 or src[a - 1] == '\n') and (b == 0 or src[b - 1] == '\n'))
    ok('C4 %d of %d regions start and end on line boundaries (own-line markers); %d need an inline marker'
       % (own, len(regions), len(regions) - own))
    # mutants on a small text
    t = 'a\n/* @extract:begin R */\nbody\n/* @extract:end R */\nz\n'
    if X.extract(t, 'R') != 'body\n':
        bad('own-line semantics: %r' % X.extract(t, 'R'))
    raises('M1 missing end', lambda: X.extract('/* @extract:begin R */x', 'R'))
    raises('M2 duplicated begin', lambda: X.extract(t.replace('a\n', '/* @extract:begin R */\n'), 'R'))
    raises('M3 end before begin', lambda: X.extract('/* @extract:end R */x/* @extract:begin R */', 'R'))
    raises('M4 unknown name', lambda: X.extract(t, 'NOPE'))
    t5 = X.plant('0123456789\nabc\n', {'A': (2, 15), 'B': (4, 11)})
    if X.extract(t5, 'A') == '23456789\nabc\n' and X.extract(t5, 'B') == '456789\n' and '@extract' not in X.extract(t5, 'A'):
        ok('M5 nested regions: no marker leaks into the outer text')
    else:
        bad('M5 nested: %r / %r' % (X.extract(t5, 'A'), X.extract(t5, 'B')))
    # review 2026-11-05 mutants (each was a false pass before the fix)
    raises('M7 plant inside a block comment', lambda: X.plant('int a; /* note */ int b;\n', {'R': (10, 21)}))
    raises('M8 plant inside a string literal', lambda: X.plant('char *s = "ab cd";\n', {'R': (13, 18)}))
    # M9: a region crossing a fragment cut, read from a split tree
    import tempfile, shutil
    sys.path.insert(0, os.path.join(GPSP, 'tools', 'refactor'))
    import split as S
    work = tempfile.mkdtemp(prefix='extest-')
    try:
        # the 01067e74 cutlist (this test's source); tools/refactor/cutlist.txt follows the tree
        fr, ro = S.read_cutlist(os.path.join(GPSP, 'tools', 'refactor', 'tests', 'fixtures', 'cutlist_01067e74.txt'))
        m = S.Model(src)
        lines = src.splitlines(keepends=True)
        # the cut between two fragments whose names sort in source order with
        # others between them (link.e -> mgift: merend.* sort in between)
        fa = {f['frag']: f for f in fr}
        la = m.resolve(fa['mgift']['occ'], fa['mgift']['anchor'])
        pi = max((it for it in m.items if it['end'] < la), key=lambda it: it['end'])
        ni = min((it for it in m.items if it['start'] >= la), key=lambda it: it['start'])
        a0 = sum(len(x) for x in lines[:pi['start'] - 1])
        b0 = sum(len(x) for x in lines[:ni['end']])
        pl = X.plant(src, {'XC': (a0, b0)})
        out, shell, _ = S.do_split(S.Model(pl), fr, ro)
        S.write_split(work, out, shell)
        shp = os.path.join(work, 'psp', 'main_psp.c')
        fe = sorted(os.path.join(work, 'psp', 'fe', f) for f in os.listdir(os.path.join(work, 'psp', 'fe')))
        good = X.extract(X.read_source(shp), 'XC') == src[a0:b0] == X.extract(X._read_sources([shp] + fe), 'XC')
        cat = ''.join(open(p, encoding='utf-8', newline='').read() for p in [shp] + fe)
        try:
            naive = X.extract(cat, 'XC') == src[a0:b0]
        except X.ExtractError:
            naive = False
        (ok if good and not naive else bad)('M9 region L:%d-%d across the link.e|mgift cut: read_source gives the old '
                                            'slice (%s); plain concatenation would not (%s)' % (pi['start'], ni['end'], good, not naive))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('extract.py self-test: %d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run(*(sys.argv[1:3])))
