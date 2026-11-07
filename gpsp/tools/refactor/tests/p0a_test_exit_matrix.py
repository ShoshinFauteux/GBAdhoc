#!/usr/bin/env python3
"""p0a_test_exit_matrix.py -- T8 exit_matrix P0a repairs, RAW view, on any tree
(PLAN-P-beta5 §3 P0a T8).  No compiler: the -E half is tests/p0a_b5.sh.

  p0a_test_exit_matrix.py MAIN_C [--cutlist CUTLIST]

  C1  the 19 x2 sites are found BY ANCHOR (A1..C4 all assigned) on this tree
  C2  P3's sed (sceKernelExitGame() -> FE_EXIT_GAME(), handoff_run( -> FE_HANDOFF(
      after the hijack) leaves exit_sites/steps/scenarios.tsv byte-identical
  M1  the same sed with the FE_* alias switched off loses the sites (the old tool
      keyed on literal names): caught
  C3  with --cutlist: a P4-simulated split (split.py split) read through the shell's
      `#include "fe/X.inc.c"` gives the same XS tables (check_gate: 0 differences),
      and fragment sites are keyed (file, line)
  M2  the split shell read WITHOUT following the includes loses the out-of-main site
      (E:stall_exit lives in a fragment): caught by the same gate
  M3  a site whose reason string changes loses its id (anchor): the x2 check fails
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(HERE)
sys.path.insert(0, R)
import exit_matrix as X  # noqa: E402
import aec_cparse as C  # noqa: E402

FAILS = 0


def ok(m):
    print('  ok    ' + m)


def bad(m):
    global FAILS
    FAILS += 1
    print('  FAIL  ' + m)


def tables(path, outdir, follow=True, alias=X.EXIT_ALIAS):
    v = X.View('raw', C.load_raw(path, follow=follow, alias=alias))
    X.write_view(v, outdir, 'raw')
    return v


def p3_sed(text):
    lines = text.split('\n')
    hj = next(i for i, l in enumerate(lines) if l.startswith('#define sceKernelExitGame()'))
    n = 0
    for i in range(hj + 1, len(lines)):
        if lines[i].lstrip().startswith('#define'):
            continue
        new = lines[i].replace('sceKernelExitGame()', 'FE_EXIT_GAME()')
        new = re.sub(r'\bhandoff_run\(', 'FE_HANDOFF(', new)
        n += (new != lines[i])
        lines[i] = new
    # the two hijack lines become P3's macros IN PLACE (same line count, PLAN §4 P3)
    assert lines[hj - 1].startswith('#define handoff_run(')
    lines[hj - 1] = '#define FE_HANDOFF(c,w) (pwr_release_all(), LAB_HANDOFF((c),(w),handoff_run))'
    lines[hj] = '#define FE_EXIT_GAME() (pwr_release_all(), sceKernelExitGame())'
    return '\n'.join(lines), n


def same_files(a, b, names=('exit_sites.tsv', 'exit_steps.tsv', 'exit_scenarios.tsv')):
    return [n for n in names if open(os.path.join(a, n), 'rb').read() != open(os.path.join(b, n), 'rb').read()]


def gate(g, c):
    """check_gate over the raw tables (copied to a 'release' dir each, the gate's layout)."""
    t = tempfile.mkdtemp(prefix='xsgate')
    for side, src in (('g', g), ('c', c)):
        for fl in X.FLAVOURS:
            shutil.copytree(src, os.path.join(t, side, fl))
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = X.check_gate(os.path.join(t, 'g'), os.path.join(t, 'c'))
    shutil.rmtree(t, ignore_errors=True)
    return rc, buf.getvalue()


def main(argv):
    if not argv:
        print(__doc__); return 2
    if argv[0] == '--p3-sed-only':      # write P3's sed of MAIN_C to OUT (for the -E run)
        t, n = p3_sed(open(argv[1], encoding='utf-8', errors='replace').read())
        open(argv[2], 'w', encoding='utf-8', newline='\n').write(t)
        print('P3 sed: %d lines -> %s' % (n, argv[2]))
        return 0
    main_c = argv[0]
    cut = argv[argv.index('--cutlist') + 1] if '--cutlist' in argv else None
    T = tempfile.mkdtemp(prefix='p0a_xs')
    try:
        base = os.path.join(T, 'base')
        v = tables(main_c, base)
        st = X.sites(v)
        ids = [s['id'] for s in st if s['func'] == 'main']
        want = [x[0] for x in X2_IDS()]
        b, out = X.check_x2(st, 'raw')
        (ok if b == 0 and all(w in ids for w in want) else bad)(
            'C1 19 x2 sites by anchor: %s (x2 check %s)' % (' '.join(
                '%s@%s' % (s['id'], s['line']) for s in st if s['func'] == 'main'), 'ok' if not b else '%d bad' % b))
        text = open(main_c, encoding='utf-8', errors='replace').read()
        mt, n = p3_sed(text)
        mp = os.path.join(T, 'p3', 'main_psp.c'); os.makedirs(os.path.dirname(mp))
        open(mp, 'w', encoding='utf-8', newline='\n').write(mt)
        tables(mp, os.path.join(T, 'p3t'))
        d = same_files(base, os.path.join(T, 'p3t'))
        (ok if not d and n >= 30 else bad)('C2 P3 sed (%d lines renamed): exit_sites/steps/scenarios.tsv %s'
                                           % (n, 'identical' if not d else 'DIFFER: %s' % d))
        tables(mp, os.path.join(T, 'p3noalias'), alias=None)
        d = same_files(base, os.path.join(T, 'p3noalias'))
        (ok if d else bad)('M1 P3 sed without the FE_* alias: tables differ (%s): caught' % ','.join(d))
        if cut:
            sp = os.path.join(T, 'split')
            r = subprocess.run([sys.executable, os.path.join(R, 'split.py'), 'split', '--src', main_c,
                                '--cutlist', cut, '--out', sp], capture_output=True, text=True)
            if r.returncode != 0:
                bad('C3 split.py split failed: %s' % (r.stdout + r.stderr)[-300:])
            else:
                shell = os.path.join(sp, 'psp', 'main_psp.c')
                vs = tables(shell, os.path.join(T, 'p4'))
                rc, txt = gate(base, os.path.join(T, 'p4'))
                frag = [s for s in X.sites(vs) if isinstance(s['line'], C.FLine)]
                (ok if rc == 0 and frag else bad)(
                    'C3 P4-simulated split, include-following raw view: XS gate %s; fragment sites keyed '
                    '(file, line): %s' % ('0 differences' if rc == 0 else txt.strip()[-200:],
                                         ', '.join('%s@%s' % (s['id'], s['line']) for s in frag) or 'NONE'))
                tables(shell, os.path.join(T, 'p4nofollow'), follow=False)
                rc2, txt2 = gate(base, os.path.join(T, 'p4nofollow'))
                (ok if rc2 != 0 else bad)('M2 split shell read without following the includes: XS gate '
                                          'fails (%s): caught' % txt2.strip().split('\n')[-1])
        # M3: a changed reason string loses the anchor
        mt3 = text.replace('"nettest_fail"', '"nettest_failed_x"', 1)
        mp3 = os.path.join(T, 'm3', 'main_psp.c'); os.makedirs(os.path.dirname(mp3))
        open(mp3, 'w', encoding='utf-8', newline='\n').write(mt3)
        v3 = X.View('raw', C.load_raw(mp3, follow=True, alias=X.EXIT_ALIAS))
        b3, o3 = X.check_x2(X.sites(v3), 'raw')
        (ok if b3 else bad)('M3 A5 reason string changed: x2 check fails (%d): caught' % b3)
    finally:
        shutil.rmtree(T, ignore_errors=True)
    print('p0a_test_exit_matrix: %s' % ('PASS' if not FAILS else '%d FAILED' % FAILS))
    return 1 if FAILS else 0


def X2_IDS():
    return X.X2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
