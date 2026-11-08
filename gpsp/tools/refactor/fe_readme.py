#!/usr/bin/env python3
"""fe_readme.py -- generate psp/fe/README.md (PLAN-P-beta5 §5.2): the module descriptions, owners
and the fragment -> module map of the split main_psp.c, plus the conventions a reader of the split
needs.  The README is a doc file in no build; it is GENERATED, never hand-edited.

Inputs (all read, none written except -o):
  * tools/refactor/cutlist.txt: the FRAG rows (fragment -> module, in TU order) and the F/G rows
    (item -> owner module -> the fragment it sits in).
  * the split tree: psp/main_psp.c (the shell) and psp/fe/*.inc.c.  The rows' anchors are resolved
    in split.py's inverse of the shell, then mapped back to the file and line that hold them, so
    every cite is `fe/X.inc.c:N` (or `main_psp.c:N` for the shell).
  * MODULES below: PLAN.md §1.2's module table (rows #0-#20, responsibility column) with the
    PLAN-P-beta5 §2 deltas, transcribed.
  * the @extract markers in the shell and fragments, and the host tests that read them
    (tools/tests/extract.py --enumerate), for the marker table; the `/* @lab */` tags.

Checks (exit 1, nothing written): the cutlist's `source:` sha256 is the inverse's (a stale cutlist
is refused); every FRAG row's anchor is the first line of its fragment file, in the shell's
include order; every F/G row's anchor sits in the fragment its row names; every LAB_HOOK( /
LAB_FFP( site carries its `/* @lab */` tag and every tag is on such a site; LAB_HANDOFF appears only
in fe_priv.h's FE_HANDOFF.

Usage:
  fe_readme.py [--gpsp DIR] [-o FILE]   write DIR/psp/fe/README.md (default DIR = this tree's gpsp/)
  fe_readme.py [--gpsp DIR] --check     exit 1 if the committed README is not what this generates
  fe_readme.py --selftest               mutants on a scratch copy (tests/fe_readme_test.py)
Exit: 0 ok, 1 refused / stale, 2 usage.
"""
import argparse
import collections
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import split as S  # noqa: E402  (read_cutlist, read_split, inverse_text, INC_RE, Refused)
import modules  # noqa: E402

# PLAN.md §1.2 module table (responsibility column, abridged only by dropping L: cites) and §1.1's
# "never real TUs, by rule"; PLAN-P-beta5 §2 adds no row.  '#' is the PLAN row number.
MODULES = collections.OrderedDict([
    ('shell', ('-', 'psp/main_psp.c itself: module info and thread attributes, `layout_pad_cold` and '
               '`layout_pad_ro`, the `#include` list (fe_priv.h, then the fragments) and `main()`, '
               'which holds boot, session, the frame loop and the teardown until phases C/D1')),
    ('paths', ('20', 'ms0 paths and boot identity: `init_paths`, the `p_*` path buffers; '
               '`g_dir_base` (exported)')),
    ('audio', ('2', 'ring, `audio_thread`, `plat_audio_frames`, start/stop (the SQ contract), resample '
               'step, status, link-audio trims; `in_rate`, `g_audio_step` (exported). Never a real TU')),
    ('iothread', ('3', 'the ADR-0025 writer thread, SRAM sync, the stop-before-shutdown contract, the SQ '
                  'stop codes. Never a real TU')),
    ('input', ('4', 'pad read, ctl maps, `plat_input_bitmask`, pad_swallow, turbo, chord state, '
               'input_fresh')),
    ('power', ('5', '`exit_cb`, `cb_thread`, `power_cb`, `pwr_hold`/`pwr_release`/`pwr_release_all`, the '
               'play lock, the park handshake (`loop_park`, `browser_park`), the sleep-quiesce ops '
               '(`sq_*`), `power_request_poll`, standby down/up, wake service and overlay; `g_running` '
               '(exported). Never a real TU')),
    ('display', ('6', 'CPU clock, display apply, colour preview, wake snapshot')),
    ('merend', ('7', 'ME renderer state, lifecycle, presenter, retire, `fill_desc`, the vmem pool, legacy '
                'me_video. Never a real TU')),
    ('pacing', ('8', 'latency tiers `lat_*`/`ll_*`/`dfr_*`/`lf_*`/`ffh_*`, the vblank target, '
                '`us_to_vblank`, frame-hist, the present-plus-overlays helper; `g_ff_uncapped`, '
                '`g_ff_mult` (exported). Never a real TU')),
    ('rewind', ('9', 'rewind tick, pool offer/check/lost/back (pool_lost runs on cb_thread), HUD, ini')),
    ('link', ('10', 'net bring-up/teardown, cable, the GB link session, link UI actions, silent wireless, '
              'RFU hooks and trace (strong overrides of weak stubs); `g_net_up`, `g_net_is_host` '
              '(exported)')),
    ('mgift', ('11', 'Mystery Gift glue (`GBADHOC_MGIFT` builds)')),
    ('boost', ('12', 'the emu boost warden')),
    ('misc', ('13', 'wall clocks, screenshot, backup_save, state slots, the cheats lock')),
    ('inproc', ('14', '`inproc_refusal`, `relaunch_eboot`, heap census helpers')),
    ('teardown', ('15', 'the exit sequencer (`session_end`, `process_end`, ...; phase C). No fragment yet: '
                  'the copies are in `main()`')),
    ('boot', ('16', '`boot_run` (phase C). No fragment yet: the boot order is in `main()`; boot already owns '
              'the boot-forever knobs written once at boot')),
    ('session', ('17', '`session_select` ... `session_pre_loop` (phase C). No fragment yet: in `main()`')),
    ('loop', ('18', 'the frame loop (phase D1). No fragment yet: the `while (g_running)` loop in `main()`')),
    ('harness', ('19', 'stall watch, DRPROF_HW, core_health, rig identity, uiscript, the sleep simulator, '
                 'testpat, nettest, vram_probe, vid_prof, preempt, ff_win, eboot_crc, '
                 '`gpsp_net_wait_beat` (exported, rig builds); compiled out of a player build except '
                 'the release-live probes')),
])

# What each @extract name prefix means (tools/refactor/docs/source-split.md, P1).  A prefix not
# listed here is reported as "a range" with its readers, and the generator does not fail on it.
PREFIX_MEANING = {
    'fn': 'one whole function: its definition line through its closing brace\'s line',
    'def': 'the `#define` line(s) a test greps, by name',
    'decl': 'one declaration / object definition a test greps, by name',
}
RANGE_MEANING = 'a statement range one test slices (its old start/end anchors are checked inside it)'

MARK = re.compile(r'/\* @extract:begin ([A-Za-z0-9_.:-]+) \*/')
LAB_TAG = '/* @lab */'
LAB_SITE = re.compile(r'\bLAB_(?:HOOK|FFP)\(')
DEFINE = re.compile(r'^\s*#\s*define\b')
SRC_SHA = re.compile(r'^# source: .*\bsha256 ([0-9a-f]{64})\b')


class Stale(Exception):
    pass


def _rel(gpsp, p):
    return os.path.relpath(p, os.path.join(gpsp, 'psp')).replace('\\', '/')


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
    idx = collections.defaultdict(list)
    for i, ln in enumerate(lines, 1):
        idx[ln].append(i)

    def resolve(occ, anchor, what):
        hits = idx.get(anchor, [])
        if occ > len(hits):
            raise Stale('%s: anchor not found in the inverse (occ %d): %r' % (what, occ, anchor))
        return where[hits[occ - 1] - 1]

    # FRAG rows: each non-shell fragment's anchor is line 1 of its file; the files are the shell's
    # includes in FRAG order; shell fragments sit in main_psp.c
    inc_order = [S.INC_RE.match(ln).group(1) for ln in incs]
    want = [f['frag'] for f in frows if f['module'] != 'shell']
    if inc_order != want:
        raise Stale('the shell includes %s, the FRAG rows list %s' % (inc_order, want))
    floc = {}
    for f in frows:
        fl, ln = resolve(f['occ'], f['anchor'], 'FRAG ' + f['frag'])
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
    rloc = {}
    for r in rows:
        fl, ln = resolve(r['occ'], r['anchor'], r['key'])
        fr = floc[r['frag']]
        if fl != fr[0] or not (fr[1] <= ln <= fr[2]):
            raise Stale('%s: sits at %s:%d, the cutlist says fragment %s (%s:%d-%d)' % (r['key'], fl, ln, r['frag'], *fr))
        rloc[r['key']] = (fl, ln)
    return {'gpsp': gpsp, 'shell': shell, 'frags': frags, 'frows': frows, 'rows': rows, 'floc': floc,
            'rloc': rloc, 'inv_sha': inv_sha, 'inv_lines': len(lines), 'inc_lines': inc_lines,
            'cut_sha': hashlib.sha256(cut_text.encode('utf-8')).hexdigest()}


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


def readers_of(gpsp, prefixes):
    """{region name or 'prefix:' : [reader]} from the host tests' string literals."""
    sys.path.insert(0, os.path.join(gpsp, 'tools', 'tests'))
    import extract  # noqa: E402
    lit = re.compile(r'["\']((?:%s):[A-Za-z0-9_.:-]*)["\']' % '|'.join(map(re.escape, sorted(prefixes))))
    use = collections.defaultdict(set)
    rd = extract.enumerate_readers(gpsp)
    for r in rd:
        t = open(os.path.join(gpsp, r), encoding='utf-8', errors='replace').read()
        for m in lit.finditer(t):
            use[m.group(1)].add(r)
    return use, rd


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


def defines(d):
    c = collections.Counter()
    for fl, t in file_texts(d):
        c['shell' if fl == 'main_psp.c' else 'fragments'] += sum(1 for ln in t.split('\n') if DEFINE.match(ln))
    for h in ('fe_priv.h', 'play_defaults.h'):
        p = os.path.join(d['gpsp'], 'psp', 'fe', h)
        c[h] = sum(1 for ln in open(p, encoding='utf-8').read().split('\n') if DEFINE.match(ln))
    return c


def cite(loc):
    return '%s:%d' % loc


def render(d):
    frows, rows, floc, rloc = d['frows'], d['rows'], d['floc'], d['rloc']
    fmod = {f['frag']: f['module'] for f in frows}
    unknown = sorted({f['module'] for f in frows} | {r['module'] for r in rows})
    unknown = [m for m in unknown if m not in MODULES]
    if unknown:
        raise Stale('modules with no MODULES row (add it from PLAN §1.2): %s' % ', '.join(unknown))
    guests = [r for r in rows if r['module'] != fmod[r['frag']]]
    regs = markers(d)
    prefixes = collections.OrderedDict()
    for n, fl, ln in regs:
        prefixes.setdefault(n.split(':', 1)[0], []).append((n, fl, ln))
    use, rd = readers_of(d['gpsp'], prefixes)
    sites, ho, bad = lab_sites(d)
    if bad:
        raise Stale('@lab: ' + '; '.join(bad))
    dc = defines(d)
    nfr = sum(1 for f in frows if f['module'] != 'shell')
    il = d['inc_lines']
    fp_inc = [i for i, ln in enumerate(d['shell'].split('\n'), 1) if ln.startswith('#include "fe/fe_priv.h"')]
    L = []
    w = L.append
    w('<!-- GENERATED by gpsp/tools/refactor/fe_readme.py; do not edit.  Regenerate after any change to')
    w('     tools/refactor/cutlist.txt, psp/main_psp.c or psp/fe/: python3 tools/refactor/fe_readme.py')
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
      '`#include` of its own beyond the source\'s. It is regenerated (`split.py split` with the cutlist) or a change '
      'is replayed into it (`split.py replay --split DIR --cutlist tools/refactor/cutlist.txt --patch P`, a patch '
      'of the inverse), never hand-edited. A fragment\'s first line is its FRAG anchor. A name sits in a fragment '
      'by position, which is not always its owner\'s fragment (see "Rows outside their owner\'s fragments").')
    noops = noop_stubs(d)
    w('- **Macros.** `fe/fe_priv.h` (included once, `main_psp.c:%s`, after the last project header) holds the '
      'macros P5 moved: %d `#define` lines, and `fe/play_defaults.h` the `PLAY_*` ship constants and link defaults '
      '(%d). They are not every macro of the TU: %d `#define` lines stay in the shell and %d in the fragments, at '
      'their definition points: the macro-globals (an object in another arm), the macros a host test reads '
      'through an @extract marker, local helpers, and %d release no-op stubs that sit in the `#else` arm beside '
      'the telemetry function they replace (%s). A macro missing from fe_priv.h is not necessarily one of the '
      'first two kinds: grep the name.'
      % (','.join(map(str, fp_inc)) or '?', dc['fe_priv.h'], dc['play_defaults.h'], dc['shell'], dc['fragments'],
         len(noops), ', '.join('`%s` %s:%d' % x for x in noops) or 'none'))
    w('- **Core-ABI hand copies.** The TU redeclares core functions and globals instead of including the core '
      'headers (index in `fe/fe_priv.h`). T14 asserts them in a compile-only check TU that is never linked: '
      '`tools/refactor/generated/<flavour>/fe_abi_asserts.h` cites every copy as `fe/X.inc.c:N` and lists the '
      'unchecked ones.')
    w('- **`/* @lab */`** is a grep tag, nothing else: it sits in front of each of the %d `LAB_HOOK(...)` / '
      '`LAB_FFP(...)` sites (Lab-build hooks from `lab_harness.h` / `lab_ffpace.h`; empty in every other build), '
      'so `grep -n "@lab" main_psp.c fe/*.inc.c` lists the Lab\'s reach into the TU. `LAB_HANDOFF` is reached only '
      'through `FE_HANDOFF` (`fe/fe_priv.h:%s`).' % (len(sites), ','.join(map(str, ho))))
    w('')
    w('### @extract markers')
    w('')
    w('`/* @extract:begin NAME */ ... /* @extract:end NAME */` delimits text a host test reads '
      '(`tools/tests/extract.py`; `read_source()` expands the fragments in place, so a test reads the TU). '
      '%d regions; each lies inside one file. Readers: the %d host tests `extract.py --enumerate` finds.'
      % (len(regs), len(rd)))
    w('')
    w('| prefix | meaning | regions | in | read by |')
    w('|---|---|---|---|---|')
    for p, rs in prefixes.items():
        fls = sorted({fl for _, fl, _ in rs}, key=lambda x: (x != 'main_psp.c', x))
        rdrs = set(use.get(p + ':', set()))
        for n, _, _ in rs:
            rdrs |= use.get(n, set())
        w('| `%s:` | %s | %d | %s | %s |' % (p, PREFIX_MEANING.get(p, RANGE_MEANING), len(rs),
                                           ', '.join('`%s`' % x for x in fls),
                                           ', '.join('`%s`' % os.path.basename(x) for x in sorted(rdrs)) or '(none found)'))
    w('')
    w('## Modules')
    w('')
    w('PLAN.md §1.2 (responsibilities) and the cutlist (owners). "owns" counts the F (function) and G (global) '
      'rows whose owner column is the module; "elsewhere" those of them that sit in another module\'s fragment.')
    w('')
    w('| §1.2 | module | responsibility | fragments | owns F / G | elsewhere |')
    w('|---|---|---|---|---|---|')
    for m, (num, resp) in MODULES.items():
        fr = [f['frag'] for f in frows if f['module'] == m]
        nf = sum(1 for r in rows if r['module'] == m and r['kind'] == 'F')
        ng = sum(1 for r in rows if r['module'] == m and r['kind'] == 'G')
        ne = sum(1 for r in guests if r['module'] == m)
        w('| %s | %s | %s | %s | %d / %d | %d |' % (num, m, resp, ', '.join('`%s`' % x for x in fr) or '-', nf, ng, ne))
    w('')
    w('## Fragments, in TU order')
    w('')
    w('| # | fragment | file | lines | module | F / G rows | rows of other modules |')
    w('|---|---|---|---|---|---|---|')
    for k, f in enumerate(frows, 1):
        fl, a, b = floc[f['frag']]
        here = [r for r in rows if r['frag'] == f['frag']]
        g = collections.Counter(r['module'] for r in here if r['module'] != f['module'])
        w('| %d | %s | `%s` | %d-%d | %s | %d / %d | %s |' % (
            k, f['frag'], fl if fl == 'main_psp.c' else fl, a, b, f['module'],
            sum(1 for r in here if r['kind'] == 'F'), sum(1 for r in here if r['kind'] == 'G'),
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
            w('- `%s` %s `%s` -> %s' % (cite(rloc[r['key']]), r['kind'], r['key'].split(':', 1)[1], r['module']))
    w('')
    w('## Functions per fragment')
    w('')
    w('Every F row (function definition), at its first line, in TU order; `(owner)` follows a function owned by '
      'another module. A name defined twice (one per `#if` arm) is listed as `name#N`.')
    w('')
    for f in frows:
        fs = [r for r in rows if r['frag'] == f['frag'] and r['kind'] == 'F']
        if not fs:
            continue
        def nm(r):
            name, n = r['key'].split(':', 1)[1].rsplit('#', 1)
            return name if n == '1' else '%s#%s' % (name, n)
        w('- **%s** (`%s`): %s' % (f['frag'], floc[f['frag']][0], ', '.join(
            '%s:%d%s' % (nm(r), rloc[r['key']][1], '' if r['module'] == f['module'] else ' (%s)' % r['module'])
            for r in fs)))
    w('')
    return '\n'.join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gpsp', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('-o', default=None, help='default GPSP/psp/fe/README.md')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest:
        sys.path.insert(0, os.path.join(HERE, 'tests'))
        import importlib
        return importlib.import_module('fe_readme_test').run(a.gpsp)
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
