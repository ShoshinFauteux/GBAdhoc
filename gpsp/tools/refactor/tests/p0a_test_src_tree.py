#!/usr/bin/env python3
"""p0a_test_src_tree.py -- P-tidy F3: layout_report.py (T10) and loop_lpsim.py (T16) read
main's loop window from the source tree they are GIVEN (--src / --ref-src, --tree / --tree-b),
not from the ELF's flavour.json commit or from the tools' own tree.

    p0a_test_src_tree.py RUN TREE [FLAVOUR...]
      RUN   a flavours.sh run (RUN/<flavour>/gpsp_adhoc.elf + flavour.json)
      TREE  a clean gpsp/ tree of RUN's commit (git archive)
      FLAVOUR  default: release lab

SHIFTED is TREE with one comment line in front of psp/main_psp.c (a symlink farm of the
rest), so `int main(` is one line off the ELF's DW_AT_decl_line.  A tool that reads the tree
it is given must REFUSE it; a tool that ignores the tree reads the ELF's real source (its
flavour.json commit) and passes.  So each refusal below proves the tree is read, and the two
tool mutants (the pre-fix calls, without the tree) must accept SHIFTED, which proves the check
can tell.  No docker; WSL or any host with the run and the tree.
Exit 0 all hold, 1 any fails, 2 usage.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(HERE)                         # gpsp/tools/refactor
sys.path.insert(0, R)
import loop_anchor  # noqa: E402

FAILS = [0]


def ok(c, msg):
    print('  %s  %s' % ('ok  ' if c else 'FAIL', msg))
    FAILS[0] += 0 if c else 1


def run(argv, env=None):
    p = subprocess.run([sys.executable, '-I'] + argv, capture_output=True, text=True, env=env)
    return p.returncode, p.stdout + p.stderr


def shifted_tree(tree, d):
    """d/gpsp: TREE with psp/main_psp.c one comment line longer, the rest linked (or copied)."""
    g = os.path.join(d, 'gpsp')
    os.makedirs(os.path.join(g, 'psp'))

    def link(src, dst):
        try:
            os.symlink(src, dst)
        except OSError:                           # no symlinks (Windows host): copy
            (shutil.copytree if os.path.isdir(src) else shutil.copy2)(src, dst)
    for n in os.listdir(tree):
        if n != 'psp':
            link(os.path.join(tree, n), os.path.join(g, n))
    for n in os.listdir(os.path.join(tree, 'psp')):
        if n != 'main_psp.c':
            link(os.path.join(tree, 'psp', n), os.path.join(g, 'psp', n))
    with open(os.path.join(tree, 'psp', 'main_psp.c'), encoding='utf-8', errors='replace', newline='') as f:
        text = f.read()
    with open(os.path.join(g, 'psp', 'main_psp.c'), 'w', encoding='utf-8', newline='') as f:
        f.write('/* p0a_test_src_tree: one line, so main is off the ELF by one */\n' + text)
    return g


def mutant_tools(d, tree):
    """d/mut/gpsp/tools/refactor: the tools' .py files with the pre-fix calls (no tree).
    The copy's own psp/main_psp.c is TREE's (its fallback source is then the right one too)."""
    m = os.path.join(d, 'mut', 'gpsp')
    mr = os.path.join(m, 'tools', 'refactor')
    os.makedirs(os.path.join(mr, 'loop_lpsim'))
    os.makedirs(os.path.join(m, 'psp'))
    shutil.copy2(os.path.join(tree, 'psp', 'main_psp.c'), os.path.join(m, 'psp', 'main_psp.c'))
    for sub in ('', 'loop_lpsim'):
        for n in os.listdir(os.path.join(R, sub)):
            if n.endswith('.py'):
                shutil.copy2(os.path.join(R, sub, n), os.path.join(mr, sub, n))
    subs = [(os.path.join(mr, 'layout_report.py'), 'r = loop_anchor.for_elf(b.e.path, tree=tree)',
             'r = loop_anchor.for_elf(b.e.path)'),
            (os.path.join(mr, 'loop_lpsim', 'loop_lpsim.py'), 'r = loop_anchor.for_elf(elf_path, main_c, tree=tree)',
             'r = loop_anchor.for_elf(elf_path, main_c)')]
    applied = 0
    for p, a, b in subs:
        t = open(p, encoding='utf-8').read()
        if a in t:
            open(p, 'w', encoding='utf-8', newline='\n').write(t.replace(a, b))
            applied += 1
    return mr, applied == len(subs)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    rundir, tree = os.path.abspath(argv[0]), os.path.abspath(argv[1])
    fls = argv[2:] or ['release', 'lab']
    if not os.path.isfile(os.path.join(tree, 'psp', 'main_psp.c')):
        print('p0a_test_src_tree: %s is not a gpsp/ tree' % tree)
        return 2
    d = tempfile.mkdtemp(prefix='p0a_src_tree.')
    try:
        sh = shifted_tree(tree, d)
        tm = os.path.join(tree, 'psp', 'main_psp.c')
        sm = os.path.join(sh, 'psp', 'main_psp.c')
        mr, applied = mutant_tools(d, tree)
        ok(applied, 'tool mutants applied (layout_report loop_sets and loop_lpsim window without the tree)')
        menv = dict(os.environ, FLAV_REPO=os.environ.get('FLAV_REPO') or loop_anchor.REPO)
        LR, LP = os.path.join(R, 'layout_report.py'), os.path.join(R, 'loop_lpsim', 'loop_lpsim.py')
        for fl in fls:
            fd = os.path.join(rundir, fl)
            e = os.path.join(fd, 'gpsp_adhoc.elf')
            if not os.path.isfile(e):
                ok(False, '[%s] no %s' % (fl, e))
                continue
            print('== %s (%s)' % (fl, e))
            r = loop_anchor.for_elf(e, tree=tree)
            ok(r['where'] == tm, '[%s] loop_anchor.for_elf(tree=TREE) reads TREE/psp/main_psp.c: %s'
               % (fl, loop_anchor.describe(r)))
            try:
                loop_anchor.for_elf(e, tree=sh)
                ok(False, '[%s] loop_anchor.for_elf(tree=SHIFTED) accepted' % fl)
            except LookupError as ex:
                ok(sm in str(ex), '[%s] loop_anchor.for_elf(tree=SHIFTED) refused, naming SHIFTED' % fl)
            # T10 layout_report
            rc, o = run([LR, fd, '--src', tree, '--no-pad-check'])
            ok(rc == 0 and 'source %s)' % tm in o, '[%s] layout_report --src TREE: rc %d, window read from TREE' % (fl, rc))
            rc, o = run([LR, fd, '--src', sh, '--no-pad-check'])
            ok(rc == 2 and 'REFUSED' in o and sm in o,
               '[%s] layout_report --src SHIFTED: rc %d, refused naming SHIFTED (the window is read from --src)' % (fl, rc))
            rc, o = run([LR, fd, '--src', tree, '--ref', fd, '--ref-src', tree, '--no-pad-check'])
            ok(rc == 0 and 'set map IDENTICAL' in o and o.count('source %s)' % tm) == 2,
               '[%s] layout_report --ref REF --ref-src TREE: rc %d, both windows from TREE, set map identical' % (fl, rc))
            rc, o = run([LR, fd, '--src', tree, '--ref', fd, '--ref-src', sh, '--no-pad-check'])
            ok(rc == 2 and 'REFUSED' in o and sm in o,
               '[%s] layout_report --ref-src SHIFTED: rc %d, refused (REF\'s window is read from --ref-src)' % (fl, rc))
            rc, o = run([os.path.join(mr, 'layout_report.py'), fd, '--src', sh, '--no-pad-check'], env=menv)
            ok(rc == 0 and sm not in o,
               '[%s] mutant layout_report (window without --src, the pre-fix call) ACCEPTS --src SHIFTED (rc %d): '
               'the check above can tell' % (fl, rc))
            # T16 loop_lpsim
            rc, o = run([LP, 'window', e, '--tree', tree])
            ok(rc == 0 and 'source %s)' % tm in o, '[%s] loop_lpsim window --tree TREE: rc %d, read from TREE' % (fl, rc))
            rc, o = run([LP, 'window', e, '--tree', sh])
            ok(rc == 2 and 'REFUSED' in o and sm in o, '[%s] loop_lpsim window --tree SHIFTED: rc %d, refused' % (fl, rc))
            rc, o = run([LP, 'loopfile', e, '--tree', sh, '--out', os.path.join(d, 'x.loop')])
            ok(rc == 2 and sm in o, '[%s] loop_lpsim loopfile --tree SHIFTED: rc %d, refused' % (fl, rc))
            rc, o = run([LP, 'setmap', e, e, '--tree', tree, '--tree-b', tree])
            ok(rc == 0 and re.search(r'set_hist_equal\s+True', o) and o.count('source %s)' % tm) == 2,
               '[%s] loop_lpsim setmap --tree TREE --tree-b TREE: rc %d, both from TREE, set map equal' % (fl, rc))
            rc, o = run([LP, 'setmap', e, e, '--tree', tree, '--tree-b', sh])
            ok(rc == 2 and sm in o, '[%s] loop_lpsim setmap --tree-b SHIFTED: rc %d, refused (ELF_B\'s tree is read)' % (fl, rc))
            rc, o = run([os.path.join(mr, 'loop_lpsim', 'loop_lpsim.py'), 'window', e, '--tree', sh], env=menv)
            ok(rc == 0 and sm not in o,
               '[%s] mutant loop_lpsim (window without --tree, the pre-fix call) ACCEPTS --tree SHIFTED (rc %d): '
               'the check above can tell' % (fl, rc))
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('p0a_test_src_tree: %s' % ('PASS' if not FAILS[0] else '%d FAILED' % FAILS[0]))
    return 1 if FAILS[0] else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
