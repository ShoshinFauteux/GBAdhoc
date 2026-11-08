#!/usr/bin/env python3
"""fe_readme_test.py -- self-test of fe_readme.py (psp/fe/README.md, PLAN-P-beta5 §5.2), on scratch
copies of what it reads (the split tree, the cutlist, the host tests that read main_psp.c), never the
tree itself.  Also run by `fe_readme.py --selftest [--gpsp DIR]`.

    python3 fe_readme_test.py [GPSP_DIR]

  C1 the committed psp/fe/README.md is what the generator gives on a copy (--check rc 0)
  C2 the README follows the cutlist: a row's owner moved to another module changes it (--check rc 1)
  C3 a CRLF cutlist (a git archive of a core.autocrlf=true checkout) gives the same README (--check rc 0)
  M1 a hand edit of the README                         -> --check STALE (rc 1)
  M2 a G row filed in another fragment than it sits in -> refused
  M3 the cutlist's source sha256 is not the inverse's  -> refused (a stale cutlist)
  M4 a LAB_HOOK( site without its /* @lab */ tag       -> refused
  M5 two fragment includes of the shell swapped        -> refused (FRAG order)
  M6 a module the PLAN §1.2 table does not know        -> refused
  M7 a single-file main_psp.c (no fragments)           -> refused (not a split tree)
"""
import contextlib
import io
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(HERE)
sys.path.insert(0, R)
import fe_readme  # noqa: E402

FAILS = []


def ok(m):
    print('  ok    ' + m)


def bad(m):
    print('  FAIL  ' + m)
    FAILS.append(m)


def scratch(gpsp, root):
    sys.path.insert(0, os.path.join(gpsp, 'tools', 'tests'))
    import extract  # noqa: E402
    rels = ['psp/main_psp.c', 'tools/refactor/cutlist.txt', 'tools/tests/extract.py']
    rels += ['psp/fe/' + f for f in sorted(os.listdir(os.path.join(gpsp, 'psp', 'fe')))
             if os.path.isfile(os.path.join(gpsp, 'psp', 'fe', f))]
    rels += extract.enumerate_readers(gpsp)
    d = tempfile.mkdtemp(prefix='fe_readme_test-', dir=root)
    for r in rels:
        os.makedirs(os.path.dirname(os.path.join(d, r)), exist_ok=True)
        shutil.copyfile(os.path.join(gpsp, r), os.path.join(d, r))
    return d


def run_gen(d, *args):
    err, out = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = fe_readme.main(['--gpsp', d] + list(args))
    return rc, out.getvalue() + err.getvalue()


def edit(path, fn):
    t = open(path, encoding='utf-8', newline='').read()
    n = fn(t)
    if n == t:
        raise SystemExit('fe_readme_test: mutant not applied to %s' % path)
    open(path, 'w', encoding='utf-8', newline='').write(n)


def resha(d):
    """set the scratch cutlist's source sha256 to the mutated tree's inverse, so a check after the
    stale-cutlist one is what refuses (the mutant is not caught by the sha alone)"""
    import hashlib
    sh, fr = fe_readme.S.read_split(d)
    sha = hashlib.sha256(fe_readme.S.inverse_text(sh, fr).encode('utf-8')).hexdigest()
    edit(os.path.join(d, 'tools/refactor/cutlist.txt'),
         lambda t: re.sub(r'(?m)^(# source: .*sha256 )[0-9a-f]{64}', r'\g<1>' + sha, t, count=1))


def refused(label, d, want):
    rc, msg = run_gen(d, '-o', os.path.join(d, 'out.md'))
    if rc == 1 and re.search(want, msg) and not os.path.exists(os.path.join(d, 'out.md')):
        ok('%s: refused (%s)' % (label, msg.strip().split('\n')[-1][:140]))
    else:
        bad('%s: rc %d, %s' % (label, rc, msg.strip()[:200]))


def run(gpsp):
    gpsp = os.path.abspath(gpsp)
    print('fe_readme self-test on %s' % gpsp)
    root = tempfile.mkdtemp(prefix='fe_readme_t-')
    try:
        d = scratch(gpsp, root)
        rc, msg = run_gen(d, '--check')
        (ok if rc == 0 else bad)('C1 the committed README reproduces on a copy (--check rc %d): %s' % (rc, msg.strip()[:100]))

        d = scratch(gpsp, root)
        cp = os.path.join(d, 'tools/refactor/cutlist.txt')
        crlf = open(cp, encoding='utf-8', newline='').read().replace('\r\n', '\n').replace('\n', '\r\n')
        open(cp, 'w', encoding='utf-8', newline='').write(crlf)
        rc, msg = run_gen(d, '--check')
        (ok if rc == 0 else bad)('C3 the same with a CRLF cutlist (a git archive of an autocrlf checkout): '
                                 '--check rc %d' % rc)

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'psp/fe/README.md'), lambda t: t.replace(' -> power\n', ' -> input\n', 1))
        rc, msg = run_gen(d, '--check')
        (ok if rc == 1 and 'STALE' in msg else bad)('M1 a hand-edited README: --check rc %d' % rc)

        d = scratch(gpsp, root)
        cp = os.path.join(d, 'tools/refactor/cutlist.txt')
        edit(cp, lambda t: re.sub(r'^(G\tg_running#1\t)power\t', r'\1input\t', t, count=1, flags=re.M))
        rc, msg = run_gen(d, '--check')
        (ok if rc == 1 and 'STALE' in msg else bad)(
            'C2 g_running#1 owner power -> input in the cutlist: the README changes (--check rc %d)' % rc)

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'tools/refactor/cutlist.txt'),
             lambda t: re.sub(r'^(G\tg_running#1\t\w+\t)harness\.a\t', r'\1power.a\t', t, count=1, flags=re.M))
        refused('M2 G:g_running#1 filed in power.a (it sits in harness.a)', d, 'g_running#1: sits at')

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'tools/refactor/cutlist.txt'),
             lambda t: re.sub(r'(?m)^(# source: .*sha256 )[0-9a-f]{64}', r'\g<1>' + 'f' * 64, t, count=1))
        refused('M3 the cutlist names another source sha256', d, 'the cutlist is stale')

        d = scratch(gpsp, root)
        fr = os.path.join(d, 'psp/fe/paths.inc.c')
        edit(fr, lambda t: t.replace('/* @lab */ LAB_HOOK(', 'LAB_HOOK(', 1))
        resha(d)
        refused('M4 a LAB_HOOK( site lost its @lab tag (fe/paths.inc.c; cutlist sha made current)', d,
                '@lab: .*without its')

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'psp/main_psp.c'),
             lambda t: t.replace('#include "fe/power.a.inc.c"\n#include "fe/iothread.inc.c"\n',
                                 '#include "fe/iothread.inc.c"\n#include "fe/power.a.inc.c"\n', 1))
        resha(d)
        refused('M5 two fragment includes swapped in the shell (cutlist sha made current)', d, 'the shell includes')

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'tools/refactor/cutlist.txt'),
             lambda t: re.sub(r'^(G\tg_running#1\t)power\t', r'\1bogus\t', t, count=1, flags=re.M))
        refused('M6 a module the §1.2 table does not know (bogus)', d, 'no MODULES row')

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'psp/main_psp.c'), lambda t: re.sub(r'(?m)^#include "fe/\S+\.inc\.c"\n', '', t))
        refused('M7 a shell with no fragment includes', d, 'not a split tree')
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print('fe_readme self-test: %s' % ('PASS' if not FAILS else '%d FAILED' % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(R, '..', '..'))))
