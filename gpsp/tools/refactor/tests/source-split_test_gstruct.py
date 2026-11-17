#!/usr/bin/env python3
"""source-split_test_gstruct.py -- self-test of C5-0's tools: `globals.py struct` (gstruct.py),
the G0R checker (g0r.py) and `globals.py census --module X --check`.  No compiler needed
(a host gcc, when present, also syntax-checks the struct output).  Also run by
`gstruct.py --selftest` and `g0r.py --selftest`.

  S1  struct st + tel + cfg on a toy source: definitions replaced by the blocks, every code
      use renamed, comments/strings untouched, volatile/arrays/section/initializers kept
  S3  (tier C C5-3) a declaration's trailing comment over several lines moves with it
  S2  the output compiles (gcc -fsyntax-only, if gcc is on PATH) in both telemetry arms
  R1..R12 refusals: extern, defined twice, exported, non-static, const, macro-global,
      aligned, section mismatch, init in st / none in cfg, mixed #if, shared line,
      fe_evt use without --tel (allowed with it), name taken
  G1  G0R equal: the toy before vs after struct (no telemetry arm)              -> ok
  G2  G0R equal with --tel in the telemetry build (TEL(stmt) = do { stmt; } while (0)) -> ok
  G3  G0R deletions in the release (TEL(stmt) = do { } while (0); no tel block)   -> ok,
      the deleted store that calls a function is listed for review
  M1  a non-rename edit (an operand changed)                                    -> G0R FAIL
  M2  the release deletes a store to a NON-telemetry member                     -> G0R FAIL
  M3  the release deletes a non-store statement (a call)                        -> G0R FAIL
  M4  a renamed site that names another member                                  -> G0R FAIL
  M5  a telfn's body differs in the release                                     -> ok only with telfn
  G4  (tier C C5-1) G0R deletions of whole telemetry statements in the release: a guarded
      `if (C) { if (call()) tel++; else tel += 2; check = 0; }` block, TEL(if (call()) tel++),
      a TEL'd local + its use, a block-local block, a telobj (min tracker) and its file-scope
      definition; every call in deleted code listed                                -> ok
  M6  a deleted condition whose statement stays                                  -> G0R FAIL
  M7  a deleted block holding a non-telemetry store                              -> G0R FAIL
  M8  a deleted declaration whose name is used after it                          -> G0R FAIL
  M9  a function-scope local deleted while still read                            -> G0R FAIL
  K1  census --module toy --check: the struct objects and the allowlist pass; a stray
      object of the module fails
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import gstruct as S  # noqa: E402
import g0r as R      # noqa: E402

FAILS = []


def ok(m):
    print('  ok    ' + m)


def bad(m):
    print('  FAIL  ' + m)
    FAILS.append(m)


def check(c, m):
    (ok if c else bad)(m)


TOY = r'''/* toy main_psp.c */
#include <stddef.h>
#define fe_evt(...) do { } while (0)
static unsigned g_ll_hit;                 /* hits */
static volatile int g_ll_state __attribute__((section(".sbss")));   /* state machine */
static int g_keep;
static unsigned g_lat_hist[8];            /* histogram */
static int g_ff_pace = 7;                 /* ship tunable */
static const char *g_ff_name = "a; b = c";
static unsigned g_lat_vc0;

static unsigned vcount(void) { return 3; }

static void step(int c)
{
   /* g_ll_hit in a comment stays */
   const char *s = "g_ll_hit";
   g_ll_hit++;
   if (c) g_ll_hit += 2;
   g_lat_hist[c & 7]++;
   g_lat_vc0 = vcount();
   g_ll_state = c ? g_ff_pace : 0;
   g_keep = g_ll_state + (int)(s != NULL) + (g_ff_name != NULL);
}
int main(void) { step(1); return g_keep; }
'''
MAP = '''# toy batch
struct pacing fe_pacing st .sbss
struct pacing_cfg fe_pacing_cfg cfg
struct pacing_tel fe_pacing_tel tel
g_ll_state pacing.ll_state
g_ff_pace pacing_cfg.ff_pace
g_ff_name pacing_cfg.ff_name
g_ll_hit pacing_tel.ll_hit
g_lat_hist pacing_tel.lat_hist
g_lat_vc0 pacing_tel.lat_vc0
'''


# Tier C C5-1: the release of a telemetry batch, as token streams (REF = the parent's release
# -E, NEW = the batch's: TEL(...) is `do { } while (0)` -> @TEL@, #ifdef GPSP_KEEP_TELEMETRY
# blocks gone).  g_ll_hit / g_lat_hist are tel members (MAP), g_min a telobj.
BLK_REF = r'''
static unsigned g_min = ~0u;
static int g_check, g_keep;
static int step(int c, unsigned vc)
{
   unsigned now = clock_us();
   if (g_check && (int)(vc - g_keep) >= 1)
   {
      if (flip_latched())
         g_ll_hit++;
      else
         g_ll_hit += 2;
      g_check = 0;
   }
   if (flip_latched())
      g_ll_hit++;
   if (c)
   {
      unsigned d = vc + 1u - g_keep;
      g_lat_hist[d < 7u ? d : 7u]++;
      g_keep = 0;
   }
   {
      unsigned age = clock_us() - now;
      if (age < 100u)
      {
         g_ll_hit++;
         g_lat_hist[0] += age;
      }
   }
   if ((unsigned)c < g_min)
      g_min = (unsigned)c;
   g_lat_hist[1] += hc();
   return g_keep + (int)now;
}
'''
BLK_NEW = r'''
static int g_check, g_keep;
static int step(int c, unsigned vc)
{
   unsigned now = clock_us();
   if (g_check && (int)(vc - g_keep) >= 1)
   {
      if (flip_latched())
         @TEL@;
      else
         @TEL@;
      g_check = 0;
   }
   @TEL@;
   if (c)
   {
      @TEL@;
      g_keep = 0;
   }
   @TEL@;
   @TEL@;
   @TEL@;
   return g_keep + (int)now;
}
'''.replace('@TEL@', 'do { } while (0)')
BLK_MUTANTS = (
    # the latch census's guard deleted, its body kept: an unconditional store to g_check
    ('M6', '   if (g_check && (int)(vc - g_keep) >= 1)\n   {\n      if (flip_latched())\n         do { } while (0);\n'
           '      else\n         do { } while (0);\n      g_check = 0;\n   }\n',
     '   g_check = 0;\n', 'a deleted condition whose statement stays'),
    # g_keep = 0 (state) deleted with the telemetry
    ('M7', '   if (c)\n   {\n      do { } while (0);\n      g_keep = 0;\n   }\n', '   do { } while (0);\n',
     'a deleted block holding a non-telemetry store'),
    # the block-local's name still used after its deleted declaration
    ('M8', '   return g_keep + (int)now;', '   return g_keep + (int)now + (int)age;', 'a deleted declaration still used'),
    # a function-scope local deleted while it is still read
    ('M9', '   unsigned now = clock_us();\n', '', 'a deleted local still read'),
)


def refused(label, src, mp, tel=False):
    try:
        S.apply(src, S.read_map(mp), None, tel)
        bad('%s: NOT refused' % label)
    except S.Refused as e:
        ok('%s: refused (%s)' % (label, str(e)[:70]))


def tel_wrap(text):
    """the C5-1 hand step after struct: telemetry stores go in TEL(); the toy defines TEL."""
    t = text
    for a in ('pacing_tel.ll_hit++;', 'pacing_tel.ll_hit += 2;', 'pacing_tel.lat_hist[c & 7]++;',
              'pacing_tel.lat_vc0 = vcount();'):
        t = t.replace(a, 'TEL(%s);' % a[:-1])
    return t.replace('#define fe_evt(...)', '#ifdef GPSP_KEEP_TELEMETRY\n#define TEL(stmt) do { stmt; } while (0)\n'
                     '#else\n#define TEL(stmt) do { } while (0)\n#endif\n#define fe_evt(...)')


def cpp(text, defs, d):
    """the toy preprocessed (gcc -E -P) when gcc exists, else None."""
    gcc = shutil.which('gcc')
    if not gcc:
        return None
    p = os.path.join(d, 'pp.c')
    open(p, 'w').write(text)
    r = subprocess.run([gcc, '-E', '-P'] + defs + [p], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def strip_includes(text):
    return text.replace('#include <stddef.h>\n', '#define NULL ((void *)0)\n')


def run():
    m = S.read_map(MAP)
    new, rep = S.apply(TOY, m)
    blocks = re.findall(r'static struct (\w+)\n\{', new)
    check(blocks == ['fe_pacing_tel', 'fe_pacing', 'fe_pacing_cfg'] or sorted(blocks) == sorted(
        ['fe_pacing', 'fe_pacing_cfg', 'fe_pacing_tel']), 'S1 three struct blocks written (%s)' % blocks)
    check(re.search(r'^static unsigned g_ll_hit;', new, re.M) is None and 'pacing_tel.ll_hit++;' in new
          and 'pacing_tel.ll_hit += 2;' in new and 'pacing_tel.lat_hist[c & 7]++' in new,
          'S1 the definitions went, every code use renamed')
    check('/* g_ll_hit in a comment stays */' in new and '"g_ll_hit"' in new,
          'S1 comments and string literals untouched')
    check('volatile int ll_state;   /* state machine */' in new and '} pacing __attribute__((section(".sbss")));' in new,
          'S1 volatile + the member comment kept; the section moved to the object')
    check('unsigned lat_hist[8];' in new and '.ff_pace = 7,' in new and '.ff_name = "a; b = c",' in new,
          'S1 array dims kept; cfg initializers designated, a `;`/`=` inside a string handled')
    check(new.count('#ifdef GPSP_KEEP_TELEMETRY') == 1, 'S1 the tel block is under #ifdef GPSP_KEEP_TELEMETRY')
    check(any('pacing_tel' in r and '3 members' in r for r in rep), 'S1 report: %s' % rep)
    # S3 (tier C C5-3): a trailing comment over three lines goes with its declaration
    ml = ('static long long g_ck;      /* one\n                             * two\n'
          '                             * three */\nstatic int g_ck2;\nint f(void) { return (int)g_ck + g_ck2; }\n')
    try:
        t3, _ = S.apply(ml, S.read_map('struct ck fe_ck st\ng_ck ck.a\ng_ck2 ck.b\n'))
        check('   long long a;   /* one\n                   * two\n                   * three */' in t3
              and t3.count('*/') == 1 and t3.count('/*') == 1,
              'S3 a three-line trailing comment moves into the struct whole (nothing left behind)')
    except S.Refused as e:
        bad('S3 refused: %s' % e)
    d = tempfile.mkdtemp(prefix='gstruct.')
    try:
        gcc = shutil.which('gcc')
        if gcc:
            for arm in (['-DGPSP_KEEP_TELEMETRY'], []):
                p = os.path.join(d, 't.c')
                open(p, 'w').write(tel_wrap(new) if not arm else new)
                r = subprocess.run([gcc, '-fsyntax-only', '-Wall'] + arm + [p], capture_output=True, text=True)
                check(r.returncode == 0, 'S2 gcc -fsyntax-only %s: %s' % (arm or 'release (TEL wrapped)',
                                                                      r.stderr.strip()[:200] or 'clean'))
        else:
            print('  skip  S2 (no gcc on PATH)')
        # refusals
        refused('R1 extern', TOY + 'extern int g_x;\n', 'struct t fe_t st\ng_x t.x\n')
        refused('R2 defined twice', TOY.replace('static int g_keep;', '#ifdef A\nstatic int g_keep;\n#else\nstatic int g_keep;\n#endif'),
                'struct t fe_t st\ng_keep t.keep\n')
        refused('R3 exported ABI', TOY + 'static int g_running;\n', 'struct t fe_t st\ng_running t.r\n')
        refused('R4 non-static', TOY + 'int g_pub;\n', 'struct t fe_t st\ng_pub t.p\n')
        refused('R5 const', TOY + 'static const int g_c = 1;\n', 'struct t fe_t cfg\ng_c t.c\n')
        refused('R6 macro-global', TOY.replace('static int g_keep;', '#ifdef A\n#define g_keep 0\n#else\nstatic int g_keep;\n#endif'),
                'struct t fe_t st\ng_keep t.keep\n')
        refused('R7 aligned', TOY + 'static int g_al __attribute__((aligned(64)));\n', 'struct t fe_t st\ng_al t.al\n')
        refused('R8 section mismatch', TOY, 'struct t fe_t st\ng_ll_state t.s\n')
        refused('R9a init in st', TOY, 'struct t fe_t st\ng_ff_pace t.p\n')
        refused('R9b no init in cfg', TOY, 'struct t fe_t cfg\ng_keep t.k\n')
        refused('R10 mixed #if', TOY.replace('static int g_keep;', '#ifdef A\nstatic int g_keep;\n#endif'),
                'struct t fe_t st\ng_keep t.k\ng_ll_hit t.h\n')
        refused('R11 shared line', TOY.replace('static int g_keep;', 'static int g_keep; static int g_k2;'),
                'struct t fe_t st\ng_keep t.k\n')
        evt = TOY.replace('g_ll_hit++;', 'g_ll_hit++; fe_evt("%u", g_ll_hit);')
        refused('R12a fe_evt use without --tel', evt, 'struct t fe_t tel\ng_ll_hit t.h\n')
        try:
            t2, _ = S.apply(evt, S.read_map('struct t fe_t tel\ng_ll_hit t.h\n'), None, True)
            check('fe_evt("%u", t.h)' in t2, 'R12b with --tel the fe_evt argument is renamed (G0R lab/harness check it)')
        except S.Refused as e:
            bad('R12b --tel refused: %s' % e)
        refused('R13 name taken', TOY, 'struct g_keep fe_t st\ng_lat_vc0 g_keep.v\n')
        # ---- G0R
        old_rel = strip_includes(TOY)
        new_rel = strip_includes(new)
        nt = tel_wrap(new_rel)
        pp = lambda text, defs: cpp(text, defs, d)
        base_lab, base_rel = pp(old_rel, ['-DGPSP_KEEP_TELEMETRY']), pp(old_rel, [])
        if base_lab is None:
            print('  skip  G1-M5 preprocessed (no gcc); token-level cases only')
            base_lab = base_rel = old_rel.replace('#define fe_evt(...) do { } while (0)\n', '')
        tk = R.tokens
        mm = S.read_map(MAP)
        # G1: no telemetry wrap, equal
        cand = pp(new_rel, ['-DGPSP_KEEP_TELEMETRY'])
        if cand is not None:
            okk, rep = R.compare(tk(base_lab), tk(cand), mm)
            check(okk, 'G1 G0R equal: before vs after struct (%s)' % rep[-1])
            lab_t = pp(nt, ['-DGPSP_KEEP_TELEMETRY'])
            okk, rep = R.compare(tk(base_lab), tk(lab_t), mm, tel=True)
            check(okk, 'G2 G0R equal --tel in the telemetry build (%s)' % rep[-1])
            rel_t = pp(nt, [])
            okk, rep = R.compare(tk(base_rel), tk(rel_t), mm, tel=True, mode='deletions')
            check(okk and any('REVIEW' in x and 'vcount' in x for x in rep),
                  'G3 G0R release: only telemetry stores deleted, the vcount() store listed (%s)' % rep[-1])
            okk, rep = R.compare(tk(base_lab), tk(pp(new_rel.replace('g_keep = pacing.ll_state +', 'g_keep = pacing.ll_state -'),
                                                     ['-DGPSP_KEEP_TELEMETRY'])), mm)
            check(not okk, 'M1 a non-rename edit fails G0R (%s)' % rep[-1][:90])
            m2 = nt.replace('pacing.ll_state = c ? pacing_cfg.ff_pace : 0;', 'TEL(pacing.ll_state = c ? pacing_cfg.ff_pace : 0);')
            okk, rep = R.compare(tk(base_rel), tk(pp(m2, [])), mm, tel=True, mode='deletions')
            check(not okk and any('not a telemetry store' in x for x in rep),
                  'M2 a deleted store to a non-telemetry member fails the release checker')
            m3 = nt.replace('int main(void) { step(1);', 'int main(void) { TEL(step(1));')
            okk, rep = R.compare(tk(base_rel), tk(pp(m3, [])), mm, tel=True, mode='deletions')
            check(not okk, 'M3 a deleted call statement fails the release checker')
            m4 = new_rel.replace('pacing_tel.ll_hit += 2;', 'pacing_tel.lat_vc0 += 2;')
            okk, rep = R.compare(tk(base_lab), tk(pp(m4, ['-DGPSP_KEEP_TELEMETRY'])), mm)
            check(not okk, 'M4 a site renamed to another member fails G0R')
            m5 = nt.replace('static unsigned vcount(void) { return 3; }',
                            '#ifdef GPSP_KEEP_TELEMETRY\nstatic unsigned vcount(void) { return 3; }\n#else\n'
                            'static unsigned vcount(void) { return 4; }\n#endif')
            okk, _ = R.compare(tk(base_rel), tk(pp(m5, [])), mm, tel=True, mode='deletions')
            mt = S.read_map(MAP + 'telfn vcount\n')
            okk2, _ = R.compare(tk(base_rel), tk(pp(m5, [])), mt, tel=True, mode='deletions')
            check(not okk and okk2, 'M5 a telfn body differing in the release: FAIL without `telfn`, ok with it')
        # ---- tier C C5-1: whole telemetry statements in the release (token level, no compiler)
        mb = S.read_map(MAP + 'telobj g_min\n')
        ref = BLK_REF
        okk, rep = R.compare(tk(ref), tk(BLK_NEW), mb, tel=True, mode='deletions')
        check(okk and any('REVIEW' in x and 'flip' in x for x in rep) and any('REVIEW' in x and 'hc' in x for x in rep),
              'G4 release: a latch census block, a TEL(if ...), a block-local and a TEL\'d local, a telobj '
              'and its file-scope definition deleted; the calls listed (%s)' % rep[-1][:110])
        for tag, old, new, what in BLK_MUTANTS:
            assert BLK_NEW.count(old) == 1, tag
            okk, rep = R.compare(tk(ref), tk(BLK_NEW.replace(old, new)), mb, tel=True, mode='deletions')
            check(not okk, '%s %s fails the release checker (%s)' % (tag, what, rep[-1][:100]))
        # ---- census --check
        import globals as G
        okc, offenders = G.module_check([
            dict(name='pacing', extern=False, owner='pacing', type='struct fe_pacing', rel=True),
            dict(name='pacing_tel', extern=False, owner='pacing', type='struct fe_pacing_tel', rel=False),
            dict(name='g_ff_uncapped', extern=False, owner='pacing', type='int', rel=True),
            dict(name='g_other', extern=False, owner='audio', type='int', rel=True)], 'pacing')
        check(okc and not offenders, 'K1 census --check: the struct objects and an allowlisted name pass')
        okc, offenders = G.module_check([
            dict(name='pacing', extern=False, owner='pacing', type='struct fe_pacing', rel=True),
            dict(name='g_ll_stray', extern=False, owner='pacing', type='unsigned', rel=True)], 'pacing')
        check(not okc and offenders == ['g_ll_stray'], 'K1 census --check: a stray object of the module fails (%s)' % offenders)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('gstruct/g0r selftest: %s' % ('PASS' if not FAILS else '%d FAILED' % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run())
