#!/usr/bin/env python3
"""source-split_test_split.py -- self-test of split.py (T4) on the real
psp/main_psp.c (no compiler; G0 of the result is pp_diff.sh's job, see
source-split_test_pp_diff.sh C3).  Also run by `split.py --selftest`.

    python source-split_test_split.py [MAIN_PSP_C] [CUTLIST]

Controls (must pass) and mutants (must be REFUSED by split.py):
  C1 cover: 5 functions in no §1.2 range and 69 in several (crit_cover), 0/0 after the plan's assignments
  C2 gen is deterministic (two runs, identical bytes) and equals the given cutlist (default: the
     01067e74 fixture, tests/fixtures/cutlist_01067e74.txt)
  C3 split -> inverse is byte-identical; every item in exactly one fragment; no fragment has an open #if
  C4 gen --carry on the same source reproduces the same cutlist
  C5 replay of a patch that edits one function body: accepted, exactly one fragment changes
  C6 own-line @extract markers around a fragment's last function (as PLAN-P-beta5 P1 plants
     them): the end marker stays in that fragment; the carried cutlist is unchanged
  M1 a function row deleted              -> unlisted
  M2 a global row duplicated             -> listed twice
  M3 a FRAG anchor moved inside an #if   -> refused (open #if)
  M4 an item's fragment column changed   -> refused (sits in X, cutlist says Y)
  M5 a new function in the source        -> unlisted
  M6 a stray `#if 1` in the source       -> refused (unbalanced)
  M7 a function renamed in the source    -> unlisted + listed-but-absent
  M8 a FRAG anchor that matches no line  -> refused
  M9 --out pointing at the source tree   -> refused (dry run only)
  M10 replay of a patch adding a function -> refused (unlisted)
  M11 gen --carry with a new function    -> refused (never guessed from line numbers)
  (review 2026-11-05) M12 row module misspelled, M13 FRAG module misspelled, M14 FRAG module / name
  disagree, M15 a function moved into another fragment -> all refused
  M16 a FRAG anchor on an own-line @extract:end marker -> refused
"""
import difflib
import io
import os
import re
import shutil
import sys
import tempfile
import contextlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import split  # noqa: E402
import modules  # noqa: E402

FAILS = []


def ok(msg):
    print('  ok    ' + msg)


def bad(msg):
    print('  FAIL  ' + msg)
    FAILS.append(msg)


def refused(label, fn, want):
    try:
        fn()
    except split.Refused as e:
        if re.search(want, str(e)):
            ok('%s: refused (%s)' % (label, str(e)[:110]))
        else:
            bad('%s: refused for another reason: %s' % (label, str(e)[:300]))
        return
    bad('%s: NOT refused' % label)


def rows_text(frags, rows):
    buf = io.StringIO()
    tmp = tempfile.mktemp(suffix='.txt')
    split.write_cutlist(tmp, frags, rows, ['test'])
    t = open(tmp, encoding='utf-8').read()
    os.remove(tmp)
    return t


def patch_of(old, new, rel='gpsp/psp/main_psp.c'):
    d = difflib.unified_diff(old.splitlines(keepends=True), new.splitlines(keepends=True),
                             'a/' + rel, 'b/' + rel)
    return 'diff --git a/%s b/%s\n' % (rel, rel) + ''.join(d)


def run(src, committed=None):
    print('split.py self-test on %s' % src)
    text = split.read_src(src)
    m = split.Model(text)
    work = tempfile.mkdtemp(prefix='splittest-')
    try:
        # C1
        none, multi = modules.cover(m.p['funcs'])
        un = [f for f in m.p['funcs'] if modules.function_module(f['name'], f['start'])[0] is None]
        if (len(none), len(multi), len(un)) == (5, 69, 0):
            ok('C1 cover: %d none / %d multi -> %d unassigned, 0 doubly assigned' % (len(none), len(multi), len(un)))
        else:
            bad('C1 cover: %d none / %d multi / %d unassigned (want 5/69/0)' % (len(none), len(multi), len(un)))
        # C2
        f1, r1, rep = split.gen(m)
        f2, r2, _ = split.gen(split.Model(text))
        t1, t2 = rows_text(f1, r1), rows_text(f2, r2)
        if t1 == t2:
            ok('C2 gen deterministic (%d fragments, %d rows)' % (len(f1), len(r1)))
        else:
            bad('C2 gen not deterministic')
        if committed:
            cf, cr = split.read_cutlist(committed)
            if rows_text(cf, cr) == t1:
                ok('C2 committed %s == a fresh gen' % os.path.basename(committed))
            else:
                bad('C2 committed cutlist differs from a fresh gen')
        frags, rows = f1, r1
        # C3
        out, shell, srep = split.do_split(m, frags, rows)
        if split.inverse_text(shell, out) == text:
            ok('C3 split -> inverse byte-identical (%d fragments, %d files, shell %d lines)'
               % (srep['fragments'], srep['files'], srep['shell_lines']))
        else:
            bad('C3 inverse differs')
        nkeys = len({it['key'] for it in m.items})
        if nkeys == len(rows) == len({r['key'] for r in rows}):
            ok('C3 %d items (%d functions, %d globals), each listed exactly once'
               % (nkeys, rep['functions'], rep['globals']))
        else:
            bad('C3 item/row count mismatch %d/%d' % (nkeys, len(rows)))
        # C4
        cpath = os.path.join(work, 'cut.txt')
        split.write_cutlist(cpath, frags, rows, ['t'])
        cf, cr = split.gen(split.Model(text), carry=split.read_cutlist(cpath))[:2]
        if rows_text(cf, cr) == t1:
            ok('C4 gen --carry reproduces the cutlist')
        else:
            bad('C4 gen --carry differs')
        # C6 (PLAN-P-beta5 P1): own-line @extract markers around the last function of a
        # fragment (as P1 plants them): the end marker stays in that fragment, so the
        # carried cutlist is unchanged; M16 a FRAG anchor put on that end marker is refused
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tests'))
        import extract as X
        lines = text.splitlines(keepends=True)
        pick = None
        for k in range(1, len(frags)):
            L = m.resolve(frags[k]['occ'], frags[k]['anchor'])
            L0 = m.resolve(frags[k - 1]['occ'], frags[k - 1]['anchor'])
            prev = [it for it in m.items if it['end'] < L]
            xi = max(prev, key=lambda it: it['end'])
            # the fragment ends with that function, then only blank lines (P1's power.b case)
            if xi['kind'] == 'F' and xi['start'] > L0 and L > modules.SHELL_RANGES[0][1] and \
                    m.cuttable(xi['end'] + 1) and all(not m.lines[q - 1].strip() for q in range(xi['end'] + 1, L)):
                pick = (k, xi)
                break
        if pick is None:
            bad('C6 no fragment ending in a function found')
        else:
            k, xi = pick
            a0 = sum(len(x) for x in lines[:xi['start'] - 1])
            b0 = sum(len(x) for x in lines[:xi['end']])
            planted = X.plant(text, {'C6': (a0, b0)})
            pm = split.Model(planted)
            lend = xi['end'] + 2
            vac = not (pm.lines[lend - 1].strip() == '/* @extract:end C6 */' and pm.cuttable(lend))
            pf, pr, prep = split.gen(pm, carry=(frags, rows))
            pout, pshell, _ = split.do_split(pm, pf, pr)
            holder = next(r['frag'] for r in pr if r['key'] == xi['key'])
            if vac:
                bad('C6 vacuous: L:%d is not a cuttable own-line end marker' % lend)
            elif (rows_text(pf, pr) == t1 and prep['units'] == rep['units'] and '/* @extract:end C6 */' in pout[holder]
                  and split.inverse_text(pshell, pout) == planted and not pm.unit_start_ok(lend)):
                ok('C6 markers planted around %s (last function of %s): the end marker (L:%d, cuttable) stays in %s; '
                   'carried cutlist unchanged, %d units' % (xi['key'], frags[k - 1]['frag'], lend, holder, prep['units']))
            else:
                bad('C6 end marker after %s: cutlist same %s, units %d/%d, in %s: %s'
                    % (xi['key'], rows_text(pf, pr) == t1, prep['units'], rep['units'], holder,
                       '/* @extract:end C6 */' in pout[holder]))
            mf16 = [dict(f) for f in pf]
            mf16[k]['occ'], mf16[k]['anchor'] = 1, '/* @extract:end C6 */'
            refused('M16 FRAG %s anchored on the end marker' % pf[k]['frag'], lambda: split.do_split(pm, mf16, pr),
                    'own-line @extract:end marker')

        # mutants on the cutlist
        frow = next(i for i, r in enumerate(rows) if r['kind'] == 'F' and r['name'] == 'power_cb')
        refused('M1 function row deleted', lambda: split.do_split(m, frags, rows[:frow] + rows[frow + 1:]), 'unlisted.*F:power_cb#1')
        grow = next(i for i, r in enumerate(rows) if r['key'] == 'G:g_vc_work#1')
        refused('M2 global row duplicated', lambda: split.do_split(m, frags, rows + [dict(rows[grow], module='power')]), 'listed twice.*G:g_vc_work#1')
        # M3: move the 'rewind' fragment's anchor onto a line inside an #if within its range
        k = next(i for i, f in enumerate(frags) if f['module'] == 'pacing' and i > 0)
        L0 = m.resolve(frags[k]['occ'], frags[k]['anchor'])
        L1 = m.resolve(frags[k + 1]['occ'], frags[k + 1]['anchor']) if k + 1 < len(frags) else len(m.lines)
        Lin = next(L for L in range(L0 + 1, L1) if m.p['pp_depth'][L] > 0 and not m.uncuttable[L]
                   and m.line_count[m.lines[L - 1]] == 1 and m.lines[L - 1].strip())
        mf = [dict(f) for f in frags]
        mf[k]['occ'], mf[k]['anchor'] = 1, m.lines[Lin - 1].rstrip('\n')
        refused('M3 FRAG %s anchor moved to L:%d (#if depth %d)' % (frags[k]['frag'], Lin, m.p['pp_depth'][Lin]),
                lambda: split.do_split(m, mf, rows), 'open #if')
        other = next(f['frag'] for f in frags if f['frag'] != rows[frow]['frag'])
        mr = [dict(r) for r in rows]
        mr[frow]['frag'] = other
        refused('M4 power_cb filed in %s' % other, lambda: split.do_split(m, frags, mr), 'F:power_cb#1 sits in')
        # mutants on the source
        t5 = text.replace('static int power_cb(int unknown, int pwrflags, void *common)\n',
                          'static int pp_new_fn(void)\n{\n   return 1;\n}\n\nstatic int power_cb(int unknown, int pwrflags, void *common)\n', 1)
        assert t5 != text
        refused('M5 a new function', lambda: split.do_split(split.Model(t5), frags, rows), 'unlisted.*F:pp_new_fn#1')
        Lmid = m.resolve(frags[5]['occ'], frags[5]['anchor']) + 1
        while m.uncuttable[Lmid] or m.p['pp_depth'][Lmid] or not m.lines[Lmid - 1].strip():
            Lmid += 1
        t6 = ''.join(m.lines[:Lmid - 1] + ['#if 1\n'] + m.lines[Lmid - 1:])
        refused('M6 stray #if 1 at L:%d' % Lmid, lambda: split.do_split(split.Model(t6), frags, rows), 'unbalanced|open #if|anchor')
        t7 = text.replace('static int power_cb(int unknown, int pwrflags, void *common)\n',
                          'static int power_cbx(int unknown, int pwrflags, void *common)\n', 1)
        refused('M7 power_cb renamed', lambda: split.do_split(split.Model(t7), frags, rows), 'unlisted.*power_cbx.*listed but not in the source.*power_cb#1')
        mf8 = [dict(f) for f in frags]
        mf8[3]['anchor'] += ' /* edited */'
        refused('M8 FRAG anchor matches nothing', lambda: split.do_split(m, mf8, rows), 'anchor not found')
        # review 2026-11-05 mutants (false passes before the fix)
        refused('M12 a row\'s owner module misspelled', lambda: split.do_split(
            m, frags, [dict(r, module='powr') if r['key'] == 'F:power_cb#1' else r for r in rows]),
            'unknown module.*F:power_cb#1 powr')
        refused('M13 a FRAG module misspelled', lambda: split.do_split(
            m, [dict(f, module='shel') if f['frag'] == 'shell.b' else f for f in frags], rows), 'unknown module.*FRAG shell.b shel')
        refused('M14 a FRAG module that disagrees with its name', lambda: split.do_split(
            m, [dict(f, module='power') if f['frag'] == 'shell.b' else f for f in frags], rows), 'does not start with its module')
        # M15: a function moved bodily into another fragment's range
        it = next(x for x in m.items if x['key'] == 'F:power_cb#1')
        body = m.lines[it['start'] - 1:it['end']]
        rest = m.lines[:it['start'] - 1] + m.lines[it['end']:]
        kk = next(i for i, x in enumerate(rest) if x.startswith('static void dump_ge_bmp(void)'))
        t15 = ''.join(rest[:kk] + body + ['\n'] + rest[kk:])
        refused('M15 power_cb moved into misc.b', lambda: split.do_split(split.Model(t15), frags, rows),
                'not after the previous fragment|sits in')
        refused('M9 --out = the source tree',
                lambda: split.guard_out(os.path.dirname(os.path.dirname(os.path.abspath(src))), src), 'dry.run only')
        # replay
        sdir = os.path.join(work, 'split')
        split.write_split(sdir, out, shell)
        split.write_cutlist(cpath, frags, rows, ['t'])
        t_ok = text.replace('static int power_cb(int unknown, int pwrflags, void *common)\n{\n',
                            'static int power_cb(int unknown, int pwrflags, void *common)\n{\n   /* replayed edit */\n', 1)
        if t_ok == text:
            bad('C5 could not build the replay patch')
        else:
            pp = os.path.join(work, 'ok.patch')
            open(pp, 'w', encoding='utf-8', newline='').write(patch_of(text, t_ok))
            d2 = os.path.join(work, 'split2')
            try:
                split.replay(sdir, cpath, pp, d2)
                s1, fr1 = split.read_split(sdir)
                s2, fr2 = split.read_split(d2)
                ch = sorted(k2 for k2 in fr1 if fr1[k2] != fr2.get(k2))
                if s1 == s2 and ch == [rows[frow]['frag']]:
                    ok('C5 replay of a power_cb body edit: only %s changed' % ch[0])
                else:
                    bad('C5 replay changed %s (shell same: %s)' % (ch, s1 == s2))
            except split.Refused as e:
                bad('C5 replay refused: %s' % e)
        pn = os.path.join(work, 'new.patch')
        open(pn, 'w', encoding='utf-8', newline='').write(patch_of(text, t5))
        refused('M10 replay of a patch adding a function', lambda: split.replay(sdir, cpath, pn, os.path.join(work, 's3')),
                'unlisted.*pp_new_fn')
        refused('M11 gen --carry with a new function', lambda: split.gen(split.Model(t5), carry=(frags, rows)),
                'not in the old cutlist.*pp_new_fn')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('split.py self-test: %d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else split.default_src()
    # default: the 01067e74 cutlist this test was written for (tools/refactor/cutlist.txt
    # follows the tree since PLAN-P-beta5 P1; the 01067e74 one is the fixture)
    cut = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'fixtures', 'cutlist_01067e74.txt')
    sys.exit(run(src, cut if os.path.exists(cut) else None))
