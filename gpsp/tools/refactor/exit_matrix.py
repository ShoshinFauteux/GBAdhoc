#!/usr/bin/env python3
"""exit_matrix.py -- T8 (PLAN §3.6, gate XS): the exit-sequence matrix of psp/main_psp.c,
per flavour, from `psp-gcc -E` output (not from the raw source).

What it builds (one directory per flavour, plus `raw/` for the raw-source view):
  exit_sites.tsv      one row per exit site (families A-C, plus out-of-band sites in
                      other functions): terminal line, code/why, the ordered teardown
                      vocabulary, the §3.3 ORDER selector and flags, and the full call list.
  exit_steps.tsv      the shared loop teardown (family D: session_end / inproc_try_return /
                      process_end, located by anchors: everything after main's frame loop),
                      one row per call or guard, mapped to PLAN §3.2's step ids, with the
                      guarding conditions and runtime-condition tags.
  exit_scenarios.tsv  the D path INTERPRETED under runtime scenarios (XMB, PROFILE,
                      LIST_OK, LIST_REFUSED, LIST_RETAIN_THEN_REFUSE, LIST_CENSUS, LIST_FORCE):
                      the ordered tracked calls each one makes.  Conditions are evaluated
                      with C short-circuit semantics; an atom it has no value for FAILS.
  exit_expect.h       the same expectations as C arrays, for tests/test_exit_model.c.
  raw_vs_e.tsv        (flavour dirs) every call that differs between the raw-source parse
                      and the -E parse, classified '#if' or 'macro' (from `-dM`); anything
                      else is UNEXPLAINED and fails.
Checks run by `gen`/`all`:
  - x2 reproduction: the A1-A5, B1-B10, C1-C4 orders of x2-exit-paths.md §2.1-2.3
    (raw source and harness -E views);
  - PLAN §3.2 order: step ids are non-decreasing along the teardown, in every flavour;
  - the release-live rows exist in the release flavour: gpsp_rtc_peek (18297),
    heap_trace_report("post_unload") (18219), the inproc_force ini read (18223) and the
    `g_relaunch != GAMELIST` audio_hash guard (18286);
  - raw vs -E differences are all '#if' or 'macro'.
`check --golden DIR --cand DIR [--allow FILE]` is the XS gate itself: per site and
flavour, the ordered vocabulary of the candidate equals the golden, or the allow-list
names that (flavour, site) with a fix commit.

Usage:
  exit_matrix.py all --baseline RUNDIR --src GPSP_ROOT --out OUTDIR      (3 flavours + raw)
  exit_matrix.py gen --flavour NAME --pp X.i --dm X.dM --raw main_psp.c --out OUTDIR
  exit_matrix.py check --golden OUTDIR --cand OUTDIR [--allow allow.tsv]
  exit_matrix.py --selftest   (parser/interpreter unit cases; tests/test_exit_matrix.sh
                               runs the baseline and the mutants)
Exit: 0 ok, 1 check failed, 2 usage.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import aec_cparse as C  # noqa: E402

FLAVOURS = ('release', 'lab', 'harness')

# ------------------------------------------------------------- site vocabulary
# P3's names for the hijacked SDK calls (PLAN-P-beta5 §4 P3): FE_EXIT_GAME() is EXIT
# (P13), FE_HANDOFF(c, w) is HO (P10).  The raw view reads them AS the call they wrap
# (aec_cparse alias at lex time), so P3's sed leaves every table identical; -E already
# shows the expansion.
EXIT_ALIAS = {'FE_EXIT_GAME': 'sceKernelExitGame', 'FE_HANDOFF': 'handoff_run'}
EXIT_NAMES = ('sceKernelExitGame', 'FE_EXIT_GAME')
HANDOFF_NAMES = ('handoff_run', 'FE_HANDOFF')
VOC = {
    'FE_EXIT_GAME': 'EXIT', 'FE_HANDOFF': 'HO',
    'io_thread_stop': 'IO', 'fe_host_shutdown': 'HOST', 'PSP_REWIND_GAME_OVER': 'RW',
    'psp_rom_pool_over': 'RW', 'psp_rewind_pwr': 'RW', 'audio_stop': 'AUD',
    'vid_term': 'VID', 'fe_evt': 'EVT', 'evt_shutdown': 'ES', 'handoff_run': 'HO',
    'lab_finish': 'HO', 'LAB_HANDOFF': 'HO', 'pwr_release_all': 'PRA',
    'relaunch_eboot': 'RL', 'sceKernelExitGame': 'EXIT', 'ui_notice': 'NOTICE',
    'ui_loading_finish': 'LOADFIN', 'me_rend_teardown': 'ME', 'me_video_teardown': 'ME',
    'me_host_shutdown': 'ME', 'net_teardown': 'NET', 'online_psp_down': 'NET',
}

# x2-exit-paths.md §2.1-2.3: id, ANCHOR, steps as x2 wrote them.  No line numbers
# (P0a: x2's 01067e74 ranges were A1 15088-15112 ... C4 16308-16328; every later tree
# moved them).  The anchor is the site's distinctive statement: the reason string it
# hands the Lab (`why`, a substring of the handoff_run/relaunch_eboot/lab_finish
# argument), or `call:NAME` for a site whose reason is empty.  Sites are matched IN
# ORDER: entry k takes the next main() site whose anchor matches, so a duplicated
# reason (B3/B4/B10 "sidecar_path_too_long") is told apart by its ordinal, and a site
# that moved, vanished or was replaced leaves its id unassigned (the x2 check fails).
X2 = [
    ('A1', '"prev_run_uncollected"', 'AUD VID EVT ES HO EXIT'),
    ('A2', 'why:NULL', 'AUD VID EVT ES HO EXIT'),
    ('A3', '"icinv_fail"', 'AUD VID EVT ES HO EXIT'),
    ('A4', '"icprobe_fail"', 'AUD VID EVT ES HO EXIT'),
    ('A5', '"nettest_fail"', 'AUD VID EVT ES HO EXIT'),
    ('B1', '"bad_variant"', 'EVT ES AUD VID HO EXIT'),
    ('B2', '"rom_path_too_long"', 'EVT ES AUD VID HO EXIT'),
    ('B3', '"sidecar_path_too_long"', 'EVT ES AUD VID HO EXIT'),
    ('B4', '"sidecar_path_too_long"', 'EVT ES AUD VID HO EXIT'),
    # HW-2 F8 (08401f3c): B5-B7 stop the display too (PE_NO_VIDTERM retired)
    ('B5', '"rom_path_too_long"', 'EVT ES AUD VID HO EXIT'),
    ('B6', '"rom_missing"', 'EVT ES AUD VID HO EXIT'),
    ('B7', '"no_rom"', 'EVT ES AUD VID HO EXIT'),
    ('B8', 'call:relaunch_eboot', 'EVT ES AUD VID RL EXIT'),
    ('B9', '"no_rom"', 'EVT ES AUD VID HO EXIT'),
    ('B10', '"sidecar_path_too_long"', 'EVT ES AUD VID HO EXIT'),
    ('C1', '"mem_budget"', 'LOADFIN NOTICE EVT ES AUD VID HO EXIT'),
    ('C2', '"load_failed"', 'LOADFIN NOTICE EVT ES AUD VID HO EXIT'),
    # HW-2 F9 (055f8a8c): the ME and its renderer stop first, as in session_end
    ('C3', '"bad_script"', 'ME IO HOST RW AUD VID EVT ES HO EXIT'),
    ('C4', '"net_failed"', 'ME IO HOST RW AUD VID EVT ES HO EXIT'),
]


def x2_anchor_ok(row, anchor):
    if anchor.startswith('call:'):
        return any(c.name == anchor[5:] for c in row['calls'])
    if anchor.startswith('why:'):
        return row['why'] == anchor[4:]
    return anchor in row['why']

# ------------------------------------------------------------- D step table (PLAN §3.2)
STEP_ORDER = ('D0 D1 D2 D3 D4 D5 D6 D7 D8 D9 D10 D11 D12 D13 D13b D14 D15 D16 D16b D17 D18 '
              'D19 D20 I1 I1b U1 U2 U3 U4 U5 U6 U7 U8 U9 U10 U11 U12 U12b U13 I3 I4 I5 '
              'P1 P2 P3 P4 P5 P6 P7 P8 P9 P10 P11 P12 P13').split()
STEP_RANK = {s: k for k, s in enumerate(STEP_ORDER)}

SE_STEPS = {  # session_end, by call name (ui_return_tick by occurrence)
    'ui_set_wake_yield': 'D1', 'sleep_quiesce_up': 'D2', 'play_lock_drop': 'D3',
    'pwr_hold': 'D4', 'ui_set_backdrop': 'D5', 'ui_return_hold': 'D5',
    'stall_watch_stop': 'D6', 'home_host_stop': 'D7', 'drprof_stop': 'D8',
    'me_rend_teardown': 'D9', 'me_video_teardown': 'D9', 'inproc_refusal': 'D10',
    'me_host_retain': 'D10', 'me_host_shutdown': 'D10', 'me_host_loaded': 'D11',
    'me_host_retained': 'D11', 'net_teardown': 'D13', 'mgift_stop': 'D13',
    'online_psp_down': 'D13b', 'emu_boost_stop': 'D14', 'io_thread_stop': 'D15',
    'ui_settings_flush': 'D16b', 'handoff_set_extra': 'D17', 'fe_host_shutdown': 'D18',
    'PSP_REWIND_GAME_OVER': 'D19', 'psp_rom_pool_over': 'D19', 'psp_rewind_pwr': 'D19',
}
UI_TICK = ['D12', 'D16', 'D20']
IN_STEPS = {  # inproc_try_return
    'inproc_refusal': 'I1', 'plat_audio_hash': 'I1b', 'plat_audio_sample_count': 'I1b',
    'dynarec_release_translation_caches': 'U1', 'dynarec_heap_tables_release': 'U2',
    'me_rend_suspend': 'U4', 'sceKernelDcacheWritebackInvalidateAll': 'U5',
    'sceKernelIcacheInvalidateAll': 'U5', 'home_host_forget': 'U6',
    'fe_host_session_reset': 'U7', 'osd_reset': 'U8', 'ui_close': 'U9',
    'vid_ambient_drop': 'U10', 'audio_stop': 'U11', 'audio_ring_reset': 'U11',
    'audio_start': 'U11', 'heap_snap': 'U12', 'heap_census': 'U12b',
    'heap_trace_report': 'U13', 'heap_snap_same': 'I3', 'fe_ini_get_int': 'I3',
    'psp_clock_apply': 'I4', 'sleep_sim_fire': 'I4', 'ctl_turbo_stop': 'I4',
    'ui_set_turbo_locked': 'I4', 'emu_boost_start': 'I4', 'pwr_release': 'I4',
    'me_host_shutdown': 'I5',
}
IN_EVT = {'audio_hash': 'I1b', 'inproc_census_mismatch': 'I3', 'inproc_return': 'I4',
          'inproc_refused': 'I5'}
PE_STEPS = {
    'audio_stop': 'P1', 'vid_term': 'P2', 'plat_audio_hash': 'P3',
    'plat_audio_sample_count': 'P3', 'shash_close': 'P4', 'gpsp_rtc_peek': 'P5',
    'psp_local_wallclock': 'P5', 'ctl_stick_name': 'P6', 'core_health_evt': 'P7',
    'evt_shutdown': 'P9', 'handoff_run': 'P10', 'lab_finish': 'P10', 'lab_active': 'P10',
    'LAB_HANDOFF': 'P10', 'relaunch_eboot': 'P12', 'sceKernelExitGame': 'P13',
    'FE_HANDOFF': 'P10', 'FE_EXIT_GAME': 'P13',
}
PE_EVT = {'audio_hash': 'P3', 'rtc_clock': 'P5', 'stick_range': 'P6', 'exit': 'P8'}

# names the model test records in the D scenarios (everything else is a helper)
TRACK_D = (set(SE_STEPS) | set(IN_STEPS) | set(PE_STEPS) |
           {'ui_return_tick', 'fe_evt', 'pwr_release_all'}) - {
    'plat_audio_hash', 'plat_audio_sample_count', 'psp_local_wallclock', 'ctl_stick_name',
    'psp_rewind_pwr', 'lab_active'}

RUNTIME_TAGS = [('g_relaunch', 'g_relaunch'), ('g_perf_rig', 'rig'), ('inproc_force', 'force'),
                ('inproc_refusal', 'refusal'), ('why', 'why'), ('exit_reason', 'reason'),
                ('heap_snap_same', 'census'), ('me_host_retain', 'retain'),
                ('me_host_loaded', 'me_loaded'), ('g_ss_browser', 'rig')]


def norm_cond(s):
    s = re.sub(r'g_relaunch\s*(==|!=)\s*(2|RELAUNCH_GAMELIST)\b', r'g_relaunch\1GL', s)
    s = re.sub(r'g_relaunch\s*(==|!=)\s*(1|RELAUNCH_PROFILE)\b', r'g_relaunch\1PROFILE', s)
    s = re.sub(r'\(\((void \*)\)0\)', 'NULL', s)
    return s


def tags_of(conds):
    tags = []
    txt = ' '.join(conds)
    for key, tag in RUNTIME_TAGS:
        if re.search(r'\b%s\b' % key, txt) and tag not in tags:
            tags.append(tag)
    return tags


def evt_word(args):
    m = re.match(r'\s*"([A-Za-z_]+)', args or '')
    return m.group(1) if m else '?'


def collapse(seq):
    out = []
    for x in seq:
        if not out or out[-1] != x:
            out.append(x)
    return out


# ------------------------------------------------------------- parse a view
class View(object):
    def __init__(self, name, toks):
        self.name, self.toks = name, toks
        self.funcs = C.functions(toks)
        lo, hi = self.funcs['main']
        self.main = C.parse_body(toks, lo, hi)
        self.lines = set(t.line for t in toks)
        # {terminal line: site id}, set from the raw view of the SAME tree so that the
        # -E views carry the raw view's ids (-E keeps source line numbers)
        self.line_ids = None


def find_loop_index(body):
    best, bk = -1, -1
    for k, s in enumerate(body.stmts):
        if isinstance(s, C.Loop) and s.word in ('while', 'for'):
            a, b = C.node_span(s)
            if b - a > best:
                best, bk = b - a, k
    return bk


def has_call(node, name):
    return any(c.name == name for c in C.walk_calls(node))


def is_goto(node, label):
    if isinstance(node, C.Simple):
        return node.kind == "goto" and (label is None or node.label == label)
    if isinstance(node, C.Block):
        return any(is_goto(s, label) for s in node.stmts)
    if isinstance(node, C.If):
        return is_goto(node.then, label) or is_goto(node.els, label)
    if isinstance(node, C.Loop):
        return is_goto(node.body, label)
    return False


def sites(view):
    """Early-exit sites of main + ExitGame sites in other functions."""
    res = []
    main = view.main

    def direct_exit(s):
        return isinstance(s, C.Simple) and any(
            c.name in EXIT_NAMES for c in C.calls_in(s.toks))
    for blk, k in C.blocks_containing(main, direct_exit):
        if blk is main:
            continue                       # the final ExitGame: family D
        calls = []
        for s in blk.stmts[:k + 1]:
            calls += C.walk_calls(s)
        res.append(_site_row(view, 'main', blk, calls))
    for fn, (lo, hi) in view.funcs.items():
        if fn == 'main':
            continue
        body = C.parse_body(view.toks, lo, hi)
        cl = C.walk_calls(body)
        if any(c.name in EXIT_NAMES for c in cl):
            res.append(_site_row(view, fn, body, cl))
    res.sort(key=lambda r: r['line'])
    _assign_ids(view, res)
    return res


def _assign_ids(view, res):
    """Site ids must not depend on line numbers: every phase-P edit moves them, and the
    golden is regenerated from later tips (PLAN §3.6).  The raw view names main's early
    sites by ORDINAL against x2's list when it has exactly x2's count; a -E view takes
    the raw view's id for the same terminal line (-E keeps source lines).  Otherwise '-'
    (which fails the x2 check).  `key` is unique per view: the id, else main#NN."""
    mains = [r for r in res if r['func'] == 'main']
    if view.line_ids is not None:
        for r in mains:
            r['id'] = view.line_ids.get(r['line'], '-')
    else:
        # by anchor, in order (X2): entry k takes the next site whose anchor matches
        for r in mains:
            r['id'] = '-'
        j = 0
        for sid, anchor, _ in X2:
            k = j
            while k < len(mains) and not x2_anchor_ok(mains[k], anchor):
                k += 1
            if k < len(mains):
                mains[k]['id'] = sid
                j = k + 1
    seen = set()
    for k, r in enumerate(mains):
        r['key'] = r['id'] if r['id'] != '-' and r['id'] not in seen else 'main#%02d' % k
        seen.add(r['id'])
    for r in res:
        if r['func'] != 'main':
            r['key'] = r['id']


def site_ids(st):
    return {r['line']: r['id'] for r in st if r['func'] == 'main' and r['id'] != '-'}


def _site_row(view, fn, blk, calls):
    term = [c for c in calls if c.name in EXIT_NAMES][-1]
    a, _ = C.node_span(blk)
    code = why = ''
    for c in calls:
        if c.name in HANDOFF_NAMES + ('relaunch_eboot', 'lab_finish'):
            parts = [p.strip().strip('()') for p in re.split(r',(?![^(]*\))', norm_cond(c.args))]
            code = parts[0] if parts else ''
            why = parts[1] if len(parts) > 1 else ''
    seq = collapse([VOC[c.name] for c in calls if c.name in VOC])
    sid = '-' if fn == 'main' else 'E:' + fn      # main's ids: _assign_ids
    return dict(id=sid, func=fn, first=a, line=term.line, code=code, why=why,
                seq=seq, calls=calls, order=order_of(seq), flags=flags_of(seq))


def order_of(seq):
    def pos(t):
        return seq.index(t) if t in seq else 99
    if min(pos('IO'), pos('HOST')) < pos('AUD'):
        return 'ORDER_CORE_FIRST'
    if pos('AUD') < pos('ES'):
        return 'ORDER_AV_FIRST'
    if pos('ES') < pos('AUD'):
        return 'ORDER_EVT_FIRST'
    return 'ORDER_NONE'


def flags_of(seq):
    f = []
    first = [t for t in seq if t not in ('LOADFIN', 'NOTICE')]
    if seq and seq[0] == 'EVT' and (len(seq) < 2 or seq[1] != 'ES') and 'ES' in seq:
        f.append('TD_PRE_EVT')
    if 'LOADFIN' in seq:
        f.append('TD_LOADFIN')
    if 'NOTICE' in seq:
        f.append('TD_NOTICE')
    if 'IO' in seq:
        f.append('TD_IO')
    if 'HOST' in seq:
        f.append('TD_CORE')
    if 'RW' in seq:
        f.append('TD_REWIND')
    if 'VID' not in seq:
        f.append('PE_NO_VIDTERM')
    if 'RL' in seq:
        f.append('PE_RELAUNCH')
    if 'HO' not in seq:
        f.append('PE_NO_HANDOFF')
    if 'EVT' not in [t for t in seq[1:]] and 'EVT' not in first:
        f.append('PE_NO_EVT')
    return f


def x2_norm(seq):
    s = [t for t in seq if t != 'PRA']
    s = collapse(s)
    if len(s) > 1 and s[0] == 'EVT' and s[1] != 'ES':
        s = s[1:]
    return s


# ------------------------------------------------------------- D region
def d_region(view):
    body = view.main
    k = find_loop_index(body)
    stmts = body.stmts[k + 1:]
    ki = next((i for i, s in enumerate(stmts) if isinstance(s, C.If) and is_goto(s, None)),
              None)
    if ki is None:
        raise SystemExit('exit_matrix: no in-process branch (goto session_top) after the loop')
    return stmts[:ki], stmts[ki], stmts[ki + 1:], body.stmts[k]


def step_rows(view):
    se, inp, pe, loop = d_region(view)
    rows = []
    tick = [0]

    def emit(kind, phase, name, line, args, conds, stmt, tok=None):
        rows.append(dict(kind=kind, phase=phase, name=name, line=line, args=args,
                         conds=[norm_cond(c) for c in conds], stmt=stmt, step='', tok=tok))

    def rec(node, phase, conds):
        if node is None:
            return
        if isinstance(node, C.Block):
            for s in node.stmts:
                rec(s, phase, conds)
        elif isinstance(node, C.If):
            c = C.text_of(node.cond)
            emit('if', phase, 'if', node.line, c, conds, node)
            for x in C.calls_in(node.cond):
                emit('call', phase, x.name, x.line, x.args, conds, node, x.tok)
            rec(node.then, phase, conds + (c,))
            if node.els is not None:
                rec(node.els, phase, conds + ('!(' + c + ')',))
        elif isinstance(node, C.Loop):
            h = C.text_of(node.head)
            for x in C.calls_in(node.head):
                emit('call', phase, x.name, x.line, x.args, conds, node, x.tok)
            rec(node.body, phase, conds + ((node.word + ':' + h) if node.word != 'do' else 'do',))
        else:
            if node.kind == 'goto':
                emit('goto', phase, 'goto', node.line, node.label, conds, node)
            for x in C.calls_in(node.toks):
                emit('call', phase, x.name, x.line, x.args, conds, node, x.tok)
    for s in se:
        rec(s, 'session_end', ())
    rec(inp, 'inproc_try_return', ())
    for s in pe:
        rec(s, 'process_end', ())
    # step ids
    last = 'D0'
    for r in rows:
        st = ''
        n, ph = r['name'], r['phase']
        if ph == 'session_end':
            if any(re.search(r'\bg_perf_rig\b', c) for c in r['conds'][:1]) and last == 'D0':
                st = 'D0'
            elif n == 'ui_return_tick':
                st = UI_TICK[min(tick[0], 2)]
                tick[0] += 1
            elif n == 'fe_evt' and evt_word(r['args']) == 'evt_drop':
                st = 'D17'
            elif n == 'snprintf' and 'extra' in r['args']:
                st = 'D17'
            else:
                st = SE_STEPS.get(n, '')
            if r['kind'] == 'if' and 'g_relaunch' in r['args'] and 'inproc_refusal' in r['args']:
                st = 'D10'
            elif r['kind'] == 'if' and 'me_host_loaded' in r['args']:
                st = 'D11'
            elif r['kind'] == 'if' and re.search(r'g_relaunch==GL', norm_cond(r['args'])):
                st = 'D5'
            elif r['kind'] == 'if' and 'g_perf_rig' in r['args'] and last == 'D0':
                st = 'D0'
        elif ph == 'inproc_try_return':
            if n == 'fe_evt':
                st = IN_EVT.get(evt_word(r['args']), '')
            elif n == 'free':
                st = 'U3'
            else:
                st = IN_STEPS.get(n, '')
            if n == 'inproc_refusal':
                st = 'I1'
            if r['kind'] == 'goto':
                st = 'I4'
            if r['kind'] == 'if':
                a = r['args']
                if re.search(r'g_relaunch==GL', norm_cond(a)):
                    st = 'I1'
                elif 'g_cable_reserve' in a:
                    st = 'U3'
                elif 'dynarec_heap_tables_release' in a:
                    st = 'U2'
                elif 'heap_snap_same' in a or 'fe_ini_get_int' in a:
                    st = 'I3'
                elif 'g_ss_browser' in a:
                    st = 'I4'
        else:
            if n == 'fe_evt':
                st = PE_EVT.get(evt_word(r['args']), '')
            elif n == 'pwr_release_all':
                st = 'P13' if last in ('P12', 'P13') or r['args'] == '' and _next_name(rows, r) == 'sceKernelExitGame' else 'P10'
            else:
                st = PE_STEPS.get(n, '')
            if r['kind'] == 'if':
                a = norm_cond(r['args'])
                if 'g_relaunch!=GL' in a:
                    st = 'P3'
                elif a.strip() == 'exit_reason':
                    st = 'P8'
                elif a.strip() == 'g_relaunch':
                    st = 'P12'
        r['mapped'] = bool(st)
        if not st:
            st = last
        r['step'] = st
        last = st
    for r in rows:
        r['tags'] = tags_of(r['conds'] + ([r['args']] if r['kind'] == 'if' else []))
    return rows, loop


def _next_name(rows, r):
    k = rows.index(r)
    for x in rows[k + 1:]:
        if x['kind'] == 'call':
            return x['name']
    return ''


# ------------------------------------------------------------- the D interpreter
SCENARIOS = [
    # name, g_relaunch, refusal call #1, refusal call #2, census_ok, force, exit_reason
    ('XMB', 0, 0, 0, 1, 0, 0),
    ('XMB_REASON', 0, 0, 0, 1, 0, 1),
    ('PROFILE', 1, 0, 0, 1, 0, 0),
    ('LIST_OK', 2, 0, 0, 1, 0, 0),
    ('LIST_REFUSED', 2, 1, 1, 1, 0, 0),
    ('LIST_RETAIN_THEN_REFUSE', 2, 0, 1, 1, 0, 0),
    ('LIST_CENSUS', 2, 0, 0, 0, 0, 0),
    ('LIST_FORCE', 2, 0, 0, 0, 1, 0),
]


class Unmapped(Exception):
    pass


class Interp(object):
    def __init__(self, scen):
        (self.sname, rel, r1, r2, census, force, reason) = scen
        self.var = {'g_relaunch': rel, 'exit_reason': reason, 'g_perf_rig': 0,
                    'g_ss_browser': 0, 'inproc_n': 0, 'g_ss_game': 0, 'g_cable_reserve': 0,
                    'dynarec_heap_tables_release': 1, 'g_inproc_me_unload': 0,
                    'have_variant': 0, 'NULL': 0, 'RELAUNCH_GAMELIST': 2,
                    'RELAUNCH_PROFILE': 1}
        self.refusal = [r1, r2]
        self.retained = 1 if (rel == 2 and not r1) else 0
        self.call = {'me_host_retain': 1, 'me_host_loaded': 1,
                     'me_host_retained': self.retained, 'heap_snap_same': census,
                     'fe_ini_get_int': force, 'lab_active': 0}
        self.events = []
        self.terminal = ''

    def _c(self, name, args=''):
        self.events.append((name, args))
        if name == 'inproc_refusal':
            return self.refusal.pop(0) if self.refusal else 1
        if name not in self.call:
            raise Unmapped('call %s()' % name)
        return self.call[name]

    def _v(self, name):
        if name not in self.var:
            raise Unmapped('variable %s' % name)
        return self.var[name]

    def eval(self, toks):
        py, i, n = [], 0, len(toks)
        while i < n:
            t = toks[i]
            x = t.text
            if t.kind == 'id' and i + 1 < n and toks[i + 1].text == '(':
                e = C.match_close(toks, i + 1)
                py.append('_c(%r, %r)' % (x, C.text_of(toks[i + 2:e])))
                i = e + 1
                continue
            if t.kind == 'id':
                py.append('_v(%r)' % x)
            elif t.kind == 'num':
                py.append(str(int(re.sub(r'[uUlL]+$', '', x), 0)))
            elif t.kind == 'str':
                py.append('1')
            elif x == '||':
                py.append(' or ')
            elif x == '&&':
                py.append(' and ')
            elif x == '!':
                py.append(' not ')
            elif x in ('==', '!=', '<', '>', '<=', '>=', '(', ')', '+', '-'):
                py.append(x)
            elif x == '&':
                pass
            else:
                raise Unmapped('operator %s at line %d' % (x, t.line))
            i += 1
        return eval(''.join(py), {'_c': self._c, '_v': self._v})

    def run(self, stmts):
        try:
            for s in stmts:
                self.exec_(s)
        except StopIteration:
            pass

    def exec_(self, node):
        if node is None:
            return
        if isinstance(node, C.Block):
            for s in node.stmts:
                self.exec_(s)
        elif isinstance(node, C.If):
            if self.eval(node.cond):
                self.exec_(node.then)
            else:
                self.exec_(node.els)
        elif isinstance(node, C.Loop):
            # loops in the teardown are bounded helpers (snprintf over gate pcs) or
            # do{}while(0): record the body once, never evaluate the head
            self.exec_(node.body)
        else:
            if node.kind == 'goto':
                self.terminal = 'goto:' + (node.label or '')
                self.events.append((self.terminal, ''))
                raise StopIteration
            if node.kind == 'return':
                self.terminal = 'return'
                raise StopIteration
            toks = node.toks
            txt = C.text_of(toks)
            for c in C.calls_in(toks):
                self.events.append((c.name, c.args))
            m = re.match(r'^(?:const\s+char\s*\*\s*)?(why|g_relaunch)\s*=\s*(.*?);?$', txt)
            if m:
                rhs = m.group(2)
                if m.group(1) == 'why':
                    if rhs.startswith('inproc_refusal'):
                        # the call was recorded above; consume its scenario value
                        self.var['why'] = self.refusal.pop(0) if self.refusal else 1
                    else:
                        self.var['why'] = 1 if rhs.startswith('"') else 0
                else:
                    self.var['g_relaunch'] = int(re.sub(r'[^0-9]', '', rhs) or 0) \
                        if re.match(r'^\d+$', rhs) else {'RELAUNCH_GAMELIST': 2}.get(rhs, 0)


def model_seq(events):
    """Tracked calls in the vocabulary test_exit_model.c records."""
    out = []
    for name, args in events:
        if name == 'lab_active':
            continue
        if name == 'lab_finish':
            name = 'handoff_run'
        if name.startswith('goto:'):
            out.append(name)
            continue
        if name not in TRACK_D:
            continue
        if name == 'fe_evt':
            name = 'fe_evt:' + evt_word(args)
        if name in ('psp_rom_pool_over', 'PSP_REWIND_GAME_OVER'):
            name = 'REWIND_GAME_OVER'
        if out and out[-1] == name == 'handoff_run':
            continue
        out.append(name)
    return out


def scenarios(view):
    se, inp, pe, _ = d_region(view)
    res = []
    for sc in SCENARIOS:
        it = Interp(sc)
        err = ''
        try:
            it.run(list(se) + [inp] + list(pe))
        except Unmapped as e:
            err = str(e)
        res.append(dict(name=sc[0], seq=model_seq(it.events), terminal=it.terminal or 'exit',
                        err=err))
    return res


# ------------------------------------------------------------- raw vs -E
def raw_vs_e(raw, ev, dm):
    """Differences between the raw and the -E call lists of the exit code (sites + D)."""
    def callset(view):
        out = {}
        for s in sites(view):
            if s['func'] != 'main':
                continue
            for c in s['calls']:
                out.setdefault((s['key'], c.name, c.line), c)
        rows, _ = step_rows(view)
        for r in rows:
            if r['kind'] == 'call':
                out.setdefault(('D', r['name'], r['line']), r)
        return out
    a, b = callset(raw), callset(ev)

    def in_dm(name):
        """a macro of this -E view: the name itself, or the FE_* macro the raw view's
        alias read AS this name (P3: the hijack is FE_EXIT_GAME/FE_HANDOFF in -dM)."""
        return name in dm or any(k in dm for k, v in EXIT_ALIAS.items() if v == name)

    def dm_text(name):
        k = name if name in dm else next((k for k, v in EXIT_ALIAS.items() if v == name and k in dm), name)
        return dm.get(k)
    raw_by_line = {}
    for t in raw.toks:
        raw_by_line.setdefault(t.line, []).append(t)
    res = []
    for k in sorted(set(a) | set(b), key=lambda k: (k[2], k[1])):
        if k in a and k in b:
            continue
        sid, name, line = k
        if k in a:
            side = 'raw_only'
            c = a[k]
            tok = c['tok'] if isinstance(c, dict) else c.tok
            stmt = c['stmt'] if isinstance(c, dict) else c.stmt
            ifs = tok.ifs if tok is not None else ()
            # the macro invocation this call sits in (an argument of fe_evt, LAB_FFP(...))
            outer = [t.text for t in _stmt_toks(stmt) + raw_by_line.get(line, [])
                     if t.kind == 'id' and in_dm(t.text) and t.text != name]
            if in_dm(name):
                why = 'macro'
                detail = '%s -> %s' % (name, (dm_text(name) or '(empty)')[:60])
            elif ifs and line not in ev.lines:
                why = '#if'
                detail = ifs[-1][:70]
            elif outer:
                why = 'macro'
                detail = 'argument of %s -> %s' % (outer[0], (dm_text(outer[0]) or '(empty)')[:50])
            elif ifs:
                why = '#if'
                detail = ifs[-1][:70]
            else:
                why = 'UNEXPLAINED'
                detail = ''
            if why == 'macro' and ifs and line not in ev.lines:
                why, detail = '#if', ifs[-1][:70]
        else:
            side = 'E_only'
            mac = [t.text for t in raw_by_line.get(line, []) if t.kind == 'id' and in_dm(t.text)]
            if mac:
                why = 'macro'
                detail = 'expanded from ' + ','.join(sorted(set(mac)))[:60]
            else:
                why = 'UNEXPLAINED'
                detail = ''
        res.append(dict(site=sid, name=name, line=line, side=side, cls=why, detail=detail))
    return res


def _stmt_toks(stmt):
    if stmt is None:
        return []
    if isinstance(stmt, C.If):
        return list(stmt.cond)
    if isinstance(stmt, C.Loop):
        return list(stmt.head)
    return list(getattr(stmt, 'toks', []))


# ------------------------------------------------------------- output
def write_view(view, outdir, flavour, dm=None, raw=None):
    os.makedirs(outdir, exist_ok=True)
    st = sites(view)
    rows, loop = step_rows(view)
    sc = scenarios(view)
    with open(os.path.join(outdir, 'exit_sites.tsv'), 'w', newline='\n') as f:
        f.write('flavour\tid\tfunc\tfirst\tline\tcode\twhy\torder\tflags\tseq\tcalls\tkey\tcallnames\n')
        for s in st:
            f.write('\t'.join([flavour, s['id'], s['func'], str(s['first']), str(s['line']),
                               s['code'], s['why'], s['order'], ','.join(s['flags']),
                               ' '.join(s['seq']),
                               ' '.join('%s@%d' % (c.name, c.line) for c in s['calls']),
                               s['key'], ' '.join(c.name for c in s['calls'])]) + '\n')
    with open(os.path.join(outdir, 'exit_steps.tsv'), 'w', newline='\n') as f:
        f.write('flavour\tn\tphase\tstep\tmapped\tline\tkind\tname\targs\ttags\tconds\n')
        for k, r in enumerate(rows):
            f.write('\t'.join([flavour, str(k), r['phase'], r['step'], 'y' if r['mapped'] else '~',
                               str(r['line']), r['kind'], r['name'], r['args'][:70].replace('\t', ' '),
                               ','.join(r['tags']), ' && '.join(r['conds'])[:160]]) + '\n')
    with open(os.path.join(outdir, 'exit_scenarios.tsv'), 'w', newline='\n') as f:
        f.write('flavour\tscenario\tterminal\terror\tseq\n')
        for s in sc:
            f.write('\t'.join([flavour, s['name'], s['terminal'], s['err'], ' '.join(s['seq'])]) + '\n')
    write_expect_h(os.path.join(outdir, 'exit_expect.h'), flavour, st, sc)
    rv = None
    if raw is not None and dm is not None:
        rv = raw_vs_e(raw, view, dm)
        with open(os.path.join(outdir, 'raw_vs_e.tsv'), 'w', newline='\n') as f:
            f.write('site\tline\tname\tside\tclass\tdetail\n')
            for r in rv:
                f.write('\t'.join([r['site'], str(r['line']), r['name'], r['side'], r['cls'],
                                   r['detail']]) + '\n')
    return st, rows, sc, rv, loop


def write_expect_h(path, flavour, st, sc):
    with open(path, 'w', newline='\n') as f:
        f.write('/* generated by exit_matrix.py from the %s flavour; do not edit */\n' % flavour)
        f.write('#define EXPECT_FLAVOUR "%s"\n' % flavour)
        f.write('static const struct exp_site { const char *id; unsigned flags; '
                'const char *seq; } EXP_SITES[] = {\n')
        for s in st:
            if s['func'] != 'main':
                continue
            fl = ' | '.join([s['order']] + s['flags']) or '0'
            f.write('   { "%s", %s, "%s" },\n' % (s['id'], fl, ' '.join(s['seq'])))
        f.write('};\n')
        f.write('static const struct exp_scen { const char *name; const char *seq; } '
                'EXP_SCEN[] = {\n')
        for s in sc:
            f.write('   { "%s", "%s" },\n' % (s['name'], ' '.join(s['seq'])))
        f.write('};\n')


# ------------------------------------------------------------- checks
def check_x2(st, label, raw_st=None):
    """raw_st: the raw view's sites.  A site absent from a -E view is accepted only when
    the raw site's exit call sits inside an #if (compiled out); any other absence fails."""
    out, bad = [], 0
    by = {s['id']: s for s in st}
    rby = {s['id']: s for s in (raw_st or [])}
    for sid, _anchor, want in X2:
        s = by.get(sid)
        if s is None:
            r = rby.get(sid)
            term = [c for c in r['calls'] if c.name in EXIT_NAMES][-1] if r else None
            if label != 'raw' and term is not None and term.tok is not None and term.tok.ifs:
                out.append('  %-4s %-8s absent (compiled out: %s)' % (sid, label, term.tok.ifs[-1][:50]))
            else:
                bad += 1
                out.append('  %-4s %-8s ABSENT' % (sid, label))
            continue
        got = x2_norm(s['seq'])
        w = want.split()
        if label == 'release':      # fe_evt compiles out of release
            w = [t for t in w if t != 'EVT']
        ok = got == w
        dropped = collapse([t for t in s['seq'] if t != 'PRA']) != got
        bad += not ok
        out.append('  %-4s %-8s %s  %s%s' % (sid, label, 'ok  ' if ok else 'DIFF', ' '.join(got),
                                            '' if ok else '   (x2: %s)' % ' '.join(w))
                   + ('   [leading diagnostic EVT not in x2]' if dropped and ok else ''))
    return bad, out


def check_order(rows):
    bad, out, prev = 0, [], None
    for r in rows:
        k = STEP_RANK[r['step']]
        if prev is not None and k < STEP_RANK[prev['step']]:
            bad += 1
            out.append('  ORDER %s:%s %s@%d after %s:%s@%d' % (
                r['phase'], r['step'], r['name'], r['line'], prev['step'], prev['name'], prev['line']))
        prev = r if r['mapped'] else prev
    return bad, out


def release_live(rows):
    need = {
        'gpsp_rtc_peek': lambda r: r['kind'] == 'call' and r['name'] == 'gpsp_rtc_peek',
        'heap_trace_report("post_unload")': lambda r: r['kind'] == 'call' and
        r['name'] == 'heap_trace_report' and 'post_unload' in r['args'],
        'inproc_force ini read': lambda r: r['kind'] == 'call' and r['name'] == 'fe_ini_get_int'
        and 'inproc_force' in r['args'],
        'audio_hash guard (g_relaunch!=GL)': lambda r: r['kind'] == 'if' and r['phase'] ==
        'process_end' and 'g_relaunch!=GL' in norm_cond(r['args']),
    }
    res = []
    for k, f in need.items():
        hit = [r for r in rows if f(r)]
        rig = bool(hit) and any(re.search(r'\bg_perf_rig\b', c) for c in hit[0]['conds'])
        res.append((k, hit[0]['line'] if hit else None, hit[0]['step'] if hit else None, rig))
    return res


def run_all(baseline, src, outdir, pp_dir=None, quiet=False, main_c=None):
    import aec_pspcc as P
    os.makedirs(outdir, exist_ok=True)
    rawpath = main_c or os.path.join(src, 'psp', 'main_psp.c')
    raw = View('raw', C.load_raw(rawpath, follow=True, alias=EXIT_ALIAS))
    report, fails = [], 0
    st_raw, rows_raw, _, _, _ = write_view(raw, os.path.join(outdir, 'raw'), 'raw')
    b, o = check_x2(st_raw, 'raw')
    fails += b
    report += ['x2 reproduction (raw source, the view x2 read):'] + o
    per = {}
    for fl in FLAVOURS:
        ppd = pp_dir or os.path.join(outdir, 'pp')
        os.makedirs(ppd, exist_ok=True)
        ip, dp = os.path.join(ppd, 'main_psp.%s.i' % fl), os.path.join(ppd, 'main_psp.%s.dM' % fl)
        if not os.path.exists(ip):
            with open(ip, 'w', newline='\n') as f:
                f.write(P.preprocess(src, os.path.join(baseline, fl), override=main_c))
        if not os.path.exists(dp):
            with open(dp, 'w', newline='\n') as f:
                f.write(P.macros(src, os.path.join(baseline, fl), override=main_c))
        dm = P.parse_dm(open(dp).read())
        v = View(fl, C.load_pp(ip))
        v.line_ids = site_ids(st_raw)
        per[fl] = write_view(v, os.path.join(outdir, fl), fl, dm, raw)
    for fl in FLAVOURS:
        st, rows, sc, rv, loop = per[fl]
        b, o = check_x2(st, fl, st_raw)
        fails += b
        report += ['', 'x2 reproduction (%s -E):' % fl] + o
        b, o = check_order(rows)
        fails += b
        nm = sum(1 for r in rows if r['kind'] == 'call' and not r['mapped'])
        report += ['', '%s: teardown rows %d (calls %d, guards %d, unmapped helper calls %d), '
                   'loop %d-%d; PLAN §3.2 order: %s' % (
                       fl, len(rows), sum(r['kind'] == 'call' for r in rows),
                       sum(r['kind'] == 'if' for r in rows), nm, C.node_span(loop)[0],
                       C.node_span(loop)[1], 'ok' if not b else '%d violations' % b)] + o
        for s in sc:
            if s['err']:
                fails += 1
                report.append('  SCENARIO %s: %s' % (s['name'], s['err']))
        report.append('  scenarios: ' + ', '.join('%s->%s(%d)' % (s['name'], s['terminal'], len(s['seq']))
                                                   for s in sc))
        cls = {}
        for r in rv:
            cls[r['cls']] = cls.get(r['cls'], 0) + 1
        report.append('  raw vs -E: %d differing calls: %s' % (
            len(rv), ', '.join('%s %d' % kv for kv in sorted(cls.items())) or 'none'))
        un = [r for r in rv if r['cls'] == 'UNEXPLAINED']
        fails += len(un)
        for r in un:
            report.append('    UNEXPLAINED %s %s@%d (%s)' % (r['site'], r['name'], r['line'], r['side']))
    rows = per['release'][1]
    report += ['', 'release-live rows (release -E):']
    for k, line, step, rig in release_live(rows):
        report.append('  %-38s %s%s' % (k, ('line %d, step %s' % (line, step)) if line else 'MISSING',
                                        '  RIG-GUARDED (behind g_perf_rig)' if rig else ''))
        fails += (line is None) + bool(rig)
    report += ['', 'sites per view: raw %d, %s' % (
        len(st_raw), ', '.join('%s %d' % (fl, len(per[fl][0])) for fl in FLAVOURS))]
    txt = '\n'.join(report) + '\n' + ('RESULT: ok\n' if not fails else 'RESULT: %d failures\n' % fails)
    with open(os.path.join(outdir, 'report.txt'), 'w', newline='\n') as f:
        f.write(txt)
    if not quiet:
        sys.stdout.write(txt)
    return 1 if fails else 0


# ------------------------------------------------------------- XS gate
def load_tsv(path):
    with open(path) as f:
        hdr = f.readline().rstrip('\n').split('\t')
        return [dict(zip(hdr, l.rstrip('\n').split('\t'))) for l in f if l.strip()]


def check_gate(golden, cand, allow=None):
    allowed = set()
    if allow and os.path.exists(allow):
        for l in open(allow):
            p = l.split('\t')
            if len(p) >= 3 and not l.startswith('#'):
                allowed.add((p[0].strip(), p[1].strip()))
    fails = 0

    def site_val(r):
        # the ordered teardown vocabulary, the exit code/why handed to the Lab, and the
        # full ordered call-name list (no lines: those move in every phase-P edit)
        return 'seq=%s | code=%s | why=%s | calls=%s' % (
            r.get('seq', ''), r.get('code', ''), r.get('why', ''), r.get('callnames', '?'))

    def table(path, key, val):
        if not os.path.exists(path):
            return {}
        rows = load_tsv(path)
        ks = [(r.get(key) or r.get('id')) for r in rows]
        dup = sorted(set(k for k in ks if ks.count(k) > 1))
        if dup:
            raise SystemExit('exit_matrix check: %s has duplicate keys %s' % (path, dup))
        return {k: val(r) for k, r in zip(ks, rows)}
    for fl in FLAVOURS:
        for fname, key, val in (('exit_sites.tsv', 'key', site_val),
                                ('exit_scenarios.tsv', 'scenario', lambda r: r['seq'])):
            gp, cp = os.path.join(golden, fl, fname), os.path.join(cand, fl, fname)
            if not os.path.exists(gp):
                fails += 1
                print('  XS FAIL %s %s: golden table missing' % (fl, fname))
                continue
            g = table(gp, key, val)
            c = table(cp, key, val)
            for k in sorted(set(g) | set(c)):
                if g.get(k) == c.get(k):
                    continue
                if (fl, k) in allowed:
                    print('  allowed %s %s' % (fl, k))
                    continue
                fails += 1
                print('  XS FAIL %s %s %s\n     golden: %s\n     cand:   %s' % (
                    fl, fname, k, g.get(k, '(absent)'), c.get(k, '(absent)')))
    print('XS: %s' % ('ok' if not fails else '%d differences' % fails))
    return 1 if fails else 0


# ------------------------------------------------------------- selftest
SELF_SRC = r'''
static void helper(void) { foo(); }
int main(int argc, char *argv[])
{
   int x = 0;
top:
   if (bad) {
      fe_evt("exit code=2");
      evt_shutdown();
      audio_stop();
      vid_term();
      handoff_run(2, "bad");
      sceKernelExitGame();
      return 0;
   }
#ifdef RIG
   if (probe) { audio_stop(); vid_term(); fe_evt("exit"); evt_shutdown(); handoff_run(0, NULL); sceKernelExitGame(); return 0; }
#endif
   while (g_running) { step(); }
   ui_set_wake_yield(NULL);
   sleep_quiesce_up("loop_exit");
   play_lock_drop("loop_exit");
   pwr_hold("teardown");
   if (g_relaunch != RELAUNCH_GAMELIST || inproc_refusal(v) || !me_host_retain(5))
      me_host_shutdown();
   ui_return_tick();
   io_thread_stop();
   ui_return_tick();
   fe_host_shutdown();
   ui_return_tick();
   if (g_relaunch == RELAUNCH_GAMELIST)
   {
      const char *why = inproc_refusal(v);
      if (!why) { audio_stop(); audio_start(); pwr_release(); goto top; }
      g_relaunch = RELAUNCH_GAMELIST;
      me_host_shutdown();
   }
   audio_stop();
   vid_term();
   evt_shutdown();
   handoff_run(exit_code, exit_reason);
   if (g_relaunch)
      relaunch_eboot(g_relaunch == RELAUNCH_GAMELIST);
   sceKernelExitGame();
   return 0;
}
'''


def selftest():
    import tempfile
    fails = 0
    d = tempfile.mkdtemp(prefix='exitm')
    p = os.path.join(d, 'm.c')
    open(p, 'w').write(SELF_SRC)
    v = View('raw', C.load_raw(p))
    st = sites(v)

    def expect(cond, msg):
        nonlocal fails
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        fails += not cond
    expect(len(st) == 2, 'two early sites found (%d)' % len(st))
    expect(st[0]['seq'] == 'EVT ES AUD VID HO EXIT'.split() and st[0]['order'] == 'ORDER_EVT_FIRST',
           'site 1 EVT-first: %s' % ' '.join(st[0]['seq']))
    expect(st[1]['order'] == 'ORDER_AV_FIRST' and st[1]['calls'][0].tok.ifs == ('#ifdef RIG',),
           'site 2 AV-first and inside #ifdef RIG')
    expect(st[0]['code'] == '2' and st[0]['why'] == '"bad"', 'code/why parsed: %s %s' % (st[0]['code'], st[0]['why']))
    rows, _ = step_rows(v)
    b, o = check_order(rows)
    expect(b == 0, 'teardown step order monotone')
    sc = {s['name']: s for s in scenarios(v)}
    expect(sc['LIST_OK']['terminal'] == 'goto:top', 'LIST_OK returns in-process')
    expect('me_host_shutdown' not in sc['LIST_OK']['seq'], 'LIST_OK never shuts the ME down')
    expect(sc['XMB']['seq'][-1] == 'sceKernelExitGame' and 'relaunch_eboot' not in sc['XMB']['seq'],
           'XMB ends in ExitGame without relaunch')
    expect('inproc_refusal' not in sc['XMB']['seq'], 'short-circuit: XMB never asks inproc_refusal')
    expect(sc['LIST_REFUSED']['seq'].count('me_host_shutdown') == 2 and
           'relaunch_eboot' in sc['LIST_REFUSED']['seq'], 'LIST_REFUSED: shutdown at D10 and I5, relaunches')
    expect(sc['LIST_RETAIN_THEN_REFUSE']['seq'].count('me_host_shutdown') == 1,
           'retain then refuse: one shutdown (I5)')
    # mutant: swap D3/D4 -> order violation
    p2 = os.path.join(d, 'm2.c')
    open(p2, 'w').write(SELF_SRC.replace('   play_lock_drop("loop_exit");\n   pwr_hold("teardown");',
                                         '   pwr_hold("teardown");\n   play_lock_drop("loop_exit");'))
    b, o = check_order(step_rows(View('raw', C.load_raw(p2)))[0])
    expect(b == 1, 'mutant D3<->D4 swap caught by the PLAN 3.2 order check')
    # mutant: unknown runtime condition -> scenario error
    p3 = os.path.join(d, 'm3.c')
    open(p3, 'w').write(SELF_SRC.replace('if (g_relaunch)\n', 'if (g_new_flag)\n'))
    sc3 = scenarios(View('raw', C.load_raw(p3)))
    expect(all('g_new_flag' in s['err'] for s in sc3 if not s['terminal'].startswith('goto')) and
           sum(bool(s['err']) for s in sc3) == 5,
           'an unmapped runtime atom fails every scenario that reaches it (5 of 8)')
    print('selftest: %s' % ('ok' if not fails else '%d failures' % fails))
    return 1 if fails else 0


def main():
    if '--selftest' in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    a = sub.add_parser('all')
    a.add_argument('--baseline', required=True)
    a.add_argument('--src', required=True)
    a.add_argument('--out', required=True)
    a.add_argument('--pp-dir')
    a.add_argument('--main-c', help='a mutated copy of psp/main_psp.c, mounted over the tree')
    g = sub.add_parser('gen')
    g.add_argument('--flavour', required=True)
    g.add_argument('--pp', required=True)
    g.add_argument('--dm')
    g.add_argument('--raw')
    g.add_argument('--out', required=True)
    c = sub.add_parser('check')
    c.add_argument('--golden', required=True)
    c.add_argument('--cand', required=True)
    c.add_argument('--allow')
    o = ap.parse_args()
    if o.cmd == 'all':
        return run_all(o.baseline, o.src, o.out, o.pp_dir, main_c=o.main_c)
    if o.cmd == 'gen':
        import aec_pspcc as P
        dm = P.parse_dm(open(o.dm).read()) if o.dm else None
        raw = View('raw', C.load_raw(o.raw, follow=True, alias=EXIT_ALIAS)) if o.raw else None
        v = View(o.flavour, C.load_pp(o.pp))
        if raw is not None:
            v.line_ids = site_ids(sites(raw))
        write_view(v, os.path.join(o.out, o.flavour), o.flavour, dm, raw)
        return 0
    if o.cmd == 'check':
        return check_gate(o.golden, o.cand, o.allow)
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
