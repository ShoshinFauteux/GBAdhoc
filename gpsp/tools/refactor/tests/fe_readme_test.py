#!/usr/bin/env python3
"""fe_readme_test.py -- self-test of fe_readme.py (psp/fe/README.md, PLAN-P-beta5 §5.2), on scratch
copies of what it reads (the split tree, the cutlist, the host tests that read main_psp.c, the T14
headers), never the tree itself.  Also run by `fe_readme.py --selftest [--gpsp DIR]`.

    python3 fe_readme_test.py [GPSP_DIR]

  C1 the committed psp/fe/README.md is what the generator gives on a copy (--check rc 0)
  C2 the README follows the cutlist: a row's owner moved to another module changes it (--check rc 1)
  C3 a CRLF cutlist (a git archive of a core.autocrlf=true checkout) gives the same README (--check rc 0)
  C4 region readers through helpers: fn:power_cb is read by test_power_callback.py
     (test_psp_runtime.marked_function), fn:audio_thread by test_audio_channel.py (a loop over a tuple),
     ff:me_rend_present by run_ff_tests.py (its own helper, 'ff:' + name)
  C5 --plan on a synthetic PLAN.md made from the transcribed tables: rc 0
  C6 the #if context of the harness rows the 2026-11-08 review named (run_testpat, psp_state_load: none)
     and of a GBADHOC_MGIFT one (mgift_start)
  P-review r3:
  C7 every table row of the generated README has its header's GFM cell count (gfm_table_errors), and
     eboot_crc's #if context cell, split the GFM way and unescaped, keeps its `|| defined(GPSP_KEEP_TELEMETRY)` (B5)
  C8 row names (B7): a name defined once is bare everywhere (no `#1` in "Rows outside their owner's
     fragments"); a name defined twice is name#1 / name#2; the "Row names" convention comes before the first
     name#N
  C9 the Macros sentence counts fe_priv.h's and play_defaults.h's #define lines without their include
     guards, and names the guards (B8a)
  C10 the qs: prefix row says two tests slice it (two read qs:dispatch); a third reader -> "three tests" (B8c)
  C11 the no-fragment-yet sentence names boot's G rows where they sit and no longer says "in `main()` today" (B8b)
  C12 the copy sites in no T14 flavour are listed (abi_asserts.unbuilt), the six of 2026-11-09 (B6)
  M15 gfm_table_errors on a row with an unescaped `||` in a code span -> 1 error, escaped -> 0; the
      generator with cell escaping removed -> refused (its own GFM check)
  M16 a fragment copy gains an extern in an arm no flavour defines -> the README lists it (7 sites)
  M1 a hand edit of the README                         -> --check STALE (rc 1)
  M2 a G row filed in another fragment than it sits in -> refused
  M3 the cutlist's source sha256 is not the inverse's  -> refused (a stale cutlist)
  M4 a LAB_HOOK( site without its /* @lab */ tag       -> refused
  M5 two fragment includes of the shell swapped        -> refused (FRAG order)
  M6 a module the PLAN §1.2 table does not know        -> refused
  M7 a single-file main_psp.c (no fragments)           -> refused (not a split tree)
  M8 a host test reading a region by a computed name   -> refused (never skipped)
  M9 a host test reading a region no marker defines    -> refused
  M10 a wrapper around marked_function (a helper of a helper) -> its region lists the test
  M11 pp_context: a directive in a comment, a continued #if, #elif/#else arms, nesting; unbalanced -> refused
  M12 --plan: a responsibility cell with words added (the review's mgift parenthetical) -> rc 1
  M13 --plan: an exported name missing from §1.3 / a forever row not in NEVER_TU     -> rc 1
  M14 an exported global (g_running) owned by another module in the cutlist          -> refused
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
    rels += ['tools/refactor/generated/%s/fe_abi_asserts.h' % fl for fl in fe_readme.T14_FLAVOURS]
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


def gen_text(d):
    rc, msg = run_gen(d, '-o', os.path.join(d, 'out.md'))
    if rc:
        return None, msg
    return open(os.path.join(d, 'out.md'), encoding='utf-8').read(), msg


def region_row(text, name):
    m = re.search(r'^\| `%s` \| `[^`]+` \| (.*) \|$' % re.escape(name), text, re.M)
    return m.group(1) if m else None


def fn_row(text, name):
    m = re.search(r'^\| `%s` \| `[^`]+` \| (.*) \|$' % re.escape(name), text, re.M)
    return m.group(1) if m else None


def synthetic_plan():
    """a PLAN.md with exactly the sections and cells plan_check reads, built from the transcription"""
    num_kind = {m: ('`fe/%s.inc.c` (F, forever)' if m in fe_readme.NEVER_TU else '`fe/%s.inc.c` (F)') % m
                for m in fe_readme.PLAN12}
    L = ['# PLAN', '', '## 1. Target architecture', '', '### 1.1 Physical shape', '',
         '**Never real TUs, by rule.** Power, audio, pacing, the ME presenter, the frame loop, and io_thread.', '',
         '### 1.2 Module list', '', '| # | File (kind) | Responsibility | From (L:) | Owns | Public surface | lines |',
         '|---|---|---|---|---|---|---|',
         '| 0 | `fe/fe_priv.h` (header) | macros | scattered | none | private | – |',
         '| 1 | `fe/play_defaults.h` | `PLAY_*` | 200 | none | private | – |']
    for m, (num, resp) in fe_readme.PLAN12.items():
        L.append('| %s | %s | %s | 1-2 | x | private | 1 |' % (num, num_kind[m], resp))
    L += ['', '### 1.3 Ownership', '',
          '- **Exported ABI keeps names and linkage.** That covers %s, the weak overrides. Two non-static symbols '
          'have no outside user: `g_rfu_activated` and `sess_cost_evt`.'
          % ', '.join('`%s` (1)' % n for v in fe_readme.EXPORTED.values() for n in v), '',
          '### 1.4 Public headers', '', '## 2. Staged milestones', '',
          '### HW-3: epoch C, the cold restructure', '',
          '**C1. Behaviour-exact exit unification** (§3). `session_end(td)`, `process_end(pe)`.',
          '**C2. `boot_run(boot_ctx*)`**, `noinline` and cold.', '**C3. Session phases and `for(;;)`** (§4).', '',
          '### HW-4: epoch D1, the loop leaves main', '', '1. **`frame_iter(loop_t *L, session_ctx *S)`, noinline**', '',
          '## 3. Exit-path unification design', '']
    return '\n'.join(L) + '\n'


def plan_rc(path):
    err, out = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = fe_readme.main(['--plan', path])
    return rc, out.getvalue() + err.getvalue()


def pp_unit():
    """M11: pp_context on a synthetic text"""
    t = ('int a;\n'                                  # 1
         '/* #if COMMENTED */\n'                     # 2
         '#if defined(A) || \\\n'                    # 3
         '    defined(B)\n'                          # 4
         'int b;\n'                                  # 5
         '#ifdef C\n'                                # 6
         'int c;\n'                                  # 7
         '#endif\n'                                  # 8
         '#elif D\n'                                 # 9
         'int d;\n'                                  # 10
         '#else /* no */\n'                          # 11
         'int e;\n'                                  # 12
         '#endif\n'                                  # 13
         'int f; /* #endif */\n')                    # 14
    c = fe_readme.pp_context(t)
    want = {1: (), 2: (), 5: ((3, ('#if defined(A) || defined(B)',)),),
            7: ((3, ('#if defined(A) || defined(B)',)), (6, ('#ifdef C',))),
            10: ((3, ('#if defined(A) || defined(B)', '#elif D')),),
            12: ((3, ('#if defined(A) || defined(B)', '#elif D', '#else')),), 14: ()}
    got = {k: c[k - 1] for k in want}
    if got == want:
        ok('M11 pp_context: comment directives ignored, continuation joined, #elif/#else arms and nesting')
    else:
        bad('M11 pp_context: %r != %r' % (got, want))
    try:
        fe_readme.pp_context('#if A\nint x;\n')
        bad('M11 an unbalanced #if was not refused')
    except fe_readme.Stale as e:
        ok('M11 an unbalanced #if: refused (%s)' % e)
    try:
        fe_readme.pp_context('int x;\n#endif\n')
        bad('M11 an #endif with no #if was not refused')
    except fe_readme.Stale as e:
        ok('M11 an #endif with no #if: refused (%s)' % e)


def section(text, head):
    """the text of a `## ` / `### ` section, up to the next heading of any level"""
    i = text.find('\n%s\n' % head)
    if i < 0:
        return None
    j = re.search(r'\n#{2,3} ', text[i + len(head) + 2:])
    return text[i:i + len(head) + 2 + (j.start() if j else len(text))]


def r3_rows(gpsp, root):
    """P-review r3 (B5-B8): C7-C12, M15, M16"""
    d = scratch(gpsp, root)
    text, msg = gen_text(d)
    if text is None:
        bad('C7-C12 generation failed: %s' % msg.strip()[:200])
        return
    errs = fe_readme.gfm_table_errors(text)
    m = re.search(r'^\| `eboot_crc` \|.*$', text, re.M)
    cells = fe_readme.gfm_cells(m.group(0)) if m else []
    ctx = cells[2].strip().replace('\\|', '|') if len(cells) == 3 else ''
    ntab = len(re.findall(r'^\|[-|]+\|$', text, re.M))
    (ok if not errs and ntab >= 6 and '|| defined(GPSP_KEEP_TELEMETRY)' in ctx else bad)(
        'C7 %d tables, %d rows with another GFM cell count than their header; eboot_crc context cell: %s'
        % (ntab, len(errs), ctx[:90]))

    out = section(text, "## Rows outside their owner's fragments") or ''
    hashed = re.findall(r'^- `[^`]+` [FG] `(\w+#\d+)` -> ', out, re.M)
    conv = text.find('- **Row names.**')
    first = min([text.find(x) for x in ('`sleep_sim_window_enter#1`', '`vid_prof_frame#1`') if text.find(x) >= 0] or [-1])
    (ok if out and not hashed and '`g_running` -> power' in out and '| `sleep_sim_window_enter#1` |' in text
     and '| `sleep_sim_window_enter#2` |' in text and 'sleep_sim_window_enter:' not in text
     and 0 <= conv < first else bad)(
        'C8 row names: %d #N names among the rows outside their owner (want 0); name#1/#2 for the two-arm names; '
        '"Row names" at %d, first name#N at %d' % (len(hashed), conv, first))

    dc, guards = fe_readme.defines(fe_readme.load(d))
    raw = {h: sum(1 for ln in open(os.path.join(d, 'psp', 'fe', h), encoding='utf-8')
                  if re.match(r'^\s*#\s*define\b', ln)) for h in ('fe_priv.h', 'play_defaults.h')}
    mac = re.search(r'^- \*\*Macros\.\*\*.*$', text, re.M)
    mac = mac.group(0) if mac else ''
    (ok if guards == {'fe_priv.h': 'FE_PRIV_H', 'play_defaults.h': 'PLAY_DEFAULTS_H'}
     and dc['fe_priv.h'] == raw['fe_priv.h'] - 1 and dc['play_defaults.h'] == raw['play_defaults.h'] - 1
     and '(%d `#define` lines, not counting its include guard `FE_PRIV_H`)' % dc['fe_priv.h'] in mac
     and '(%d, not counting its include guard `PLAY_DEFAULTS_H`)' % dc['play_defaults.h'] in mac else bad)(
        'C9 Macros: fe_priv.h %d of %d #define lines, play_defaults.h %d of %d (guards %s)'
        % (dc['fe_priv.h'], raw['fe_priv.h'], dc['play_defaults.h'], raw['play_defaults.h'], guards))

    qs = re.search(r'^\| `qs:` \| ([^|]*) \|', text, re.M)
    sl = re.search(r'^\| `sl:` \| ([^|]*) \|', text, re.M)
    good = (qs and qs.group(1).startswith('a statement range two tests slice ')
            and sl and sl.group(1).startswith('a statement range one test slices '))
    d2 = scratch(gpsp, root)
    edit(os.path.join(d2, 'tools/tests/test_volume.py'), lambda t: t + '\n_r3 = extract.extract(src, "qs:dispatch")\n')
    t2, msg2 = gen_text(d2)
    qs2 = re.search(r'^\| `qs:` \| ([^|]*) \|', t2 or '', re.M)
    (ok if good and qs2 and qs2.group(1).startswith('a statement range three tests slice ') else bad)(
        'C10 qs: %r, sl: %r; with a third reader of qs:dispatch: %r'
        % (qs and qs.group(1)[:40], sl and sl.group(1)[:40], qs2 and qs2.group(1)[:40]))

    later = re.search(r'^teardown, boot, session, loop have no fragment yet.*$', text, re.M)
    later = later.group(0) if later else ''
    (ok if later and 'in `main()` today' not in later and 'boot owns 5 G rows, which sit in `fe/link.d.inc.c` '
     '(lines 893, 894, 895: `g_start_keepalive`' in later and 'teardown, session and loop own none' in later else bad)(
        'C11 the no-fragment-yet sentence: %s' % later[:260])

    unb = section(text, '### Core-ABI copies in no T14 flavour') or ''
    got = re.findall(r'^\| `(\w+)` \| `([^`]+)` \| (\w+) \|', unb, re.M)
    # the shell's site by its text: a comment-only edit of the shell moves its line (P-tidy: the
    # file header 37 -> 10 lines took it from main_psp.c:2828 to :2801); the fragments' cites hold
    reopens = [i for i, ln in enumerate(open(os.path.join(gpsp, 'psp', 'main_psp.c'), encoding='utf-8'), 1)
               if re.match(r'\s*extern u32 gamepak_reopens;', ln)]
    want = [('jit_pub_inline', 'fe/inproc.a.inc.c:304'), ('xlat_scan_fast', 'fe/inproc.a.inc.c:305'),
            # HW-2 (claude/hw2-exit) moved gbab_reg/fe_gbadual_running_inst: harness.f +16
            # (stall_exit's comment and checks); rom_hash_census: HW-1 613 -> 625 (the cb_stack
            # EVT in core_health_evt), HW-2 +16 -> 641; the shell's site is found by its text (above)
            ('gbab_reg', 'fe/harness.f.inc.c:303'), ('fe_gbadual_running_inst', 'fe/harness.f.inc.c:304'),
            ('rom_hash_census', 'fe/harness.f.inc.c:641'),
            ('gamepak_reopens', 'main_psp.c:%d' % reopens[0] if len(reopens) == 1 else 'main_psp.c:?')]
    (ok if [(n, c) for n, c, _ in got] == want and '6 of its 124 copy sites' in text else bad)(
        'C12 copy sites in no T14 flavour: %s' % ', '.join('%s %s' % (c, n) for n, c, _ in got))

    one = '| a | b | c |\n|---|---|---|\n| `x` | `y` | `#if A || B` |\n'
    esc = '| a | b | c |\n|---|---|---|\n| `x` | `y` | `#if A \\|\\| B` |\n'
    e1, e0 = fe_readme.gfm_table_errors(one), fe_readme.gfm_table_errors(esc)
    (ok if len(e1) == 1 and e1[0][1:3] == (3, 5) and not e0 else bad)(
        'M15a gfm_table_errors: unescaped `||` in a code span -> %r; escaped -> %r' % (e1, e0))
    keep = fe_readme.cell
    fe_readme.cell = lambda s: str(s)
    try:
        d3 = scratch(gpsp, root)
        refused('M15b the generator with cell escaping removed', d3, r'table row\(s\) would render with another GFM cell count')
    finally:
        fe_readme.cell = keep

    d4 = scratch(gpsp, root)
    edit(os.path.join(d4, 'psp/fe/paths.inc.c'),
         lambda t: t + '#ifdef T14_NO_FLAVOUR_DEFINES_THIS\nextern unsigned t14_unbuilt_probe;\n#endif\n')
    resha(d4)
    t4, msg4 = gen_text(d4)
    u4 = re.findall(r'^\| `(\w+)` \| `([^`]+)` \|', section(t4 or '', '### Core-ABI copies in no T14 flavour') or '', re.M)
    (ok if t4 and len(u4) == 7 and any(n == 't14_unbuilt_probe' and c.startswith('fe/paths.inc.c:') for n, c in u4)
     else bad)('M16 an extern in an arm no flavour defines, added to fe/paths.inc.c: %d sites listed (%s)'
               % (len(u4), msg4.strip()[:120] if t4 is None else ', '.join(n for n, _ in u4)))


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
        text, msg = gen_text(d)
        if text is None:
            bad('C4/C6 generation failed: %s' % msg.strip()[:200])
        else:
            for reg, rdr in (('fn:power_cb', 'test_power_callback.py'), ('fn:audio_thread', 'test_audio_channel.py'),
                             ('ff:me_rend_present', 'run_ff_tests.py'), ('fn:sleep_park_hold', 'test_sleep_quiesce.py')):
                row = region_row(text, reg)
                (ok if row and '`%s`' % rdr in row else bad)('C4 %s read by %s: %s' % (reg, rdr, row))
            for fn, want in (('run_testpat', 'none: every build'), ('psp_state_load', 'none: every build'),
                             ('mgift_start', '`#ifdef GBADHOC_MGIFT`'), ('cheats_link_poll', '`#ifdef GPSP_CHEATS`')):
                row = fn_row(text, fn)
                (ok if row and row.startswith(want) else bad)('C6 %s #if context: %s' % (fn, row))

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'psp/fe/README.md'), lambda t: t.replace(' -> power\n', ' -> input\n', 1))
        rc, msg = run_gen(d, '--check')
        (ok if rc == 1 and 'STALE' in msg else bad)('M1 a hand-edited README: --check rc %d' % rc)

        d = scratch(gpsp, root)
        cp = os.path.join(d, 'tools/refactor/cutlist.txt')
        edit(cp, lambda t: re.sub(r'^(G\tg_loop_iter#1\t)power\t', r'\1input\t', t, count=1, flags=re.M))
        rc, msg = run_gen(d, '--check')
        (ok if rc == 1 and 'STALE' in msg else bad)(
            'C2 g_loop_iter#1 owner power -> input in the cutlist: the README changes (--check rc %d)' % rc)

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
             lambda t: re.sub(r'^(G\tg_loop_iter#1\t)power\t', r'\1bogus\t', t, count=1, flags=re.M))
        refused('M6 a module the §1.2 table does not know (bogus)', d, 'no MODULES row')

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'psp/main_psp.c'), lambda t: re.sub(r'(?m)^#include "fe/\S+\.inc\.c"\n', '', t))
        refused('M7 a shell with no fragment includes', d, 'not a split tree')

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'tools/tests/test_volume.py'),
             lambda t: t + '\n_m8 = extract.extract(src, "".join(["fn:", "main"]))\n')
        refused('M8 test_volume.py reads a region by a computed name', d, r'test_volume\.py:\d+: reads an @extract region whose name')

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'tools/tests/test_volume.py'), lambda t: t + '\n_m9 = extract.extract(src, "fn:no_such_fn")\n')
        refused('M9 test_volume.py reads fn:no_such_fn', d, 'fn:no_such_fn .*which no marker defines')

        d = scratch(gpsp, root)
        before = region_row(gen_text(d)[0] or '', 'fn:loop_park') or ''
        edit(os.path.join(d, 'tools/tests/test_audio_channel.py'),
             lambda t: t + '\n\ndef _m10(s, n):\n    return marked_function(s, n)\n\n\n_m10(SOURCE, "loop_park")\n')
        text, msg = gen_text(d)
        row = region_row(text or '', 'fn:loop_park')
        (ok if row and '`test_audio_channel.py`' in row and '`test_audio_channel.py`' not in before else bad)(
            'M10 a wrapper of marked_function: fn:loop_park %r -> %r' % (before, row))

        pp_unit()

        pp = os.path.join(root, 'PLAN.md')
        open(pp, 'w', encoding='utf-8').write(synthetic_plan())
        rc, msg = plan_rc(pp)
        (ok if rc == 0 else bad)('C5 --plan on the synthetic PLAN.md: rc %d %s' % (rc, msg.strip()[:160]))
        open(pp, 'w', encoding='utf-8').write(synthetic_plan().replace('| Mystery Gift glue |',
                                                                       '| Mystery Gift glue (`GBADHOC_MGIFT` builds) |'))
        rc, msg = plan_rc(pp)
        (ok if rc == 1 and 'mgift (row 11)' in msg else bad)('M12 --plan, the mgift cell differs: rc %d %s' % (rc, msg.strip()[:160]))
        open(pp, 'w', encoding='utf-8').write(synthetic_plan().replace('`g_ff_mult` (1), ', ''))
        rc, msg = plan_rc(pp)
        (ok if rc == 1 and 'EXPORTED' in msg else bad)('M13a --plan, g_ff_mult not in §1.3: rc %d %s' % (rc, msg.strip()[:160]))
        open(pp, 'w', encoding='utf-8').write(synthetic_plan().replace('`fe/boost.inc.c` (F)', '`fe/boost.inc.c` (F, forever)'))
        rc, msg = plan_rc(pp)
        (ok if rc == 1 and 'NEVER_TU' in msg else bad)('M13b --plan, boost made a forever row: rc %d %s' % (rc, msg.strip()[:160]))

        d = scratch(gpsp, root)
        edit(os.path.join(d, 'tools/refactor/cutlist.txt'),
             lambda t: re.sub(r'^(G\tg_running#1\t)power\t', r'\1input\t', t, count=1, flags=re.M))
        refused('M14 g_running (exported, power) owned by input in the cutlist', d, 'EXPORTED g_running: owned by input')

        r3_rows(gpsp, root)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print('fe_readme self-test: %s' % ('PASS' if not FAILS else '%d FAILED' % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(R, '..', '..'))))
