#!/usr/bin/env python3
"""fe_readme.py -- generate psp/fe/README.md (PLAN-P-beta5 §5.2): the module descriptions, owners
and the fragment -> module map of the split main_psp.c, plus the conventions a reader of the split
needs.  The README is a doc file in no build; it is GENERATED, never hand-edited.

Inputs (all read, none written except -o):
  * tools/refactor/cutlist.txt: the FRAG rows (fragment -> module, in TU order) and the F/G rows
    (item -> owner module -> the fragment it sits in).
  * the split tree: psp/main_psp.c (the shell) and psp/fe/*.inc.c.  The rows' anchors are resolved
    in split.py's inverse of the shell, then mapped back to the file and line that hold them, so
    every cite is `fe/X.inc.c:N` (or `main_psp.c:N` for the shell).  The `#if` context of every row
    is read from the same inverse (comments and strings blanked, `\\`-continued directives joined).
  * PLAN12 below: PLAN.md §1.2's module table, the Responsibility cell of the shell row and of the
    19 module rows, VERBATIM (no word added or dropped; its line numbers are 01067e74's, its §
    are PLAN.md's).  EXPORTED (PLAN.md §1.3's exported ABI, by module), NEVER_TU (§1.1's "never real
    TUs", = the "(F, forever)" rows of §1.2) and LATER (the PLAN §2 items that create the modules
    with no fragment yet) are transcribed beside it.  `--plan PLAN.md` checks all four against the
    plan, cell for cell (exit 1 on any difference); EXPORTED is also checked against the cutlist
    (every name is a row its module owns).
  * the @extract markers in the shell and fragments, and the host tests that read them
    (tools/tests/extract.py --enumerate).  A test reads a region by calling `extract.extract(src,
    NAME)`, or a helper that passes its argument on (test_psp_runtime.marked_function reads
    'fn:' + name, run_ff_tests.function 'ff:' + name; helpers are found, not listed here).  NAME is
    resolved from the test's own literals with `ast`: a string, a `+` of strings, a loop or
    comprehension variable over a tuple/list literal (or a module-level name bound to one), a
    module-level dict literal subscripted.  The table lists each region's readers.
  * tools/refactor/generated/<flavour>/fe_abi_asserts.h: T14's counts per flavour, and with
    `abi_asserts.unbuilt` (a raw scan of the TU, every #if arm) the copy sites no flavour has.
  * the `/* @lab */` tags; fe/fe_priv.h's FE_HANDOFF; the include guards of fe_priv.h and
    play_defaults.h (not counted as moved macros).

Every table cell is written with each `|` escaped as `\\|`: GFM splits a table row at every
unescaped `|`, inside a code span too (GFM spec, tables extension, example 200), and drops the
cells past the header's count.  The rendered text is checked (gfm_table_errors) before it is
written or compared: a row whose GFM cell count is not its header's is refused.

Checks (exit 1, nothing written): the cutlist's `source:` sha256 is the inverse's (a stale cutlist
is refused); every FRAG row's anchor is the first line of its fragment file, in the shell's
include order; every F/G row's anchor sits in the fragment its row names; every LAB_HOOK( /
LAB_FFP( site carries its `/* @lab */` tag and every tag is on such a site; LAB_HANDOFF appears only
in fe_priv.h's FE_HANDOFF; a module with no PLAN12 row; an EXPORTED name that is not a row of its
module; a module with no FRAG row that LATER does not name, or a LATER module that has one; a host
test whose region NAME this tool cannot resolve from literals (never guessed, never skipped), or
that names a region no marker defines; an `#if` group that does not balance in the inverse; a raw
scan T14 refuses (abi_asserts.unbuilt); a table row with another GFM cell count than its header.

Usage:
  fe_readme.py [--gpsp DIR] [-o FILE]   write DIR/psp/fe/README.md (default DIR = this tree's gpsp/)
  fe_readme.py [--gpsp DIR] --check     exit 1 if the committed README is not what this generates
  fe_readme.py --plan PLAN.md           exit 1 if PLAN12 / EXPORTED / NEVER_TU / LATER differ from the plan
  fe_readme.py --gfm FILE...            exit 1 if a table row of FILE has another GFM cell count than
                                        its header (any Markdown file; prints the rows)
  fe_readme.py --selftest               mutants on a scratch copy (tests/fe_readme_test.py)
Exit: 0 ok, 1 refused / stale / differs, 2 usage.
"""
import argparse
import ast
import collections
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import split as S  # noqa: E402  (read_cutlist, read_split, inverse_text, INC_RE, Refused, csrc)
import modules  # noqa: E402
import abi_asserts as A  # noqa: E402  (unbuilt: the copy sites no T14 flavour compiles)

# PLAN.md §1.2's module table: '#' and the Responsibility cell, VERBATIM (`--plan` compares them
# with the plan's cells byte for byte).  Rows 0 and 1 of the plan are the two headers (fe_priv.h,
# play_defaults.h), not cutlist modules, so they are not here.
PLAN12 = collections.OrderedDict([
    ('shell', ('–', 'includes, pads, `main()` (§4)')),
    ('paths', ('20', 'ms0 paths and boot identity: `init_paths`, the `p_*` path buffers')),
    ('audio', ('2', 'Ring, `audio_thread`, `plat_audio_frames`, start/stop (SQ contract), resample step, status, '
                    'link-audio trims')),
    ('iothread', ('3', 'ADR-0025 writer thread, SRAM sync, stop-before-shutdown contract, SQ stop codes')),
    ('input', ('4', 'Pad read, ctl maps, `plat_input_bitmask`, pad_swallow, turbo, chord state, input_fresh')),
    ('power', ('5', '`exit_cb`, `cb_thread`, `power_cb`, `pwr_hold`/`pwr_release`/`pwr_release_all`, play lock, '
                    'park handshake (`loop_park`, `browser_park`), sleep-quiesce ops (`sq_*` 3977-4095), '
                    '`power_request_poll`, standby down/up, wake service and overlay')),
    ('display', ('6', 'CPU clock, display apply, colour preview, wake snapshot')),
    ('merend', ('7', 'ME renderer state, lifecycle, presenter, retire, `fill_desc`, vmem pool, legacy me_video '
                     '(until the owner decides)')),
    ('pacing', ('8', 'Latency tiers `lat_*`/`ll_*`/`dfr_*`/`lf_*`/`ffh_*`, vblank target, `us_to_vblank`, '
                     'frame-hist, the present-plus-overlays helper (3 exact copies, r5 Q6)')),
    ('rewind', ('9', 'Rewind tick, pool offer/check/lost/back (pool_lost runs on cb_thread), HUD, ini')),
    ('link', ('10', 'Net bring-up/teardown (both entry points kept), cable, GB link session, link UI actions, '
                    'silent wireless, RFU hooks and trace (strong overrides of weak stubs)')),
    ('mgift', ('11', 'Mystery Gift glue')),
    ('boost', ('12', 'Emu boost warden')),
    ('misc', ('13', 'Wall clocks (one noinline `psp_datetime_seconds`), screenshot, backup_save, state slots, '
                    'cheats lock')),
    ('inproc', ('14', '`inproc_refusal`, `relaunch_eboot`, heap census helpers')),
    ('teardown', ('15', '`session_end(td)`, `inproc_try_return()`, `process_end(pe)`, `exit_early(x)` (§3)')),
    ('boot', ('16', '`boot_run(boot_ctx*)`, noinline cold; owns `cfg_doc` in its frame (§0.2-1); the boot order '
                    'of §4.4')),
    ('session', ('17', '`session_select`, `session_configure`, `session_load`, `session_net_up`, '
                       '`session_pre_loop`')),
    ('loop', ('18', 'Until D1: the loop body included textually into `main`. From D1: '
                    '`frame_iter(loop_t*, session_ctx*)` noinline + `frame_iter_rare()` noinline cold')),
    ('harness', ('19', 'Stall watch, DRPROF_HW, core_health, rig identity, uiscript, sleep simulator, testpat, '
                       'nettest, vram_probe, vid_prof, preempt, ff_win, eboot_crc, `gpsp_net_wait_beat`')),
])
# PLAN.md §1.3 "Exported ABI keeps names and linkage" (the weak overrides are named there only as a
# group, so they are not listed), by owner module.  Each name must be a cutlist row of that module.
EXPORTED = collections.OrderedDict([
    ('paths', ['g_dir_base']), ('power', ['g_running']), ('pacing', ['g_ff_uncapped', 'g_ff_mult']),
    ('audio', ['in_rate', 'g_audio_step']), ('link', ['g_net_up', 'g_net_is_host']),
    ('harness', ['gpsp_net_wait_beat']),
])
# PLAN.md §1.1 "Never real TUs, by rule" = the "(F, forever)" rows of §1.2's File (kind) column
NEVER_TU = ('audio', 'iothread', 'power', 'merend', 'pacing', 'loop')
# the PLAN §2 items that create the modules that have no fragment yet (HW-3 = epoch C, HW-4 = epoch D1)
LATER = collections.OrderedDict([('teardown', 'HW-3 C1'), ('boot', 'HW-3 C2'), ('session', 'HW-3 C3'),
                                 ('loop', 'HW-4 D1')])
ROLES = ('harness', 'mgift')      # the modules whose F rows are listed with their #if context

# What each @extract name prefix means (tools/refactor/docs/source-split.md, P1).  A prefix not
# listed here is reported as "a range" with its readers, and the generator does not fail on it.
PREFIX_MEANING = {
    'fn': 'one whole function: its definition line through its closing brace\'s line',
    'def': 'the `#define` line(s) a test greps, by name',
    'decl': 'one declaration / object definition a test greps, by name',
}
NUM_WORD = {1: 'one', 2: 'two', 3: 'three', 4: 'four', 5: 'five'}


def range_meaning(nreaders):
    """the meaning of an unlisted prefix, with how many host tests slice each of its regions
    (from the data: [readers of each region of the prefix])"""
    ks = sorted(set(nreaders))
    if ks == [0]:
        who = 'no host test slices'
    elif len(ks) == 1:
        who = '%s test%s slice%s' % (NUM_WORD.get(ks[0], str(ks[0])), '' if ks[0] == 1 else 's', 's' if ks[0] == 1 else '')
    else:
        who = '%d to %d tests slice' % (ks[0], ks[-1])
    return 'a statement range %s (its old start/end anchors are checked inside it)' % who

MARK = re.compile(r'/\* @extract:begin ([A-Za-z0-9_.:-]+) \*/')
LAB_TAG = '/* @lab */'
LAB_SITE = re.compile(r'\bLAB_(?:HOOK|FFP)\(')
DEFINE = re.compile(r'^\s*#\s*define\b')
SRC_SHA = re.compile(r'^# source: .*\bsha256 ([0-9a-f]{64})\b')
PP_DIR = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b')
T14_HEAD = re.compile(r'checked: (\d+) declarations, (\d+) constants; unchecked: (\d+)')
T14_FLAVOURS = ('release', 'lab', 'harness')


class Stale(Exception):
    pass


# ----------------------------------------------------------------- #if context
def pp_context(text):
    """[frames] per line of text (index 0 = line 1): the #if groups open at that line, outermost
    first, each (line of its #if, (arm directive, ...)) -- the arms seen so far; the last one is the
    arm the line is in.  Comments and strings are blanked first (csrc), so a directive inside a
    comment does not count; a `\\`-continued directive is joined; a directive line carries the
    context it sits in (before it takes effect)."""
    clean = S.csrc.blank_comments_strings(text).split('\n')
    stack, out, k, n = [], [], 0, len(clean)

    def snap():
        return tuple((o, tuple(a)) for o, a in stack)
    while k < n:
        m = PP_DIR.match(clean[k])
        if not m:
            out.append(snap())
            k += 1
            continue
        first, full = k, clean[k]
        while full.rstrip().endswith('\\') and k + 1 < n:
            k += 1
            full = full.rstrip()[:-1] + ' ' + clean[k].strip()
        out += [snap()] * (k - first + 1)
        kw = m.group(1)
        rest = ' '.join(full[m.end():].split())
        d = '#%s%s' % (kw, ' ' + rest if rest else '')
        if kw in ('if', 'ifdef', 'ifndef'):
            stack.append((first + 1, [d]))
        elif not stack:
            raise Stale('line %d: %s with no open #if' % (first + 1, d))
        elif kw == 'endif':
            stack.pop()
        else:
            stack[-1][1].append(d)
        k += 1
    if stack:
        raise Stale('%d #if group(s) still open at the end (the first at line %d)' % (len(stack), stack[0][0]))
    return out


def ctx_text(frames, where):
    """'`#ifdef X` (fe/a.inc.c:N)' per open group, outermost first, joined by ' > '"""
    out = []
    for opn, arms in frames:
        at = '%s:%d' % where[opn - 1]
        if len(arms) == 1:
            out.append('`%s` (%s)' % (arms[0], at))
        else:
            out.append('`%s` after `%s` (%s)' % (arms[-1], '`, `'.join(arms[:-1]), at))
    return ' > '.join(out)


# ----------------------------------------------------------------- GFM tables
GFM_DELIM = re.compile(r'^\|(?:\s*:?-+:?\s*\|)+\s*$')


def cell(s):
    """one table cell as written: every `|` escaped as `\\|`.  GFM splits a row at every unescaped
    `|`, inside a code span too; an escaped one renders as `|` (GFM spec, tables extension, ex. 200)."""
    return str(s).replace('|', '\\|')


def trow(*cells):
    return '| %s |' % ' | '.join(cell(c) for c in cells)


def gfm_cells(line):
    """the cells of a GFM table row, split the way cmark-gfm splits them: at every `|` that a
    backslash does not escape (a backslash escapes the next character, whatever it is); one leading
    and one trailing `|` are the row's edges, not delimiters"""
    s = line.strip()
    if s.startswith('|'):
        s = s[1:]
    cells, cur, i = [], [], 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            cur.append(s[i:i + 2])
            i += 2
            continue
        if s[i] == '|':
            cells.append(''.join(cur))
            cur = []
        else:
            cur.append(s[i])
        i += 1
    if ''.join(cur).strip():
        cells.append(''.join(cur))
    return cells


def gfm_table_errors(text):
    """[(line, header cells, row cells, row)] for every row of every GFM table in text (a `|` line
    followed by a delimiter row, then the `|` lines after it) whose cell count is not the header's:
    GFM drops a row's cells past the header's count and pads a short row"""
    lines, bad, i = text.split('\n'), [], 0
    while i < len(lines):
        if lines[i].startswith('|') and i + 1 < len(lines) and GFM_DELIM.match(lines[i + 1]):
            n = len(gfm_cells(lines[i]))
            j = i + 1
            while j < len(lines) and lines[j].startswith('|'):
                m = len(gfm_cells(lines[j]))
                if m != n:
                    bad.append((j + 1, n, m, lines[j]))
                j += 1
            i = j
        else:
            i += 1
    return bad


# ----------------------------------------------------------------- the tree
def load(gpsp):
    psp = os.path.join(gpsp, 'psp')
    shell_p = os.path.join(psp, 'main_psp.c')
    cut_p = os.path.join(gpsp, 'tools', 'refactor', 'cutlist.txt')
    shell_text = open(shell_p, encoding='utf-8', newline='').read()
    incs = [ln for ln in shell_text.splitlines(keepends=True) if S.INC_RE.match(ln)]
    if not incs:
        raise Stale('%s includes no fe/X.inc.c: not a split tree (the README describes the split)' % shell_p)
    shell, frags = S.read_split(gpsp)
    inv = S.inverse_text(shell, frags)
    frows, rows = S.read_cutlist(cut_p)
    # universal newlines: the content git stores (LF).  A `git archive` on a core.autocrlf=true checkout
    # writes cutlist.txt (text=auto) with CRLF; its README must still be current (self-test C3)
    cut_text = open(cut_p, encoding='utf-8').read()
    shas = [m.group(1) for m in (SRC_SHA.match(ln) for ln in cut_text.split('\n')) if m]
    inv_sha = hashlib.sha256(inv.encode('utf-8')).hexdigest()
    if shas != [inv_sha]:
        raise Stale('cutlist.txt names source sha256 %s, the split tree\'s inverse is %s: the cutlist is stale '
                    '(regenerate it: split.py gen --split <gpsp> --carry tools/refactor/cutlist.txt)'
                    % (', '.join(shas) or 'none', inv_sha))
    # inverse line -> (file, line)
    where = []
    for i, ln in enumerate(shell.splitlines(keepends=True), 1):
        m = S.INC_RE.match(ln)
        if m:
            n = frags[m.group(1)].count('\n')
            where += [('fe/%s.inc.c' % m.group(1), j) for j in range(1, n + 1)]
        else:
            where.append(('main_psp.c', i))
    lines = inv.splitlines()
    if len(where) != len(lines):
        raise Stale('internal: %d mapped lines for %d inverse lines' % (len(where), len(lines)))
    try:
        ctx = pp_context(inv)
    except Stale as e:
        raise Stale('#if groups of the inverse: %s' % e)
    idx = collections.defaultdict(list)
    for i, ln in enumerate(lines, 1):
        idx[ln].append(i)

    def resolve(occ, anchor, what):
        hits = idx.get(anchor, [])
        if occ > len(hits):
            raise Stale('%s: anchor not found in the inverse (occ %d): %r' % (what, occ, anchor))
        return hits[occ - 1]

    # FRAG rows: each non-shell fragment's anchor is line 1 of its file; the files are the shell's
    # includes in FRAG order; shell fragments sit in main_psp.c
    inc_order = [S.INC_RE.match(ln).group(1) for ln in incs]
    want = [f['frag'] for f in frows if f['module'] != 'shell']
    if inc_order != want:
        raise Stale('the shell includes %s, the FRAG rows list %s' % (inc_order, want))
    floc = {}
    for f in frows:
        fl, ln = where[resolve(f['occ'], f['anchor'], 'FRAG ' + f['frag']) - 1]
        if f['module'] != 'shell' and (fl, ln) != ('fe/%s.inc.c' % f['frag'], 1):
            raise Stale('FRAG %s: anchor resolves to %s:%d, not line 1 of fe/%s.inc.c' % (f['frag'], fl, ln, f['frag']))
        if f['module'] == 'shell' and fl != 'main_psp.c':
            raise Stale('FRAG %s (shell): anchor resolves to %s:%d' % (f['frag'], fl, ln))
        floc[f['frag']] = (fl, ln)
    # shell fragments' extents in main_psp.c (shell.a from its anchor to the line before the first
    # include, shell.b from its anchor to the end)
    shell_lines = shell.count('\n')
    sh = [f for f in frows if f['module'] == 'shell']
    inc_lines = [i for i, ln in enumerate(shell.splitlines(keepends=True), 1) if S.INC_RE.match(ln)]
    for f in sh:
        a = floc[f['frag']][1]
        nxt = [i for i in inc_lines if i > a]
        later = [floc[g['frag']][1] for g in sh if floc[g['frag']][1] > a]
        b = min(nxt + later) - 1 if (nxt or later) else shell_lines
        floc[f['frag']] = ('main_psp.c', a, b)
    for f in frows:
        if f['module'] != 'shell':
            floc[f['frag']] = ('fe/%s.inc.c' % f['frag'], 1, frags[f['frag']].count('\n'))
    rloc, rctx = {}, {}
    for r in rows:
        i = resolve(r['occ'], r['anchor'], r['key'])
        fl, ln = where[i - 1]
        fr = floc[r['frag']]
        if fl != fr[0] or not (fr[1] <= ln <= fr[2]):
            raise Stale('%s: sits at %s:%d, the cutlist says fragment %s (%s:%d-%d)' % (r['key'], fl, ln, r['frag'], *fr))
        rloc[r['key']] = (fl, ln)
        rctx[r['key']] = ctx[i - 1]
    return {'gpsp': gpsp, 'shell': shell, 'frags': frags, 'frows': frows, 'rows': rows, 'floc': floc,
            'rloc': rloc, 'rctx': rctx, 'where': where, 'ctx': ctx, 'inv_sha': inv_sha, 'inv_lines': len(lines),
            'inc_lines': inc_lines, 'cut_sha': hashlib.sha256(cut_text.encode('utf-8')).hexdigest()}


def file_texts(d):
    """[(cite file, text)] of the TU's own files in include order: the shell, then each fragment."""
    out = [('main_psp.c', d['shell'])]
    for f in d['frows']:
        if f['module'] != 'shell':
            out.append(('fe/%s.inc.c' % f['frag'], d['frags'][f['frag']]))
    return out


def markers(d):
    regs = []
    for fl, t in file_texts(d):
        for i, ln in enumerate(t.split('\n'), 1):
            for m in MARK.finditer(ln):
                regs.append((m.group(1), fl, i))
    return regs


# ----------------------------------------------------------------- who reads which region
class Unresolved(Exception):
    pass


def _parents(tree):
    par = {}
    for node in ast.walk(tree):
        for ch in ast.iter_child_nodes(node):
            par[ch] = node
    return par


def _module_bind(tree, name):
    """the value nodes of module-level `name = ...` statements"""
    out = []
    for st in tree.body:
        if isinstance(st, ast.Assign):
            out += [st.value for t in st.targets if isinstance(t, ast.Name) and t.id == name]
        elif isinstance(st, ast.AnnAssign) and isinstance(st.target, ast.Name) and st.target.id == name and st.value:
            out.append(st.value)
    return out


def _seq(node, tree):
    """the element nodes of a tuple/list literal, or of the one module-level name bound to one"""
    if isinstance(node, (ast.Tuple, ast.List)):
        return node.elts
    if isinstance(node, ast.Name):
        vals = _module_bind(tree, node.id)
        if len(vals) == 1 and isinstance(vals[0], (ast.Tuple, ast.List)):
            return vals[0].elts
    raise Unresolved(ast.unparse(node))


def _target_path(target, name):
    """the index path of `name` inside a for/comprehension target, or None"""
    if isinstance(target, ast.Name):
        return [] if target.id == name else None
    if isinstance(target, (ast.Tuple, ast.List)):
        for i, t in enumerate(target.elts):
            p = _target_path(t, name)
            if p is not None:
                return [i] + p
    return None


def _strs(node, tree, par):
    """every string the expression can take, from the file's own literals; Unresolved otherwise"""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return [node.value]
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return [a + b for a in _strs(node.left, tree, par) for b in _strs(node.right, tree, par)]
    if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Name):
        vals = _module_bind(tree, node.value.id)
        if len(vals) == 1 and isinstance(vals[0], ast.Dict):
            return [s for v in vals[0].values for s in _strs(v, tree, par)]
        raise Unresolved(ast.unparse(node))
    if isinstance(node, ast.Name):
        cur = node
        while cur in par:
            up = par[cur]
            gens = []
            if isinstance(up, (ast.For, ast.AsyncFor)) and cur is not up.iter and cur is not up.target:
                gens = [(up.target, up.iter)]
            elif isinstance(up, (ast.ListComp, ast.SetComp, ast.GeneratorExp, ast.DictComp)) \
                    and not isinstance(cur, ast.comprehension):
                gens = [(g.target, g.iter) for g in up.generators]
            for tgt, it in gens:
                path = _target_path(tgt, node.id)
                if path is not None:
                    out = []
                    for el in _seq(it, tree):
                        for i in path:
                            el = _seq(el, tree)[i]
                        out += _strs(el, tree, par)
                    return out
            if isinstance(up, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
                args = up.args
                if node.id in [a.arg for a in args.posonlyargs + args.args + args.kwonlyargs]:
                    raise Unresolved('%s (a parameter of %s)' % (node.id, getattr(up, 'name', 'lambda')))
            cur = up
        vals = _module_bind(tree, node.id)
        if len(vals) == 1:
            return _strs(vals[0], tree, par)
        raise Unresolved(node.id)
    raise Unresolved(ast.unparse(node))


def _params(fn):
    return [a.arg for a in fn.args.posonlyargs + fn.args.args]


def _arg(call, idx, pname):
    if idx < len(call.args) and not any(isinstance(a, ast.Starred) for a in call.args[:idx + 1]):
        return call.args[idx]
    for kw in call.keywords:
        if kw.arg == pname:
            return kw.value
    return None


def region_reads(gpsp, readers):
    """({reader: {region: [line, ...]}}, {reader: n reads}) for the host tests that read main_psp.c.

    The primitive is `X.extract(src, NAME)` with X bound by `import extract` (or `extract(src, NAME)`
    from `from extract import extract`).  A function whose own read passes one of its parameters on
    (NAME = param, 'pre' + param or param + 'post') is a HELPER: its call sites read pre+arg+post, in
    the file that defines it and in every file that imports it by name (`from M import f`, `M.f`).
    Helpers of helpers are found by iterating to a fixpoint.  Raises Stale on a read whose NAME this
    cannot resolve from literals."""
    trees = {}
    tests_dir = os.path.join(gpsp, 'tools', 'tests')

    def tree_of(rel):
        if rel not in trees:
            p = os.path.join(gpsp, rel)
            t = ast.parse(open(p, encoding='utf-8').read(), rel)
            trees[rel] = (t, _parents(t))
        return trees[rel]

    def module_file(rel, mod):
        for base in (os.path.dirname(os.path.join(gpsp, rel)), tests_dir):
            p = os.path.join(base, mod + '.py')
            if os.path.exists(p):
                return os.path.relpath(p, gpsp).replace('\\', '/')
        return None

    def names_of(rel):
        """{local name: ('extract-module',) | ('extract-fn',) | ('import', file, name) | ('module', file)}"""
        t, _ = tree_of(rel)
        out = {}
        for st in ast.walk(t):
            if isinstance(st, ast.Import):
                for a in st.names:
                    if a.name == 'extract':
                        out[a.asname or a.name] = ('extract-module',)
                    elif module_file(rel, a.name):
                        out[a.asname or a.name] = ('module', module_file(rel, a.name))
            elif isinstance(st, ast.ImportFrom) and st.module and not st.level:
                mf = module_file(rel, st.module)
                for a in st.names:
                    if st.module == 'extract' and a.name == 'extract':
                        out[a.asname or a.name] = ('extract-fn',)
                    elif mf:
                        out[a.asname or a.name] = ('import', mf, a.name)
        return out

    helpers = {}            # (file, function) -> (param index, param name, prefix, suffix)

    def callee(rel, call, nm):
        """(kind, spec) of a call: ('read', None) for extract.extract, ('helper', spec), or None"""
        f = call.func
        if isinstance(f, ast.Attribute) and isinstance(f.value, ast.Name):
            b = nm.get(f.value.id)
            if b and b[0] == 'extract-module' and f.attr == 'extract':
                return ('read', (1, 'name', '', ''))
            if b and b[0] == 'module' and (b[1], f.attr) in helpers:
                return ('helper', helpers[(b[1], f.attr)])
        elif isinstance(f, ast.Name):
            b = nm.get(f.id)
            if b and b[0] == 'extract-fn':
                return ('read', (1, 'name', '', ''))
            if b and b[0] == 'import' and (b[1], b[2]) in helpers:
                return ('helper', helpers[(b[1], b[2])])
            if b is None and (rel, f.id) in helpers:
                return ('helper', helpers[(rel, f.id)])
        return None

    def enclosing_fn(node, par):
        while node in par:
            node = par[node]
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                return node
        return None

    def param_form(arg, fn):
        """(index, name, prefix, suffix) when arg is fn's parameter, 'pre' + it or it + 'post'"""
        if fn is None:
            return None
        ps = _params(fn)
        if isinstance(arg, ast.Name) and arg.id in ps:
            return (ps.index(arg.id), arg.id, '', '')
        if isinstance(arg, ast.BinOp) and isinstance(arg.op, ast.Add):
            l, r = arg.left, arg.right
            if isinstance(l, ast.Constant) and isinstance(l.value, str) and isinstance(r, ast.Name) and r.id in ps:
                return (ps.index(r.id), r.id, l.value, '')
            if isinstance(r, ast.Constant) and isinstance(r.value, str) and isinstance(l, ast.Name) and l.id in ps:
                return (ps.index(l.id), l.id, '', r.value)
        return None

    files = list(readers)
    # the files a reader imports from (helpers may live there), transitively within tools/
    k = 0
    while k < len(files):
        for b in names_of(files[k]).values():
            if b[0] in ('import', 'module') and b[1] not in files:
                files.append(b[1])
        k += 1
    for _ in range(8):                       # helpers of helpers: fixpoint
        changed = False
        for rel in files:
            t, par = tree_of(rel)
            nm = names_of(rel)
            for call in [n for n in ast.walk(t) if isinstance(n, ast.Call)]:
                c = callee(rel, call, nm)
                if not c:
                    continue
                idx, pn, pre, suf = c[1]
                arg = _arg(call, idx, pn)
                fn = enclosing_fn(call, par)
                pf = param_form(arg, fn) if arg is not None else None
                if pf and par.get(fn) is t and (rel, fn.name) not in helpers:
                    helpers[(rel, fn.name)] = (pf[0], pf[1], pre + pf[2], pf[3] + suf)
                    changed = True
        if not changed:
            break
    reads, nreads = {}, {}
    for rel in readers:
        t, par = tree_of(rel)
        nm = names_of(rel)
        got = collections.defaultdict(list)
        for call in [n for n in ast.walk(t) if isinstance(n, ast.Call)]:
            c = callee(rel, call, nm)
            if not c:
                continue
            idx, pn, pre, suf = c[1]
            arg = _arg(call, idx, pn)
            fn = enclosing_fn(call, par)
            if arg is None:
                raise Stale('%s:%d: a region read with no NAME argument' % (rel, call.lineno))
            if fn is not None and (rel, fn.name) in helpers and param_form(arg, fn):
                continue                     # a helper's own read: counted at its call sites
            try:
                for s in _strs(arg, t, par):
                    got[pre + s + suf].append(call.lineno)
            except Unresolved as e:
                raise Stale('%s:%d: reads an @extract region whose name is not a literal this tool can resolve '
                            '(%s); write it as a literal, a loop over a literal tuple or a dict literal, '
                            'or teach fe_readme.region_reads the form' % (rel, call.lineno, e))
        reads[rel] = dict(got)
        nreads[rel] = sum(len(v) for v in got.values())
    return reads, nreads, sorted('%s.%s' % (os.path.basename(f)[:-3], n) for f, n in helpers)


# ----------------------------------------------------------------- the rest of the inputs
def lab_sites(d):
    tags, sites, bad = 0, [], []
    for fl, t in file_texts(d):
        for i, ln in enumerate(t.split('\n'), 1):
            tags_here = ln.count(LAB_TAG)
            tags += tags_here
            calls = list(LAB_SITE.finditer(ln))
            code = [m for m in calls if not ln.lstrip().startswith(('#', '/*', '*'))
                    or ln.lstrip().startswith(LAB_TAG)]
            for m in code:
                if not ln[:m.start()].rstrip().endswith(LAB_TAG):
                    bad.append('%s:%d: %s without its %s tag' % (fl, i, m.group(0)[:-1], LAB_TAG))
                else:
                    sites.append((fl, i, m.group(0)[:-1]))
            if tags_here != len([m for m in code if ln[:m.start()].rstrip().endswith(LAB_TAG)]):
                bad.append('%s:%d: %d %s tag(s), %d tagged LAB_HOOK/LAB_FFP site(s)' % (fl, i, tags_here, LAB_TAG, len(code)))
            if re.search(r'\bLAB_HANDOFF\b', ln):
                bad.append('%s:%d: LAB_HANDOFF outside fe_priv.h\'s FE_HANDOFF' % (fl, i))
    fp = os.path.join(d['gpsp'], 'psp', 'fe', 'fe_priv.h')
    ho = []
    for i, ln in enumerate(open(fp, encoding='utf-8').read().split('\n'), 1):
        if re.search(r'\bLAB_HANDOFF\b', ln) and DEFINE.match(ln):
            if not re.match(r'\s*#\s*define\s+FE_HANDOFF\b', ln):
                bad.append('fe/fe_priv.h:%d: LAB_HANDOFF in a #define other than FE_HANDOFF' % i)
            ho.append(i)
    if len(ho) != 1:
        bad.append('fe/fe_priv.h: %d #define lines name LAB_HANDOFF (want exactly FE_HANDOFF\'s)' % len(ho))
    if tags != len(sites):
        bad.append('%d %s tags for %d LAB_HOOK/LAB_FFP sites' % (tags, LAB_TAG, len(sites)))
    return sites, ho, bad


NOOP_DEF = re.compile(r'^\s*#\s*define\s+(\w+)\s*\([^)]*\)\s+(?:do\s*\{\s*(?:\(void\)\s*\(\w+\)\s*;\s*)*\}\s*while\s*\(0\)'
                      r'|\(\(void\)\s*(?:0|\(\w+\))\))\s*(?:/\*.*)?$')


def noop_stubs(d):
    """[(name, file, line)] of the shell's and fragments' release no-op stub macros
    (`#define f(...) do { } while (0)` / `((void)0)`), which stay beside the functions they replace."""
    out = []
    for fl, t in file_texts(d):
        for i, ln in enumerate(t.split('\n'), 1):
            m = NOOP_DEF.match(ln)
            if m:
                out.append((m.group(1), fl, i))
    return out


def include_guard(text):
    """the include guard macro of a header: `#ifndef X` and `#define X` (nothing after it) as its
    first two directives and `#endif` as its last; None otherwise.  Comments are blanked first."""
    dirs = [ln.strip() for ln in S.csrc.blank_comments_strings(text).split('\n') if ln.lstrip().startswith('#')]
    if len(dirs) < 3:
        return None
    a = re.match(r'#\s*ifndef\s+(\w+)$', dirs[0])
    b = re.match(r'#\s*define\s+(\w+)$', dirs[1])
    if a and b and a.group(1) == b.group(1) and re.match(r'#\s*endif\b', dirs[-1]):
        return a.group(1)
    return None


def defines(d):
    """({'shell'|'fragments'|header: #define lines}, {header: its include guard or None}); a header's
    count leaves out its include guard's #define, which no step moved"""
    c, guards = collections.Counter(), {}
    for fl, t in file_texts(d):
        c['shell' if fl == 'main_psp.c' else 'fragments'] += sum(1 for ln in t.split('\n') if DEFINE.match(ln))
    for h in ('fe_priv.h', 'play_defaults.h'):
        p = os.path.join(d['gpsp'], 'psp', 'fe', h)
        t = open(p, encoding='utf-8').read()
        guards[h] = include_guard(t)
        c[h] = sum(1 for ln in t.split('\n') if DEFINE.match(ln)) - (1 if guards[h] else 0)
    return c, guards


def t14_counts(gpsp):
    """[(flavour, asserted declarations, constants, unchecked)] from the committed generated headers"""
    out = []
    for fl in T14_FLAVOURS:
        p = os.path.join(gpsp, 'tools', 'refactor', 'generated', fl, 'fe_abi_asserts.h')
        if not os.path.exists(p):
            raise Stale('no %s (regenerate T14: abi_asserts.py all)' % p)
        m = T14_HEAD.search(open(p, encoding='utf-8').read())
        if not m:
            raise Stale('%s: no "checked: N declarations, M constants; unchecked: K" line' % p)
        out.append((fl,) + tuple(int(x) for x in m.groups()))
    return out


def cite(loc):
    return '%s:%d' % loc


def dup_names(rows):
    """{(kind, name)} the cutlist defines more than once (keys name#1, name#2, ...)"""
    c = collections.Counter((r['kind'], r['name']) for r in rows)
    return {k for k, v in c.items() if v > 1}


def row_name(r, dups):
    """a row's name: bare when the cutlist defines the name once; `name#N` (its N-th definition in
    TU order, the cutlist key) for every definition of a name it defines more than once"""
    name, n = r['key'].split(':', 1)[1].rsplit('#', 1)
    return '%s#%s' % (name, n) if (r['kind'], name) in dups else name


def module_checks(d):
    rows = {r['key']: r for r in d['rows']}
    frag_mods = {f['module'] for f in d['frows']}
    bad = []
    for m, names in EXPORTED.items():
        for n in names:
            keys = [k for k in rows if k.split(':', 1)[1].rsplit('#', 1)[0] == n]
            if not keys:
                bad.append('EXPORTED %s (%s): no cutlist row' % (n, m))
            elif any(rows[k]['module'] != m for k in keys):
                bad.append('EXPORTED %s: owned by %s in the cutlist, not %s' % (
                    n, ', '.join(sorted({rows[k]['module'] for k in keys})), m))
    for m in PLAN12:
        if m not in frag_mods and m not in LATER:
            bad.append('module %s has no FRAG row and LATER does not name it' % m)
        if m in frag_mods and m in LATER:
            bad.append('module %s has a FRAG row, but LATER says PLAN %s creates it' % (m, LATER[m]))
    if bad:
        raise Stale('; '.join(bad))


# ----------------------------------------------------------------- the README
def render(d):
    frows, rows, floc, rloc, rctx, where = d['frows'], d['rows'], d['floc'], d['rloc'], d['rctx'], d['where']
    fmod = {f['frag']: f['module'] for f in frows}
    unknown = sorted({f['module'] for f in frows} | {r['module'] for r in rows})
    unknown = [m for m in unknown if m not in PLAN12]
    if unknown:
        raise Stale('modules with no MODULES row (add it from PLAN §1.2): %s' % ', '.join(unknown))
    module_checks(d)
    guests = [r for r in rows if r['module'] != fmod[r['frag']]]
    regs = markers(d)
    regnames = {n for n, _, _ in regs}
    prefixes = collections.OrderedDict()
    for n, fl, ln in regs:
        prefixes.setdefault(n.split(':', 1)[0], []).append((n, fl, ln))
    sys.path.insert(0, os.path.join(d['gpsp'], 'tools', 'tests'))
    import extract  # noqa: E402
    rd = extract.enumerate_readers(d['gpsp'])
    reads, nreads, helpers = region_reads(d['gpsp'], rd)
    use = collections.defaultdict(set)
    for r, got in reads.items():
        for n in got:
            if n not in regnames:
                raise Stale('%s reads @extract region %s (line %s), which no marker defines'
                            % (r, n, ', '.join(map(str, got[n]))))
            use[n].add(r)
    sites, ho, bad = lab_sites(d)
    if bad:
        raise Stale('@lab: ' + '; '.join(bad))
    t14 = t14_counts(d['gpsp'])
    heads = [os.path.join(d['gpsp'], 'tools', 'refactor', 'generated', fl, 'fe_abi_asserts.h') for fl in T14_FLAVOURS]
    try:
        unb, ust = A.unbuilt(d['gpsp'], heads)
    except (A.Unbuilt, ValueError) as e:
        raise Stale('T14 unbuilt (abi_asserts.py unbuilt): %s' % e)
    at_inv = {loc: i for i, loc in enumerate(where)}
    unb_ctx = []
    for c, n, k, _ in unb:
        f, ln = c.rsplit(':', 1)
        i = at_inv.get((f, int(ln)))
        if i is None or not d['ctx'][i]:
            raise Stale('T14 unbuilt site %s %s: %s' % (c, n, 'not a line of the TU' if i is None else 'in no #if group'))
        unb_ctx.append((c, n, k, ctx_text(d['ctx'][i], where)))
    dc, guards = defines(d)
    dups = dup_names(rows)
    frag_order = [f['frag'] for f in frows]

    def tu_key(r):
        return (frag_order.index(r['frag']), rloc[r['key']][1])
    nfr = sum(1 for f in frows if f['module'] != 'shell')
    il = d['inc_lines']
    fp_inc = [i for i, ln in enumerate(d['shell'].split('\n'), 1) if ln.startswith('#include "fe/fe_priv.h"')]
    L = []
    w = L.append
    w('<!-- GENERATED by gpsp/tools/refactor/fe_readme.py; do not edit.  Regenerate after any change to')
    w('     tools/refactor/cutlist.txt, psp/main_psp.c, psp/fe/, the host tests that read main_psp.c or')
    w('     tools/refactor/generated/: python3 tools/refactor/fe_readme.py')
    w('     (python3 tools/refactor/fe_readme.py --check exits 1 when this file is stale).')
    w('     inputs: cutlist.txt sha256 %s; split.py inverse of the shell sha256 %s (%d lines) -->'
      % (d['cut_sha'][:16], d['inv_sha'][:16], d['inv_lines']))
    w('')
    w('# psp/fe: the fragments of main_psp.c')
    w('')
    w('`psp/main_psp.c` is ONE translation unit split into %d fragments (PLAN-P-beta5 §5): the shell '
      '`psp/main_psp.c` keeps `shell.a` (lines %d-%d) and `shell.b` (lines %d-%d, `main()`), and includes '
      'the other %d as `psp/fe/<name>.inc.c` at lines %d-%d, once each, in source order. '
      'This file is documentation only: it is in no build.'
      % (len(frows), floc[frows[0]['frag']][1], floc[frows[0]['frag']][2], floc[frows[-1]['frag']][1],
         floc[frows[-1]['frag']][2], nfr, il[0], il[-1]))
    w('')
    w('## Conventions')
    w('')
    w('- **"main_psp.c" means the TU.** In comments, docs and tool output, `main_psp.c` (and "this file") names the '
      'translation unit: the shell plus the fragments, in include order. "Above", "below", "earlier" and "later" '
      'follow that order, across fragment files: the end of `fe/power.a.inc.c` is directly above the start of '
      '`fe/iothread.inc.c`. `split.py inverse --split gpsp -o F` writes that order out as one file.')
    w('- **Line cites.** A tool cites a line of the split tree as `fe/X.inc.c:N`, or `main_psp.c:N` for the shell '
      '(T14 `abi_asserts.py`, T8\'s raw view). A `main_psp.c:N` or `L:N`/`N:N` written before the split (comments, '
      'docs, records) is a line of the single file it was written against; `split.py inverse` reproduces the '
      'single file of this tree, which is what `tools/refactor/cutlist.txt` describes (its `source:` line names '
      'the inverse\'s sha256).')
    w('- **Fragments are generated.** Each `fe/*.inc.c` is a verbatim slice: no banner, no include guard, no '
      '`#include` of its own beyond the source\'s. It is regenerated or a change is replayed into it, never '
      'hand-edited. `split.py split` and `split.py gen` read the single file, so on this tree they take the '
      'inverse: regenerate with `split.py inverse --split gpsp -o F`, then `split.py split --src F --cutlist '
      'gpsp/tools/refactor/cutlist.txt --out DIR` (the cutlist: `split.py gen --split gpsp --carry '
      'gpsp/tools/refactor/cutlist.txt -o C`). Replay a change with `split.py replay --split gpsp '
      '--cutlist gpsp/tools/refactor/cutlist.txt --patch P --out DIR2`, where P is a git diff of `psp/main_psp.c` '
      'as one file (the inverse\'s text): replay applies the file whose `+++ b/` path ends in `main_psp.c` and '
      'ignores any other file in P. DIR and DIR2 are new directories; copy '
      'their `psp/main_psp.c` and `psp/fe/*.inc.c` over the tree. A fragment\'s first line is its FRAG anchor. '
      'A name sits in a fragment by position, which is not always its owner\'s fragment (see "Rows outside '
      'their owner\'s fragments").')
    noops = noop_stubs(d)

    def gtxt(h):
        return ', not counting its include guard `%s`' % guards[h] if guards[h] else ''

    def andlist(xs):
        xs = list(xs)
        return xs[0] if len(xs) == 1 else ', '.join(xs[:-1]) + ' and ' + xs[-1]
    w('- **Macros.** `fe/fe_priv.h` (included once, `main_psp.c:%s`, after the last project header) holds the '
      'macros P5 moved (%d `#define` lines%s), and `fe/play_defaults.h` the `PLAY_*` ship constants and link '
      'defaults (%d%s). They are not every macro of the TU: %d `#define` lines stay in the shell and %d in the '
      'fragments, at their definition points: the macro-globals (an object in another arm), the macros a host '
      'test reads through an @extract marker, local helpers, and %d release no-op stubs that sit in the `#else` '
      'arm beside the telemetry function they replace (%s). A macro missing from fe_priv.h is not necessarily '
      'one of the first two kinds: grep the name.'
      % (','.join(map(str, fp_inc)) or '?', dc['fe_priv.h'], gtxt('fe_priv.h'), dc['play_defaults.h'],
         gtxt('play_defaults.h'), dc['shell'], dc['fragments'],
         len(noops), ', '.join('`%s` %s:%d' % x for x in noops) or 'none'))
    w('- **Core-ABI hand copies.** The TU redeclares core functions and globals instead of including the core '
      'headers. T14 (`abi_asserts.py`) asserts a copy when the check TU\'s headers declare its name (`#include '
      '"common.h"`, the core\'s header chain, and `psp/sleep_q.h`), in a compile-only check TU that is never '
      'linked. `tools/refactor/generated/<flavour>/fe_abi_asserts.h` cites every copy in that flavour\'s `-E` as '
      '`fe/X.inc.c:N`: the asserted ones, then the UNCHECKED list, the copies in that `-E` that T14 cannot compare '
      '(a name the check TU\'s headers do not declare, or a frontend-only type) -- %s. A copy in an `#if` arm that '
      'none of %s compiles is in no generated header: `abi_asserts.py unbuilt --src gpsp` lists those sites from '
      'a raw scan of the TU with every `#if` arm kept. Today that is %d of its %d copy sites (the headers cite the '
      'other %d, and %d copies that a macro call expands, which the raw text does not show): "Core-ABI copies in '
      'no T14 flavour" below. The index in `fe/fe_priv.h` is PLAN-P-beta5 §5.2\'s set (every name T14 asserts, '
      'plus six it cannot), not every copy.'
      % ('; '.join('%s %d asserted + %d constants, %d unchecked' % x for x in t14), andlist(T14_FLAVOURS),
         len(unb), ust['raw'], ust['cited'], len(ust['macro'])))
    dl = []
    for kn in sorted(dups, key=lambda k: min(tu_key(r) for r in rows if (r['kind'], r['name']) == k)):
        rs = sorted([r for r in rows if (r['kind'], r['name']) == kn], key=tu_key)
        inner = [rctx[r['key']][-1] if rctx[r['key']] else None for r in rs]
        one_if = None not in inner and len({x[0] for x in inner}) == 1 and len({x[1][-1] for x in inner}) == len(inner)
        dl.append('`%s` (%s, %s%s)' % (kn[1], kn[0], ', '.join('`%s`' % cite(rloc[r['key']]) for r in rs),
                                       ': one per arm of the `#if` group at %s' % cite(where[inner[0][0] - 1]) if one_if else ''))
    w('- **Row names.** A cutlist row is one F (function definition) or G (global definition) site. Below, a name '
      'the cutlist defines once is written bare; a name it defines more than once is written `name#N` for each '
      'definition, N counting them in TU order (the cutlist\'s key): %s.' % ('; '.join(dl) or 'none'))
    w('- **`/* @lab */`** is a grep tag, nothing else: it sits in front of each of the %d `LAB_HOOK(...)` / '
      '`LAB_FFP(...)` sites (Lab-build hooks from `lab_harness.h` / `lab_ffpace.h`; empty in every other build), '
      'so `grep -n "@lab" main_psp.c fe/*.inc.c` lists the Lab\'s reach into the TU. `LAB_HANDOFF` is reached only '
      'through `FE_HANDOFF` (`fe/fe_priv.h:%s`).' % (len(sites), ','.join(map(str, ho))))
    w('- **`#if` context** (below) is the chain of `#if` groups a line sits in, outermost first, each with the '
      'line of its `#if` and the arm (`#else` after `#if X` is the arm where X is false). It is read from the '
      'source text; whether a build compiles a line depends on that build\'s defines.')
    w('')
    w('### Core-ABI copies in no T14 flavour')
    w('')
    w('The copy sites (`extern` declarations at file or block scope, file-scope prototypes of a function the TU '
      'does not define) that none of `tools/refactor/generated/{%s}/fe_abi_asserts.h` cites, so nothing checks '
      'them: `abi_asserts.py unbuilt` (the same extraction as T14\'s `-E` view, run on the raw TU with every `#if` '
      'arm kept). Each sits in an `#if` arm that none of those flavours compiles.' % ','.join(T14_FLAVOURS))
    w('')
    if unb_ctx:
        w(trow('copy', 'at', 'kind', '`#if` context'))
        w('|---|---|---|---|')
        for c, n, k, cx in unb_ctx:
            w(trow('`%s`' % n, '`%s`' % c, k, cx))
    else:
        w('None: every copy site is in some T14 flavour\'s `-E`.')
    w('')
    w('### @extract markers')
    w('')
    w('`/* @extract:begin NAME */ ... /* @extract:end NAME */` delimits text a host test reads '
      '(`tools/tests/extract.py`; `read_source()` expands the fragments in place, so a test reads the TU). '
      '%d regions; each lies inside one file. Readers: the %d host tests `extract.py --enumerate` finds. A test '
      'reads a region with `extract.extract(src, NAME)` or through a helper that passes its argument on (%s); '
      'NAME is resolved from the test\'s literals, and the generator refuses a read it cannot resolve.'
      % (len(regs), len(rd), ', '.join('`%s`' % h for h in helpers) or 'none'))
    w('')
    w(trow('prefix', 'meaning', 'regions', 'in', 'read by'))
    w('|---|---|---|---|---|')
    for p, rs in prefixes.items():
        fls = sorted({fl for _, fl, _ in rs}, key=lambda x: (x != 'main_psp.c', x))
        rdrs = set()
        for n, _, _ in rs:
            rdrs |= use.get(n, set())
        w(trow('`%s:`' % p, PREFIX_MEANING.get(p) or range_meaning([len(use.get(n, ())) for n, _, _ in rs]), len(rs),
               ', '.join('`%s`' % x for x in fls),
               ', '.join('`%s`' % os.path.basename(x) for x in sorted(rdrs)) or '(none found)'))
    w('')
    w('Every region, in TU order, and the host tests that read it:')
    w('')
    w(trow('region', 'begins at', 'read by'))
    w('|---|---|---|')
    for n, fl, ln in regs:
        w(trow('`%s`' % n, '`%s:%d`' % (fl, ln),
               ', '.join('`%s`' % os.path.basename(x) for x in sorted(use.get(n, ()))) or '(none)'))
    noread = [os.path.basename(r) for r in rd if not nreads[r]]
    if noread:
        w('')
        w('Readers that read the TU text but no region: %s.' % ', '.join('`%s`' % x for x in noread))
    w('')
    w('## Modules')
    w('')
    w('"responsibility" is PLAN.md §1.2\'s cell, verbatim (its line numbers are 01067e74\'s, its § PLAN.md\'s). '
      '"exported" is PLAN.md §1.3\'s exported ABI (the weak overrides are a group there, not names). "never a TU" '
      'marks PLAN.md §1.1\'s "never real TUs, by rule". The rest comes from the cutlist and the tree: "owns" counts '
      'the F (function) and G (global) rows whose owner column is the module; "elsewhere" those of them that sit in '
      'another module\'s fragment; "in no `#if`" those of them that sit in no `#if` group of the TU, so every build '
      'of main_psp.c compiles them.')
    w('')
    w(trow('§1.2', 'module', 'responsibility (PLAN.md §1.2)', 'exported (§1.3)', 'never a TU (§1.1)', 'fragments',
           'owns F / G', 'elsewhere', 'in no `#if` F / G'))
    w('|---|---|---|---|---|---|---|---|---|')
    for m, (num, resp) in PLAN12.items():
        fr = [f['frag'] for f in frows if f['module'] == m]
        mine = [r for r in rows if r['module'] == m]
        nf = sum(1 for r in mine if r['kind'] == 'F')
        ng = sum(1 for r in mine if r['kind'] == 'G')
        ne = sum(1 for r in guests if r['module'] == m)
        uf = sum(1 for r in mine if r['kind'] == 'F' and not rctx[r['key']])
        ug = sum(1 for r in mine if r['kind'] == 'G' and not rctx[r['key']])
        w(trow(num, m, resp, ', '.join('`%s`' % x for x in EXPORTED.get(m, [])) or '-',
               'yes' if m in NEVER_TU else '-', ', '.join('`%s`' % x for x in fr) or '-', '%d / %d' % (nf, ng), ne,
               '%d / %d' % (uf, ug)))
    nofrag = [m for m in PLAN12 if m in LATER]
    have, none = [], []
    for m in nofrag:
        mine = sorted([r for r in rows if r['module'] == m], key=tu_key)
        if not mine:
            none.append(m)
            continue
        kinds = [(k, sum(1 for r in mine if r['kind'] == k)) for k in ('F', 'G')]
        byfile = collections.OrderedDict()
        for r in mine:
            byfile.setdefault(rloc[r['key']][0], []).append(r)
        # a fragment is not main_psp.c, so a row there is outside main() (which is in shell.b)
        outside = all(rloc[r['key']][0] != 'main_psp.c' for r in mine)
        have.append('%s owns %s, which sit%s in %s%s (see "Rows outside their owner\'s fragments")' % (
            m, andlist(['%d %s row%s' % (n, k, '' if n == 1 else 's') for k, n in kinds if n]),
            's' if len(mine) == 1 else '',
            andlist(['`%s` (line%s %s: %s)' % (
                fl, '' if len(rs) == 1 else 's', ', '.join(map(str, sorted({rloc[r['key']][1] for r in rs}))),
                ', '.join('`%s`' % row_name(r, dups) for r in rs)) for fl, rs in byfile.items()]),
            ', outside `main()`' if outside else ''))
    tail = '; '.join(have) if have else 'none of them owns one'
    if have and none:
        tail += '; %s own%s none' % (andlist(none), 's' if len(none) == 1 else '')
    w('')
    w('%s have no fragment yet (no FRAG row): PLAN.md §2 creates them later (%s). Of the cutlist\'s rows, %s.'
      % (', '.join(nofrag), ', '.join('%s %s' % (m, LATER[m]) for m in nofrag), tail))
    w('')
    w('### %s functions and their `#if` context' % ' and '.join(ROLES))
    w('')
    w('Every F row of these modules, at its first line (TU order). A row in no `#if` group is compiled by every '
      'build of main_psp.c, a player build included.')
    for m in ROLES:
        fs = sorted([r for r in rows if r['module'] == m and r['kind'] == 'F'], key=tu_key)
        nonif = [r for r in fs if not rctx[r['key']]]
        w('')
        w('**%s**: %d F rows, %d in no `#if`.' % (m, len(fs), len(nonif)))
        w('')
        w(trow('function', 'at', '`#if` context'))
        w('|---|---|---|')
        for r in fs:
            w(trow('`%s`' % row_name(r, dups), '`%s`' % cite(rloc[r['key']]),
                   ctx_text(rctx[r['key']], where) or 'none: every build'))
    w('')
    w('## Fragments, in TU order')
    w('')
    w(trow('#', 'fragment', 'file', 'lines', 'module', 'F / G rows', 'rows of other modules'))
    w('|---|---|---|---|---|---|---|')
    for k, f in enumerate(frows, 1):
        fl, a, b = floc[f['frag']]
        here = [r for r in rows if r['frag'] == f['frag']]
        g = collections.Counter(r['module'] for r in here if r['module'] != f['module'])
        w(trow(k, f['frag'], '`%s`' % fl, '%d-%d' % (a, b), f['module'],
               '%d / %d' % (sum(1 for r in here if r['kind'] == 'F'), sum(1 for r in here if r['kind'] == 'G')),
               ', '.join('%s %d' % (mm, g[mm]) for mm in modules.MODULE_ORDER if g[mm]) or '-'))
    w('')
    w('## Rows outside their owner\'s fragments')
    w('')
    w('%d cutlist rows sit in a fragment of another module: the split keeps TU order, so a definition stays '
      'where the source had it (PLAN §1.3: the owner is the module that writes it; a definition must precede '
      'its first use). Per fragment, in TU order: where the row\'s first line is, its kind, name and owner.' % len(guests))
    for f in frows:
        gs = [r for r in guests if r['frag'] == f['frag']]
        if not gs:
            continue
        w('')
        w('**%s** (module %s):' % (f['frag'], f['module']))
        w('')
        for r in gs:
            w('- `%s` %s `%s` -> %s' % (cite(rloc[r['key']]), r['kind'], row_name(r, dups), r['module']))
    w('')
    w('## Functions per fragment')
    w('')
    w('Every F row (function definition), at its first line, in TU order; `(owner)` follows a function owned by '
      'another module. Names as in "Row names" above (`name#N` only for a name defined more than once).')
    w('')
    for f in frows:
        fs = [r for r in rows if r['frag'] == f['frag'] and r['kind'] == 'F']
        if not fs:
            continue
        w('- **%s** (`%s`): %s' % (f['frag'], floc[f['frag']][0], ', '.join(
            '%s:%d%s' % (row_name(r, dups), rloc[r['key']][1], '' if r['module'] == f['module'] else ' (%s)' % r['module'])
            for r in fs)))
    w('')
    text = '\n'.join(L)
    bad = gfm_table_errors(text)
    if bad:
        raise Stale('%d table row(s) would render with another GFM cell count than their header (an unescaped `|` '
                    'in a cell?): %s' % (len(bad), '; '.join('line %d: %d cells, header %d' % (b[0], b[2], b[1])
                                                            for b in bad[:6])))
    return text


# ----------------------------------------------------------------- --plan
def plan_check(path):
    """[difference] between PLAN12 / EXPORTED / NEVER_TU / LATER and PLAN.md"""
    t = open(path, encoding='utf-8').read()
    try:
        s12 = t.index('### 1.2 Module list')
        s13 = t.index('### 1.3', s12)
        s14 = t.index('### 1.4', s13)
        s11 = t.index('### 1.1', 0, s12)
        s2 = t.index('## 2. Staged milestones')
        s3 = t.index('## 3.', s2)
    except ValueError as e:
        return ['%s: a section heading is missing (%s)' % (path, e)]
    cells = {}
    for ln in t[s12:s13].split('\n'):
        if ln.startswith('| ') and not ln.startswith('| #') and not ln.startswith('|---'):
            c = [x.strip() for x in ln.split('|')[1:-1]]
            cells[c[0]] = c
    diffs = []
    for m, (num, resp) in PLAN12.items():
        c = cells.get(num)
        if c is None:
            diffs.append('%s: PLAN §1.2 has no row %s' % (m, num))
        elif c[2] != resp:
            diffs.append('%s (row %s): PLAN12 %r != PLAN %r' % (m, num, resp, c[2]))
    known = {n for n, _ in PLAN12.values()} | {'0', '1'}
    for num in cells:
        if num not in known:
            diffs.append('PLAN §1.2 row %s is not in PLAN12' % num)
    forever = {m for m, (num, _) in PLAN12.items() if num in cells and 'forever' in cells[num][1]}
    if forever != set(NEVER_TU):
        diffs.append('NEVER_TU %s != the "(F, forever)" rows of §1.2 %s' % (sorted(NEVER_TU), sorted(forever)))
    rule = re.search(r'\*\*Never real TUs, by rule\.\*\*([^\n]*)', t[s11:s12])
    if not rule:
        diffs.append('§1.1 has no "Never real TUs, by rule." sentence')
    b13 = re.search(r'\*\*Exported ABI keeps names and linkage\.\*\*([^\n]*)', t[s13:s14])
    if not b13:
        diffs.append('§1.3 has no "Exported ABI keeps names and linkage." bullet')
    else:
        # the bullet's next sentence names two non-exported symbols that P2 may make static
        named = set(re.findall(r'`(\w+)`', b13.group(1).split('Two non-static')[0]))
        ours = {n for v in EXPORTED.values() for n in v}
        if named != ours:
            diffs.append('EXPORTED %s != the names of §1.3\'s exported-ABI bullet %s' % (sorted(ours), sorted(named)))
    sec2 = t[s2:s3]
    for m, item in LATER.items():
        hw, it = item.split()
        h = sec2.find('### %s:' % hw)
        if h < 0:
            diffs.append('LATER %s: no "### %s:" in PLAN §2' % (m, hw))
            continue
        e = sec2.find('\n### ', h + 1)
        body = sec2[h:e if e > 0 else len(sec2)]
        pat = {'teardown': r'\*\*C1\. [^\n]*session_end', 'boot': r'\*\*C2\. `boot_run',
               'session': r'\*\*C3\. Session phases', 'loop': r'frame_iter\('}[m]
        if it[0] == 'C' and not re.search(pat, body):
            diffs.append('LATER %s: %s %s does not name it in PLAN §2' % (m, hw, it))
        if it == 'D1' and not (re.search(r'epoch D1', body) and re.search(pat, body)):
            diffs.append('LATER %s: %s is not epoch D1 with frame_iter in PLAN §2' % (m, hw))
    return diffs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gpsp', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('-o', default=None, help='default GPSP/psp/fe/README.md')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--plan', default=None, help='PLAN.md: check the transcribed tables against it, write nothing')
    ap.add_argument('--gfm', nargs='+', default=None, metavar='FILE',
                    help='check the GFM cell count of every table row of FILE (any Markdown file), write nothing')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.gfm:
        rc = 0
        for p in a.gfm:
            t = open(p, encoding='utf-8', newline='').read().replace('\r\n', '\n')
            bad = gfm_table_errors(t)
            ls = t.split('\n')
            ntab = sum(1 for i in range(len(ls) - 1) if ls[i].startswith('|') and GFM_DELIM.match(ls[i + 1])
                       and (i == 0 or not ls[i - 1].startswith('|')))
            print('fe_readme --gfm %s: %d tables, %d rows whose GFM cell count is not their header\'s' % (p, ntab, len(bad)))
            for ln, n, m, row in bad:
                print('  line %d: header %d cells, row %d: %s' % (ln, n, m, row[:120]))
            rc |= 1 if bad else 0
        return rc
    if a.selftest:
        sys.path.insert(0, os.path.join(HERE, 'tests'))
        import importlib
        return importlib.import_module('fe_readme_test').run(a.gpsp)
    if a.plan:
        diffs = plan_check(a.plan)
        for x in diffs:
            print('fe_readme: PLAN DIFFERS: %s' % x, file=sys.stderr)
        if not diffs:
            print('fe_readme: PLAN12 (%d rows), EXPORTED (%d names), NEVER_TU (%d), LATER (%d) == %s'
                  % (len(PLAN12), sum(len(v) for v in EXPORTED.values()), len(NEVER_TU), len(LATER), a.plan))
        return 1 if diffs else 0
    out = a.o or os.path.join(a.gpsp, 'psp', 'fe', 'README.md')
    try:
        text = render(load(a.gpsp))
    except (Stale, S.Refused) as e:
        print('fe_readme: REFUSED: %s' % e, file=sys.stderr)
        return 1
    if a.check:
        old = open(out, encoding='utf-8', newline='').read() if os.path.exists(out) else None
        if old == text:
            print('fe_readme: %s is current' % out)
            return 0
        print('fe_readme: STALE: %s is %s what the cutlist and the split tree give; regenerate it'
              % (out, 'missing, not' if old is None else 'not'), file=sys.stderr)
        return 1
    with open(out, 'w', encoding='utf-8', newline='\n') as fo:
        fo.write(text)
    print('fe_readme: wrote %s (%d lines)' % (out, text.count('\n')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
