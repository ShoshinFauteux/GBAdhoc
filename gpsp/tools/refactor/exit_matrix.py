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
  - hold balance (Epoch C C0a-5b): main's pwr_hold/pwr_release ledger is the same on
    every branch that meets again, every `goto L` arrives with the depth L was first
    reached with, the frame loop is entered holding nothing and each pass is balanced,
    and every other function that takes a hold gives it back on every return.
`check --golden DIR --cand DIR [--allow FILE]` is the XS gate itself: per site and
flavour, the ordered vocabulary of the candidate equals the golden, or the allow-list
names that (flavour, site) with a fix commit.  Since Epoch C C0a-5 it also compares, per
site and per scenario, the TEXT of every call that passes a string literal (the fe_evt
format strings and their arguments, the handoff reasons, the notices: what the Lab's
parsers and the player read) and the power-hold state (which holds a site exits with,
the ledger of a scenario).

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
    # HW-1 H3: C1 / C2 show their notice through boot_stop_notice (fe/power.b:
    # ui_notice with the boot's hold dropped while it waits)
    'boot_stop_notice': 'NOTICE',
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
# HW-1 F2 (claude/hw1-sleep): the teardown's pwr_hold("teardown") (D4) now comes BEFORE
# play_lock_drop("loop_exit") (D3), PLAN HW-1 "after the swap the order is L:18097 ->
# L:18098 -> L:18100 -> L:18099"; the step names keep PLAN §3.2's numbering.
STEP_ORDER = ('D0 D1 D2 D4 D3 D5 D6 D7 D8 D9 D10 D11 D12 D13 D13b D14 D15 D16 D16b D17 D18 '
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


def call_text(name, args):
    """`name(args)` for a call that passes a string literal, else None.  This is the text
    the gate compares (C0a-5a): log lines, reasons and notices are strings."""
    if '"' not in (args or ''):
        return None
    return '%s(%s)' % (name, re.sub(r'\s+', ' ', args).strip())


TEXT_SEP = ' ;; '
_LITS = re.compile(r'^\s*((?:"(?:\\.|[^"\\])*"\s*)+)')


def evt_fmt(args):
    """The format literal(s) of an fe_evt call as C source (adjacent literals kept as
    written), or '"?"' when the first argument is not a literal."""
    m = _LITS.match(args or '')
    return m.group(1).strip() if m else '"?"'


def collapse(seq):
    out = []
    for x in seq:
        if not out or out[-1] != x:
            out.append(x)
    return out


# ------------------------------------------------------------- parse a view
# Epoch C's exit helpers (fe/teardown.inc.c, PLAN-EPOCHC-beta5 §3 C1).  Every view reads
# a call statement of one of them AS ITS BODY, its flags folded (aec_cparse.inline_helpers),
# so a site that calls them has the calls, order, texts and hold of the copy it replaced,
# and the gate compares them with the golden unchanged.  They are never sites themselves.
EPOCHC_HELPERS = ('exit_early_begin', 'exit_early_end', 'session_end', 'process_end')


class View(object):
    def __init__(self, name, toks):
        toks = C.inline_helpers(toks, EPOCHC_HELPERS)
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
        if fn == 'main' or fn in EPOCHC_HELPERS:
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
    text = [x for x in (call_text(c.name, c.args) for c in calls) if x]
    return dict(id=sid, func=fn, first=a, line=term.line, code=code, why=why,
                seq=seq, calls=calls, order=order_of(seq), flags=flags_of(seq), text=text,
                hold='?')


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
    if 'ME' in seq:
        f.append('TD_ME')                   # HW-2 F9: C3/C4 stop the ME first
    if 'VID' not in seq:
        f.append('PE_NO_VIDTERM')
    if 'RL' in seq:
        f.append('PE_RELAUNCH')
    if 'HO' not in seq:
        f.append('PE_NO_HANDOFF')
    if seq.count('HO') > 1:
        f.append('PE_HO_BRANCHES')          # HW-2 F7: A5 hands off 0 or 5 from two branches
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
    # name, g_relaunch, refusal call #1, refusal call #2, census_ok, force, exit_reason,
    # g_exit_req (HW-2 F1: the sticky HOME latch)
    ('XMB', 0, 0, 0, 1, 0, 0, 0),
    ('XMB_REASON', 0, 0, 0, 1, 0, 1, 0),
    ('PROFILE', 1, 0, 0, 1, 0, 0, 0),
    ('LIST_OK', 2, 0, 0, 1, 0, 0, 0),
    ('LIST_REFUSED', 2, 1, 1, 1, 0, 0, 0),
    ('LIST_RETAIN_THEN_REFUSE', 2, 0, 1, 1, 0, 0, 0),
    ('LIST_CENSUS', 2, 0, 0, 0, 0, 0, 0),
    ('LIST_FORCE', 2, 0, 0, 0, 1, 0, 0),
]
# HW-2 F1 (Epoch C C0a-5c): HOME during a quit.  These exist only on a tree whose main()
# reads g_exit_req.  inproc_refusal() returns "home" once the latch is set, so "HOME
# before call #N" is "call #N refuses" with the latch up; process_end then drops the
# relaunch.  HOME inside U1-U13 lets the return finish and re-enters the list with
# g_running 0 (the list exits as "home": site B9).
# HW-2 F5: a link session that could not put the game back sets g_link_fatal_why with
# g_running = 0; the loop's end turns it into the exit code and reason.  Exists only on
# a tree whose main() reads g_link_fatal_why.  (9th field: that atom.)
SCENARIOS_F5 = [
    ('LINK_FATAL', 0, 0, 0, 1, 0, 0, 0, 1),
]
SCENARIOS_F1 = [
    ('HOME_BEFORE_TEARDOWN', 2, 1, 1, 1, 0, 0, 1),     # refused at D10 and I1, no relaunch
    ('HOME_BEFORE_REFUSAL2', 2, 0, 1, 1, 0, 0, 1),     # ME retained, refused at I1, no relaunch
    ('HOME_IN_UNLOAD', 2, 0, 0, 1, 0, 0, 1),           # returns in-process with g_running 0
    ('HOME_PROFILE', 1, 0, 0, 1, 0, 0, 1),             # a profile relaunch becomes an exit
]


def main_reads(view, name):
    lo, hi = view.funcs['main']
    return any(t.kind == 'id' and t.text == name for t in view.toks[lo:hi])


def scenario_list(view):
    return (SCENARIOS + (SCENARIOS_F5 if main_reads(view, 'g_link_fatal_why') else [])
            + (SCENARIOS_F1 if main_reads(view, 'g_exit_req') else []))


class Unmapped(Exception):
    pass


class Interp(object):
    def __init__(self, scen):
        (self.sname, rel, r1, r2, census, force, reason, xreq) = scen[:8]
        fatal = scen[8] if len(scen) > 8 else 0
        self.var = {'g_relaunch': rel, 'exit_reason': reason, 'g_perf_rig': 0,
                    'g_exit_req': xreq, 'g_running': 0,
                    'g_link_fatal_why': fatal, 'g_link_fatal_code': 4 if fatal else 0,
                    'exit_code': 0,
                    'g_ss_browser': 0, 'inproc_n': 0, 'g_ss_game': 0, 'g_cable_reserve': 0,
                    'dynarec_heap_tables_release': 1, 'g_inproc_me_unload': 0,
                    'have_variant': 0, 'NULL': 0, 'RELAUNCH_GAMELIST': 2,
                    'RELAUNCH_PROFILE': 1,
                    # HW-1 H1 r5: the ME-lost wake's hold, handed to the teardown's right after
                    # pwr_hold("teardown"); none of these quits is that wake (test_sleep_holds.py
                    # runs the hand-over against the lock count)
                    'g_wake_lost_hold': 0}
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
                # what the next session starts with: g_running as the reset left it (1;
                # HW-2 F1: 0 when HOME was pressed during the return)
                self.events.append(('g_running=%d' % self.var['g_running'], ''))
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
            m = re.match(r'^(?:const\s+char\s*\*\s*)?(why|g_relaunch|g_running|exit_reason|exit_code)'
                         r'\s*=\s*(.*?);?$', txt)
            if m:
                rhs = m.group(2)
                if m.group(1) in ('exit_reason', 'exit_code'):
                    # HW-2 F5: `exit_code = g_link_fatal_code; exit_reason = g_link_fatal_why;`
                    # a string is a reason; a variable gives its scenario value; anything
                    # else (0, NULL, ((void *)0)) clears it
                    self.var[m.group(1)] = 1 if rhs.startswith('"') else \
                        int(self.var.get(rhs, 0)) if re.match(r'^[A-Za-z_]\w*$', rhs) else 0
                elif m.group(1) == 'g_running':
                    # the in-process reset's `g_running = 1` (HW-2 F1: `= !g_exit_req`)
                    self.var['g_running'] = int(bool(self.eval(toks[2:-1] if toks[-1].text == ';'
                                                               else toks[2:])))
                elif m.group(1) == 'why':
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
        if name.startswith(('goto:', 'g_running=')):
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


def scenario_hold(seq, terminal):
    """The pwr_hold ledger of one scenario, from the loop's exit (depth 0, checked by the
    hold walk) to its terminal.  -> ('max=N end=N', [problems])."""
    d = mx = 0
    bad = []
    for k, n in enumerate(seq):
        if n == 'pwr_hold':
            d += 1
            mx = max(mx, d)
        elif n == 'pwr_release':
            d -= 1
            if d < 0:
                bad.append('pwr_release with nothing held (call %d)' % k)
                d = 0
        elif n == 'pwr_release_all':
            d = 0
        elif n in ('handoff_run', 'sceKernelExitGame') and d:
            bad.append('%s reached holding %d' % (n, d))
    if terminal.startswith('goto') and d:
        bad.append('returns to the list holding %d' % d)
    return 'max=%d end=%d' % (mx, d), bad


def scenarios(view):
    se, inp, pe, _ = d_region(view)
    res = []
    for sc in scenario_list(view):
        it = Interp(sc)
        err = ''
        try:
            it.run(list(se) + [inp] + list(pe))
        except Unmapped as e:
            err = str(e)
        seq = model_seq(it.events)
        hold, hbad = scenario_hold(seq, it.terminal or 'exit')
        res.append(dict(name=sc[0], seq=seq, terminal=it.terminal or 'exit', err=err,
                        text=[x for x in (call_text(n, a) for n, a in it.events) if x],
                        evt=[evt_fmt(a) for n, a in it.events if n == 'fe_evt'],
                        hold=hold, hold_bad=hbad))
    return res


# ------------------------------------------------------------- hold balance (C0a-5b)
HOLD_CLEAR = ('pwr_release_all', 'relaunch_eboot') + EXIT_NAMES + HANDOFF_NAMES
# A function that is ALLOWED to return with a hold it did not take or give back, with
# the reason.  Add a row only with the commit that makes it true.
HOLD_KNOWN = {
    # fe/rewind.inc.c: one hold, toggled by state.  It takes "rewind" when g_rw_pwr goes
    # 0 -> 1 and gives it back when it goes 1 -> 0; PSP_REWIND_GAME_OVER() (D19, C3/C4)
    # calls it with the rewind off, and every exit ends in pwr_release_all.
    'psp_rewind_pwr': 'a toggle on g_rw_pwr (hold on 0->1, release on 1->0), by design',
    # fe/power.e.inc.c (HW-1 H1): the ME-lost wake takes "me_lost" and keeps it until the
    # loop's teardown, which takes "teardown" and only then drops it (HOLD_HANDOVER).
    'wake_service': 'the ME-lost wake hands its hold to the teardown (g_wake_lost_hold, HW-1 H1)',
}
# A hold handed from one function to another through a latch: an `if (LATCH)` whose arm
# releases it is depth-neutral in the receiving function (the hold was never on its
# ledger).  {latch: the function that takes it}.
HOLD_HANDOVER = {'g_wake_lost_hold': 'wake_service'}


class HoldWalk(object):
    """Structural ledger of the DIRECT pwr_hold / pwr_release calls of one function body.
    Depth is a tuple of the hold names.  pwr_release_all (and the exit macros, the handoff
    and relaunch_eboot, which run it first) clear it.  Branches that meet again must
    agree; a branch that ends in goto / return / break / continue / an exit does not
    meet.  Loop and switch bodies must be balanced per pass.
    A function other than main may DROP its caller's hold and take it again (HW-1's
    boot_stop_notice: the hold is not kept across a notice that waits on the player):
    a pwr_release with nothing of its own held is noted as LENT, and the next pwr_hold
    gives it back.  It must be back before the function returns; main() has no caller
    and may not do this."""
    LENT = '(caller\'s hold, dropped)'

    def __init__(self, fn):
        self.fn, self.bad = fn, []
        self.lends = 0
        self.labels, self.gotos, self.returns = {}, [], []
        self.term = {}          # line of an EXIT statement -> the holds it exits with
        self.at = {}            # id(node) -> depth on entry (for the frame loop)
        self.pre = None         # depth before the last clear of this straight-line run

    def stmt(self, node, d, brk):
        """-> depth after the statement, or None when control does not fall through."""
        if node is None:
            return d
        self.at[id(node)] = d
        if isinstance(node, C.Block):
            for s in node.stmts:
                if d is None:
                    if isinstance(s, C.Simple) and s.kind == 'label':
                        d = self.labels.get(s.label)
                        if d is None:
                            g = [x for x in self.gotos if x[0] == s.label]
                            d = g[0][2] if g else ()
                    elif isinstance(s, C.Simple) and s.kind == 'case':
                        d = brk[1] if brk else ()
                    else:
                        continue            # unreachable until the next label
                d = self.stmt(s, d, brk)
            return d
        if isinstance(node, C.If):
            if C.text_of(node.cond).strip() in HOLD_HANDOVER and node.els is None and \
                    [c.name for c in C.walk_calls(node.then)] == ['pwr_release']:
                self.handover = getattr(self, 'handover', 0) + 1
                return d                    # the handed-over hold's release: not ours
            d = self.calls(C.calls_in(node.cond), d, node.line)
            a = self.stmt(node.then, d, brk)
            b = self.stmt(node.els, d, brk) if node.els is not None else d
            if a is None:
                return b
            if b is None:
                return a
            if a != b:
                self.bad.append('%s:%s the branches of this `if` meet holding %s and %s' % (
                    self.fn, node.line, list(a), list(b)))
            return a if len(a) >= len(b) else b
        if isinstance(node, C.Loop):
            d = self.calls(C.calls_in(node.head), d, node.line)
            mine = [node.word, d, []]
            e = self.stmt(node.body, d, mine)
            ends = [x for x in mine[2]] + ([(node.line, e)] if e is not None else [])
            for ln, x in ends:
                if x != d:
                    self.bad.append('%s:%s a pass of this `%s` (entered holding %s) ends at '
                                    'line %s holding %s' % (self.fn, node.line, node.word,
                                                            list(d), ln, list(x)))
            return d
        k = node.kind
        if k == 'label':
            if node.label in self.labels and self.labels[node.label] != d:
                self.bad.append('%s:%s label %s reached holding %s and %s' % (
                    self.fn, node.line, node.label, list(self.labels[node.label]), list(d)))
            self.labels.setdefault(node.label, d)
            return d
        if k == 'case':
            return d
        cl = C.calls_in(node.toks)
        is_exit = any(c.name in EXIT_NAMES for c in cl)
        d0 = d
        d = self.calls(cl, d, node.line)
        if is_exit:
            self.term[node.line] = self.pre if self.pre is not None else d0
            self.pre = None
            return None
        if d:
            self.pre = None
        if k == 'goto':
            self.gotos.append((node.label, node.line, d))
            return None
        if k == 'return':
            self.returns.append((node.line, d))
            return None
        if k in ('break', 'continue'):
            if brk is not None:
                brk[2].append((node.line, d))
            return None
        return d

    def calls(self, cl, d, line):
        for c in cl:
            if c.name == 'pwr_hold':
                m = re.match(r'\s*"([^"]*)"', c.args)
                if d and d[-1] == self.LENT:
                    d = d[:-1]              # the caller's hold, taken again
                else:
                    d = d + ((m.group(1) if m else '?'),)
                self.pre = None
            elif c.name == 'pwr_release':
                if not d and self.fn != 'main':
                    d = (self.LENT,)
                    self.lends += 1
                else:
                    if not d or d[-1] == self.LENT:
                        self.bad.append('%s:%s pwr_release with nothing held' % (self.fn, line))
                    d = d[:-1]
                self.pre = None
            elif c.name in HOLD_CLEAR:
                if self.pre is None:
                    self.pre = d
                d = ()
        return d

    def finish(self):
        for lab, line, d in self.gotos:
            want = self.labels.get(lab)
            if want is not None and want != d:
                self.bad.append('%s:%s goto %s holding %s, but %s is reached holding %s' % (
                    self.fn, line, lab, list(d), lab, list(want)))


def hold_check(view):
    """-> (problems, {terminal line: 'boot,..' or '-'}, summary lines)."""
    bad, out = [], []
    w = HoldWalk('main')
    end = w.stmt(view.main, (), None)
    w.finish()
    bad += w.bad
    loop = view.main.stmts[find_loop_index(view.main)]
    le = w.at.get(id(loop))
    if le:
        bad.append('main:%s the frame loop is entered holding %s (it could never park)' % (
            loop.line, list(le)))
    out.append('  main: session label(s) %s; frame loop entered holding %s; %d goto, %d exit '
               'statements' % (', '.join('%s=%s' % (k, list(v)) for k, v in sorted(w.labels.items()))
                               or '-', list(le or ()), len(w.gotos), len(w.term)))
    nfn, lent = 0, []
    for fn, (lo, hi) in sorted(view.funcs.items()):
        if fn == 'main' or fn in ('pwr_hold', 'pwr_release', 'pwr_release_all'):
            continue
        if fn in EPOCHC_HELPERS:
            continue        # walked as part of main(), where every call is its body
        if not any(t.kind == 'id' and t.text in ('pwr_hold', 'pwr_release')
                   for t in view.toks[lo:hi]):
            continue
        nfn += 1
        f = HoldWalk(fn)
        e = f.stmt(C.parse_body(view.toks, lo, hi), (), None)
        f.finish()
        probs = list(f.bad)
        for ln, d in f.returns + ([('end', e)] if e is not None else []):
            if d:
                probs.append('%s:%s returns holding %s' % (fn, ln, list(d)))
        if f.lends and not probs:
            lent.append(fn)
        if fn in HOLD_KNOWN:
            out.append('  KNOWN %s: %s (%d findings)' % (fn, HOLD_KNOWN[fn], len(probs)))
        else:
            bad += probs
    out.append('  %d other functions take or release a hold; each gives back what it takes' % nfn
               if not bad else '  %d other functions take or release a hold' % nfn)
    if lent:
        out.append('  %s: drop%s the caller\'s hold and take%s it again before returning' % (
            ', '.join(lent), 's' if len(lent) == 1 else '', 's' if len(lent) == 1 else ''))
    return bad, {ln: (','.join(d) or '-') for ln, d in w.term.items()}, out


# ------------------------------------------------------------- raw vs -E
def raw_vs_e(raw, ev, dm):
    """Differences between the raw and the -E call lists of the exit code (sites + D)."""
    # A call copied out of an Epoch C helper sits at its call's line; its helper line
    # (Tok.oline) tells the copies apart, here and in raw_by_line.
    def ol(tok):
        return getattr(tok, 'oline', None) if tok is not None else None

    def callset(view):
        out = {}
        for s in sites(view):
            if s['func'] != 'main':
                continue
            for c in s['calls']:
                out.setdefault((s['key'], c.name, c.line, ol(c.tok)), c)
        rows, _ = step_rows(view)
        for r in rows:
            if r['kind'] == 'call':
                out.setdefault(('D', r['name'], r['line'], ol(r['tok'])), r)
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
        raw_by_line.setdefault((t.line, t.oline), []).append(t)
    res = []
    for k in sorted(set(a) | set(b), key=lambda k: (k[2], k[1], str(k[3]))):
        if k in a and k in b:
            continue
        sid, name, line, oline = k
        if k in a:
            side = 'raw_only'
            c = a[k]
            tok = c['tok'] if isinstance(c, dict) else c.tok
            stmt = c['stmt'] if isinstance(c, dict) else c.stmt
            ifs = tok.ifs if tok is not None else ()
            # the macro invocation this call sits in (an argument of fe_evt, LAB_FFP(...))
            outer = [t.text for t in _stmt_toks(stmt) + raw_by_line.get((line, oline), [])
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
            mac = [t.text for t in raw_by_line.get((line, oline), []) if t.kind == 'id' and in_dm(t.text)]
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
    view.hold_bad, term_hold, view.hold_out = hold_check(view)
    for s in st:
        s['hold'] = term_hold.get(s['line'], '-') if s['func'] == 'main' else '-'
    with open(os.path.join(outdir, 'exit_sites.tsv'), 'w', newline='\n', encoding='utf-8') as f:
        f.write('flavour\tid\tfunc\tfirst\tline\tcode\twhy\torder\tflags\tseq\tcalls\tkey\tcallnames'
                '\thold\ttext\n')
        for s in st:
            f.write('\t'.join([flavour, s['id'], s['func'], str(s['first']), str(s['line']),
                               s['code'], s['why'], s['order'], ','.join(s['flags']),
                               ' '.join(s['seq']),
                               ' '.join('%s@%d' % (c.name, c.line) for c in s['calls']),
                               s['key'], ' '.join(c.name for c in s['calls']), s['hold'],
                               TEXT_SEP.join(s['text']).replace('\t', ' ')]) + '\n')
    with open(os.path.join(outdir, 'exit_steps.tsv'), 'w', newline='\n') as f:
        f.write('flavour\tn\tphase\tstep\tmapped\tline\tkind\tname\targs\ttags\tconds\n')
        for k, r in enumerate(rows):
            f.write('\t'.join([flavour, str(k), r['phase'], r['step'], 'y' if r['mapped'] else '~',
                               str(r['line']), r['kind'], r['name'], r['args'][:70].replace('\t', ' '),
                               ','.join(r['tags']), ' && '.join(r['conds'])[:160]]) + '\n')
    with open(os.path.join(outdir, 'exit_scenarios.tsv'), 'w', newline='\n', encoding='utf-8') as f:
        f.write('flavour\tscenario\tterminal\terror\tseq\thold\ttext\n')
        for s in sc:
            f.write('\t'.join([flavour, s['name'], s['terminal'], s['err'], ' '.join(s['seq']),
                               s['hold'], TEXT_SEP.join(s['text']).replace('\t', ' ')]) + '\n')
    write_expect_h(os.path.join(outdir, 'exit_expect.h'), flavour, st, sc, rows)
    rv = None
    if raw is not None and dm is not None:
        rv = raw_vs_e(raw, view, dm)
        with open(os.path.join(outdir, 'raw_vs_e.tsv'), 'w', newline='\n') as f:
            f.write('site\tline\tname\tside\tclass\tdetail\n')
            for r in rv:
                f.write('\t'.join([r['site'], str(r['line']), r['name'], r['side'], r['cls'],
                                   r['detail']]) + '\n')
    return st, rows, sc, rv, loop


def write_expect_h(path, flavour, st, sc, feat=None):
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
        # what the tree has that the reference model follows (tests/abi-exit-ci_teardown_ref.inc.c)
        se = [r for r in (feat or []) if r['phase'] == 'session_end' and r['kind'] == 'call']
        names = [r['name'] for r in se]
        if 'pwr_hold' in names and 'play_lock_drop' in names and \
                names.index('pwr_hold') < names.index('play_lock_drop'):
            f.write('#define EXPECT_D4_FIRST 1         /* HW-1 F2: the teardown takes the hold first */\n')
        if 'ui_settings_flush' in names:
            f.write('#define EXPECT_SETTINGS_FLUSH 1   /* HW-2 F3: D16b */\n')
        if any(s['name'] == 'LINK_FATAL' for s in sc):
            f.write('#define EXPECT_F5 1               /* HW-2 F5: g_link_fatal_why */\n')
        if any(s['name'].startswith('HOME_') for s in sc):
            f.write('#define EXPECT_F1 1               /* HW-2 F1: g_exit_req */\n')
        f.write('/* evt: the fe_evt format strings of the scenario, in order, each followed '
                'by \\x1f */\n')
        f.write('#define EXPECT_HAS_EVT 1\n')
        f.write('static const struct exp_scen { const char *name; const char *seq; '
                'const char *evt; } EXP_SCEN[] = {\n')
        for s in sc:
            f.write('   { "%s", "%s",\n     %s },\n' % (
                s['name'], ' '.join(s['seq']),
                ' '.join('%s "\\x1f"' % e for e in s['evt']) or '""'))
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


def default_on_defines(makefile):
    """-D names psp/Makefile adds by default: `NAME ?= 1` followed by an
    `ifeq ($(NAME),1)` block with `CFLAGS += -DX`.  Used to refuse a STALE baseline: the
    -E views are made with the compile line of the baseline run's make log, and a log of
    an older tree lacks the defines newer code is guarded by (the 01067e74 baseline has no
    -DGBADHOC_MGIFT: the teardown's mgift_stop(1) then reads as compiled out, and every
    model run fails against a golden that is wrong, not against the code)."""
    on, cur, res = set(), None, set()
    try:
        lines = open(makefile, encoding='utf-8', errors='replace').read().split('\n')
    except OSError:
        return res
    for ln in lines:
        m = re.match(r'^([A-Z_0-9]+)\s*\?=\s*1\s*$', ln)
        if m:
            on.add(m.group(1))
            continue
        m = re.match(r'^ifeq \(\$\(([A-Z_0-9]+)\),\s*1\)', ln)
        if m:
            cur = m.group(1) if m.group(1) in on else None
            continue
        if ln.startswith(('endif', 'else')):
            cur = None
        elif cur:
            res |= set(re.findall(r'^CFLAGS \+= .*?-D([A-Za-z_0-9]+)', ln))
    return res


def run_all(baseline, src, outdir, pp_dir=None, quiet=False, main_c=None, over=None):
    """over: {'fe/X.inc.c': host file}, mutated fragment copies (read by the raw view and
    bind-mounted over the tree for -E), as main_c is a mutated copy of the shell."""
    import aec_pspcc as P
    os.makedirs(outdir, exist_ok=True)
    over = over or {}
    extra = [(h, rel) for rel, h in sorted(over.items())]
    rawpath = main_c or os.path.join(src, 'psp', 'main_psp.c')
    raw = View('raw', C.load_raw(rawpath, follow=True, alias=EXIT_ALIAS,
                                 base=os.path.join(src, 'psp'), over=over))
    report, fails = [], 0
    want_d = default_on_defines(os.path.join(src, 'psp', 'Makefile'))
    for fl in FLAVOURS:
        try:
            have = set(a[2:].split('=')[0] for a in P.compile_argv(os.path.join(baseline, fl))
                       if a.startswith('-D'))
        except (SystemExit, OSError):
            continue                     # --pp-dir runs need no make log
        miss = sorted(want_d - have)
        if miss:
            fails += 1
            report.append('STALE BASELINE %s: its compile line lacks -D%s, which the psp/Makefile of this tree '
                          'turns on by default; give --baseline a flavours.sh run '
                          'of this tree (or of one with the same defines)' % (fl, ' -D'.join(miss)))
    st_raw, rows_raw, _, _, _ = write_view(raw, os.path.join(outdir, 'raw'), 'raw')
    b, o = check_x2(st_raw, 'raw')
    fails += b
    report += ['x2 reproduction (raw source, the view x2 read):'] + o
    per, views = {}, {}
    for fl in FLAVOURS:
        ppd = pp_dir or os.path.join(outdir, 'pp')
        os.makedirs(ppd, exist_ok=True)
        ip, dp = os.path.join(ppd, 'main_psp.%s.i' % fl), os.path.join(ppd, 'main_psp.%s.dM' % fl)
        if not os.path.exists(ip):
            with open(ip, 'w', newline='\n') as f:
                f.write(P.preprocess(src, os.path.join(baseline, fl), override=main_c,
                                     extra=extra))
        if not os.path.exists(dp):
            with open(dp, 'w', newline='\n') as f:
                f.write(P.macros(src, os.path.join(baseline, fl), override=main_c,
                                 extra=extra))
        dm = P.parse_dm(open(dp).read())
        v = View(fl, C.load_pp(ip))
        v.line_ids = site_ids(st_raw)
        views[fl] = v
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
            for hb in s['hold_bad']:
                fails += 1
                report.append('  HOLD scenario %s: %s' % (s['name'], hb))
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
    report += ['', 'hold balance (pwr_hold / pwr_release, structural; raw source):'] + raw.hold_out
    hb = list(raw.hold_bad)
    for fl in FLAVOURS:
        hb += ['[%s -E] %s' % (fl, x) for x in views[fl].hold_bad if x not in raw.hold_bad]
    for x in hb:
        fails += 1
        report.append('  HOLD %s' % x)
    report.append('  hold balance: %s' % ('ok' if not hb else '%d problems' % len(hb)))
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
    with open(path, encoding='utf-8') as f:
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
        return 'seq=%s | code=%s | why=%s | calls=%s | hold=%s | text=%s' % (
            r.get('seq', ''), r.get('code', ''), r.get('why', ''), r.get('callnames', '?'),
            r.get('hold', '(no hold column: regenerate the golden)'),
            r.get('text', '(no text column: regenerate the golden)'))

    def scen_val(r):
        return 'seq=%s | hold=%s | text=%s' % (
            r['seq'], r.get('hold', '(no hold column: regenerate the golden)'),
            r.get('text', '(no text column: regenerate the golden)'))

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
                                ('exit_scenarios.tsv', 'scenario', scen_val)):
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
   pwr_hold("teardown");
   play_lock_drop("loop_exit");
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
    # mutant: D3/D4 back in the pre-F2 order (lock dropped before the hold) -> order violation
    p2 = os.path.join(d, 'm2.c')
    open(p2, 'w').write(SELF_SRC.replace('   pwr_hold("teardown");\n   play_lock_drop("loop_exit");',
                                         '   play_lock_drop("loop_exit");\n   pwr_hold("teardown");'))
    b, o = check_order(step_rows(View('raw', C.load_raw(p2)))[0])
    expect(b == 1, 'mutant D3<->D4 swap (pre-F2 order) caught by the PLAN 3.2 order check')
    # mutant: unknown runtime condition -> scenario error
    p3 = os.path.join(d, 'm3.c')
    open(p3, 'w').write(SELF_SRC.replace('if (g_relaunch)\n', 'if (g_new_flag)\n'))
    sc3 = scenarios(View('raw', C.load_raw(p3)))
    expect(all('g_new_flag' in s['err'] for s in sc3 if not s['terminal'].startswith('goto')) and
           sum(bool(s['err']) for s in sc3) == 5,
           'an unmapped runtime atom fails every scenario that reaches it (5 of 8)')
    # ---- Epoch C C0a-5a: the text of calls that pass a string literal
    expect(st[0]['text'] == ['fe_evt("exit code=2")', 'handoff_run(2, "bad")'],
           'site 1 text: %s' % TEXT_SEP.join(st[0]['text']))
    expect(evt_fmt('"a=%d " "b=%u", x, y') == '"a=%d " "b=%u"' and evt_fmt('fmt, x') == '"?"',
           'fe_evt format literals are taken as written (adjacent literals kept)')
    # ---- C0a-5b: hold balance
    hb, th, _ = hold_check(v)
    expect(hb == [] and sorted(th.values()) == ['-', '-', 'teardown'] and
           scenario_hold(sc['LIST_OK']['seq'], 'goto:top') == ('max=1 end=0', []),
           'hold balance: clean source has no problem; LIST_OK ledger max=1 end=0 (%s)' % hb)
    p4 = os.path.join(d, 'm4.c')
    open(p4, 'w').write(SELF_SRC.replace('audio_start(); pwr_release(); goto top;',
                                         'audio_start(); goto top;'))
    v4 = View('raw', C.load_raw(p4))
    hb, _, _ = hold_check(v4)
    s4 = {s['name']: s for s in scenarios(v4)}
    expect(any("goto top holding ['teardown']" in x for x in hb) and
           s4['LIST_OK']['hold_bad'] == ['returns to the list holding 1'],
           'mutant: the return without pwr_release: the goto and the LIST_OK ledger are caught')
    p5 = os.path.join(d, 'm5.c')
    open(p5, 'w').write(SELF_SRC.replace(
        'static void helper(void) { foo(); }',
        'static int helper(void) { pwr_hold("h"); if (foo()) return 1; pwr_release(); return 0; }'))
    hb, _, _ = hold_check(View('raw', C.load_raw(p5)))
    expect(len(hb) == 1 and "helper:" in hb[0] and "returns holding ['h']" in hb[0],
           'mutant: a helper that returns early with its hold is caught (%s)' % hb)
    p6 = os.path.join(d, 'm6.c')
    open(p6, 'w').write(SELF_SRC.replace('   while (g_running) { step(); }',
                                         '   pwr_hold("boot");\n   while (g_running) { step(); }'))
    hb, _, _ = hold_check(View('raw', C.load_raw(p6)))
    expect(any('frame loop is entered holding' in x for x in hb),
           'mutant: a hold still up at the frame loop is caught')
    p7 = os.path.join(d, 'm7.c')
    open(p7, 'w').write(SELF_SRC.replace('   if (bad) {', '   pwr_hold("boot");\n   if (bad) {').replace(
        '   while (g_running) { step(); }', '   pwr_release();\n   while (g_running) { step(); }'))
    v7 = View('raw', C.load_raw(p7))
    hb, th, _ = hold_check(v7)
    expect(sorted(th.values()) == ['boot', 'boot', 'teardown'] and hb == [],
           'sites under a per-session boot hold report it (%s), balanced' % sorted(th.values()))
    p7b = os.path.join(d, 'm7b.c')
    open(p7b, 'w').write(SELF_SRC.replace('top:\n', '   pwr_hold("boot");\ntop:\n').replace(
        '   while (g_running) { step(); }', '   pwr_release();\n   while (g_running) { step(); }'))
    hb, _, _ = hold_check(View('raw', C.load_raw(p7b)))
    expect(any("goto top holding []" in x and "reached holding ['boot']" in x for x in hb),
           'mutant: a hold taken once before the session label, released per session: the goto '
           'arrives with a different depth: caught')
    # ---- C0a-5c: HW-2 F1 atoms
    expect(len(sc) == len(SCENARIOS) and sc['LIST_OK']['seq'][-2:] == ['g_running=0', 'goto:top'],
           'a tree without g_exit_req has no HOME_* scenario; a return that never sets g_running '
           'reports g_running=0')
    f1 = SELF_SRC.replace('audio_start(); pwr_release(); goto top;',
                          'audio_start(); g_running = !g_exit_req; pwr_release(); goto top;') \
                 .replace('   if (g_relaunch)\n', '   if (g_exit_req)\n      g_relaunch = 0;\n   if (g_relaunch)\n')
    p8 = os.path.join(d, 'm8.c')
    open(p8, 'w').write(f1)
    s8 = {s['name']: s for s in scenarios(View('raw', C.load_raw(p8)))}
    expect(len(s8) == len(SCENARIOS) + len(SCENARIOS_F1) and not any(s['err'] for s in s8.values()),
           'F1 source: the four HOME_* scenarios are added and evaluate')
    expect('relaunch_eboot' not in s8['HOME_PROFILE']['seq'] and
           'relaunch_eboot' not in s8['HOME_BEFORE_TEARDOWN']['seq'] and
           'relaunch_eboot' not in s8['HOME_BEFORE_REFUSAL2']['seq'] and
           'relaunch_eboot' in s8['PROFILE']['seq'] and 'relaunch_eboot' in s8['LIST_REFUSED']['seq'],
           'F1: HOME turns every relaunch into an exit; without HOME they still relaunch')
    expect(s8['HOME_IN_UNLOAD']['seq'][-2:] == ['g_running=0', 'goto:top'] and
           s8['LIST_OK']['seq'][-2:] == ['g_running=1', 'goto:top'],
           'F1: HOME inside the unload returns to the list with g_running 0 (LIST_OK: 1)')
    p9 = os.path.join(d, 'm9.c')
    open(p9, 'w').write(f1.replace('   if (g_exit_req)\n      g_relaunch = 0;\n', '   (void)g_exit_req;\n'))
    s9 = {s['name']: s for s in scenarios(View('raw', C.load_raw(p9)))}
    expect('relaunch_eboot' in s9['HOME_PROFILE']['seq'] and
           s9['HOME_PROFILE']['seq'] != s8['HOME_PROFILE']['seq'],
           'mutant: process_end without the g_exit_req guard relaunches after HOME: caught')
    p11 = os.path.join(d, 'm11.c')
    open(p11, 'w').write(SELF_SRC.replace(
        'static void helper(void) { foo(); }',
        'static void helper(void) { pwr_release(); ui_notice("t"); pwr_hold("boot"); }'))
    hb, _, ho = hold_check(View('raw', C.load_raw(p11)))
    expect(hb == [] and any('helper: drops the caller' in x for x in ho),
           'a helper that drops its caller\'s hold for a notice and takes it again is balanced')
    open(p11, 'w').write(SELF_SRC.replace(
        'static void helper(void) { foo(); }',
        'static void helper(void) { pwr_release(); ui_notice("t"); }'))
    hb, _, _ = hold_check(View('raw', C.load_raw(p11)))
    expect(len(hb) == 1 and 'helper:end returns holding' in hb[0],
           'mutant: the same helper without the second pwr_hold is caught (%s)' % hb)
    # ---- HW-2 F5: the link-fatal atom and its scenario
    p10 = os.path.join(d, 'm10.c')
    open(p10, 'w').write(SELF_SRC.replace(
        '   ui_set_wake_yield(NULL);\n',
        '   if (g_link_fatal_why && !exit_reason) { exit_code = g_link_fatal_code; '
        'exit_reason = g_link_fatal_why; }\n   ui_set_wake_yield(NULL);\n').replace(
        '   evt_shutdown();\n   handoff_run(exit_code, exit_reason);',
        '   if (exit_reason) fe_evt("exit code=%d reason=%s", exit_code, exit_reason);\n'
        '   else fe_evt("exit code=%d", exit_code);\n   evt_shutdown();\n'
        '   handoff_run(exit_code, exit_reason);'))
    s10 = {s['name']: s for s in scenarios(View('raw', C.load_raw(p10)))}
    expect('LINK_FATAL' in s10 and not s10['LINK_FATAL']['err'] and
           s10['LINK_FATAL']['evt'] == ['"exit code=%d reason=%s"'] and s10['XMB']['evt'] == ['"exit code=%d"']
           and 'LINK_FATAL' not in sc,
           'F5 source: LINK_FATAL exists, and its exit line carries the reason (XMB: none)')
    expect(flags_of('ME IO HOST RW AUD VID EVT ES HO EXIT'.split())[:4] == ['TD_IO', 'TD_CORE', 'TD_REWIND', 'TD_ME'],
           'HW-2 F9: a site that stops the ME first carries TD_ME')
    # Epoch C helpers: the same two sites through exit_early_begin/_end read as the copies
    hsrc = SELF_SRC.replace(
        'static void helper(void) { foo(); }\n',
        'static void helper(void) { foo(); }\n'
        'enum { XE_EVT_FIRST = 1u << 0, XE_ME = 1u << 1 };\n'
        'static inline __attribute__((always_inline)) void exit_early_begin(unsigned f)\n'
        '{\n   if (f & XE_ME) { me_rend_teardown("exit"); }\n'
        '   if (!(f & XE_EVT_FIRST)) { audio_stop(); vid_term(); }\n}\n'
        'static inline __attribute__((always_inline)) void exit_early_end(unsigned f, int code, const char *why)\n'
        '{\n   evt_shutdown();\n   if (f & XE_EVT_FIRST) { audio_stop(); vid_term(); } else { }\n'
        '   handoff_run(code, why);\n   sceKernelExitGame();\n}\n').replace(
        '      fe_evt("exit code=2");\n      evt_shutdown();\n      audio_stop();\n      vid_term();\n'
        '      handoff_run(2, "bad");\n      sceKernelExitGame();\n',
        '      fe_evt("exit code=2");\n      exit_early_end(XE_EVT_FIRST, 2, "bad");\n').replace(
        'if (probe) { audio_stop(); vid_term(); fe_evt("exit"); evt_shutdown(); handoff_run(0, NULL); sceKernelExitGame(); return 0; }',
        'if (probe) { exit_early_begin(0); fe_evt("exit"); exit_early_end(0, 0, NULL); return 0; }')
    ph = os.path.join(d, 'mh.c')
    open(ph, 'w').write(hsrc)
    sh = sites(View('raw', C.load_raw(ph)))
    same = len(sh) == len(st) and all(
        a['seq'] == b['seq'] and a['code'] == b['code'] and a['why'] == b['why'] and
        a['text'] == b['text'] and [c.name for c in a['calls']] == [c.name for c in b['calls']]
        for a, b in zip(sh, st))
    expect(same and sh[1]['calls'][0].tok.ifs == ('#ifdef RIG',),
           'Epoch C helpers inlined: the sites equal the pasted copies (calls, text, code, #if)')
    hm = hsrc.replace('if (f & XE_EVT_FIRST) { audio_stop(); vid_term(); } else { }',
                      'if (f & XE_EVT_FIRST) { audio_stop(); } else { }')
    open(ph, 'w').write(hm)
    sm = sites(View('raw', C.load_raw(ph)))
    expect(sm[0]['seq'] != st[0]['seq'] and sm[1]['seq'] == st[1]['seq'],
           'mutant: a helper arm loses vid_term: only the sites that take that arm change')
    # an argument is one value: `f & XC` with f = `XA | XB` is 0 (not XA | (XB & XC))
    pp = os.path.join(d, 'mp.c')
    open(pp, 'w').write('enum { XA = 1, XB = 2, XC = 4 };\n'
                        'static inline void h(unsigned f) { if (f & XC) { bad(); } else { good(); } }\n'
                        'int main(void) { h(XA | XB); return 0; }\n')
    tp = C.inline_helpers(C.load_raw(pp), ('h',))
    lo, hi = C.functions(tp)['main']
    names = [c.name for c in C.walk_calls(C.parse_body(tp, lo, hi))]
    expect(names == ['good'], 'helper flags: an OR of flags is bracketed before it is tested (%s)' % names)
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
    a.add_argument('--over', action='append', default=[], metavar='fe/X.inc.c=FILE',
                   help='a mutated copy of a fragment, read in its place (repeatable)')
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
        return run_all(o.baseline, o.src, o.out, o.pp_dir, main_c=o.main_c,
                       over=dict(x.split('=', 1) for x in o.over))
    if o.cmd == 'gen':
        import aec_pspcc as P
        dm = P.parse_dm(open(o.dm).read()) if o.dm else None
        raw = View('raw', C.load_raw(o.raw, follow=True, alias=EXIT_ALIAS)) if o.raw else None  # noqa: E501
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
