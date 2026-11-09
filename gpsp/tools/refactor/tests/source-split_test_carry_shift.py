#!/usr/bin/env python3
"""source-split_test_carry_shift.py -- split.py gen --carry keeps the tree's cuts when only line
numbers move (P-tidy).  No compiler, no docker.

    python source-split_test_carry_shift.py [GPSP_DIR]     (default: this tool's gpsp/ tree, split)

gen places a cut by line-number rules (modules.SHELL_RANGES is 01067e74's L:1-160), so before the
fix a change that only moves lines -- main_psp.c's file header shortened from 37 to 10 lines --
moved the paths fragment's first line up into the shell's include list, and the documented regen
(`split.py gen --split GPSP --carry cutlist.txt`, then `split.py split`) no longer gave back the
tree it ran on.  With --carry, a gap that holds a carried FRAG anchor is cut there.
  C1 gen --carry on this tree's inverse reproduces its cutlist (FRAG and item rows)
  C2 the shell's header comment body removed (the shell shrinks): the same FRAG rows; split gives
     back every fragment byte for byte and the shell less those lines
  C3 40 comment lines added to the header (the shell grows): likewise
  M1 the pre-fix rule (no carried cut preferred), on C2's shrunk source: a FRAG anchor moves
"""
import importlib.util
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(HERE)
sys.path.insert(0, R)
import split as S  # noqa: E402

FAILS = [0]


def ok(c, msg):
    print('  %s  %s' % ('ok  ' if c else 'FAIL', msg))
    FAILS[0] += 0 if c else 1


def rows_of(frags, rows):
    return ([(f['frag'], f['module'], f['occ'], f['anchor']) for f in frags],
            [(r['key'], r['module'], r['frag'], r['occ'], r['anchor']) for r in rows])


def header_span(text):
    """(first, last) 1-based lines of the header comment's BODY: line 2 up to the line before the
    first line that closes it (` */`)."""
    ls = text.split('\n')
    end = next(i for i, l in enumerate(ls) if l.rstrip() == '*/' or l.rstrip().endswith(' */'))
    return 2, end          # lines 2..end are 1-based end = index of the closing line


def split_back(mod, text, frags, rows, gpsp):
    """split TEXT with (frags, rows) in memory and compare with the tree: -> (fragments equal, shell)"""
    out, shell, _ = mod.do_split(mod.Model(text), frags, rows)
    _, tree_frags = S.read_split(gpsp)
    return out == tree_frags, shell


def main(argv):
    gpsp = os.path.abspath(argv[0]) if argv else os.path.normpath(os.path.join(R, '..', '..'))
    cut = os.path.join(gpsp, 'tools', 'refactor', 'cutlist.txt')
    shell, frags = S.read_split(gpsp)
    inv = S.inverse_text(shell, frags)
    carry = S.read_cutlist(cut)
    want = rows_of(*carry)

    f1, r1, _ = S.gen(S.Model(inv), carry)
    ok(rows_of(f1, r1) == want, 'C1 gen --carry on this tree\'s inverse (%d lines) reproduces its cutlist: %d FRAG, '
       '%d item rows' % (inv.count('\n'), len(f1), len(r1)))

    a, b = header_span(inv)
    ls = inv.split('\n')
    shrunk = '\n'.join(ls[:a - 1] + ls[b:])
    f2, r2, _ = S.gen(S.Model(shrunk), carry)
    same2 = rows_of(f2, r2)[0] == want[0]
    eq2, sh2 = split_back(S, shrunk, f2, r2, gpsp)
    ok(same2 and eq2 and sh2.count('\n') == shell.count('\n') - (b - a + 1),
       'C2 header body (lines %d-%d) removed: the same %d FRAG rows; split gives back all %d fragments, '
       'the shell %d lines shorter' % (a, b, len(f2), len(frags), b - a + 1))

    grown = '\n'.join(ls[:1] + [' * p-tidy shift line %d' % k for k in range(40)] + ls[1:])
    f3, r3, _ = S.gen(S.Model(grown), carry)
    eq3, sh3 = split_back(S, grown, f3, r3, gpsp)
    ok(rows_of(f3, r3)[0] == want[0] and eq3 and sh3.count('\n') == shell.count('\n') + 40,
       'C3 40 header lines added: the same FRAG rows; split gives back all fragments, the shell 40 lines longer')

    # M1: the pre-fix rule, a mutated copy of split.py (no carried cut preferred)
    src = open(os.path.join(R, 'split.py'), encoding='utf-8').read()
    old = '        kept = [L for L in cands if L in carried_cuts]\n'
    d = tempfile.mkdtemp(prefix='carry_shift.')
    try:
        if old not in src:
            ok(False, 'M1 mutant not applied (the kept-cut line is not in split.py)')
        else:
            p = os.path.join(d, 'split_prefix.py')
            open(p, 'w', encoding='utf-8').write(src.replace(old, '        kept = []\n'))
            spec = importlib.util.spec_from_file_location('split_prefix', p)
            mut = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mut)
            fm, rm, _ = mut.gen(mut.Model(shrunk), carry)
            moved = [(x, y) for x, y in zip(want[0], rows_of(fm, rm)[0]) if x != y]
            ok(moved, 'M1 the pre-fix rule on the shrunk shell moves %d FRAG anchor(s): %s'
               % (len(moved), '; '.join('%s %r -> %r' % (x[0], x[3][:40], y[3][:40]) for x, y in moved[:3])))
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('source-split_test_carry_shift: %s' % ('PASS' if not FAILS[0] else '%d FAILED' % FAILS[0]))
    return 1 if FAILS[0] else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
