#!/usr/bin/env python3
"""source-split_test_globals.py -- self-test of globals.py (T9) on the real
psp/main_psp.c.  Also run by `globals.py --selftest`.

  C1 census: 0 unassigned; the 23 attributed definitions found (g_vc_work 2247,
     g_all_clean 3566, 19 pins 3148-3171, 2 pins 13740-13741)
  C2 vs x1's census (scratch/vars_g.json, if present): none of its 620 entries
     lost, the only new definitions are those 23, and all 566 x1 definitions get
     the same lifetime class and group as x1 gave them
  C3 main()'s phases found by anchor: session_top 15251, loop 16368-18067
  M1 a new global with no known prefix          -> unassigned, census exit 1
  M2 three attribute placements                  -> all parsed (before/after the declarator, on an array)
  R1 rename of a plain global                    -> every code token, no comment/string touched
  M3 rename of a global used inside fe_evt(...)  -> refused (r6 B3)
  M4 rename of exported ABI (g_running)          -> refused
  M5 rename onto an existing name                -> refused
  M6 rename of a shadowed global                 -> refused
  M7 rename of an extern-only name               -> refused
  (adversarial review 2026-11-05; each was a false pass)
  M8 rename of a global used in a #define body   -> refused
  M9 rename of a non-static global another TU names (or with no tree) -> refused
  M10 anonymous aggregate type + initializer     -> parsed as a definition (g_fxs_gba, L:6773)
  M11 an unterminated quote (#error can't)       -> later line numbers and the #endif kept
"""
import contextlib
import io
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import csrc     # noqa: E402
import globals as G  # noqa: E402

FAILS = []
GPSP = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
SRC = os.path.join(GPSP, 'psp', 'main_psp.c')
BASIS = os.path.normpath(os.path.join(GPSP, '..', '..', 'mainpsp-map', 'out', 'scratch', 'vars_g.json'))
X1LIFE = {'per-frame/loop-mutable': 'loop', 'boot-forever': 'boot', 'per-session': 'session',
          'boot-forever (never written after init)': 'const', 'harness-only (compiled out of release)': 'H-cut',
          'dead/variant (compiled out of release and rig)': 'variant',
          'harness-only (release-compiled, harness/telemetry functions only)': 'H-rel',
          'write-only in release (only ++/= in release; read only by fe_evt/harness)': 'WO-rel'}
EXPECT23 = {'g_vc_work': 2247, 'g_all_clean': 3566, 'g_dfr_clk_n': 13740, 'g_dfr_slow': 13741}


def ok(m):
    print('  ok    ' + m)


def bad(m):
    print('  FAIL  ' + m)
    FAILS.append(m)


def refused(label, fn, want):
    try:
        fn()
    except G.Refused as e:
        (ok if re.search(want, str(e)) else bad)('%s: refused (%s)' % (label, str(e)[:120]))
        return
    bad('%s: NOT refused' % label)


def run(src=SRC):
    print('globals.py self-test on %s' % src)
    text = open(src, encoding='utf-8', newline='').read()
    rows, meta = G.census(src, GPSP, None)
    defs = [r for r in rows if not r['extern']]
    att = [r for r in defs if r['attrs']]
    un = [r for r in defs if r['owner'] is None]
    pins = [r for r in att if r['section'] in ('.sbss', '.sdata')]
    lines_ok = all(EXPECT23.get(r['name'], r['line']) == r['line'] for r in att) and \
        all(3148 <= r['line'] <= 3171 or 13740 <= r['line'] <= 13741 for r in pins)
    if len(att) == 23 and len(pins) == 21 and not un and lines_ok:
        ok('C1 %d definitions, 0 unassigned; 23 attributed found (21 .sbss/.sdata pins, g_vc_work, g_all_clean)' % len(defs))
    else:
        bad('C1 attributed %d (pins %d), unassigned %d, lines ok %s' % (len(att), len(pins), len(un), lines_ok))
    if os.path.exists(BASIS):
        b = json.load(open(BASIS))
        bk = {(x['name'], x['line'], x['extern']) for x in b}
        nk = {(r['name'], r['line'], r['extern']): r for r in rows}
        lost = bk - set(nk)
        newdefs = sorted(k for k in nk if k not in bk and not k[2])
        diff = [x['name'] for x in b if not x['extern'] and
                (X1LIFE.get(x['life']) != nk[(x['name'], x['line'], False)]['life']
                 or x['group'][:3] != nk[(x['name'], x['line'], False)]['group'])]
        if not lost and {k[0] for k in newdefs} == {r['name'] for r in att} | G.X1_ANON_AGG and not diff:
            ok('C2 x1 basis: 0 of %d lost, %d new definitions = the 23 attributed + %s (anonymous aggregate), '
               '0 lifetime/group differences over x1\'s %d definitions'
               % (len(bk), len(newdefs), ','.join(sorted(G.X1_ANON_AGG)), sum(1 for x in b if not x['extern'])))
        else:
            bad('C2 lost %d, new %d, class/group diffs %s' % (len(lost), len(newdefs), diff[:8]))
    else:
        print('  skip  C2 (no %s)' % BASIS)
    if (meta['session_top'], meta['loop0'], meta['loop1']) == (15251, 16368, 18067):
        ok('C3 phases by anchor: session_top 15251, loop 16368-18067')
    else:
        bad('C3 phases %s' % ((meta['session_top'], meta['loop0'], meta['loop1']),))

    work = tempfile.mkdtemp(prefix='globtest-')
    try:
        mut = text + '\nstatic int zz_orphan_state;\n'
        p = os.path.join(work, 'main_psp.c')
        open(p, 'w', encoding='utf-8', newline='').write(mut)
        r2, m2 = G.census(p, None, None)
        with contextlib.redirect_stdout(io.StringIO()):
            rc = G.report(r2, m2)
        if rc == 1 and [r['name'] for r in r2 if not r['extern'] and r['owner'] is None] == ['zz_orphan_state']:
            ok('M1 a global with no known prefix is UNASSIGNED and the census exits 1')
        else:
            bad('M1 rc %d' % rc)
        snippet = ('static int __attribute__((section(".sbss"))) g_ff_t1;\n'
                   'static unsigned g_ff_t2[4] __attribute__((aligned(16), section(".sdata"))) = {1, 2};\n'
                   'static volatile int g_ff_t3 __attribute__((unused)), g_ff_t4;\n')
        d = csrc.parse(snippet, None, want_refs=False)
        got = {v['name']: v['section'] for v in d['vars']}
        if got == {'g_ff_t1': '.sbss', 'g_ff_t2': '.sdata', 'g_ff_t3': None, 'g_ff_t4': None}:
            ok('M2 attributes before/after the declarator, on an array, and on a multi-declarator line parsed')
        else:
            bad('M2 parsed %s' % got)

        dd = csrc.parse(text, None)
        spans = dd['spans']

        def hits(nm):
            return [m.start() for m in re.finditer(r'\b%s\b' % re.escape(nm), dd['clean'])]

        def in_span(pos):
            return any(a <= pos <= b for a, b, k in spans)
        cands = [v['name'] for v in dd['vars'] if not v['extern'] and v['static'] and v['name'] not in dd['shadow']
                 and not v['name'].startswith('gpsp_')]
        comments = re.findall(r'/\*.*?\*/', text, re.S)
        bcl = csrc.blank_comments_strings(text).split('\n')
        pp_text = '\n'.join(ln for k, ln in enumerate(bcl) if dd['pp_line'][k + 1])
        cands = [n for n in cands if not re.search(r'\b%s\b' % re.escape(n), pp_text)]
        plain = next(n for n in cands if len(hits(n)) >= 4 and not any(in_span(x) for x in hits(n))
                     and any(re.search(r'\b%s\b' % n, c) for c in comments))
        t2, n = G.rename(text, plain, plain + '_rn')
        dd2 = csrc.parse(t2, None, want_refs=False)
        comments_same = re.findall(r'/\*.*?\*/', text, re.S) == re.findall(r'/\*.*?\*/', t2, re.S)
        if (n == len(hits(plain)) and comments_same and plain + '_rn' in {v['name'] for v in dd2['vars']}
                and not re.search(r'\b%s\b' % plain, dd2['clean'])):
            ok('R1 rename %s: %d code tokens, every comment unchanged (it is named in one), none left in code' % (plain, n))
        else:
            bad('R1 rename %s: %d tokens, comments same %s' % (plain, n, comments_same))
        evt = next(n2 for n2 in cands if any(in_span(x) for x in hits(n2)))
        refused('M3 rename %s (used inside fe_evt/LAB args)' % evt, lambda: G.rename(text, evt, evt + '_rn'), 'compiles out|r6 B3')
        refused('M4 rename g_running', lambda: G.rename(text, 'g_running', 'g_run'), 'exported ABI')
        refused('M5 rename %s onto g_running' % plain, lambda: G.rename(text, plain, 'g_running'), 'already names')
        # no defined global is shadowed in 01067e74 (x1 B8's gamepak_reopens is an extern), so seed one
        tsh = text + '\nstatic void zz_shadow(void)\n{\n   int %s = 0;\n   (void)%s;\n}\n' % (plain, plain)
        refused('M6 rename %s with a seeded shadowing local' % plain, lambda: G.rename(tsh, plain, plain + '_rn'), 'shadowed')
        ext = next(v['name'] for v in dd['vars'] if v['extern']
                   and not any(w['name'] == v['name'] and not w['extern'] for w in dd['vars']))
        refused('M7 rename extern-only %s' % ext, lambda: G.rename(text, ext, ext + '_rn'), 'not a file-scope global')
        # review 2026-11-05 mutants (each was a false pass before the fix)
        k0 = text.index('\nstatic int power_cb(')
        tdef = text[:k0] + '\n#define ZZ_SET_PLAIN(v) (%s = (v))' % plain + text[k0:]
        refused('M8 rename %s used in a #define body' % plain, lambda: G.rename(tdef, plain, plain + '_rn'),
                'preprocessor directive')
        decl = re.search(r'^static ([^\n;]*\b%s\b)' % re.escape(plain), text, re.M)
        tns = text[:decl.start()] + decl.group(1) + text[decl.end():]
        root2 = os.path.join(work, 'tree')
        os.makedirs(os.path.join(root2, 'psp'))
        open(os.path.join(root2, 'psp', 'other.c'), 'w').write('extern int %s;\nint f(void){return %s;}\n' % (plain, plain))
        refused('M9 rename a non-static %s that another TU names' % plain,
                lambda: G.rename(tns, plain, plain + '_rn', root2), 'another TU')
        refused('M9b rename a non-static %s with no tree to check' % plain,
                lambda: G.rename(tns, plain, plain + '_rn'), 'external linkage')
        anon = ('static const struct { signed char a, b; const char *nm; }\nzz_tab[] = {\n   { 1, 2, "x" },\n'
                '   { 3, 4, "y" },\n};\nstatic struct { int a; } zz_one, zz_two;\n')
        dn = {v['name'] for v in csrc.parse(anon, None, want_refs=False)['vars']}
        if dn == {'zz_tab', 'zz_one', 'zz_two'}:
            ok('M10 an anonymous aggregate type with an initializer is a definition (L:6773 g_fxs_gba shape)')
        else:
            bad('M10 anonymous aggregate parsed as %s' % sorted(dn))
        ap = "#if 0\n#error can't\n#endif\nstatic int zz_after;\n"
        da = csrc.parse(ap, None, want_refs=False)
        if [(v['name'], v['line']) for v in da['vars']] == [('zz_after', 4)] and da['final_depth'] == 0:
            ok('M11 an unterminated quote (apostrophe in a #error line) keeps every later line number and the #endif')
        else:
            bad('M11 %s' % [(v['name'], v['line']) for v in da['vars']])
    finally:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
    print('globals.py self-test: %d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1] if len(sys.argv) > 1 else SRC))
