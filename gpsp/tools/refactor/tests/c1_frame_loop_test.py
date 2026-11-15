#!/usr/bin/env python3
"""c1_frame_loop_test.py -- tier C C-1 commit 1 (health-out/design/C-1.md; CRITIQUE C-1):
the tools that key on main()'s loop read the C-1 shape, where the loop is the file-scope
function frame_loop() that only main() calls.  No compiler, no ELF (the ELF side of the
function mode is loop_anchor.py --selftest F1-F7, with injected DWARF facts).

  G1  globals census, old shape: a global written only in main's loop is `loop`
  G2  globals census, C-1 shape: the same global written in frame_loop is still `loop`
      (frame_loop is defined before main: by line it would read as `boot`), and a global
      written in a function only the loop calls is `loop` too
  G3  globals census, both shapes at once -> refused
  A1  loop_anchor.resolve_tree on the same two sources: func main / frame_loop
  E1  exit_matrix.EPOCHC_MOVED and epochc_exitmap.AS_MAIN name frame_loop
  R1  ci_greps R6 counts a function only frame_loop calls as main-only
  M1  modules.py puts frame_loop in module 18 (loop)
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
FAILS = []


def check(c, m):
    print('  %s  %s' % ('ok  ' if c else 'FAIL', m))
    if not c:
        FAILS.append(m)


OLD = r'''static volatile int g_running = 1;
static int g_inloop;
static int g_helped;
static int g_sess;
static void helper(void) { g_helped++; }
int main(int argc, char *argv[])
{
   for (;;)
   {
   g_sess = argc;
   while (g_running)
   {
      g_inloop++;
      helper();
   }
   break;
   }
   return g_inloop + g_helped + g_sess;
}
'''
NEW = r'''static volatile int g_running = 1;
static int g_inloop;
static int g_helped;
static int g_sess;
static void helper(void) { g_helped++; }
static __attribute__((noinline, noclone, section(".text.startup.fe_frame_loop")))
void frame_loop(int *L)
{
   int n = *L;
   while (g_running)
   {
      g_inloop++;
      helper();
   }
   *L = n;
}
int main(int argc, char *argv[])
{
   int L = 0;
   for (;;)
   {
   g_sess = argc;
   frame_loop(&L);
   break;
   }
   return g_inloop + g_helped + g_sess;
}
'''


def run():
    import globals as G
    import loop_anchor as LA
    d = tempfile.mkdtemp(prefix='c1fl.')
    try:
        life = {}
        for tag, src in (('old', OLD), ('new', NEW)):
            p = os.path.join(d, tag + '.c')
            open(p, 'w', newline='\n').write(src)
            rows, meta = G.census(p, None, None)
            life[tag] = {r['name']: r['life'] for r in rows if not r['extern']}
            life[tag + '_meta'] = (meta['loop0'], meta['loop1'])
        check(life['old'].get('g_inloop') == 'loop' and life['old'].get('g_sess') == 'session',
              'G1 old shape: g_inloop loop, g_sess session (%s)' % life['old'])
        check(life['new'].get('g_inloop') == 'loop' and life['new'].get('g_helped') == 'loop'
              and life['new'].get('g_sess') == 'session' and life['new_meta'] == (6, 16),
              'G2 C-1 shape: frame_loop\'s writes and its callee\'s are loop, g_sess session; loop L:%s-%s (%s)'
              % (life['new_meta'] + (life['new'],)))
        both = NEW.replace('   frame_loop(&L);\n', '   frame_loop(&L);\n   while (g_running)\n   {\n      g_inloop++;\n   }\n')
        p = os.path.join(d, 'both.c')
        open(p, 'w', newline='\n').write(both)
        try:
            G.census(p, None, None)
            check(False, 'G3 both shapes accepted by the census')
        except SystemExit:
            check(True, 'G3 both shapes at once -> the census refuses')
        ro, rn = LA.resolve_tree(OLD), LA.resolve_tree(NEW)
        check(ro['func'] == 'main' and rn['func'] == 'frame_loop' and (rn['lo'], rn['hi'], rn['fn_line']) == (6, 16, 7),
              'A1 loop_anchor: old -> main L:%s-%s, C-1 -> frame_loop L:%s-%s' % (ro['lo'], ro['hi'], rn['lo'], rn['hi']))
        import exit_matrix as E
        import epochc_exitmap as X
        check('frame_loop' in E.EPOCHC_MOVED and X.as_main('frame_loop') == 'main',
              'E1 T8 splices frame_loop as main()\'s text; the exit inventory files its rows under main')
        import ci_greps as CG
        os.makedirs(os.path.join(d, 't', 'psp'))
        open(os.path.join(d, 't', 'psp', 'main_psp.c'), 'w', newline='\n').write(NEW)
        rows = {n: (ni, sec) for n, _, ni, sec, _ in (CG.r6_main_only(os.path.join(d, 't')) or [])}
        check(rows.get('helper') == (False, False) and rows.get('frame_loop') == (True, True),
              'R1 R6: helper (only the loop calls it) is main-only; frame_loop is main-only with noinline+section (%s)' % rows)
        import modules as MO
        check(MO.function_module('frame_loop', 1) == ('loop', 'name'), 'M1 modules.py: frame_loop is module loop (18)')
    finally:
        import shutil
        shutil.rmtree(d, ignore_errors=True)
    print('c1_frame_loop_test: %s' % ('PASS' if not FAILS else '%d FAILED' % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run())
